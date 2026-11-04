//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "lir/edge/edge.h"

#include <array>
#include <atomic>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>
#include <deque>
#include <future>
#include <map>
#include <stdexcept>

namespace lir::edge {

namespace {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;

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
    const auto remote = _stream.lowest_layer().remote_endpoint(ec);
    _peer = ec ? "unknown" : remote.address().to_string() + ":" + std::to_string(remote.port());
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

  void terminate() override {
    if (_closed) return;
    _closed = true;
    boost::system::error_code ignored;
    _stream.lowest_layer().shutdown(tcp::socket::shutdown_both, ignored);
    _stream.lowest_layer().close(ignored);
    // The link and this connection hold each other; the cycle ends here.
    if (auto link = std::move(_link)) link->closed();
    _forget(this);
  }

 private:
  void read() {
    _stream.async_read_some(asio::buffer(_buffer), [self = this->shared_from_this()](const boost::system::error_code& ec, size_t got) {
      if (ec) return self->terminate();
      if (self->_closing || self->_closed) return;
      if (self->_link) self->_link->receive(std::span<const uint8_t>(self->_buffer.data(), got));
      if (!self->_closing && !self->_closed) self->read();
    });
  }

  void write() {
    _writing = true;
    asio::async_write(_stream, asio::buffer(_queue.front()), [self = this->shared_from_this()](const boost::system::error_code& ec, size_t) {
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

}  // namespace

struct Edge::State : std::enable_shared_from_this<Edge::State> {
  State(asio::io_context& io, EdgeConfig edge_config, const dannan::contract::TimeSource& time, contract::Authenticator& authenticator,
        dannan::status::ScopedLogger edge_logger)
      : strand(asio::make_strand(io)),
        acceptor(strand),
        tls_acceptor(strand),
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
      auto deadline = std::make_shared<asio::steady_timer>(self->strand);
      deadline->expires_after(self->config.tls->handshake_timeout);
      deadline->async_wait([stream](const boost::system::error_code& timer_ec) {
        if (timer_ec) return;
        boost::system::error_code closing;
        stream->lowest_layer().close(closing);
      });
      stream->async_handshake(asio::ssl::stream_base::server, [self, stream, deadline](const boost::system::error_code& handshake_ec) {
        deadline->cancel();
        if (handshake_ec || self->stopped) {
          if (handshake_ec) self->logger.debug("TLS handshake failed: " + handshake_ec.message());
          return;
        }
        self->adopt(std::make_shared<TlsConnection>(std::move(*stream), self->config.maximum_outbound_bytes, self->logger, self->forgetter()));
      });
      self->accept_tls();
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
  std::optional<asio::ssl::context> tls;
  asio::steady_timer timer;
  EdgeConfig config;
  dannan::status::ScopedLogger logger;
  broker::Broker broker;
  std::map<ClientConnection*, std::weak_ptr<ClientConnection>> connections;
  bool started = false;
  bool stopped = false;
  std::atomic<uint16_t> port{0};
  std::atomic<uint16_t> tls_port{0};
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
  const tcp::endpoint endpoint(asio::ip::make_address(state.config.listen_host), state.config.port);
  state.acceptor.open(endpoint.protocol());
  state.acceptor.set_option(tcp::acceptor::reuse_address(true));
  state.acceptor.bind(endpoint);
  state.acceptor.listen();
  state.port = state.acceptor.local_endpoint().port();
  if (const auto& tls = state.config.tls) {
    state.tls.emplace(asio::ssl::context::tls_server);
    state.tls->set_options(asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 | asio::ssl::context::no_sslv3 |
                           asio::ssl::context::no_tlsv1 | asio::ssl::context::no_tlsv1_1);
    state.tls->use_certificate_chain_file(tls->certificate_chain.string());
    state.tls->use_private_key_file(tls->private_key.string(), asio::ssl::context::pem);
    const tcp::endpoint tls_endpoint(asio::ip::make_address(tls->listen_host), tls->port);
    state.tls_acceptor.open(tls_endpoint.protocol());
    state.tls_acceptor.set_option(tcp::acceptor::reuse_address(true));
    state.tls_acceptor.bind(tls_endpoint);
    state.tls_acceptor.listen();
    state.tls_port = state.tls_acceptor.local_endpoint().port();
    state.logger.info("MQTT over TLS listening on " + tls->listen_host + ":" + std::to_string(state.tls_port.load()));
  }
  state.started = true;
  state.logger.info("MQTT listening on " + state.config.listen_host + ":" + std::to_string(state.port.load()));
  asio::post(state.strand, [self = _state] {
    self->accept();
    if (self->tls) self->accept_tls();
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
