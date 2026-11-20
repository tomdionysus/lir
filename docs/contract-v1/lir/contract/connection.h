//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include "lir/mqtt/wire.h"

namespace lir::contract {

// A client certificate the listener verified: its subject (RFC 2253 form)
// and its SHA-256 thumbprint, over the DER (RFC 8705's x5t#S256).
struct ClientCertificate {
  std::string subject;
  std::array<uint8_t, 32> thumbprint{};
};

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
  // The certificate the client presented, verified; nothing without one, or
  // off TLS.
  virtual std::optional<ClientCertificate> certificate() const { return std::nullopt; }
  // How the client reached this node: "tcp", "tls", "ws", "wss", or
  // another word for a transport no client can be redirected to.
  virtual std::string transport() const { return "tcp"; }
};

}  // namespace lir::contract
