//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "lir/rules/rules.h"

#include <gtest/gtest.h>

namespace lir::rules {
namespace {

using broker::Message;
using mqtt::QoS;

Message msg(std::string topic, std::string payload, QoS qos = QoS::at_most_once) {
  Message m;
  m.topic = std::move(topic);
  m.payload = broker::Bytes(payload.begin(), payload.end());
  m.qos = qos;
  m.origin = "sensor-1";
  return m;
}

std::string text(const Message& m) { return {m.payload.begin(), m.payload.end()}; }

const dannan::contract::TimePoint t0{};

TEST(Rules, InvalidRulesAreRefusedWithAReason) {
  RuleSet rules;
  const std::vector<std::pair<std::string, std::string>> bad{
      {"not json", "unclosed {"},
      {"not an object", "[1]"},
      {"no from", R"({"do": {"drop": true}})"},
      {"bad filter", R"({"from": "a/#/b", "do": {"drop": true}})"},
      {"shared", R"({"from": "$share/g/a", "do": {"drop": true}})"},
      {"no do", R"({"from": "a"})"},
      {"empty do", R"({"from": "a", "do": []})"},
      {"unknown action", R"({"from": "a", "do": {"explode": true}})"},
      {"unknown key", R"({"from": "a", "do": {"drop": true}, "when": 1})"},
      {"unknown path", R"({"from": "a", "where": {"size": {">": 1}}, "do": {"drop": true}})"},
      {"unknown operator", R"({"from": "a", "where": {"qos": {"~": 1}}, "do": {"drop": true}})"},
      {"in needs array", R"({"from": "a", "where": {"qos": {"in": 1}}, "do": {"drop": true}})"},
      {"bad placeholder", R"({"from": "a", "do": {"republish": {"topic": "x/{nope}"}}})"},
      {"bad qos", R"({"from": "a", "do": {"republish": {"topic": "x", "qos": 3}}})"},
      {"payload and transform", R"({"from": "a", "do": {"republish": {"topic": "x", "payload": 1, "transform": {}}}})"},
      {"bad expiry", R"({"from": "a", "do": {"set": {"expiry": 0}}})"},
  };
  for (const auto& [why, json] : bad) EXPECT_TRUE(rules.set("r", json)) << why;
  EXPECT_TRUE(rules.empty());
}

TEST(Rules, ARefusedRuleReplacesTheOldOne) {
  RuleSet rules;
  ASSERT_FALSE(rules.set("r", R"({"from": "a", "do": {"drop": true}})"));
  EXPECT_EQ(rules.size(), 1u);
  EXPECT_TRUE(rules.set("r", "nonsense"));
  EXPECT_TRUE(rules.empty());
}

TEST(Rules, DropOnlyWhatMatches) {
  RuleSet rules;
  ASSERT_FALSE(rules.set("junk", R"({"from": "junk/#", "do": {"drop": true}})"));
  auto junk = msg("junk/a", "x");
  EXPECT_TRUE(rules.apply(junk, t0).drop);
  auto fine = msg("fine/a", "x");
  EXPECT_FALSE(rules.apply(fine, t0).drop);
  // MQTT's '$' rule holds for rule filters too.
  ASSERT_FALSE(rules.set("all", R"({"from": "#", "do": {"drop": true}})"));
  auto system = msg("$lir/nodes/x", "x");
  EXPECT_FALSE(rules.apply(system, t0).drop);
}

TEST(Rules, ConditionsOnPayloadFields) {
  RuleSet rules;
  ASSERT_FALSE(rules.set("hot", R"({"from": "t/+", "where": {"payload.value": {">": 30}}, "do": {"drop": true}})"));
  auto hot = msg("t/1", R"({"value": 31.5})");
  EXPECT_TRUE(rules.apply(hot, t0).drop);
  auto cold = msg("t/1", R"({"value": 12})");
  EXPECT_FALSE(rules.apply(cold, t0).drop);
  auto text_payload = msg("t/1", "31.5 degrees");
  EXPECT_FALSE(rules.apply(text_payload, t0).drop);
  auto missing = msg("t/1", R"({"other": 99})");
  EXPECT_FALSE(rules.apply(missing, t0).drop);
}

TEST(Rules, NestedPathsArraysAndExists) {
  RuleSet rules;
  ASSERT_FALSE(rules.set("r", R"({"from": "t", "where": {"payload.readings.1.ok": {"=": true}, "payload.note": {"exists": false}}, "do": {"drop": true}})"));
  auto yes = msg("t", R"({"readings": [{"ok": false}, {"ok": true}]})");
  EXPECT_TRUE(rules.apply(yes, t0).drop);
  auto noted = msg("t", R"({"readings": [{"ok": false}, {"ok": true}], "note": "x"})");
  EXPECT_FALSE(rules.apply(noted, t0).drop);
  auto short_array = msg("t", R"({"readings": [{"ok": true}]})");
  EXPECT_FALSE(rules.apply(short_array, t0).drop);
}

TEST(Rules, ConditionsOnPropertiesAndThePublisher) {
  RuleSet rules;
  ASSERT_FALSE(rules.set("r", R"({"from": "t", "where": {
      "any": [{"qos": {"=": 1}}, {"user.site": {"in": ["north", "south"]}}],
      "not": {"client": {"=": "trusted"}},
      "retain": {"=": false},
      "content_type": {"!=": "text/plain"}}, "do": {"drop": true}})"));
  auto qos1 = msg("t", "x", QoS::at_least_once);
  EXPECT_TRUE(rules.apply(qos1, t0).drop);
  auto north = msg("t", "x");
  north.user_properties = {{"site", "north"}};
  EXPECT_TRUE(rules.apply(north, t0).drop);
  auto neither = msg("t", "x");
  EXPECT_FALSE(rules.apply(neither, t0).drop);
  auto trusted = msg("t", "x", QoS::at_least_once);
  trusted.origin = "trusted";
  EXPECT_FALSE(rules.apply(trusted, t0).drop);
  auto plain = msg("t", "x", QoS::at_least_once);
  plain.content_type = "text/plain";
  EXPECT_FALSE(rules.apply(plain, t0).drop);
}

TEST(Rules, RepublishFillsTemplatesFromTheWildcards) {
  RuleSet rules;
  ASSERT_FALSE(rules.set("alert", R"({"from": "sensors/+/temp/#", "do": {"republish": {"topic": "alerts/{1}/{2}", "qos": 1,
      "payload": "{client} says {payload.value} on {topic}"}}})"));
  auto m = msg("sensors/kitchen/temp/a/b", R"({"value": 40})");
  const auto out = rules.apply(m, t0);
  ASSERT_EQ(out.republished.size(), 1u);
  const auto& r = out.republished[0];
  EXPECT_EQ(r.topic, "alerts/kitchen/a/b");
  EXPECT_EQ(r.qos, QoS::at_least_once);
  EXPECT_FALSE(r.retain);
  EXPECT_EQ(text(r), "sensor-1 says 40 on sensors/kitchen/temp/a/b");
  EXPECT_EQ(r.hops, 1);
  // The original goes on unchanged.
  EXPECT_FALSE(out.drop);
}

TEST(Rules, RepublishWithoutPayloadCarriesTheOriginal) {
  RuleSet rules;
  ASSERT_FALSE(rules.set("copy", R"({"from": "a", "do": {"republish": {"topic": "b", "retain": true}}})"));
  auto m = msg("a", "raw bytes", QoS::exactly_once);
  m.content_type = "application/octet-stream";
  const auto out = rules.apply(m, t0);
  ASSERT_EQ(out.republished.size(), 1u);
  EXPECT_EQ(text(out.republished[0]), "raw bytes");
  EXPECT_EQ(out.republished[0].qos, QoS::exactly_once);
  EXPECT_TRUE(out.republished[0].retain);
  EXPECT_EQ(out.republished[0].content_type, "application/octet-stream");
}

TEST(Rules, ATemplatedTopicThatIsNotANameIsSkipped) {
  RuleSet rules;
  // {1} may be empty or hold a wildcard character only if the topic did, which a
  // name cannot; an empty level stays valid, so build an invalid one from {payload}.
  ASSERT_FALSE(rules.set("r", R"({"from": "a", "do": {"republish": {"topic": "{payload.t}"}}})"));
  auto m = msg("a", R"({"t": "x/+/y"})");
  const auto out = rules.apply(m, t0);
  EXPECT_TRUE(out.republished.empty());
  EXPECT_EQ(out.skipped, 1u);
}

TEST(Rules, TransformPicksRenamesAndAdds) {
  RuleSet rules;
  ASSERT_FALSE(rules.set("shape", R"({"from": "t/+", "do": {"republish": {"topic": "out/{1}",
      "transform": {"pick": ["value", "unit"], "rename": {"value": "celsius"}, "add": {"room": "{1}", "source": {"client": "{client}"}, "v": 2}}}}})"));
  auto m = msg("t/hall", R"({"value": 21, "unit": "C", "secret": "s"})");
  const auto out = rules.apply(m, t0);
  ASSERT_EQ(out.republished.size(), 1u);
  EXPECT_EQ(text(out.republished[0]), R"({"unit":"C","celsius":21,"room":"hall","source":{"client":"sensor-1"},"v":2})");
  EXPECT_EQ(out.republished[0].content_type, "application/json");
  // Not a JSON object: nothing to transform.
  auto raw = msg("t/hall", "21");
  const auto skipped = rules.apply(raw, t0);
  EXPECT_TRUE(skipped.republished.empty());
  EXPECT_EQ(skipped.skipped, 1u);
}

TEST(Rules, SetChangesTheMessageForLaterRulesAndDelivery) {
  RuleSet rules;
  ASSERT_FALSE(rules.set("a-tag", R"({"from": "t", "do": {"set": {"user": {"seen": "{client}"}, "content_type": "application/json", "expiry": 60}}})"));
  ASSERT_FALSE(rules.set("b-check", R"({"from": "t", "where": {"user.seen": {"=": "sensor-1"}}, "do": {"drop": true}})"));
  auto m = msg("t", "{}");
  const auto out = rules.apply(m, t0);
  EXPECT_TRUE(out.drop);
  ASSERT_EQ(m.user_properties.size(), 1u);
  EXPECT_EQ(m.user_properties[0], (std::pair<std::string, std::string>{"seen", "sensor-1"}));
  EXPECT_EQ(m.content_type, "application/json");
  EXPECT_EQ(m.expires_at, t0 + std::chrono::seconds(60));
}

TEST(Rules, RulesApplyInNameOrder) {
  RuleSet rules;
  ASSERT_FALSE(rules.set("2-second", R"({"from": "t", "do": {"set": {"user": {"order": "2"}}}})"));
  ASSERT_FALSE(rules.set("1-first", R"({"from": "t", "do": {"set": {"user": {"order": "1"}}}})"));
  auto m = msg("t", "");
  rules.apply(m, t0);
  ASSERT_EQ(m.user_properties.size(), 2u);
  EXPECT_EQ(m.user_properties[0].second, "1");
  EXPECT_EQ(m.user_properties[1].second, "2");
}

}  // namespace
}  // namespace lir::rules
