//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <gtest/gtest.h>

#include <map>
#include <string>

#include "dannan/test/fixtures/stepped_time.h"
#include "lir/auth/lugh.h"

// The Lugh plugin (the design's section 1): a token as the CONNECT password
// or the "lugh" authentication method's data, its "lir:" scopes the grant,
// its end the connection's. Lugh's own verification is stood in for by the
// tokens a test names; what is Lir's is what it does with the answer.
namespace lir::auth {
namespace {

using namespace std::chrono_literals;
namespace reason = mqtt::reason;
using Kind = contract::AuthDecision::Kind;

mqtt::Bytes bytes(std::string_view text) { return mqtt::Bytes(text.begin(), text.end()); }

struct Plugin {
  Plugin(std::optional<Filters> anonymous = std::nullopt)
      : lugh(
            [this](std::string_view token) -> std::variant<lugh::client::AccessToken, std::string> {
              const auto found = tokens.find(std::string(token));
              if (found == tokens.end()) return std::string("not a token of a trusted issuer");
              return found->second;
            },
            std::move(anonymous), time, wall) {}

  // A token for `subject` with `scopes`, ending `life` from now.
  void issue(std::string token, std::string subject, std::vector<std::string> scopes, std::chrono::seconds life = 3600s) {
    lugh::client::AccessToken access;
    access.subject = std::move(subject);
    access.scopes = std::move(scopes);
    access.expires = wall.unix_ms() / 1000 + static_cast<uint64_t>(life.count());
    tokens[std::move(token)] = std::move(access);
  }
  contract::AuthDecision password(std::string token, std::optional<std::string> username = std::nullopt) {
    mqtt::Connect connect;
    connect.username = std::move(username);
    connect.password = bytes(token);
    return lugh.connect(connect, {});
  }

  dannan::test::SteppedTime time;
  dannan::test::SteppedWall wall;
  std::map<std::string, lugh::client::AccessToken> tokens;
  Lugh lugh;
};

TEST(AuthLugh, ATokenAsThePasswordIsGrantedItsScopes) {
  Plugin plugin;
  plugin.issue("t", "alice", {"lir:pub:sensors/+", "lir:sub:sensors/#", "openid"});
  const auto decision = plugin.password("t", "alice");
  ASSERT_EQ(decision.kind, Kind::accept);
  EXPECT_TRUE(decision.grant->may_publish("sensors/kitchen", false));
  EXPECT_FALSE(decision.grant->may_publish("sensors/kitchen", true));
  EXPECT_FALSE(decision.grant->may_publish("other", false));
  EXPECT_TRUE(decision.grant->may_subscribe("sensors/#"));
  EXPECT_FALSE(decision.grant->may_subscribe("#"));
}

// A level written {sub} is the token's subject.
TEST(AuthLugh, TheSubjectTakesItsPlaceInAScope) {
  Plugin plugin;
  plugin.issue("t", "alice", {"lir:pub:users/{sub}/#"});
  const auto grant = plugin.password("t").grant;
  ASSERT_TRUE(grant);
  EXPECT_TRUE(grant->may_publish("users/alice/status", false));
  EXPECT_FALSE(grant->may_publish("users/bob/status", false));
}

// The connection ends when the token does: the grant says when, on the
// links' own clock.
TEST(AuthLugh, TheGrantEndsWithTheToken) {
  Plugin plugin;
  plugin.issue("t", "alice", {"lir:sub:a"}, 600s);
  const auto grant = plugin.password("t").grant;
  ASSERT_TRUE(grant);
  ASSERT_TRUE(grant->expires());
  EXPECT_EQ(*grant->expires() - plugin.time.now(), std::chrono::milliseconds(600s));
}

TEST(AuthLugh, AnUnknownTokenIsABadPassword) {
  Plugin plugin;
  EXPECT_EQ(plugin.password("forged").reason, reason::bad_user_name_or_password);
}

// The user name, when one is given, is the token's subject.
TEST(AuthLugh, AUserNameMustBeTheTokensSubject) {
  Plugin plugin;
  plugin.issue("t", "alice", {"lir:sub:a"});
  EXPECT_EQ(plugin.password("t", "alice").kind, Kind::accept);
  EXPECT_EQ(plugin.password("t", "mallory").reason, reason::bad_user_name_or_password);
}

// MQTT 5 enhanced authentication, method "lugh", the token as its data: at
// CONNECT, and again on a long-lived connection with a fresh token.
TEST(AuthLugh, TheLughMethodCarriesTheToken) {
  Plugin plugin;
  plugin.issue("t", "alice", {"lir:sub:a"});
  plugin.issue("fresh", "alice", {"lir:sub:b"});
  mqtt::Connect connect;
  connect.properties.authentication_method = "lugh";
  connect.properties.authentication_data = bytes("t");
  EXPECT_EQ(plugin.lugh.connect(connect, {}).kind, Kind::accept);
  const auto again = plugin.lugh.auth("c", "lugh", bytes("fresh"), {});
  ASSERT_EQ(again.kind, Kind::accept);
  EXPECT_TRUE(again.grant->may_subscribe("b"));
  EXPECT_FALSE(again.grant->may_subscribe("a"));
  EXPECT_EQ(plugin.lugh.auth("c", "lugh", bytes("forged"), {}).reason, reason::bad_user_name_or_password);
  connect.properties.authentication_method = "scram";
  EXPECT_EQ(plugin.lugh.connect(connect, {}).reason, reason::bad_authentication_method);
  EXPECT_EQ(plugin.lugh.auth("c", "scram", bytes("t"), {}).reason, reason::bad_authentication_method);
}

// A token bound to a certificate serves only the connection that presented
// it (RFC 8705); one bound to a DPoP key, which MQTT cannot prove, none.
TEST(AuthLugh, ABoundTokenServesOnlyItsHolder) {
  Plugin plugin;
  plugin.issue("cert", "alice", {"lir:sub:a"});
  plugin.tokens["cert"].x5t = std::array<uint8_t, 32>{1};
  contract::Peer holder;
  holder.certificate_thumbprint = std::array<uint8_t, 32>{1};
  contract::Peer other;
  other.certificate_thumbprint = std::array<uint8_t, 32>{2};
  mqtt::Connect connect;
  connect.password = bytes("cert");
  EXPECT_EQ(plugin.lugh.connect(connect, holder).kind, Kind::accept);
  EXPECT_EQ(plugin.lugh.connect(connect, other).reason, reason::not_authorized);
  EXPECT_EQ(plugin.lugh.connect(connect, {}).reason, reason::not_authorized);
  plugin.issue("dpop", "alice", {"lir:sub:a"});
  plugin.tokens["dpop"].jkt = "thumbprint";
  EXPECT_EQ(plugin.password("dpop").reason, reason::not_authorized);
}

// No credential is anonymous, as the node allows; a user name alone is a
// credential with nothing to check it by.
TEST(AuthLugh, AClientWithNoCredentialIsAnonymous) {
  Plugin refusing;
  EXPECT_EQ(refusing.lugh.connect({}, {}).reason, reason::not_authorized);
  Filters allowed;
  allowed.subscribe = {"public/#"};
  Plugin allowing(allowed);
  const auto decision = allowing.lugh.connect({}, {});
  ASSERT_EQ(decision.kind, Kind::accept);
  EXPECT_TRUE(decision.grant->may_subscribe("public/news"));
  mqtt::Connect named;
  named.username = "alice";
  EXPECT_EQ(allowing.lugh.connect(named, {}).reason, reason::bad_user_name_or_password);
}

}  // namespace
}  // namespace lir::auth
