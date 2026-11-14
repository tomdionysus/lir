//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <gtest/gtest.h>

#include <array>
#include <string>

#include "dannan/identity/signing.h"
#include "dannan/status/log.h"
#include "dannan/test/fixtures/stepped_time.h"
#include "lir/auth/lugh.h"
#include "lugh/client/jose.h"
#include "lugh/core/encoding.h"

// The Lugh plugin (design, section 1; the maintainer 2026-11-14, P10): Lir's
// TokenVerifier over lugh_client's Verifier. The tokens here are real,
// signed by Lugh's own code under a key the verifier is given, so what is
// tested is the adapter's reading of what Lugh says, not a stand-in.
namespace lir::auth {
namespace {

using namespace std::chrono_literals;
namespace reason = mqtt::reason;

constexpr const char* kIssuer = "https://lugh.example";

class LughVerifierTest : public ::testing::Test {
 protected:
  LughVerifierTest() : key(dannan::identity::SigningKey::generate(dannan::identity::SigningAlgorithm::ed25519)), verifier({kIssuer}, wall), adapter(verifier) {
    lugh::client::VerifyKey public_key{"k1", key.algorithm(), key.public_key()};
    boost::json::array keys;
    keys.push_back(*lugh::client::public_jwk(public_key));
    boost::json::object jwks;
    jwks["keys"] = std::move(keys);
    verifier.set_keys(kIssuer, jwks);
  }

  boost::json::object claims() const {
    boost::json::object c;
    c["iss"] = kIssuer;
    c["sub"] = "device-17";
    c["client_id"] = "fleet";
    c["jti"] = "j1";
    c["aud"] = boost::json::array{"lugh", "lir"};
    c["scope"] = "openid lir:pub:sensors/{sub}/%23";
    c["exp"] = wall.unix_ms() / 1000 + 900;
    return c;
  }
  std::string sign(const boost::json::object& c) const { return lugh::client::jws_sign(key, "k1", "at+jwt", c); }
  static std::string thumbprint(uint8_t fill, size_t size) {
    std::vector<unsigned char> bytes(size, fill);
    return lugh::encoding::base64url(bytes);
  }

  dannan::test::SteppedTime time;
  dannan::test::SteppedWall wall;
  dannan::identity::SigningKey key;
  lugh::client::Verifier verifier;
  LughVerifier adapter;
};

TEST_F(LughVerifierTest, AGenuineTokenGivesWhatLirReads) {
  const auto verified = adapter.verify(sign(claims()));
  ASSERT_TRUE(std::holds_alternative<TokenClaims>(verified)) << std::get<std::string>(verified);
  const auto& c = std::get<TokenClaims>(verified);
  EXPECT_EQ(c.subject, "device-17");
  EXPECT_EQ(c.audiences, (std::vector<std::string>{"lugh", "lir"}));
  EXPECT_EQ(c.scopes, (std::vector<std::string>{"openid", "lir:pub:sensors/{sub}/%23"}));
  EXPECT_EQ(c.expires_unix, wall.unix_ms() / 1000 + 900);
  EXPECT_FALSE(c.bound_to_key);
  EXPECT_FALSE(c.bound_certificate);
}

TEST_F(LughVerifierTest, ACertificateBoundTokenCarriesItsThumbprint) {
  auto c = claims();
  c["cnf"] = boost::json::object{{"x5t#S256", thumbprint(7, 32)}};
  const auto verified = adapter.verify(sign(c));
  ASSERT_TRUE(std::holds_alternative<TokenClaims>(verified)) << std::get<std::string>(verified);
  std::array<uint8_t, 32> sevens{};
  sevens.fill(7);
  EXPECT_EQ(std::get<TokenClaims>(verified).bound_certificate, sevens);
}

// RFC 8705: x5t#S256 is a SHA-256 thumbprint. Anything else could never
// match a certificate, so it is refused rather than read as unbound.
TEST_F(LughVerifierTest, AThumbprintThatIsNotSHA256IsRefused) {
  for (const auto& bad : {thumbprint(7, 20), std::string("not base64url!")}) {
    auto c = claims();
    c["cnf"] = boost::json::object{{"x5t#S256", bad}};
    EXPECT_TRUE(std::holds_alternative<std::string>(adapter.verify(sign(c)))) << bad;
  }
}

TEST_F(LughVerifierTest, AKeyBoundTokenIsMarkedBound) {
  auto c = claims();
  c["cnf"] = boost::json::object{{"jkt", "0ZcOCORZNYy-DWpqq30jZyJGHTN0d2HglBV3uiguA4I"}};
  const auto verified = adapter.verify(sign(c));
  ASSERT_TRUE(std::holds_alternative<TokenClaims>(verified)) << std::get<std::string>(verified);
  EXPECT_TRUE(std::get<TokenClaims>(verified).bound_to_key);
}

TEST_F(LughVerifierTest, AForgedTokenSaysWhy) {
  const auto other = dannan::identity::SigningKey::generate(dannan::identity::SigningAlgorithm::ed25519);
  const auto forged = lugh::client::jws_sign(other, "k1", "at+jwt", claims());
  const auto verified = adapter.verify(forged);
  ASSERT_TRUE(std::holds_alternative<std::string>(verified));
  EXPECT_FALSE(std::get<std::string>(verified).empty());
}

// The whole path a client takes: its token as the CONNECT password, verified
// by Lugh, read by the authenticator into its topic scopes.
TEST_F(LughVerifierTest, AClientSignsInWithALughToken) {
  TokenAuthenticator authenticator(adapter, time, wall, {},
                                   dannan::status::ScopedLogger{std::make_shared<dannan::status::ConsoleSink>(dannan::status::LogLevel::error), "lir"});
  const auto token = sign(claims());
  mqtt::Connect connect;
  connect.client_id = "c";
  connect.password = mqtt::Bytes(token.begin(), token.end());
  const auto decision = authenticator.connect(connect, {});
  ASSERT_EQ(decision.kind, contract::AuthDecision::Kind::accept);
  EXPECT_TRUE(decision.grant->may_publish("sensors/device-17/temperature", false));
  EXPECT_FALSE(decision.grant->may_publish("sensors/device-18/temperature", false));
  EXPECT_EQ(decision.grant->expires(), time.now() + 15min);
  connect.password = mqtt::Bytes{'x'};
  EXPECT_EQ(authenticator.connect(connect, {}).reason, reason::bad_user_name_or_password);
}

}  // namespace
}  // namespace lir::auth
