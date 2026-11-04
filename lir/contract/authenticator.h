//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "lir/mqtt/packet.h"

// Who may connect, publish and subscribe. Lugh's tokens and scopes plug in
// here (ACTIVE.md, finding 4); until then a node runs AllowAll or its own.
namespace lir::contract {

struct AuthDecision {
  enum class Kind { accept, refuse, challenge };
  Kind kind = Kind::accept;
  // On refuse: a 5.0 CONNACK reason code (0x80 and up). A 3.x client is
  // told the nearest 3.x return code.
  uint8_t reason = mqtt::reason::success;
  // 5.0 enhanced authentication: data sent back in CONNACK or AUTH.
  std::optional<mqtt::Bytes> data;

  static AuthDecision accept() { return {}; }
  static AuthDecision refuse(uint8_t reason) { return {Kind::refuse, reason, {}}; }
  static AuthDecision challenge(mqtt::Bytes data) { return {Kind::challenge, mqtt::reason::continue_authentication, std::move(data)}; }
};

// Called only from the broker's strand. Every call answers from what it
// holds and waits on nothing: a check that needs the network is made
// before, and its answer cached here.
class Authenticator {
 public:
  virtual ~Authenticator() = default;
  // The CONNECT as sent. A challenge is only allowed when the CONNECT named
  // an authentication method (5.0).
  virtual AuthDecision connect(const mqtt::Connect& connect) = 0;
  // A 5.0 AUTH continuing an exchange or reauthenticating: the method the
  // exchange began with, and the client's data.
  virtual AuthDecision auth(const std::string& client_id, const std::string& method, const mqtt::Bytes& data) = 0;
  virtual bool may_publish(const std::string& client_id, std::string_view topic) = 0;
  virtual bool may_subscribe(const std::string& client_id, std::string_view filter) = 0;
};

// Accepts everyone and everything; refuses any enhanced authentication
// method, since it knows none.
class AllowAll final : public Authenticator {
 public:
  AuthDecision connect(const mqtt::Connect& connect) override {
    return connect.properties.authentication_method ? AuthDecision::refuse(mqtt::reason::bad_authentication_method) : AuthDecision::accept();
  }
  AuthDecision auth(const std::string&, const std::string&, const mqtt::Bytes&) override {
    return AuthDecision::refuse(mqtt::reason::bad_authentication_method);
  }
  bool may_publish(const std::string&, std::string_view) override { return true; }
  bool may_subscribe(const std::string&, std::string_view) override { return true; }
};

}  // namespace lir::contract
