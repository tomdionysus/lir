//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/strand.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "dannan/contract/time_source.h"
#include "dannan/status/log.h"
#include "lir/broker/broker.h"
#include "lir/contract/authenticator.h"

// The edge: where MQTT clients connect. Listeners on the node's io context
// (TCP, and TLS, WebSocket and WebSocket over TLS as configured), one
// connection per socket, and the broker they share, all on one
// strand, so the broker's confinement (broker.h) holds by construction. A
// slow client is closed once what is queued for it passes a bound, never
// waited for.
namespace lir::edge {

// MQTT over TLS on its own port, beside the plain one: the same broker.
struct TlsConfig {
  std::string listen_host = "0.0.0.0";
  // Zero: any; tls_port() says which.
  uint16_t port = 8883;
  // PEM files: the server's certificate chain, leaf first, and its key.
  std::filesystem::path certificate_chain;
  std::filesystem::path private_key;
  // A client that has not finished its handshake within this is closed.
  std::chrono::milliseconds handshake_timeout{10000};
  // Client certificates (mutual TLS): the PEM file of the CAs a client's
  // certificate must chain to. Empty: none is asked for.
  std::filesystem::path client_ca = {};
  // With client_ca: whether a client without a certificate is refused at
  // the handshake, or let in with none (the authenticator then decides).
  bool require_client_certificate = true;
};

// A server context for `tls`: TLS 1.2 and up, its certificate and key, and
// client certificates as it asks. Throws boost::system::system_error for a
// file that cannot be loaded. The HTTP server's too.
std::unique_ptr<boost::asio::ssl::context> server_context(const TlsConfig& tls);

// MQTT over WebSocket (MQTT 5.0 and 3.1.1, section 6): one MQTT byte
// stream carried in binary frames, the "mqtt" subprotocol ("mqttv3.1" for a
// 3.1 client), on any path; a text frame closes the connection.
struct WebSocketConfig {
  std::string listen_host = "0.0.0.0";
  // Zero: any; websocket_port() says which.
  uint16_t port = 8080;
  // A client that has not finished its upgrade within this is closed.
  std::chrono::milliseconds handshake_timeout{10000};
};

struct EdgeConfig {
  std::string listen_host = "0.0.0.0";
  // Zero: any; port() says which.
  uint16_t port = 1883;
  // Bytes queued for one client before it is closed as too slow.
  size_t maximum_outbound_bytes = 16u << 20;
  // How often the broker's poll() runs: keep alive, wills, expiry.
  std::chrono::milliseconds poll_every{1000};
  broker::Config broker;
  // Nothing: no TLS listener.
  std::optional<TlsConfig> tls;
  // Nothing: no WebSocket listener.
  std::optional<WebSocketConfig> websocket;
  // WebSocket over TLS (wss), with its own certificate and key. Nothing:
  // none.
  std::optional<TlsConfig> secure_websocket;
};

class Edge {
 public:
  using Strand = boost::asio::strand<boost::asio::io_context::executor_type>;

  // The time source and authenticator are the caller's and outlive the
  // edge.
  Edge(boost::asio::io_context& io, EdgeConfig config, const dannan::contract::TimeSource& time, contract::Authenticator& authenticator,
       dannan::status::ScopedLogger logger);
  // Stops if running.
  ~Edge();
  Edge(const Edge&) = delete;
  Edge& operator=(const Edge&) = delete;

  // Binds and listens, on each configured port. Throws
  // boost::system::system_error when an address cannot be bound or the
  // certificate or key cannot be loaded. Waits on the bind.
  void start();
  // Closes the listener and every client, then waits for the strand to
  // have done so. Never from one of the io context's threads.
  void stop();
  uint16_t port() const noexcept;
  // Zero without a TLS listener.
  uint16_t tls_port() const noexcept;
  // Zero without the listener.
  uint16_t websocket_port() const noexcept;
  uint16_t secure_websocket_port() const noexcept;

  // Runs `fn` with the broker on its strand. Waits on nothing.
  void post(std::function<void(broker::Broker&)> fn);
  // Runs `fn` there and waits for its result. Never from one of the io
  // context's threads.
  template <typename T>
  T call(std::function<T(broker::Broker&)> fn);
  broker::Stats stats();

  Strand& strand() noexcept;
  // The broker itself, for code already running on strand(); anywhere else
  // it is a data race.
  broker::Broker& broker() noexcept;

 private:
  struct State;
  void wait(std::function<void(broker::Broker&)> fn);
  std::shared_ptr<State> _state;
};

template <typename T>
T Edge::call(std::function<T(broker::Broker&)> fn) {
  T out{};
  wait([&out, &fn](broker::Broker& broker) { out = fn(broker); });
  return out;
}

}  // namespace lir::edge
