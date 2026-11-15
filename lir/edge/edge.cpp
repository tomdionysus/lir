//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "lir/edge/edge.h"

#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <array>
#include <atomic>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/stream_traits.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <deque>
#include <future>
#include <map>
#include <stdexcept>

#include "dannan/identity/crypto.h"
#include "dannan/listen/beast.h"
#include "dannan/listen/listener.h"

namespace lir::edge {

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;

using dannan::listen::Stream;

// How long a clean end may take before the socket is closed regardless:
// the listeners' close bound, and a WebSocket's close frame's.
constexpr auto clean_end_deadline = std::chrono::seconds(2);

std::string peer_of(const Stream& stream) {
  const auto& endpoint = stream.peer();
  return endpoint.address().to_string() + ":" + std::to_string(endpoint.port());
}

}  // namespace

bool presented_unverified(const dannan::listen::Stream& stream) {
  const auto& presented = stream.client_certificate();
  return presented && !presented->verified;
}

std::optional<contract::ClientCertificate> verified_certificate(const dannan::listen::Stream& stream) {
  const auto& presented = stream.client_certificate();
  if (!presented || !presented->verified) return std::nullopt;
  contract::ClientCertificate out;
  out.subject = presented->subject;
  out.thumbprint = presented->thumbprint;
  return out;
}

// A listener for the edge on its strand: the stream's executor is the
// strand itself, so every connection is confined with the broker.
dannan::listen::ListenerConfig listener_config(std::string name, const std::string& host, uint16_t port, size_t outbound, const TlsConfig* tls) {
  dannan::listen::ListenerConfig config;
  config.name = std::move(name);
  config.host = host;
  config.port = port;
  config.outbound_bound = outbound;
  config.close_bound = clean_end_deadline;
  config.strand_per_stream = false;
  if (tls) {
    config.certificates =
        std::make_shared<dannan::listen::Certificates>(dannan::listen::CertificatePaths{tls->certificate_chain, tls->private_key, tls->client_ca});
    // TLS 1.2 and up, as before: MQTT's small devices do not all speak 1.3.
    config.tls.minimum = dannan::listen::TlsVersion::tls12;
    using dannan::listen::ClientCertificates;
    config.tls.client_certificates =
        tls->client_ca.empty() ? ClientCertificates::never : (tls->require_client_certificate ? ClientCertificates::required : ClientCertificates::optional);
    config.handshake_timeout = tls->handshake_timeout;
  }
  return config;
}

namespace {

// The subprotocol to answer with, from the comma-separated list a client
// offered: "mqtt", else "mqttv3.1" (a 3.1 client); "" when nothing was
// offered; nothing when only others were.
std::optional<std::string> subprotocol(std::string_view offered) {
  bool any = false;
  bool v31 = false;
  while (!offered.empty()) {
    const size_t comma = offered.find(',');
    auto item = offered.substr(0, comma);
    while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) item.remove_prefix(1);
    while (!item.empty() && (item.back() == ' ' || item.back() == '\t')) item.remove_suffix(1);
    if (!item.empty()) any = true;
    if (item == "mqtt") return std::string("mqtt");
    if (item == "mqttv3.1") v31 = true;
    if (comma == std::string_view::npos) break;
    offered.remove_prefix(comma + 1);
  }
  if (v31) return std::string("mqttv3.1");
  if (any) return std::nullopt;
  return std::string();
}

// How long stop() and a waited call give the strand before giving up.
constexpr auto strand_deadline = 10s;

// A client connection as the edge holds it.
class ClientConnection : public contract::Connection {
 public:
  // Closes the socket now and tells the link, which is idempotent.
  virtual void terminate() = 0;
};

// MQTT straight over a listener's stream, TCP or TLS. Every member is
// touched on the edge's strand, which is the stream's executor. The stream
// queues what is sent, closes a reader too slow for the listener's outbound
// bound, and writes what is queued before its clean close.
class RawConnection final : public ClientConnection, public std::enable_shared_from_this<RawConnection> {
 public:
  RawConnection(std::shared_ptr<Stream> stream, std::function<void(ClientConnection*)> forget)
      : _stream(std::move(stream)), _forget(std::move(forget)), _peer(peer_of(*_stream)), _certificate(verified_certificate(*_stream)) {}

  void start(std::shared_ptr<broker::Link> link) {
    _link = std::move(link);
    read();
  }

  void send(mqtt::Bytes bytes) override {
    if (_closing || _closed) return;
    _stream->send(std::string(bytes.begin(), bytes.end()));
  }

  void close() override {
    if (_closing || _closed) return;
    _closing = true;
    _stream->when_finished([self = shared_from_this()] { self->terminate(); });
    _stream->close();
  }

  std::string peer() const override { return _peer; }
  std::optional<contract::ClientCertificate> certificate() const override { return _certificate; }
  std::string transport() const override { return _stream->transport(); }

  void terminate() override {
    if (_closed) return;
    _closed = true;
    _stream->abort();
    // The link and this connection hold each other; the cycle ends here.
    if (auto link = std::move(_link)) link->closed();
    _forget(this);
  }

 private:
  void read() {
    _stream->async_read_some(asio::buffer(_buffer), [self = shared_from_this()](const boost::system::error_code& ec, size_t got) {
      // A close the broker asked for ends the pending read: the stream's
      // clean end, not this, finishes the connection.
      if (self->_closing || self->_closed) return;
      if (ec) return self->terminate();
      if (self->_link) self->_link->receive(std::span<const uint8_t>(self->_buffer.data(), got));
      if (!self->_closing && !self->_closed) self->read();
    });
  }

  std::shared_ptr<Stream> _stream;
  std::function<void(ClientConnection*)> _forget;
  std::string _peer;
  std::optional<contract::ClientCertificate> _certificate;
  std::shared_ptr<broker::Link> _link;
  std::array<uint8_t, 16384> _buffer{};
  bool _closing = false;
  bool _closed = false;
};

// MQTT over WebSocket (section 6) over a listener's stream, TCP or TLS: one
// MQTT byte stream in binary frames, written one message at a time from a
// queue of the edge's own under the same bound, and a close frame before
// the stream's clean close (dannan/listen/beast.h). Every member is touched
// on the edge's strand.
class WebSocketConnection final : public ClientConnection, public std::enable_shared_from_this<WebSocketConnection> {
 public:
  WebSocketConnection(std::shared_ptr<Stream> stream, size_t maximum, dannan::status::ScopedLogger logger, std::function<void(ClientConnection*)> forget)
      : _stream(std::move(stream)),
        _ws(*_stream),
        _maximum(maximum),
        _logger(std::move(logger)),
        _forget(std::move(forget)),
        _peer(peer_of(*_stream)),
        _certificate(verified_certificate(*_stream)) {}

  // The upgrade, within `within`: read the request, choose the subprotocol,
  // answer. A request that is not a WebSocket upgrade, or offers only
  // subprotocols that are not MQTT's, gets 400 and is closed. `done` is told
  // on the strand whether the client is upgraded.
  void upgrade(std::chrono::milliseconds within, std::function<void(bool)> done) {
    auto deadline = std::make_shared<asio::steady_timer>(_stream->get_executor(), within);
    auto settled = std::make_shared<bool>(false);
    deadline->async_wait([stream = _stream, settled](const boost::system::error_code& ec) {
      if (ec || *settled) return;
      *settled = true;
      stream->abort();
    });
    auto settle = [deadline, settled] {
      *settled = true;
      deadline->cancel();
    };
    auto buffer = std::make_shared<beast::flat_buffer>();
    auto request = std::make_shared<http::request<http::empty_body>>();
    http::async_read(*_stream, *buffer, *request, [self = shared_from_this(), buffer, request, settle, done](const boost::system::error_code& ec, size_t) {
      if (ec) {
        settle();
        self->_stream->abort();
        return done(false);
      }
      const auto chosen = subprotocol(std::string_view((*request)[http::field::sec_websocket_protocol]));
      if (!websocket::is_upgrade(*request) || !chosen) {
        auto refusal = std::make_shared<http::response<http::string_body>>(http::status::bad_request, request->version());
        refusal->set(http::field::connection, "close");
        refusal->body() = "MQTT over WebSocket: an upgrade offering the mqtt subprotocol\n";
        refusal->prepare_payload();
        http::async_write(*self->_stream, *refusal, [self, refusal, settle, done](const boost::system::error_code&, size_t) {
          settle();
          self->_stream->close();
          done(false);
        });
        return;
      }
      self->_ws.binary(true);
      self->_ws.set_option(websocket::stream_base::decorator([protocol = *chosen](websocket::response_type& response) {
        if (!protocol.empty()) response.set(http::field::sec_websocket_protocol, protocol);
      }));
      self->_ws.async_accept(*request, [self, settle, done](const boost::system::error_code& accept_ec) {
        settle();
        if (accept_ec) self->_stream->abort();
        done(!accept_ec);
      });
    });
  }

  void start(std::shared_ptr<broker::Link> link) {
    _link = std::move(link);
    read();
  }

  void send(mqtt::Bytes bytes) override {
    if (_closing || _closed) return;
    if (_queued + bytes.size() > _maximum) {
      // Never waited for: the client is dropped, as an abnormal close, once
      // the broker's current call has returned.
      _logger.info("closing " + _peer + ": over " + std::to_string(_maximum) + " bytes queued");
      _closing = true;
      asio::post(_stream->get_executor(), [self = shared_from_this()] { self->terminate(); });
      return;
    }
    _queued += bytes.size();
    _queue.push_back(std::move(bytes));
    if (!_writing) write();
  }

  void close() override {
    if (_closed) return;
    _closing = true;
    if (!_writing) end();
  }

  std::string peer() const override { return _peer; }
  std::optional<contract::ClientCertificate> certificate() const override { return _certificate; }
  std::string transport() const override { return _stream->is_tls() ? "wss" : "ws"; }

  void terminate() override {
    if (_closed) return;
    _closed = true;
    if (_ending) _ending->cancel();
    _stream->abort();
    // The link and this connection hold each other; the cycle ends here.
    if (auto link = std::move(_link)) link->closed();
    _forget(this);
  }

 private:
  // A close the broker asked for, once everything queued is written: the
  // close frame, then the stream's clean close, bounded, then the socket.
  void end() {
    if (_closed || _ending) return;
    if (!_ws.is_open()) return terminate();
    _ending.emplace(_stream->get_executor(), clean_end_deadline);
    _ws.async_close(websocket::close_code::normal, [self = shared_from_this()](const boost::system::error_code&) { self->terminate(); });
    _ending->async_wait([self = shared_from_this()](const boost::system::error_code& ec) {
      if (!ec) self->terminate();
    });
  }

  void read() {
    _ws.async_read_some(asio::buffer(_buffer), [self = shared_from_this()](const boost::system::error_code& ec, size_t got) {
      if (ec) return self->terminate();
      if (self->_closing || self->_closed) return;
      if (!self->_ws.got_binary()) {
        self->_logger.info("closing " + self->_peer + ": a WebSocket text frame");
        return self->terminate();
      }
      if (self->_link) self->_link->receive(std::span<const uint8_t>(self->_buffer.data(), got));
      if (!self->_closing && !self->_closed) self->read();
    });
  }

  void write() {
    _writing = true;
    _ws.async_write(asio::buffer(_queue.front()), [self = shared_from_this()](const boost::system::error_code& ec, size_t) {
      self->_writing = false;
      if (ec) return self->terminate();
      self->_queued -= self->_queue.front().size();
      self->_queue.pop_front();
      if (self->_closed) return;
      if (!self->_queue.empty()) return self->write();
      if (self->_closing) self->end();
    });
  }

  std::shared_ptr<Stream> _stream;
  websocket::stream<Stream&> _ws;
  const size_t _maximum;
  dannan::status::ScopedLogger _logger;
  std::function<void(ClientConnection*)> _forget;
  std::string _peer;
  std::optional<contract::ClientCertificate> _certificate;
  std::shared_ptr<broker::Link> _link;
  std::array<uint8_t, 16384> _buffer{};
  std::deque<mqtt::Bytes> _queue;
  size_t _queued = 0;
  bool _writing = false;
  bool _closing = false;
  bool _closed = false;
  std::optional<asio::steady_timer> _ending;
};

}  // namespace

// The verified client certificate: its subject in RFC 2253 form, and the
// SHA-256 of its DER.
std::optional<contract::ClientCertificate> verified_certificate(SSL* ssl) {
  if (!ssl || SSL_get_verify_result(ssl) != X509_V_OK) return std::nullopt;
  X509* certificate = SSL_get1_peer_certificate(ssl);
  if (!certificate) return std::nullopt;
  std::optional<contract::ClientCertificate> out;
  unsigned char* der = nullptr;
  const int der_size = i2d_X509(certificate, &der);
  BIO* bio = BIO_new(BIO_s_mem());
  if (der_size > 0 && bio && X509_NAME_print_ex(bio, X509_get_subject_name(certificate), 0, XN_FLAG_RFC2253) >= 0) {
    char* data = nullptr;
    const long size = BIO_get_mem_data(bio, &data);
    if (size >= 0) {
      out.emplace();
      out->subject = std::string(data, static_cast<size_t>(size));
      out->thumbprint = dannan::identity::sha256(std::span<const uint8_t>(der, static_cast<size_t>(der_size))).bytes;
    }
  }
  if (bio) BIO_free(bio);
  OPENSSL_free(der);
  X509_free(certificate);
  return out;
}

std::unique_ptr<asio::ssl::context> server_context(const TlsConfig& tls, bool datagram) {
  std::unique_ptr<asio::ssl::context> context;
  if (datagram) {
    // Asio has no DTLS method: the context is OpenSSL's, wrapped.
    SSL_CTX* raw = SSL_CTX_new(DTLS_server_method());
    if (!raw) throw std::runtime_error("no DTLS context");
    SSL_CTX_set_min_proto_version(raw, DTLS1_2_VERSION);
    context = std::make_unique<asio::ssl::context>(raw);
  } else {
    context = std::make_unique<asio::ssl::context>(asio::ssl::context::tls_server);
    context->set_options(asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 | asio::ssl::context::no_sslv3 | asio::ssl::context::no_tlsv1 |
                         asio::ssl::context::no_tlsv1_1);
  }
  context->use_certificate_chain_file(tls.certificate_chain.string());
  context->use_private_key_file(tls.private_key.string(), asio::ssl::context::pem);
  if (!tls.client_ca.empty()) {
    context->load_verify_file(tls.client_ca.string());
    context->set_verify_mode(asio::ssl::verify_peer | (tls.require_client_certificate ? asio::ssl::verify_fail_if_no_peer_cert : 0));
  }
  return context;
}

struct Edge::State : std::enable_shared_from_this<Edge::State> {
  State(asio::io_context& io_context, EdgeConfig edge_config, const dannan::contract::TimeSource& time, contract::Authenticator& authenticator,
        dannan::status::ScopedLogger edge_logger)
      : io(io_context),
        strand(asio::make_strand(io_context)),
        timer(strand),
        config(std::move(edge_config)),
        logger(std::move(edge_logger)),
        broker(config.broker, time, authenticator, logger.child("broker")) {}

  std::function<void(ClientConnection*)> forgetter() {
    return [weak = weak_from_this()](ClientConnection* gone) {
      if (const auto state = weak.lock()) {
        state->connections.erase(gone);
        state->settle();
      }
    };
  }

  // While stopping: tells stop() once the last connection has gone.
  void settle() {
    if (!drained || !connections.empty()) return;
    drained->set_value();
    drained.reset();
  }

  template <typename Connection>
  void adopt(std::shared_ptr<Connection> connection) {
    connections.emplace(connection.get(), connection);
    connection->start(broker.accept(connection));
  }

  // MQTT over TCP or TLS: a client once its stream is handed up (after its
  // TLS handshake, within the listener's deadline). On the strand.
  void serve(std::shared_ptr<Stream> stream) {
    if (stopped || presented_unverified(*stream)) return stream->abort();
    adopt(std::make_shared<RawConnection>(std::move(stream), forgetter()));
  }

  // MQTT over WebSocket: a client once its upgrade is done. On the strand.
  void serve_websocket(std::shared_ptr<Stream> stream, std::chrono::milliseconds within) {
    if (stopped || presented_unverified(*stream)) return stream->abort();
    auto connection = std::make_shared<WebSocketConnection>(std::move(stream), config.maximum_outbound_bytes, logger, forgetter());
    connection->upgrade(within, [self = shared_from_this(), connection](bool upgraded) {
      if (!upgraded) return;
      if (self->stopped) return connection->terminate();
      self->adopt(connection);
    });
  }

  // A listener handing its streams to `serve` on the strand.
  std::unique_ptr<dannan::listen::StreamListener> listen(dannan::listen::ListenerConfig settings, std::function<void(std::shared_ptr<Stream>)> serve) {
    auto listener = std::make_unique<dannan::listen::StreamListener>(io, std::move(settings));
    listener->start(strand, [weak = weak_from_this(), serve = std::move(serve)](std::shared_ptr<Stream> stream) {
      if (weak.lock()) return serve(std::move(stream));
      stream->abort();
    });
    return listener;
  }

  std::vector<dannan::listen::StreamListener*> listeners() {
    std::vector<dannan::listen::StreamListener*> out;
    for (auto* each : {plain.get(), tls.get(), ws.get(), wss.get()}) {
      if (each) out.push_back(each);
    }
    return out;
  }

  void poll() {
    timer.expires_after(config.poll_every);
    timer.async_wait([self = shared_from_this()](const boost::system::error_code& ec) {
      if (ec || self->stopped) return;
      self->broker.poll();
      self->poll();
    });
  }

  asio::io_context& io;
  Strand strand;
  // Dannan's public listeners (dannan::listen): accept, TLS, its deadline and
  // the close bound are theirs; what flows over a stream is the edge's.
  std::unique_ptr<dannan::listen::StreamListener> plain;
  std::unique_ptr<dannan::listen::StreamListener> tls;
  std::unique_ptr<dannan::listen::StreamListener> ws;
  std::unique_ptr<dannan::listen::StreamListener> wss;
  asio::steady_timer timer;
  EdgeConfig config;
  dannan::status::ScopedLogger logger;
  broker::Broker broker;
  std::map<ClientConnection*, std::weak_ptr<ClientConnection>> connections;
  // Set while stop() waits for the connections to end cleanly.
  std::shared_ptr<std::promise<void>> drained;
  bool started = false;
  bool stopped = false;
  std::atomic<uint16_t> port{0};
  std::atomic<uint16_t> tls_port{0};
  std::atomic<uint16_t> ws_port{0};
  std::atomic<uint16_t> wss_port{0};
};

Edge::Edge(asio::io_context& io, EdgeConfig config, const dannan::contract::TimeSource& time, contract::Authenticator& authenticator,
           dannan::status::ScopedLogger logger)
    : _state(std::make_shared<State>(io, std::move(config), time, authenticator, std::move(logger))) {}

Edge::~Edge() {
  try {
    stop();
  } catch (const std::exception& e) {
    _state->logger.error(std::string("stopping the edge: ") + e.what());
  }
}

void Edge::start() {
  auto& state = *_state;
  const auto& config = state.config;
  const auto serve = [weak = std::weak_ptr<State>(_state)](std::shared_ptr<Stream> stream) {
    if (const auto s = weak.lock()) s->serve(std::move(stream));
  };
  state.plain = state.listen(listener_config("mqtt", config.listen_host, config.port, config.maximum_outbound_bytes, nullptr), serve);
  state.port = state.plain->port();
  if (const auto& tls = config.tls) {
    state.tls = state.listen(listener_config("mqtt-tls", tls->listen_host, tls->port, config.maximum_outbound_bytes, &*tls), serve);
    state.tls_port = state.tls->port();
    state.logger.info("MQTT over TLS listening on " + tls->listen_host + ":" + std::to_string(state.tls_port.load()));
  }
  // A WebSocket writes through its own queue, so the stream's bound is unused.
  if (const auto& ws = config.websocket) {
    state.ws = state.listen(listener_config("mqtt-ws", ws->listen_host, ws->port, 0, nullptr),
                            [weak = std::weak_ptr<State>(_state), within = ws->handshake_timeout](std::shared_ptr<Stream> stream) {
                              if (const auto s = weak.lock()) s->serve_websocket(std::move(stream), within);
                            });
    state.ws_port = state.ws->port();
    state.logger.info("MQTT over WebSocket listening on " + ws->listen_host + ":" + std::to_string(state.ws_port.load()));
  }
  if (const auto& wss = config.secure_websocket) {
    // The TLS handshake has the listener's deadline, the upgrade its own.
    state.wss = state.listen(listener_config("mqtt-wss", wss->listen_host, wss->port, 0, &*wss),
                             [weak = std::weak_ptr<State>(_state), within = wss->handshake_timeout](std::shared_ptr<Stream> stream) {
                               if (const auto s = weak.lock()) s->serve_websocket(std::move(stream), within);
                             });
    state.wss_port = state.wss->port();
    state.logger.info("MQTT over WebSocket and TLS listening on " + wss->listen_host + ":" + std::to_string(state.wss_port.load()));
  }
  state.started = true;
  state.logger.info("MQTT listening on " + config.listen_host + ":" + std::to_string(state.port.load()));
  asio::post(state.strand, [self = _state] { self->poll(); });
}

void Edge::stop() {
  if (!_state->started) return;
  const bool first = call<bool>([state = _state](broker::Broker&) {
    if (state->stopped) return false;
    state->stopped = true;
    state->timer.cancel();
    return true;
  });
  if (!first) return;
  // No new client from here. Waits on each listener's own strand, so it is
  // made from this thread, never the edge's strand.
  for (auto* listener : _state->listeners()) listener->stop_accepting();
  // Then every client is told and its connection ended cleanly, each within
  // its own bound; whatever is left after it is closed at once.
  auto drained = std::make_shared<std::promise<void>>();
  auto all_gone = drained->get_future();
  wait([state = _state, drained](broker::Broker& broker) {
    state->drained = drained;
    broker.shutting_down();
    state->settle();
  });
  all_gone.wait_for(clean_end_deadline + 1s);
  wait([state = _state](broker::Broker&) {
    state->drained.reset();
    std::vector<std::shared_ptr<ClientConnection>> open;
    for (const auto& [raw, weak] : state->connections) {
      if (auto connection = weak.lock()) open.push_back(std::move(connection));
    }
    for (const auto& connection : open) connection->terminate();
  });
  // Streams no connection held (an upgrade under way) go too.
  for (auto* listener : _state->listeners()) listener->close_all();
}

uint16_t Edge::port() const noexcept { return _state->port.load(); }

uint16_t Edge::tls_port() const noexcept { return _state->tls_port.load(); }

uint16_t Edge::websocket_port() const noexcept { return _state->ws_port.load(); }

uint16_t Edge::secure_websocket_port() const noexcept { return _state->wss_port.load(); }

void Edge::post(std::function<void(broker::Broker&)> fn) {
  asio::post(_state->strand, [state = _state, fn = std::move(fn)] { fn(state->broker); });
}

void Edge::wait(std::function<void(broker::Broker&)> fn) {
  auto done = std::make_shared<std::promise<void>>();
  auto finished = done->get_future();
  asio::post(_state->strand, [state = _state, fn = std::move(fn), done] {
    try {
      fn(state->broker);
      done->set_value();
    } catch (...) {
      done->set_exception(std::current_exception());
    }
  });
  if (finished.wait_for(strand_deadline) != std::future_status::ready) throw std::runtime_error("the edge's strand did not answer within its deadline");
  finished.get();
}

broker::Stats Edge::stats() {
  return call<broker::Stats>([](broker::Broker& broker) { return broker.stats(); });
}

Edge::Strand& Edge::strand() noexcept { return _state->strand; }

broker::Broker& Edge::broker() noexcept { return _state->broker; }

}  // namespace lir::edge
