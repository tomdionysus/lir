//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "lir/contract/authenticator.h"

// Topic scopes: what a client may publish, retain and subscribe to, as
// carried in a Lugh token (design, section 1: Lir checks the token's scopes
// itself, Tom 2026-11-04). The grammar, decided by Tom on 2026-11-04
// (../lugh/TODO/2026-11-04-oauth.md, "Topic scopes, for Lir"):
//
//   "lir:" ("pub" | "sub" | "retain") ":" percent-encoded MQTT filter
//
// pub is a PUBLISH without the retain flag to a topic the filter matches;
// retain is a PUBLISH with it (and does not imply pub); sub is a SUBSCRIBE
// whose filter the granted one covers, the filter inside "$share/<g>/" for
// a shared one. A level that is exactly "{sub}" is the token's subject;
// nothing else is substituted ({client} was dropped: a client chooses its
// own identifier).
namespace lir::auth {

// %XX decoded; nothing else ('+' stays '+'). Throws std::invalid_argument
// for a malformed escape.
std::string percent_decode(std::string_view text);

struct TopicScopes {
  std::vector<std::string> publish;
  std::vector<std::string> subscribe;
  std::vector<std::string> retain;
  bool empty() const noexcept { return publish.empty() && subscribe.empty() && retain.empty(); }
};

struct ParsedScopes {
  TopicScopes scopes;
  // The "lir:" scopes left out: malformed, an unknown action, an invalid
  // filter, a brace that is not a whole "{sub}" level, or a subject that is
  // not one literal level. Scopes for other applications are skipped
  // silently.
  std::vector<std::string> refused;
};
ParsedScopes parse_scopes(std::span<const std::string> scopes, std::string_view subject = {});

// A connection's grant from its scopes, until `expires`.
class ScopeGrant final : public contract::Grant {
 public:
  explicit ScopeGrant(TopicScopes scopes, std::optional<dannan::contract::TimePoint> expires = std::nullopt);
  bool may_publish(std::string_view topic, bool retain) const override;
  bool may_subscribe(std::string_view filter) const override;
  std::optional<dannan::contract::TimePoint> expires() const override { return _expires; }

 private:
  TopicScopes _scopes;
  std::optional<dannan::contract::TimePoint> _expires;
};

// What an anonymous client may do when anonymous clients are allowed and no
// scopes are given: publish, retain and subscribe anywhere, and read the
// system topics.
TopicScopes default_anonymous_scopes();

// The authenticator a node runs until the Lugh plugin: anonymous clients
// (no user name, no password, no authentication method) are refused unless
// allowed, and then held to `anonymous`; a client presenting a credential
// is refused, since there is nothing here to check it against (design,
// section 1: anonymous refused by default, a node flag allows them with a
// configured scope set).
class AnonymousAuthenticator final : public contract::Authenticator {
 public:
  // Nothing: anonymous clients are refused.
  explicit AnonymousAuthenticator(std::optional<TopicScopes> anonymous);
  contract::AuthDecision connect(const mqtt::Connect& connect) override;
  contract::AuthDecision auth(const std::string& client_id, const std::string& method, const mqtt::Bytes& data) override;

 private:
  std::shared_ptr<const ScopeGrant> _anonymous;
};

}  // namespace lir::auth
