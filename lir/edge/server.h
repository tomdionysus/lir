//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/io_context.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "dannan/context.h"
#include "dannan/contract/time_source.h"
#include "dannan/listen/certificates.h"
#include "dannan/listen/listener.h"
#include "lir/broker/delivery.h"
#include "lir/broker/link.h"
#include "lir/contract/authenticator.h"
#include "lir/edge/edge.h"
#include "lir/system/views.h"

// Lir on one Dannan node: the node's Delivery, its edge, the listeners
// clients reach it on, and the adverts that say so in Dannan's roster ("lir",
// and "lir.endpoints", lir/contract/endpoints.h), which is how a client and
// every other node find it. Every node runs the same; none is special.
namespace lir::edge {

// One listener: where it is served, and its TLS pair when it speaks TLS.
// Port zero: any; Server::port_of says which.
struct Endpoint {
  std::string address = "0.0.0.0";
  uint16_t port = 0;
  std::optional<dannan::listen::CertificatePaths> tls;
};

// The listeners are named as every node program of the stack names its own
// (Dannan's add_listener_settings): mqtt, mqtts, ws and wss, each advertised
// in "lir.endpoints" under its transport (tcp, tls, ws, wss).
struct ServerConfig {
  // MQTT over TCP, always served.
  Endpoint mqtt{"0.0.0.0", 1883, std::nullopt};
  // MQTT over TLS; it must carry its pair.
  std::optional<Endpoint> mqtts;
  // MQTT over WebSocket, plaintext.
  std::optional<Endpoint> ws;
  // MQTT over WebSocket on TLS; it must carry its pair.
  std::optional<Endpoint> wss;
  // The host clients are told to dial; empty: mqtt's address, or the node's
  // own advertised host when that is a wildcard.
  std::string advertise;
  // Asked of a TLS client under `optional`; a chain that does not verify
  // reaches the authenticator as no certificate.
  dannan::listen::ClientCertificates client_certificates = dannan::listen::ClientCertificates::never;
  broker::Link::Limits limits;
  // How long a clean close may take.
  std::chrono::milliseconds close_bound{2000};
  // How often the node's figures are looked at for its "lir.figures" advert,
  // and the most often another node's are rendered.
  std::chrono::milliseconds figures_every{1000};
};

class Server {
 public:
  // `io`, `context` and `authenticator` outlive the server; so does `time`,
  // the listeners' clock (the world's in a simulation). Opens Lir's channel
  // on the node: throws std::invalid_argument if it is already open, for an
  // mqtts or wss endpoint without its pair or a plaintext one with one, and
  // std::runtime_error for TLS material it cannot load.
  Server(boost::asio::io_context& io, dannan::Context& context, contract::Authenticator& authenticator, ServerConfig config,
         const dannan::contract::TimeSource* time = &dannan::contract::steady_time_source());
  // Stops the listeners. Every client's link refers to the server's
  // Delivery: call stop() and wait for its done before destroying it.
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  // Binds and serves, then advertises. Throws std::system_error when it
  // cannot bind.
  void start();
  // Withdraws the adverts and stops accepting, then closes every client
  // within the close bound; `done` is posted to `executor` once all have
  // gone. Once.
  void stop(boost::asio::any_io_executor executor, std::function<void()> done);

  uint16_t port() const noexcept { return port_of("tcp"); }
  // Zero without TLS.
  uint16_t tls_port() const noexcept { return port_of("tls"); }
  // The port served for "tcp", "tls", "ws" or "wss"; zero for one not served.
  uint16_t port_of(std::string_view transport) const noexcept;
  // The listener for a transport, if served: a test reaches it in memory
  // (StreamListener::inject).
  dannan::listen::StreamListener& listener() noexcept { return *listener_of("tcp"); }
  dannan::listen::StreamListener* tls_listener() noexcept { return listener_of("tls"); }
  dannan::listen::StreamListener* listener_of(std::string_view transport) noexcept;
  broker::Delivery& delivery() noexcept { return _delivery; }
  system::Views& views() noexcept { return _views; }

 private:
  std::string advertised_host() const;
  // Advertises the node's figures when they changed, then again after
  // figures_every, until stop.
  void tick();

  // What a tick reads, behind a lock its stop takes: a tick already queued
  // when the server stops finds it stopped.
  struct Ticker {
    std::mutex mutex;
    bool stopped = false;
    Server* server = nullptr;
    std::string last;
    uint64_t published = 0;
  };

  boost::asio::io_context& _io;
  dannan::Context& _context;
  ServerConfig _config;
  broker::Delivery _delivery;
  system::Views _views;
  Edge _edge;
  std::shared_ptr<Ticker> _ticker;
  // By transport, as lir.endpoints names them.
  std::map<std::string, std::unique_ptr<dannan::listen::StreamListener>, std::less<>> _listeners;
  bool _started = false;
  bool _stopped = false;
};

}  // namespace lir::edge
