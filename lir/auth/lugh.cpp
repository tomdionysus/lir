//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "lir/auth/lugh.h"

#include <chrono>
#include <utility>

#include "lugh/client/scopes.h"
#include "lugh/client/topics.h"

namespace lir::auth {

namespace {

// What a token's "lir:" scopes let the connection do, until the token ends.
class TokenGrant final : public contract::Grant {
 public:
  TokenGrant(lugh::client::TopicGrants grants, dannan::contract::TimePoint ends) : _grants(std::move(grants)), _ends(ends) {}
  bool may_publish(std::string_view topic, bool retain) const override { return lugh::client::may_publish(_grants, topic, retain); }
  bool may_subscribe(std::string_view filter) const override { return lugh::client::may_subscribe(_grants, filter); }
  std::optional<dannan::contract::TimePoint> expires() const override { return _ends; }

 private:
  lugh::client::TopicGrants _grants;
  dannan::contract::TimePoint _ends;
};

std::string_view text(const mqtt::Bytes& bytes) { return {reinterpret_cast<const char*>(bytes.data()), bytes.size()}; }

}  // namespace

Lugh::Lugh(Verify verify, std::optional<Filters> anonymous, const dannan::contract::TimeSource& time, const dannan::contract::WallClock& wall)
    : _verify(std::move(verify)), _anonymous(std::move(anonymous)), _time(time), _wall(wall) {}

contract::AuthDecision Lugh::connect(const mqtt::Connect& connect, const contract::Peer& peer) {
  if (const auto& method = connect.properties.authentication_method) {
    if (*method != lugh_method) return contract::AuthDecision::refuse(mqtt::reason::bad_authentication_method);
    if (!connect.properties.authentication_data) return contract::AuthDecision::refuse(mqtt::reason::bad_user_name_or_password);
    return check(text(*connect.properties.authentication_data), connect.username, peer);
  }
  if (connect.password) return check(text(*connect.password), connect.username, peer);
  if (connect.username) return contract::AuthDecision::refuse(mqtt::reason::bad_user_name_or_password);
  return _anonymous.connect(connect, peer);
}

contract::AuthDecision Lugh::auth(const std::string&, const std::string& method, const mqtt::Bytes& data, const contract::Peer& peer) {
  if (method != lugh_method) return contract::AuthDecision::refuse(mqtt::reason::bad_authentication_method);
  return check(text(data), std::nullopt, peer);
}

contract::AuthDecision Lugh::check(std::string_view token, const std::optional<std::string>& username, const contract::Peer& peer) const {
  auto verified = _verify(token);
  auto* access = std::get_if<lugh::client::AccessToken>(&verified);
  if (!access) return contract::AuthDecision::refuse(mqtt::reason::bad_user_name_or_password);
  if (username && *username != access->subject) return contract::AuthDecision::refuse(mqtt::reason::bad_user_name_or_password);
  // A token bound to a DPoP key cannot be proved over MQTT; one bound to a
  // certificate only over the connection that presented it (RFC 8705).
  if (access->jkt) return contract::AuthDecision::refuse(mqtt::reason::not_authorized);
  if (access->x5t && peer.certificate_thumbprint != access->x5t) return contract::AuthDecision::refuse(mqtt::reason::not_authorized);
  // exp is wall time; the link counts on its own clock, so the end is told
  // as what remains of the token from now.
  const auto now_ms = static_cast<int64_t>(_wall.unix_ms());
  const auto remaining = std::chrono::milliseconds(static_cast<int64_t>(access->expires) * 1000 - now_ms);
  auto grants = lugh::client::TopicGrants::parse(access->scopes, access->subject);
  return contract::AuthDecision::accept(std::make_shared<TokenGrant>(std::move(grants), _time.now() + remaining));
}

}  // namespace lir::auth
