//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <optional>
#include <string>

#include "lir/mqtt/wire.h"

namespace lir::contract {

// The transport under one MQTT client: a TCP or TLS socket at the edge, a
// buffer in a test. The broker calls it only from its own strand.
class Connection {
 public:
  virtual ~Connection() = default;
  // Queues whole packets in order. Waits on nothing; a slow peer is the
  // transport's to bound, and it closes rather than block.
  virtual void send(mqtt::Bytes bytes) = 0;
  // Closes after what was queued is flushed. Idempotent; nothing is sent
  // after. Waits on nothing.
  virtual void close() = 0;
  // For logs: the peer's address.
  virtual std::string peer() const = 0;
  // The subject of the certificate the client presented, verified (RFC 2253
  // form); nothing without one, or off TLS.
  virtual std::optional<std::string> certificate_subject() const { return std::nullopt; }
  // How the client reached this node: "tcp", "tls", "ws", "wss", or
  // another word for a transport no client can be redirected to.
  virtual std::string transport() const { return "tcp"; }
};

}  // namespace lir::contract
