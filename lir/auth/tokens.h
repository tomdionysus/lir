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
#include <string_view>
#include <variant>
#include <vector>

#include "dannan/contract/time_source.h"
#include "dannan/status/log.h"
#include "lir/auth/scopes.h"

// Clients that sign in with a Lugh access token (design, section 1; Tom
// 2026-11-04; ../lugh/TODO/2026-11-04-oauth.md, "Topic scopes, for Lir").
// The token is the CONNECT password, or the authentication data of the MQTT
// 5 method "lugh", which also re-authenticates with a fresh one. Lir checks
// what is Lir's: "lir" among the audiences, the topic scopes with {sub} as
// the grant until exp, and a bound token only where its binding can be
// checked. Whether the token is genuine (the JWS, the key by kid, typ,
// iss, nbf and exp) is Lugh's code, behind TokenVerifier: verification
// stays Lugh's (stack decision, 2026-11-04), so Lir has no JOSE of its own.
namespace lir::auth {

// What a verified access token (RFC 9068) says, as far as Lir needs.
struct TokenClaims {
  std::string subject;
  std::vector<std::string> audiences;
  // The scope claim, split on spaces.
  std::vector<std::string> scopes;
  dannan::contract::TimePoint expires{};
  // cnf.jkt: bound to a DPoP key (RFC 9449), which MQTT cannot prove.
  bool bound_to_key = false;
  // cnf x5t#S256, decoded (RFC 8705): good only from that certificate.
  std::optional<std::array<uint8_t, 32>> bound_certificate;
};

// Says whether a token is genuine and current. Answers from what it holds
// (keys fetched and cached before), on the broker's strand, and waits on
// nothing.
class TokenVerifier {
 public:
  virtual ~TokenVerifier() = default;
  // The claims, or why the token is not good.
  virtual std::variant<TokenClaims, std::string> verify(std::string_view token, dannan::contract::TimePoint now) const = 0;
};

struct TokenConfig {
  // Lir's audience, as registered in Lugh.
  std::string audience = "lir";
  // The MQTT 5 authentication method that carries a token.
  std::string method = "lugh";
  // Clients with no credential at all: refused when nothing, else held to
  // these scopes.
  std::optional<TopicScopes> anonymous;
};

class TokenAuthenticator final : public contract::Authenticator {
 public:
  // The verifier and the time source outlive the authenticator.
  TokenAuthenticator(const TokenVerifier& verifier, const dannan::contract::TimeSource& time, TokenConfig config, dannan::status::ScopedLogger logger);
  contract::AuthDecision connect(const mqtt::Connect& connect, const contract::Peer& peer) override;
  contract::AuthDecision auth(const std::string& client_id, const std::string& method, const mqtt::Bytes& data, const contract::Peer& peer) override;

 private:
  contract::AuthDecision decide(std::string_view token, const contract::Peer& peer);

  const TokenVerifier& _verifier;
  const dannan::contract::TimeSource& _time;
  TokenConfig _config;
  dannan::status::ScopedLogger _logger;
  std::shared_ptr<const ScopeGrant> _anonymous;
};

}  // namespace lir::auth
