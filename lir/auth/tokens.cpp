//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "lir/auth/tokens.h"

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace lir::auth {

namespace reason = mqtt::reason;
using contract::AuthDecision;

TokenAuthenticator::TokenAuthenticator(const TokenVerifier& verifier, const dannan::contract::TimeSource& time, const dannan::contract::WallClock& wall,
                                       TokenConfig config, dannan::status::ScopedLogger logger)
    : _verifier(verifier), _time(time), _wall(wall), _config(std::move(config)), _logger(std::move(logger)) {
  if (_config.anonymous) _anonymous = std::make_shared<ScopeGrant>(*_config.anonymous);
}

AuthDecision TokenAuthenticator::connect(const mqtt::Connect& connect, const contract::Peer& peer) {
  if (const auto& method = connect.properties.authentication_method) {
    if (*method != _config.method) return AuthDecision::refuse(reason::bad_authentication_method);
    const auto& data = connect.properties.authentication_data;
    if (!data || data->empty()) return AuthDecision::refuse(reason::bad_user_name_or_password);
    return decide(std::string_view(reinterpret_cast<const char*>(data->data()), data->size()), peer);
  }
  if (connect.password) {
    return decide(std::string_view(reinterpret_cast<const char*>(connect.password->data()), connect.password->size()), peer);
  }
  if (connect.username) return AuthDecision::refuse(reason::bad_user_name_or_password);
  if (!_anonymous) return AuthDecision::refuse(reason::not_authorized);
  return AuthDecision::accept(_anonymous);
}

AuthDecision TokenAuthenticator::auth(const std::string&, const std::string& method, const mqtt::Bytes& data, const contract::Peer& peer) {
  if (method != _config.method) return AuthDecision::refuse(reason::bad_authentication_method);
  return decide(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()), peer);
}

AuthDecision TokenAuthenticator::decide(std::string_view token, const contract::Peer& peer) {
  auto verified = _verifier.verify(token);
  if (const auto* why = std::get_if<std::string>(&verified)) {
    _logger.info("refusing a token from " + peer.address + ": " + *why);
    return AuthDecision::refuse(reason::bad_user_name_or_password);
  }
  const auto& claims = std::get<TokenClaims>(verified);
  // exp is read on the wall clock and the grant kept on the steady one: the
  // time left, from both clocks read now.
  const uint64_t wall_ms = _wall.unix_ms();
  const uint64_t expires_ms = claims.expires_unix > UINT64_MAX / 1000 ? UINT64_MAX : claims.expires_unix * 1000;
  if (expires_ms <= wall_ms) return AuthDecision::refuse(reason::bad_user_name_or_password);
  // Capped at ten years, so the sum stays in the steady clock's range.
  constexpr uint64_t longest_ms = 10ull * 365 * 24 * 3600 * 1000;
  const auto left = std::chrono::milliseconds(static_cast<int64_t>(std::min(expires_ms - wall_ms, longest_ms)));
  const auto expires = _time.now() + left;
  if (std::find(claims.audiences.begin(), claims.audiences.end(), _config.audience) == claims.audiences.end()) {
    _logger.info("refusing " + claims.subject + " from " + peer.address + ": the token is not for " + _config.audience);
    return AuthDecision::refuse(reason::not_authorized);
  }
  if (claims.bound_to_key) {
    _logger.info("refusing " + claims.subject + " from " + peer.address + ": the token is bound to a DPoP key");
    return AuthDecision::refuse(reason::not_authorized);
  }
  if (claims.bound_certificate && claims.bound_certificate != peer.certificate_thumbprint) {
    _logger.info("refusing " + claims.subject + " from " + peer.address + ": the token is bound to another certificate");
    return AuthDecision::refuse(reason::not_authorized);
  }
  auto parsed = parse_scopes(claims.scopes, claims.subject);
  for (const auto& scope : parsed.refused) _logger.warn("ignoring the scope " + scope + " in a token for " + claims.subject);
  return AuthDecision::accept(std::make_shared<ScopeGrant>(std::move(parsed.scopes), expires));
}

}  // namespace lir::auth
