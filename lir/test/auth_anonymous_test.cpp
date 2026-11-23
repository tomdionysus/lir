//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <gtest/gtest.h>

#include <stdexcept>

#include "lir/auth/anonymous.h"

// Anonymous clients (the design's section 1): refused by default, or held to
// the topic filters a node allows them. One connection's decision, made from
// memory, so these need no cluster.
namespace lir::auth {
namespace {

namespace reason = mqtt::reason;
using Kind = contract::AuthDecision::Kind;

Filters sensors() {
  Filters out;
  out.publish = {"sensors/+/temperature"};
  out.retain = {"status/#"};
  out.subscribe = {"sensors/#", "status/+"};
  return out;
}

TEST(AuthAnonymous, EveryClientIsRefusedByDefault) {
  Anonymous anonymous(std::nullopt);
  const auto decision = anonymous.connect({}, {});
  EXPECT_EQ(decision.kind, Kind::refuse);
  EXPECT_EQ(decision.reason, reason::not_authorized);
}

TEST(AuthAnonymous, AnAllowedClientIsHeldToTheFilters) {
  Anonymous anonymous(sensors());
  const auto decision = anonymous.connect({}, {});
  ASSERT_EQ(decision.kind, Kind::accept);
  ASSERT_TRUE(decision.grant);
  EXPECT_TRUE(decision.grant->may_publish("sensors/kitchen/temperature", false));
  EXPECT_FALSE(decision.grant->may_publish("sensors/kitchen/humidity", false));
  EXPECT_TRUE(decision.grant->may_subscribe("sensors/kitchen/#"));
  EXPECT_FALSE(decision.grant->may_subscribe("#"));
}

// A credential is not anonymous, and nothing here can check one.
TEST(AuthAnonymous, AClientWithACredentialIsRefused) {
  Anonymous anonymous(sensors());
  mqtt::Connect named;
  named.username = "someone";
  EXPECT_EQ(anonymous.connect(named, {}).reason, reason::bad_user_name_or_password);
  mqtt::Connect password;
  password.password = mqtt::Bytes{'x'};
  EXPECT_EQ(anonymous.connect(password, {}).reason, reason::bad_user_name_or_password);
  mqtt::Connect method;
  method.properties.authentication_method = "lugh";
  EXPECT_EQ(anonymous.connect(method, {}).reason, reason::bad_authentication_method);
  EXPECT_EQ(anonymous.auth("c", "lugh", {}, {}).reason, reason::bad_authentication_method);
}

// `retain` is a PUBLISH with the retain flag, and does not imply `publish`.
TEST(AuthAnonymous, RetainAndPublishAreGrantedApart) {
  const FilterGrant grant(sensors());
  EXPECT_TRUE(grant.may_publish("status/door", true));
  EXPECT_FALSE(grant.may_publish("status/door", false));
  EXPECT_FALSE(grant.may_publish("sensors/kitchen/temperature", true));
}

// A shared subscription is checked by the filter inside it.
TEST(AuthAnonymous, ASharedFilterIsCheckedByItsFilter) {
  const FilterGrant grant(sensors());
  EXPECT_TRUE(grant.may_subscribe("$share/workers/sensors/+/temperature"));
  EXPECT_FALSE(grant.may_subscribe("$share/workers/other/#"));
  EXPECT_FALSE(grant.may_subscribe("$share//sensors/#"));
}

// '#' and '+' at the first level do not reach '$' topics [MQTT-4.7.2-1]: a
// grant of "#" is no grant of the system topics.
TEST(AuthAnonymous, AWildcardGrantDoesNotReachTheSystemTopics) {
  Filters everything;
  everything.publish = {"#"};
  everything.subscribe = {"#"};
  const FilterGrant grant(everything);
  EXPECT_TRUE(grant.may_subscribe("a/b"));
  EXPECT_FALSE(grant.may_subscribe("$lir/cluster"));
  EXPECT_FALSE(grant.may_publish("$lir/cluster", false));
}

TEST(AuthAnonymous, AnInvalidTopicIsNeverGranted) {
  const FilterGrant grant(sensors());
  EXPECT_FALSE(grant.may_publish("sensors/+/temperature", false));
  EXPECT_FALSE(grant.may_subscribe("sensors/#/x"));
}

TEST(AuthAnonymous, AnInvalidOrSharedFilterIsRefusedWhenConfigured) {
  Filters bad;
  bad.subscribe = {"a/#/b"};
  EXPECT_THROW(FilterGrant{bad}, std::invalid_argument);
  Filters shared;
  shared.publish = {"$share/g/a"};
  EXPECT_THROW(Anonymous{shared}, std::invalid_argument);
}

}  // namespace
}  // namespace lir::auth
