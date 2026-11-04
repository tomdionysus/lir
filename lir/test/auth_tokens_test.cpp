//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <gtest/gtest.h>

#include <map>

#include "dannan/status/log.h"
#include "dannan/test/fixtures/stepped_time.h"
#include "lir/auth/tokens.h"

// The Lugh authenticator's own checks (../lugh/TODO/2026-11-04-oauth.md,
// "Topic scopes, for Lir"): the token as the CONNECT password or in the
// "lugh" method's data, "lir" among its audiences, its topic scopes with
// {sub} as the grant until exp, and a bound token honoured only when its
// binding can be checked. The signature and the rest of RFC 9068 are the
// verifier's, which is Lugh's code; here it is a table.
namespace lir::auth {
namespace {

using namespace std::chrono_literals;
using dannan::contract::TimePoint;
namespace reason = mqtt::reason;

class TableVerifier final : public TokenVerifier {
 public:
  std::variant<TokenClaims, std::string> verify(std::string_view token, TimePoint) const override {
    const auto it = tokens.find(std::string(token));
    if (it == tokens.end()) return std::string("signature does not verify");
    return it->second;
  }
  std::map<std::string, TokenClaims> tokens;
};

class TokenAuthenticatorTest : public ::testing::Test {
 protected:
  TokenAuthenticatorTest() {
    TokenClaims device;
    device.subject = "device-17";
    device.audiences = {"lugh", "lir"};
    device.scopes = {"openid", "lir:pub:sensors/{sub}/%23", "lir:sub:commands/{sub}/%23", "lugh:me.read"};
    device.expires = time.now() + 15min;
    verifier.tokens["good"] = device;
    auto other = device;
    other.audiences = {"lugh"};
    verifier.tokens["other-audience"] = other;
    auto dpop = device;
    dpop.bound_to_key = true;
    verifier.tokens["dpop-bound"] = dpop;
    auto mtls = device;
    mtls.bound_certificate = thumbprint(7);
    verifier.tokens["certificate-bound"] = mtls;
    auto narrow = device;
    narrow.scopes = {"lir:pub:sensors/{sub}/temperature"};
    narrow.expires = time.now() + 30min;
    verifier.tokens["narrow"] = narrow;
  }

  static std::array<uint8_t, 32> thumbprint(uint8_t fill) {
    std::array<uint8_t, 32> out{};
    out.fill(fill);
    return out;
  }
  static mqtt::Connect with_password(const std::string& token) {
    mqtt::Connect c;
    c.client_id = "c";
    c.password = mqtt::Bytes(token.begin(), token.end());
    return c;
  }
  static mqtt::Connect with_method(const std::string& method, const std::string& token) {
    mqtt::Connect c;
    c.client_id = "c";
    c.properties.authentication_method = method;
    c.properties.authentication_data = mqtt::Bytes(token.begin(), token.end());
    return c;
  }
  TokenAuthenticator make(TokenConfig config = {}) {
    return TokenAuthenticator(verifier, time, std::move(config),
                              dannan::status::ScopedLogger{std::make_shared<dannan::status::ConsoleSink>(dannan::status::LogLevel::error), "lir"});
  }

  dannan::test::SteppedTime time;
  TableVerifier verifier;
};

TEST_F(TokenAuthenticatorTest, ATokenAsThePasswordGrantsItsTopicScopes) {
  auto authenticator = make();
  const auto decision = authenticator.connect(with_password("good"), {});
  ASSERT_EQ(decision.kind, contract::AuthDecision::Kind::accept);
  ASSERT_TRUE(decision.grant);
  EXPECT_TRUE(decision.grant->may_publish("sensors/device-17/temperature", false));
  EXPECT_FALSE(decision.grant->may_publish("sensors/device-18/temperature", false));
  EXPECT_FALSE(decision.grant->may_publish("sensors/device-17/temperature", true));
  EXPECT_TRUE(decision.grant->may_subscribe("commands/device-17/#"));
  EXPECT_FALSE(decision.grant->may_subscribe("commands/#"));
}

TEST_F(TokenAuthenticatorTest, TheGrantEndsAtTheTokensExpiry) {
  auto authenticator = make();
  const auto decision = authenticator.connect(with_password("good"), {});
  ASSERT_TRUE(decision.grant);
  EXPECT_EQ(decision.grant->expires(), verifier.tokens["good"].expires);
}

TEST_F(TokenAuthenticatorTest, ATokenThatDoesNotVerifyIsABadPassword) {
  auto authenticator = make();
  EXPECT_EQ(authenticator.connect(with_password("forged"), {}).reason, reason::bad_user_name_or_password);
}

TEST_F(TokenAuthenticatorTest, AnExpiredTokenIsABadPassword) {
  auto authenticator = make();
  time.advance(16min);
  EXPECT_EQ(authenticator.connect(with_password("good"), {}).reason, reason::bad_user_name_or_password);
}

TEST_F(TokenAuthenticatorTest, ATokenForAnotherAudienceIsNotAuthorised) {
  auto authenticator = make();
  EXPECT_EQ(authenticator.connect(with_password("other-audience"), {}).reason, reason::not_authorized);
}

// A DPoP proof needs an HTTP request to sign; MQTT has none, so a token
// bound to a key can never be checked here, and a bound token is never a
// bearer token.
TEST_F(TokenAuthenticatorTest, ATokenBoundToAKeyIsRefused) {
  auto authenticator = make();
  EXPECT_EQ(authenticator.connect(with_password("dpop-bound"), {}).reason, reason::not_authorized);
}

TEST_F(TokenAuthenticatorTest, ACertificateBoundTokenNeedsThatCertificate) {
  auto authenticator = make();
  contract::Peer none;
  contract::Peer wrong{"", "CN=other", thumbprint(8)};
  contract::Peer right{"", "CN=device-17", thumbprint(7)};
  EXPECT_EQ(authenticator.connect(with_password("certificate-bound"), none).reason, reason::not_authorized);
  EXPECT_EQ(authenticator.connect(with_password("certificate-bound"), wrong).reason, reason::not_authorized);
  EXPECT_EQ(authenticator.connect(with_password("certificate-bound"), right).kind, contract::AuthDecision::Kind::accept);
}

TEST_F(TokenAuthenticatorTest, TheLughMethodCarriesTheTokenInItsData) {
  auto authenticator = make();
  const auto decision = authenticator.connect(with_method("lugh", "good"), {});
  ASSERT_EQ(decision.kind, contract::AuthDecision::Kind::accept);
  EXPECT_TRUE(decision.grant->may_publish("sensors/device-17/x", false));
  EXPECT_EQ(authenticator.connect(with_method("SCRAM-SHA-256", "good"), {}).reason, reason::bad_authentication_method);
}

TEST_F(TokenAuthenticatorTest, ReauthenticatingWithAFreshTokenReplacesTheGrant) {
  auto authenticator = make();
  const std::string token = "narrow";
  const auto decision = authenticator.auth("c", "lugh", mqtt::Bytes(token.begin(), token.end()), {});
  ASSERT_EQ(decision.kind, contract::AuthDecision::Kind::accept);
  EXPECT_TRUE(decision.grant->may_publish("sensors/device-17/temperature", false));
  EXPECT_FALSE(decision.grant->may_publish("sensors/device-17/humidity", false));
  EXPECT_EQ(decision.grant->expires(), verifier.tokens["narrow"].expires);
  EXPECT_EQ(authenticator.auth("c", "other", mqtt::Bytes(token.begin(), token.end()), {}).reason, reason::bad_authentication_method);
  const std::string forged = "forged";
  EXPECT_EQ(authenticator.auth("c", "lugh", mqtt::Bytes(forged.begin(), forged.end()), {}).reason, reason::bad_user_name_or_password);
}

TEST_F(TokenAuthenticatorTest, AUserNameWithoutATokenIsABadPassword) {
  auto authenticator = make();
  mqtt::Connect c;
  c.client_id = "c";
  c.username = "device-17";
  EXPECT_EQ(authenticator.connect(c, {}).reason, reason::bad_user_name_or_password);
}

TEST_F(TokenAuthenticatorTest, AnonymousClientsAreRefusedUnlessAllowed) {
  mqtt::Connect bare;
  bare.client_id = "c";
  auto refusing = make();
  EXPECT_EQ(refusing.connect(bare, {}).reason, reason::not_authorized);
  TokenConfig config;
  config.anonymous = TopicScopes{{"public/#"}, {"public/#"}, {}};
  auto allowing = make(config);
  const auto decision = allowing.connect(bare, {});
  ASSERT_EQ(decision.kind, contract::AuthDecision::Kind::accept);
  EXPECT_TRUE(decision.grant->may_publish("public/x", false));
  EXPECT_FALSE(decision.grant->may_publish("private/x", false));
}

}  // namespace
}  // namespace lir::auth
