//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <atomic>
#include <memory>
#include <optional>
#include <string>

#include "dannan/context.h"
#include "dannan/listen/listener.h"
#include "dannan/listen/stream.h"
#include "dannan/status/log.h"
#include "lir/broker/delivery.h"
#include "lir/broker/link.h"
#include "lir/contract/authenticator.h"
#include "lir/contract/connection.h"
#include "lir/system/views.h"

// The MQTT edge of one node (the rewrite's design, "Connection"): each stream
// Dannan's listener accepts is one client, served by a broker::Link. The
// listener is Dannan's (accept, TLS, limits, the bounded close, a stream in
// memory inside the simulation's world); the edge only reads what arrives
// into the link and gives the link the stream to answer through. Lir keeps no
// socket of its own.
namespace lir::edge {

// A Dannan stream as the contract's Connection.
class StreamConnection final : public contract::Connection {
 public:
  explicit StreamConnection(std::shared_ptr<dannan::listen::Stream> stream);

  void send(mqtt::Bytes bytes) override;
  void close() override;
  std::string peer() const override;
  // Only a certificate whose chain the listener verified.
  std::optional<contract::ClientCertificate> certificate() const override;
  // The stream's: "tcp", "tls", or "memory" inside a test.
  std::string transport() const override;

 private:
  std::shared_ptr<dannan::listen::Stream> _stream;
  std::string _peer;
  std::atomic<bool> _closed{false};
};

// "host:port", an IPv6 host in brackets.
std::string endpoint_string(const boost::asio::ip::tcp::endpoint& endpoint);

class Edge {
 public:
  // `delivery`, `authenticator` and `time` outlive every stream served.
  // `logger` is each link's.
  // `views`, when given, outlives every stream too (broker::Link::create).
  Edge(broker::Delivery& delivery, contract::Authenticator& authenticator, dannan::Time& time, dannan::status::ScopedLogger logger,
       broker::Link::Limits limits = {}, system::Views* views = nullptr);
  Edge(const Edge&) = delete;
  Edge& operator=(const Edge&) = delete;

  // The listener's Accepted: `listener.start(executor, edge.accepted())`.
  // Each stream must be on a strand of its own (the listener's
  // strand_per_stream, its default), which becomes its link's.
  dannan::listen::StreamListener::Accepted accepted();
  void serve(std::shared_ptr<dannan::listen::Stream> stream);
  // The same for a listener of MQTT over WebSocket (edge/websocket.h).
  dannan::listen::StreamListener::Accepted accepted_websocket();

 private:
  broker::Delivery& _delivery;
  contract::Authenticator& _authenticator;
  dannan::Time& _time;
  dannan::status::ScopedLogger _logger;
  broker::Link::Limits _limits;
  system::Views* _views;
};

}  // namespace lir::edge
