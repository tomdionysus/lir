//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <variant>

#include "dannan/contract/time_source.h"
#include "lir/auth/anonymous.h"
#include "lir/contract/authenticator.h"
#include "lugh/client/tokens.h"

// The optional Lugh plugin (the design's section 1): a Lugh access token as
// the CONNECT password, or as the data of the MQTT 5 authentication method
// "lugh", which also re-authenticates a long-lived connection with a fresh
// one. The token is checked by Lugh's lugh_client (RelyingParty), offline,
// and its "lir:" scopes become the connection's grant, checked by Lugh's
// lugh::client_topics on lir::mqtt. A client with no credential is
// anonymous, as the node allows. Lir keeps no account or key of its own.
namespace lir::auth {

inline constexpr std::string_view lugh_method = "lugh";

class Lugh final : public contract::Authenticator {
 public:
  // An access token for this broker's audience, or why not:
  // lugh::client::RelyingParty::verify.
  using Verify = std::function<std::variant<lugh::client::AccessToken, std::string>(std::string_view token)>;

  // `time` is the links' clock, which a grant's end is told on; `wall` the
  // clock a token's exp is read against. Both outlive this.
  Lugh(Verify verify, std::optional<Filters> anonymous, const dannan::contract::TimeSource& time, const dannan::contract::WallClock& wall);

  contract::AuthDecision connect(const mqtt::Connect& connect, const contract::Peer& peer) override;
  contract::AuthDecision auth(const std::string& client_id, const std::string& method, const mqtt::Bytes& data, const contract::Peer& peer) override;

 private:
  // The grant a token gives, or the refusal. A user name, when given, must
  // be the token's subject.
  contract::AuthDecision check(std::string_view token, const std::optional<std::string>& username, const contract::Peer& peer) const;

  Verify _verify;
  Anonymous _anonymous;
  const dannan::contract::TimeSource& _time;
  const dannan::contract::WallClock& _wall;
};

}  // namespace lir::auth
