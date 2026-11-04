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

namespace lir::edge {

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;

// Writing one buffer whole: a byte stream, or one binary WebSocket message.
template <typename Stream, typename Handler>
void write_all(Stream& stream, asio::const_buffer buffer, Handler&& handler) {
  asio::async_write(stream, buffer, std::forward<Handler>(handler));
}
template <typename Next, typename Handler>
void write_all(websocket::stream<Next>& stream, asio::const_buffer buffer, Handler&& handler) {
  stream.async_write(buffer, std::forward<Handler>(handler));
}

// Whether what was just read may not carry MQTT: a text frame (section 6).
template <typename Stream>
bool wrong_frame(const Stream&) {
  return false;
}
template <typename Next>
bool wrong_frame(const websocket::stream<Next>& stream) {
  return !stream.got_binary();
}

// The TLS session under a stream, if any.
template <typename Stream>
SSL* ssl_of(Stream&) {
  return nullptr;
}
inline SSL* ssl_of(asio::ssl::stream<tcp::socket>& stream) { return stream.native_handle(); }
template <typename Next>
SSL* ssl_of(websocket::stream<Next>& stream) {
  return ssl_of(stream.next_layer());
}

// How a client on this stream reached the node.
inline std::string transport_of(const tcp::socket&) { return "tcp"; }
inline std::string transport_of(const asio::ssl::stream<tcp::socket>&) { return "tls"; }
inline std::string transport_of(const websocket::stream<tcp::socket>&) { return "ws"; }
inline std::string transport_of(const websocket::stream<asio::ssl::stream<tcp::socket>>&) { return "wss"; }

// The verified client certificate's subject, RFC 2253 form.
std::optional<std::string> subject_of(SSL* ssl) {
  if (!ssl || SSL_get_verify_result(ssl) != X509_V_OK) return std::nullopt;
  X509* certificate = SSL_get1_peer_certificate(ssl);
  if (!certificate) return std::nullopt;
  std::optional<std::string> out;
  if (BIO* bio = BIO_new(BIO_s_mem())) {
    if (X509_NAME_print_ex(bio, X509_get_subject_name(certificate), 0, XN_FLAG_RFC2253) >= 0) {
      char* data = nullptr;
      const long size = BIO_get_mem_data(bio, &data);
      if (size >= 0) out = std::string(data, static_cast<size_t>(size));
    }
    BIO_free(bio);
  }
  X509_free(certificate);
  return out;
}

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

// One client over `Stream`: a TCP socket, or TLS over one. Every member is
// touched only on the edge's strand.
template <typename Stream>
class StreamConnection final : public ClientConnection, public std::enable_shared_from_this<StreamConnection<Stream>> {
 public:
  StreamConnection(Stream stream, size_t maximum, dannan::status::ScopedLogger logger, std::function<void(ClientConnection*)> forget)
      : _stream(std::move(stream)), _maximum(maximum), _logger(std::move(logger)), _forget(std::move(forget)) {
    boost::system::error_code ec;
    const auto remote = beast::get_lowest_layer(_stream).remote_endpoint(ec);
    _peer = ec ? "unknown" : remote.address().to_string() + ":" + std::to_string(remote.port());
    _subject = subject_of(ssl_of(_stream));
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
      asio::post(_stream.get_executor(), [self = this->shared_from_this()] { self->terminate(); });
      return;
    }
    _queued += bytes.size();
    _queue.push_back(std::move(bytes));
    if (!_writing) write();
  }

  void close() override {
    if (_closed) return;
    _closing = true;
    if (!_writing) terminate();
  }

  std::string peer() const override { return _peer; }
  std::optional<std::string> certificate_subject() const override { return _subject; }
  std::string transport() const override { return transport_of(_stream); }

  void terminate() override {
    if (_closed) return;
    _closed = true;
    boost::system::error_code ignored;
    beast::get_lowest_layer(_stream).shutdown(tcp::socket::shutdown_both, ignored);
    beast::get_lowest_layer(_stream).close(ignored);
    // The link and this connection hold each other; the cycle ends here.
    if (auto link = std::move(_link)) link->closed();
    _forget(this);
  }

 private:
  void read() {
    _stream.async_read_some(asio::buffer(_buffer), [self = this->shared_from_this()](const boost::system::error_code& ec, size_t got) {
      if (ec) return self->terminate();
      if (self->_closing || self->_closed) return;
      if (wrong_frame(self->_stream)) {
        self->_logger.info("closing " + self->_peer + ": a WebSocket text frame");
        return self->terminate();
      }
      if (self->_link) self->_link->receive(std::span<const uint8_t>(self->_buffer.data(), got));
      if (!self->_closing && !self->_closed) self->read();
    });
  }

  void write() {
    _writing = true;
    write_all(_stream, asio::buffer(_queue.front()), [self = this->shared_from_this()](const boost::system::error_code& ec, size_t) {
      self->_writing = false;
      if (ec) return self->terminate();
      self->_queued -= self->_queue.front().size();
      self->_queue.pop_front();
      if (self->_closed) return;
      if (!self->_queue.empty()) return self->write();
      if (self->_closing) self->terminate();
    });
  }

  Stream _stream;
  const size_t _maximum;
  dannan::status::ScopedLogger _logger;
  std::function<void(ClientConnection*)> _forget;
  std::string _peer;
  std::optional<std::string> _subject;
  std::shared_ptr<broker::Link> _link;
  std::array<uint8_t, 16384> _buffer{};
  std::deque<mqtt::Bytes> _queue;
  size_t _queued = 0;
  bool _writing = false;
  bool _closing = false;
  bool _closed = false;
};

using TcpConnection = StreamConnection<tcp::socket>;
using TlsStream = asio::ssl::stream<tcp::socket>;
using TlsConnection = StreamConnection<TlsStream>;
using WebSocket = websocket::stream<tcp::socket>;
using WebSocketConnection = StreamConnection<WebSocket>;
using SecureWebSocket = websocket::stream<TlsStream>;
using SecureWebSocketConnection = StreamConnection<SecureWebSocket>;

void listen_on(tcp::acceptor& acceptor, const std::string& host, uint16_t port) {
  const tcp::endpoint endpoint(asio::ip::make_address(host), port);
  acceptor.open(endpoint.protocol());
  acceptor.set_option(tcp::acceptor::reuse_address(true));
  acceptor.bind(endpoint);
  acceptor.listen();
}

}  // namespace

std::unique_ptr<asio::ssl::context> server_context(const TlsConfig& tls) {
  auto context = std::make_unique<asio::ssl::context>(asio::ssl::context::tls_server);
  context->set_options(asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 | asio::ssl::context::no_sslv3 | asio::ssl::context::no_tlsv1 |
                       asio::ssl::context::no_tlsv1_1);
  context->use_certificate_chain_file(tls.certificate_chain.string());
  context->use_private_key_file(tls.private_key.string(), asio::ssl::context::pem);
  if (!tls.client_ca.empty()) {
    context->load_verify_file(tls.client_ca.string());
    context->set_verify_mode(asio::ssl::verify_peer | (tls.require_client_certificate ? asio::ssl::verify_fail_if_no_peer_cert : 0));
  }
  return context;
}

struct Edge::State : std::enable_shared_from_this<Edge::State> {
  State(asio::io_context& io, EdgeConfig edge_config, const dannan::contract::TimeSource& time, contract::Authenticator& authenticator,
        dannan::status::ScopedLogger edge_logger)
      : strand(asio::make_strand(io)),
        acceptor(strand),
        tls_acceptor(strand),
        ws_acceptor(strand),
        wss_acceptor(strand),
        timer(strand),
        config(std::move(edge_config)),
        logger(std::move(edge_logger)),
        broker(config.broker, time, authenticator, logger.child("broker")) {}

  void accept() {
    acceptor.async_accept(strand, [self = shared_from_this()](const boost::system::error_code& ec, tcp::socket socket) {
      if (self->stopped) return;
      if (ec) {
        self->logger.warn("accept failed: " + ec.message());
      } else {
        boost::system::error_code ignored;
        socket.set_option(tcp::no_delay(true), ignored);
        self->adopt(std::make_shared<TcpConnection>(std::move(socket), self->config.maximum_outbound_bytes, self->logger, self->forgetter()));
      }
      self->accept();
    });
  }

  std::function<void(ClientConnection*)> forgetter() {
    return [weak = weak_from_this()](ClientConnection* gone) {
      if (const auto state = weak.lock()) state->connections.erase(gone);
    };
  }

  template <typename Connection>
  void adopt(std::shared_ptr<Connection> connection) {
    connections.emplace(connection.get(), connection);
    connection->start(broker.accept(connection));
  }

  // A TLS client becomes a connection once its handshake is done, within
  // the handshake timeout.
  void accept_tls() {
    tls_acceptor.async_accept(strand, [self = shared_from_this()](const boost::system::error_code& ec, tcp::socket socket) {
      if (self->stopped) return;
      if (ec) {
        self->logger.warn("TLS accept failed: " + ec.message());
        return self->accept_tls();
      }
      boost::system::error_code ignored;
      socket.set_option(tcp::no_delay(true), ignored);
      auto stream = std::make_shared<TlsStream>(std::move(socket), *self->tls);
      auto deadline = self->deadline_for(stream, self->config.tls->handshake_timeout);
      stream->async_handshake(asio::ssl::stream_base::server, [self, stream, deadline](const boost::system::error_code& handshake_ec) {
        deadline->settle();
        if (handshake_ec || self->stopped) {
          if (handshake_ec) self->logger.debug("TLS handshake failed: " + handshake_ec.message());
          return;
        }
        self->adopt(std::make_shared<TlsConnection>(std::move(*stream), self->config.maximum_outbound_bytes, self->logger, self->forgetter()));
      });
      self->accept_tls();
    });
  }

  // A socket still in its handshake is closed at the deadline, unless the
  // handshake settled first. The flag, not the cancel, decides: a timer
  // already due cannot be cancelled, and by then the stream may have moved
  // into its connection.
  struct Handshake {
    explicit Handshake(Strand& strand) : timer(strand) {}
    void settle() {
      settled = true;
      timer.cancel();
    }
    asio::steady_timer timer;
    bool settled = false;
  };

  template <typename Stream>
  std::shared_ptr<Handshake> deadline_for(std::shared_ptr<Stream> stream, std::chrono::milliseconds after) {
    auto handshake = std::make_shared<Handshake>(strand);
    handshake->timer.expires_after(after);
    handshake->timer.async_wait([stream, handshake](const boost::system::error_code& ec) {
      if (ec || handshake->settled) return;
      handshake->settled = true;
      boost::system::error_code closing;
      beast::get_lowest_layer(*stream).close(closing);
    });
    return handshake;
  }

  // The upgrade: read the request, choose the subprotocol, answer; a
  // request that is not a WebSocket upgrade, or offers only subprotocols
  // that are not MQTT, gets 400 and is closed.
  template <typename Next>
  void upgrade(std::shared_ptr<websocket::stream<Next>> ws, std::shared_ptr<Handshake> deadline) {
    auto buffer = std::make_shared<beast::flat_buffer>();
    auto request = std::make_shared<http::request<http::empty_body>>();
    http::async_read(ws->next_layer(), *buffer, *request,
                     [self = shared_from_this(), ws, deadline, buffer, request](const boost::system::error_code& ec, size_t) {
                       if (ec || self->stopped) {
                         deadline->settle();
                         return;
                       }
                       const auto chosen = subprotocol(std::string_view((*request)[http::field::sec_websocket_protocol]));
                       if (!websocket::is_upgrade(*request) || !chosen) {
                         auto refusal = std::make_shared<http::response<http::string_body>>(http::status::bad_request, request->version());
                         refusal->set(http::field::connection, "close");
                         refusal->body() = "MQTT over WebSocket: an upgrade offering the mqtt subprotocol\n";
                         refusal->prepare_payload();
                         http::async_write(ws->next_layer(), *refusal, [ws, deadline, refusal](const boost::system::error_code&, size_t) {
                           deadline->settle();
                           boost::system::error_code ignored;
                           beast::get_lowest_layer(*ws).close(ignored);
                         });
                         return;
                       }
                       ws->binary(true);
                       ws->set_option(websocket::stream_base::decorator([protocol = *chosen](websocket::response_type& response) {
                         if (!protocol.empty()) response.set(http::field::sec_websocket_protocol, protocol);
                       }));
                       ws->async_accept(*request, [self, ws, deadline](const boost::system::error_code& accept_ec) {
                         deadline->settle();
                         if (accept_ec || self->stopped) return;
                         self->adopt(std::make_shared<StreamConnection<websocket::stream<Next>>>(std::move(*ws), self->config.maximum_outbound_bytes,
                                                                                                 self->logger, self->forgetter()));
                       });
                     });
  }

  void accept_websocket() {
    ws_acceptor.async_accept(strand, [self = shared_from_this()](const boost::system::error_code& ec, tcp::socket socket) {
      if (self->stopped) return;
      if (ec) {
        self->logger.warn("WebSocket accept failed: " + ec.message());
        return self->accept_websocket();
      }
      boost::system::error_code ignored;
      socket.set_option(tcp::no_delay(true), ignored);
      auto ws = std::make_shared<WebSocket>(std::move(socket));
      self->upgrade(ws, self->deadline_for(ws, self->config.websocket->handshake_timeout));
      self->accept_websocket();
    });
  }

  void accept_secure_websocket() {
    wss_acceptor.async_accept(strand, [self = shared_from_this()](const boost::system::error_code& ec, tcp::socket socket) {
      if (self->stopped) return;
      if (ec) {
        self->logger.warn("WebSocket TLS accept failed: " + ec.message());
        return self->accept_secure_websocket();
      }
      boost::system::error_code ignored;
      socket.set_option(tcp::no_delay(true), ignored);
      auto ws = std::make_shared<SecureWebSocket>(std::move(socket), *self->wss);
      // One deadline for the TLS handshake and the upgrade together.
      auto deadline = self->deadline_for(ws, self->config.secure_websocket->handshake_timeout);
      ws->next_layer().async_handshake(asio::ssl::stream_base::server, [self, ws, deadline](const boost::system::error_code& handshake_ec) {
        if (handshake_ec || self->stopped) {
          if (handshake_ec) self->logger.debug("WebSocket TLS handshake failed: " + handshake_ec.message());
          deadline->settle();
          return;
        }
        self->upgrade(ws, deadline);
      });
      self->accept_secure_websocket();
    });
  }

  void poll() {
    timer.expires_after(config.poll_every);
    timer.async_wait([self = shared_from_this()](const boost::system::error_code& ec) {
      if (ec || self->stopped) return;
      self->broker.poll();
      self->poll();
    });
  }

  Strand strand;
  tcp::acceptor acceptor;
  tcp::acceptor tls_acceptor;
  tcp::acceptor ws_acceptor;
  tcp::acceptor wss_acceptor;
  std::unique_ptr<asio::ssl::context> tls;
  std::unique_ptr<asio::ssl::context> wss;
  asio::steady_timer timer;
  EdgeConfig config;
  dannan::status::ScopedLogger logger;
  broker::Broker broker;
  std::map<ClientConnection*, std::weak_ptr<ClientConnection>> connections;
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
  listen_on(state.acceptor, state.config.listen_host, state.config.port);
  state.port = state.acceptor.local_endpoint().port();
  if (const auto& tls = state.config.tls) {
    state.tls = server_context(*tls);
    listen_on(state.tls_acceptor, tls->listen_host, tls->port);
    state.tls_port = state.tls_acceptor.local_endpoint().port();
    state.logger.info("MQTT over TLS listening on " + tls->listen_host + ":" + std::to_string(state.tls_port.load()));
  }
  if (const auto& ws = state.config.websocket) {
    listen_on(state.ws_acceptor, ws->listen_host, ws->port);
    state.ws_port = state.ws_acceptor.local_endpoint().port();
    state.logger.info("MQTT over WebSocket listening on " + ws->listen_host + ":" + std::to_string(state.ws_port.load()));
  }
  if (const auto& wss = state.config.secure_websocket) {
    state.wss = server_context(*wss);
    listen_on(state.wss_acceptor, wss->listen_host, wss->port);
    state.wss_port = state.wss_acceptor.local_endpoint().port();
    state.logger.info("MQTT over WebSocket and TLS listening on " + wss->listen_host + ":" + std::to_string(state.wss_port.load()));
  }
  state.started = true;
  state.logger.info("MQTT listening on " + state.config.listen_host + ":" + std::to_string(state.port.load()));
  asio::post(state.strand, [self = _state] {
    self->accept();
    if (self->tls) self->accept_tls();
    if (self->config.websocket) self->accept_websocket();
    if (self->wss) self->accept_secure_websocket();
    self->poll();
  });
}

void Edge::stop() {
  if (!_state->started) return;
  wait([state = _state](broker::Broker&) {
    if (state->stopped) return;
    state->stopped = true;
    boost::system::error_code ignored;
    state->acceptor.close(ignored);
    state->tls_acceptor.close(ignored);
    state->ws_acceptor.close(ignored);
    state->wss_acceptor.close(ignored);
    state->timer.cancel();
    std::vector<std::shared_ptr<ClientConnection>> open;
    for (const auto& [raw, weak] : state->connections) {
      if (auto connection = weak.lock()) open.push_back(std::move(connection));
    }
    for (const auto& connection : open) connection->terminate();
  });
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
