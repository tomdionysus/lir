//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "dannan/contract/time_source.h"
#include "lir/contract/connection.h"
#include "lir/mqtt/packet.h"

// Who may connect, and what a connection may then do. An accepted
// connection is given a Grant, held by its link for the connection's life
// and replaced when a 5.0 client re-authenticates; publish and subscribe
// are checked against it, so the authenticator keeps no per-client state.
// Lugh's token scopes plug in as one kind of grant (auth/scopes.h).
namespace lir::contract {

// What one connection may do. Answered from memory on the broker's strand.
class Grant {
 public:
  virtual ~Grant() = default;
  // A PUBLISH (or a will) to `topic`; `retain` is the packet's retain flag.
  virtual bool may_publish(std::string_view topic, bool retain) const = 0;
  // A SUBSCRIBE to `filter` as sent, `$share/<group>/` included.
  virtual bool may_subscribe(std::string_view filter) const = 0;
  // When the credential behind the grant ends: the connection is then
  // closed with 0x87 (Not authorized) unless a re-authentication replaced
  // the grant first. Nothing: never.
  virtual std::optional<dannan::contract::TimePoint> expires() const { return std::nullopt; }
};

// Everything, for ever.
class Unrestricted final : public Grant {
 public:
  bool may_publish(std::string_view, bool) const override { return true; }
  bool may_subscribe(std::string_view) const override { return true; }
};

// Nothing: what an accept without a grant is held to.
class Nothing final : public Grant {
 public:
  bool may_publish(std::string_view, bool) const override { return false; }
  bool may_subscribe(std::string_view) const override { return false; }
};

struct AuthDecision {
  enum class Kind { accept, refuse, challenge };
  Kind kind = Kind::accept;
  // On refuse: a 5.0 CONNACK reason code (0x80 and up). A 3.x client is
  // told the nearest 3.x return code.
  uint8_t reason = mqtt::reason::success;
  // 5.0 enhanced authentication: data sent back in CONNACK or AUTH.
  std::optional<mqtt::Bytes> data;
  // On accept: what the connection may do. Null is held as Nothing.
  std::shared_ptr<const Grant> grant;

  static AuthDecision accept(std::shared_ptr<const Grant> grant, std::optional<mqtt::Bytes> data = {}) {
    return {Kind::accept, mqtt::reason::success, std::move(data), std::move(grant)};
  }
  static AuthDecision refuse(uint8_t reason) { return {Kind::refuse, reason, {}, {}}; }
  static AuthDecision challenge(mqtt::Bytes data) { return {Kind::challenge, mqtt::reason::continue_authentication, std::move(data), {}}; }
};

// Who is connecting, as the transport knows it.
struct Peer {
  std::string address;
  // The verified client certificate's subject (RFC 2253 form), when the
  // listener asks for client certificates and one was presented.
  std::optional<std::string> certificate_subject;
  // Its SHA-256 thumbprint, for a token bound to it (RFC 8705, cnf
  // x5t#S256).
  std::optional<std::array<uint8_t, 32>> certificate_thumbprint;
};

// Called only from the broker's strand. Every call answers from what it
// holds and waits on nothing: a check that needs the network is made
// before, and its answer cached here.
class Authenticator {
 public:
  virtual ~Authenticator() = default;
  // The CONNECT as sent. A challenge is only allowed when the CONNECT named
  // an authentication method (5.0).
  virtual AuthDecision connect(const mqtt::Connect& connect, const Peer& peer) = 0;
  // A 5.0 AUTH continuing an exchange or reauthenticating: the method the
  // exchange began with, the client's data, and the peer as at connect. An
  // accept on re-authentication replaces the connection's grant.
  virtual AuthDecision auth(const std::string& client_id, const std::string& method, const mqtt::Bytes& data, const Peer& peer) = 0;
};

// Accepts everyone with an Unrestricted grant; refuses any enhanced
// authentication method, since it knows none. For tests and embedding.
class AllowAll final : public Authenticator {
 public:
  AuthDecision connect(const mqtt::Connect& connect, const Peer&) override {
    return connect.properties.authentication_method ? AuthDecision::refuse(mqtt::reason::bad_authentication_method)
                                                    : AuthDecision::accept(std::make_shared<Unrestricted>());
  }
  AuthDecision auth(const std::string&, const std::string&, const mqtt::Bytes&, const Peer&) override {
    return AuthDecision::refuse(mqtt::reason::bad_authentication_method);
  }
};

}  // namespace lir::contract
