//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "lir/auth/scopes.h"

#include <algorithm>
#include <stdexcept>

#include "lir/mqtt/topic.h"

namespace lir::auth {

namespace {

constexpr std::string_view kPrefix = "lir:";

// The filter decoded level by level, "{sub}" levels replaced by the
// subject as it is; nothing when a brace is anywhere else, an escape is
// malformed, a decoded level holds a '/' or a NUL, or the subject is not one
// literal level.
std::optional<std::string> filter_of(std::string_view encoded, std::string_view subject) {
  std::string out;
  bool first = true;
  for (const auto level : mqtt::levels(encoded)) {
    if (!first) out += '/';
    first = false;
    if (level == "{sub}") {
      if (subject.empty() || subject.find_first_of("/+#") != std::string_view::npos || subject.find('\0') != std::string_view::npos) return std::nullopt;
      out += subject;
      continue;
    }
    if (level.find_first_of("{}") != std::string_view::npos) return std::nullopt;
    std::string decoded;
    try {
      decoded = percent_decode(level);
    } catch (const std::invalid_argument&) {
      return std::nullopt;
    }
    if (decoded.find_first_of(std::string_view("/\0", 2)) != std::string::npos) return std::nullopt;
    out += decoded;
  }
  return out;
}

}  // namespace

ParsedScopes parse_scopes(std::span<const std::string> scopes, std::string_view subject) {
  ParsedScopes out;
  for (const auto& scope : scopes) {
    if (!std::string_view(scope).starts_with(kPrefix)) continue;
    const std::string_view rest = std::string_view(scope).substr(kPrefix.size());
    const size_t colon = rest.find(':');
    std::vector<std::string>* into = nullptr;
    if (colon != std::string_view::npos) {
      const auto action = rest.substr(0, colon);
      if (action == "pub") into = &out.scopes.publish;
      if (action == "sub") into = &out.scopes.subscribe;
      if (action == "retain") into = &out.scopes.retain;
    }
    std::optional<std::string> filter;
    if (into) filter = filter_of(rest.substr(colon + 1), subject);
    if (!filter || mqtt::is_shared(*filter) || !mqtt::valid_topic_filter(*filter)) {
      out.refused.push_back(scope);
      continue;
    }
    into->push_back(std::move(*filter));
  }
  return out;
}

ScopeGrant::ScopeGrant(TopicScopes scopes, std::optional<dannan::contract::TimePoint> expires) : _scopes(std::move(scopes)), _expires(expires) {}

bool ScopeGrant::may_publish(std::string_view topic, bool retain) const {
  const auto& granted = retain ? _scopes.retain : _scopes.publish;
  return std::any_of(granted.begin(), granted.end(), [&](const std::string& filter) { return mqtt::matches(filter, topic); });
}

bool ScopeGrant::may_subscribe(std::string_view filter) const {
  std::string inner(filter);
  if (mqtt::is_shared(filter)) {
    const auto shared = mqtt::parse_shared(filter);
    if (!shared) return false;
    inner = shared->filter;
  }
  if (!mqtt::valid_topic_filter(inner)) return false;
  return std::any_of(_scopes.subscribe.begin(), _scopes.subscribe.end(), [&](const std::string& granted) { return mqtt::covers(granted, inner); });
}

TopicScopes default_anonymous_scopes() { return {{"#"}, {"#", "$lir/#", "$dannan/#"}, {"#"}}; }

AnonymousAuthenticator::AnonymousAuthenticator(std::optional<TopicScopes> anonymous) {
  if (anonymous) _anonymous = std::make_shared<ScopeGrant>(std::move(*anonymous));
}

contract::AuthDecision AnonymousAuthenticator::connect(const mqtt::Connect& connect, const contract::Peer&) {
  if (connect.properties.authentication_method) return contract::AuthDecision::refuse(mqtt::reason::bad_authentication_method);
  if (connect.username || connect.password) return contract::AuthDecision::refuse(mqtt::reason::bad_user_name_or_password);
  if (!_anonymous) return contract::AuthDecision::refuse(mqtt::reason::not_authorized);
  return contract::AuthDecision::accept(_anonymous);
}

contract::AuthDecision AnonymousAuthenticator::auth(const std::string&, const std::string&, const mqtt::Bytes&) {
  return contract::AuthDecision::refuse(mqtt::reason::bad_authentication_method);
}

}  // namespace lir::auth
