//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "lir/client/transport.h"

#include <openssl/ssl.h>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>

namespace lir::client {

namespace asio = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = asio::ip::tcp;

namespace {

constexpr std::chrono::seconds kCloseBound{2};

class Plain final : public Transport, public std::enable_shared_from_this<Plain> {
 public:
  explicit Plain(tcp::socket socket) : _socket(std::move(socket)) {}
  void read_some(std::span<uint8_t> buffer, std::function<void(boost::system::error_code, size_t)> done) override {
    _socket.async_read_some(asio::buffer(buffer.data(), buffer.size()),
                            [self = shared_from_this(), done = std::move(done)](auto error, size_t n) { done(error, n); });
  }
  void write(dannan::net::Bytes bytes, std::function<void(boost::system::error_code)> done) override {
    auto held = std::make_shared<dannan::net::Bytes>(std::move(bytes));
    asio::async_write(_socket, asio::buffer(*held), [self = shared_from_this(), held, done = std::move(done)](auto error, size_t) { done(error); });
  }
  void close() override {
    boost::system::error_code ignored;
    _socket.shutdown(tcp::socket::shutdown_both, ignored);
    _socket.close(ignored);
  }

 private:
  tcp::socket _socket;
};

class Secure final : public Transport, public std::enable_shared_from_this<Secure> {
 public:
  Secure(std::shared_ptr<ssl::context> context, ssl::stream<tcp::socket> stream)
      : _context(std::move(context)), _stream(std::move(stream)), _timer(_stream.get_executor()) {}
  void read_some(std::span<uint8_t> buffer, std::function<void(boost::system::error_code, size_t)> done) override {
    _stream.async_read_some(asio::buffer(buffer.data(), buffer.size()),
                            [self = shared_from_this(), done = std::move(done)](auto error, size_t n) { done(error, n); });
  }
  void write(dannan::net::Bytes bytes, std::function<void(boost::system::error_code)> done) override {
    auto held = std::make_shared<dannan::net::Bytes>(std::move(bytes));
    asio::async_write(_stream, asio::buffer(*held), [self = shared_from_this(), held, done = std::move(done)](auto error, size_t) { done(error); });
  }
  void close() override {
    if (_closing) return;
    _closing = true;
    // close_notify, within a bound: a server that never answers it is not waited for.
    _timer.expires_after(kCloseBound);
    _timer.async_wait([self = shared_from_this()](auto error) {
      if (!error) self->drop();
    });
    _stream.async_shutdown([self = shared_from_this()](auto) {
      self->_timer.cancel();
      self->drop();
    });
  }

 private:
  void drop() {
    boost::system::error_code ignored;
    _stream.lowest_layer().close(ignored);
  }

  std::shared_ptr<ssl::context> _context;
  ssl::stream<tcp::socket> _stream;
  asio::steady_timer _timer;
  bool _closing = false;
};

std::shared_ptr<ssl::context> client_context(const Tls& tls) {
  auto context = std::make_shared<ssl::context>(ssl::context::tls_client);
  context->set_options(ssl::context::default_workarounds | ssl::context::no_sslv2 | ssl::context::no_sslv3 | ssl::context::no_tlsv1 | ssl::context::no_tlsv1_1);
  if (tls.trust.empty()) {
    context->set_default_verify_paths();
  } else {
    context->load_verify_file(tls.trust.string());
  }
  context->set_verify_mode(ssl::verify_peer);
  if (!tls.certificate_chain.empty() && !tls.private_key.empty()) {
    context->use_certificate_chain_file(tls.certificate_chain.string());
    context->use_private_key_file(tls.private_key.string(), ssl::context::pem);
  }
  return context;
}

bool is_address(const std::string& name) {
  boost::system::error_code error;
  (void)asio::ip::make_address(name, error);
  return !error;
}

// One connection attempt: each step checks `finished`, so the deadline and the
// steps tell `done` once between them.
struct Attempt : std::enable_shared_from_this<Attempt> {
  Attempt(asio::any_io_executor on, std::string to, std::optional<Tls> with, std::function<void(std::shared_ptr<Transport>, std::string)> then)
      : executor(on), resolver(on), socket(on), timer(on), host(std::move(to)), tls(std::move(with)), done(std::move(then)) {}

  void finish(std::shared_ptr<Transport> transport, std::string why) {
    if (finished) return;
    finished = true;
    timer.cancel();
    if (!transport) {
      resolver.cancel();
      boost::system::error_code ignored;
      socket.close(ignored);
      if (secure) secure->lowest_layer().close(ignored);
    }
    asio::post(executor,
               [done = std::move(done), transport = std::move(transport), why = std::move(why)]() mutable { done(std::move(transport), std::move(why)); });
  }

  void handshake() {
    std::shared_ptr<ssl::context> context;
    try {
      context = client_context(*tls);
    } catch (const std::exception& e) {
      return finish(nullptr, "TLS for " + host + " cannot be set up: " + e.what());
    }
    const std::string name = tls->server_name.empty() ? host : tls->server_name;
    secure.emplace(std::move(socket), *context);
    // RFC 6066, 3: an IP address is never sent as SNI.
    if (!is_address(name)) SSL_set_tlsext_host_name(secure->native_handle(), name.c_str());
    secure->set_verify_callback(ssl::host_name_verification(name));
    secure->async_handshake(ssl::stream_base::client, [self = shared_from_this(), context](auto error) {
      if (self->finished) return;
      if (error) return self->finish(nullptr, "the TLS handshake with " + self->host + " failed: " + error.message());
      self->finish(std::make_shared<Secure>(context, std::move(*self->secure)), {});
    });
  }

  asio::any_io_executor executor;
  tcp::resolver resolver;
  tcp::socket socket;
  std::optional<ssl::stream<tcp::socket>> secure;
  asio::steady_timer timer;
  std::string host;
  std::optional<Tls> tls;
  std::function<void(std::shared_ptr<Transport>, std::string)> done;
  bool finished = false;
};

}  // namespace

void connect(asio::any_io_executor executor, const std::string& host, uint16_t port, const std::optional<Tls>& tls, std::chrono::milliseconds deadline,
             std::function<void(std::shared_ptr<Transport>, std::string)> done) {
  auto attempt = std::make_shared<Attempt>(executor, host, tls, std::move(done));
  asio::post(executor, [attempt, port, deadline] {
    attempt->timer.expires_after(deadline);
    attempt->timer.async_wait([attempt](auto error) {
      if (!error) attempt->finish(nullptr, "connecting to " + attempt->host + " timed out");
    });
    attempt->resolver.async_resolve(attempt->host, std::to_string(port), [attempt](auto error, tcp::resolver::results_type results) {
      if (attempt->finished) return;
      if (error) return attempt->finish(nullptr, "cannot resolve " + attempt->host + ": " + error.message());
      asio::async_connect(attempt->socket, results, [attempt](auto connect_error, const tcp::endpoint&) {
        if (attempt->finished) return;
        if (connect_error) return attempt->finish(nullptr, "cannot connect to " + attempt->host + ": " + connect_error.message());
        boost::system::error_code ignored;
        attempt->socket.set_option(tcp::no_delay(true), ignored);
        if (attempt->tls) return attempt->handshake();
        attempt->finish(std::make_shared<Plain>(std::move(attempt->socket)), {});
      });
    });
  });
}

}  // namespace lir::client
