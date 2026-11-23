//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "lir/auth/anonymous.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "lir/mqtt/topic.h"

namespace lir::auth {

namespace {

void check(const std::vector<std::string>& filters, std::string_view what) {
  for (const auto& filter : filters)
    if (mqtt::is_shared(filter) || !mqtt::valid_topic_filter(filter))
      throw std::invalid_argument("the " + std::string(what) + " filter \"" + filter + "\" is not a valid topic filter");
}

bool any_matches(const std::vector<std::string>& filters, std::string_view topic) {
  return std::any_of(filters.begin(), filters.end(), [&](const auto& filter) { return mqtt::matches(filter, topic); });
}

}  // namespace

FilterGrant::FilterGrant(Filters filters) : _filters(std::move(filters)) {
  check(_filters.publish, "publish");
  check(_filters.retain, "retain");
  check(_filters.subscribe, "subscribe");
}

bool FilterGrant::may_publish(std::string_view topic, bool retain) const {
  if (!mqtt::valid_topic_name(topic)) return false;
  return any_matches(retain ? _filters.retain : _filters.publish, topic);
}

bool FilterGrant::may_subscribe(std::string_view filter) const {
  std::string inner(filter);
  if (mqtt::is_shared(filter)) {
    const auto shared = mqtt::parse_shared(filter);
    if (!shared) return false;
    inner = shared->filter;
  } else if (!mqtt::valid_topic_filter(filter)) {
    return false;
  }
  return std::any_of(_filters.subscribe.begin(), _filters.subscribe.end(), [&](const auto& granted) { return mqtt::covers(granted, inner); });
}

Anonymous::Anonymous(std::optional<Filters> allowed) {
  if (allowed) _grant = std::make_shared<const FilterGrant>(std::move(*allowed));
}

contract::AuthDecision Anonymous::connect(const mqtt::Connect& connect, const contract::Peer&) {
  if (connect.properties.authentication_method) return contract::AuthDecision::refuse(mqtt::reason::bad_authentication_method);
  if (connect.username || connect.password) return contract::AuthDecision::refuse(mqtt::reason::bad_user_name_or_password);
  if (!_grant) return contract::AuthDecision::refuse(mqtt::reason::not_authorized);
  return contract::AuthDecision::accept(_grant);
}

contract::AuthDecision Anonymous::auth(const std::string&, const std::string&, const mqtt::Bytes&, const contract::Peer&) {
  return contract::AuthDecision::refuse(mqtt::reason::bad_authentication_method);
}

}  // namespace lir::auth
