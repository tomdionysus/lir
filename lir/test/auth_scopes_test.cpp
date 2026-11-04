//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <gtest/gtest.h>

#include <stdexcept>

#include "lir/auth/scopes.h"

namespace lir::auth {
namespace {

using Strings = std::vector<std::string>;

TEST(AuthScopes, PercentDecodesOnlyEscapes) {
  EXPECT_EQ(percent_decode("a%20b+c%2b"), "a b+c+");
  EXPECT_EQ(percent_decode("%7Bx%7d"), "{x}");
  EXPECT_THROW(percent_decode("a%2"), std::invalid_argument);
  EXPECT_THROW(percent_decode("a%zz"), std::invalid_argument);
}

TEST(AuthScopes, ParsesEachAction) {
  const Strings in{"lir:pub:a/b", "lir:sub:a/%23", "lir:retain:status/+", "lir:sub:$lir/%23"};
  const auto parsed = parse_scopes(in);
  EXPECT_TRUE(parsed.refused.empty());
  EXPECT_EQ(parsed.scopes.publish, (Strings{"a/b"}));
  EXPECT_EQ(parsed.scopes.subscribe, (Strings{"a/#", "$lir/#"}));
  EXPECT_EQ(parsed.scopes.retain, (Strings{"status/+"}));
}

TEST(AuthScopes, WildcardsMayAlsoBeLiteral) {
  // The proposed grammar leaves '+', '#' and '$' unencoded.
  const auto parsed = parse_scopes(Strings{"lir:sub:a/#", "lir:pub:+/x"});
  EXPECT_EQ(parsed.scopes.subscribe, (Strings{"a/#"}));
  EXPECT_EQ(parsed.scopes.publish, (Strings{"+/x"}));
}

TEST(AuthScopes, DecodesSpacesAndNonAscii) {
  const auto parsed = parse_scopes(Strings{"lir:pub:room%201/t%C3%A9mp"});
  EXPECT_EQ(parsed.scopes.publish, (Strings{"room 1/t\xC3\xA9mp"}));
}

TEST(AuthScopes, OtherApplicationsAreSkippedSilently) {
  const auto parsed = parse_scopes(Strings{"openid", "macha:read", "lir:pub:a"});
  EXPECT_TRUE(parsed.refused.empty());
  EXPECT_EQ(parsed.scopes.publish, (Strings{"a"}));
}

TEST(AuthScopes, MalformedLirScopesAreRefused) {
  const Strings bad{"lir:pub",     "lir:read:a",    "lir:pub:",           "lir:sub:a/#/b",  "lir:sub:a+",
                    "lir:pub:a%2", "lir:pub:a%00b", "lir:sub:$share/g/a", "lir:pub:x{sub}", "lir:pub:{client}/a"};
  const auto parsed = parse_scopes(bad);
  EXPECT_TRUE(parsed.scopes.empty());
  EXPECT_EQ(parsed.refused, bad);
}

TEST(AuthScopes, SubjectFillsAWholeLevel) {
  const auto parsed = parse_scopes(Strings{"lir:pub:devices/{sub}/#", "lir:sub:{sub}"}, "d-17");
  EXPECT_EQ(parsed.scopes.publish, (Strings{"devices/d-17/#"}));
  EXPECT_EQ(parsed.scopes.subscribe, (Strings{"d-17"}));
}

TEST(AuthScopes, SubjectThatIsNotOneLiteralLevelMatchesNothing) {
  for (const std::string subject : {"a/b", "a+", "#", ""}) {
    const auto parsed = parse_scopes(Strings{"lir:pub:devices/{sub}"}, subject);
    EXPECT_TRUE(parsed.scopes.empty()) << subject;
    EXPECT_EQ(parsed.refused.size(), 1u) << subject;
  }
}

TEST(AuthScopes, AnEncodedBraceIsLiteral) {
  const auto parsed = parse_scopes(Strings{"lir:pub:%7Bsub%7D"}, "d");
  EXPECT_EQ(parsed.scopes.publish, (Strings{"{sub}"}));
}

// Strings Lugh's encoder produced (Lugh develop 2c63171), decoded as Lugh
// decodes them.
TEST(AuthScopes, LughsEncoderRoundTrips) {
  const auto parsed = parse_scopes(
      Strings{"lir:retain:home/living%20room/%7Bsub%7D/{sub}/caf%C3%A9%20100%25/+/#", "lir:sub:$lir/#", "lir:pub:/a/", "lir:pub:devices/{sub}/#"}, "d-1");
  EXPECT_TRUE(parsed.refused.empty());
  EXPECT_EQ(parsed.scopes.retain, (Strings{"home/living room/{sub}/d-1/caf\xC3\xA9 100%/+/#"}));
  EXPECT_EQ(parsed.scopes.subscribe, (Strings{"$lir/#"}));
  EXPECT_EQ(parsed.scopes.publish, (Strings{"/a/", "devices/d-1/#"}));
}

TEST(AuthScopes, WhatLughRefusesLirRefusesToo) {
  const Strings bad{"lir:sub:a%2Bb", "lir:pub:a%2Fb", "lir:pub:a%00b", "lir:pub:100%", "lir:pub:%zz", "lir:pub:devices/{client}/#"};
  const auto parsed = parse_scopes(bad, "d-1");
  EXPECT_TRUE(parsed.scopes.empty());
  EXPECT_EQ(parsed.refused, bad);
}

TEST(AuthScopes, ASubjectIsNeverDecoded) {
  const auto parsed = parse_scopes(Strings{"lir:pub:d/{sub}"}, "a%41");
  EXPECT_EQ(parsed.scopes.publish, (Strings{"d/a%41"}));
}

TEST(AuthScopes, PublishAndRetainAreSeparate) {
  const ScopeGrant grant({{"a/#"}, {}, {"status/+"}});
  EXPECT_TRUE(grant.may_publish("a/b", false));
  EXPECT_FALSE(grant.may_publish("a/b", true));
  EXPECT_TRUE(grant.may_publish("status/x", true));
  EXPECT_FALSE(grant.may_publish("status/x", false));
  EXPECT_FALSE(grant.may_publish("b", false));
}

TEST(AuthScopes, PublishKeepsTheDollarRule) {
  const ScopeGrant grant({{"#"}, {}, {}});
  EXPECT_TRUE(grant.may_publish("a", false));
  EXPECT_FALSE(grant.may_publish("$macha/x", false));
}

TEST(AuthScopes, SubscribeNeedsCoverage) {
  const ScopeGrant grant({{}, {"sensors/+/temp", "$lir/#"}, {}});
  EXPECT_TRUE(grant.may_subscribe("sensors/1/temp"));
  EXPECT_TRUE(grant.may_subscribe("sensors/+/temp"));
  EXPECT_FALSE(grant.may_subscribe("sensors/#"));
  EXPECT_FALSE(grant.may_subscribe("#"));
  EXPECT_TRUE(grant.may_subscribe("$lir/nodes/+"));
  EXPECT_FALSE(grant.may_subscribe("$dannan/#"));
}

TEST(AuthScopes, SharedSubscriptionsAreCheckedByTheirFilter) {
  const ScopeGrant grant({{}, {"jobs/#"}, {}});
  EXPECT_TRUE(grant.may_subscribe("$share/workers/jobs/+"));
  EXPECT_FALSE(grant.may_subscribe("$share/workers/other"));
  EXPECT_FALSE(grant.may_subscribe("$share/workers"));
}

TEST(AuthScopes, AnonymousAuthenticatorRefusesByDefault) {
  AnonymousAuthenticator refuse(std::nullopt);
  mqtt::Connect c;
  EXPECT_EQ(refuse.connect(c, {}).kind, contract::AuthDecision::Kind::refuse);
  EXPECT_EQ(refuse.connect(c, {}).reason, mqtt::reason::not_authorized);
}

TEST(AuthScopes, AnonymousAuthenticatorHoldsAllowedClientsToTheirScopes) {
  AnonymousAuthenticator allow(TopicScopes{{"open/#"}, {"open/#"}, {}});
  mqtt::Connect c;
  const auto decision = allow.connect(c, {});
  ASSERT_EQ(decision.kind, contract::AuthDecision::Kind::accept);
  ASSERT_TRUE(decision.grant);
  EXPECT_TRUE(decision.grant->may_publish("open/a", false));
  EXPECT_FALSE(decision.grant->may_publish("closed/a", false));
  EXPECT_FALSE(decision.grant->may_publish("open/a", true));
  EXPECT_FALSE(decision.grant->expires());
}

TEST(AuthScopes, AnonymousAuthenticatorRefusesCredentials) {
  AnonymousAuthenticator allow(default_anonymous_scopes());
  mqtt::Connect named;
  named.username = "someone";
  EXPECT_EQ(allow.connect(named, {}).reason, mqtt::reason::bad_user_name_or_password);
  mqtt::Connect keyed;
  keyed.password = mqtt::Bytes{'k'};
  EXPECT_EQ(allow.connect(keyed, {}).reason, mqtt::reason::bad_user_name_or_password);
  mqtt::Connect method;
  method.properties.authentication_method = "lugh";
  EXPECT_EQ(allow.connect(method, {}).reason, mqtt::reason::bad_authentication_method);
  EXPECT_EQ(allow.auth("c", "lugh", {}).reason, mqtt::reason::bad_authentication_method);
}

TEST(AuthScopes, DefaultAnonymousScopesReadTheSystemTopics) {
  const ScopeGrant grant(default_anonymous_scopes());
  EXPECT_TRUE(grant.may_subscribe("#"));
  EXPECT_TRUE(grant.may_subscribe("$lir/nodes/+"));
  EXPECT_TRUE(grant.may_subscribe("$dannan/#"));
  EXPECT_FALSE(grant.may_subscribe("$macha/#"));
  EXPECT_TRUE(grant.may_publish("a", true));
}

}  // namespace
}  // namespace lir::auth
