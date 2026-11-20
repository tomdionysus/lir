//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <gtest/gtest.h>

#include <string>

#include "lir/mqtt/topic.h"

// MQTT 5.0 sections 4.7 and 4.8.2, 3.1.1 section 4.7, statement by statement.
// 3.1.1's statements are the same rules under other numbers, so each test
// names both.
namespace lir::mqtt {
namespace {

TEST(MqttTopic, MQTT_4_7_0_1_and_MQTT311_4_7_1_1_NoWildcardInATopicName) {
  for (const char* name : {"sport/+", "sport/#", "+", "#", "a+b", "a#b", "sport/tennis+"}) EXPECT_FALSE(valid_topic_name(name)) << name;
  for (const char* name : {"sport", "sport/tennis/player1", "/", "a//b", "$SYS/x"}) EXPECT_TRUE(valid_topic_name(name)) << name;
}

TEST(MqttTopic, MQTT_4_7_1_1_and_MQTT311_4_7_1_2_MultiLevelWildcardAloneAndLast) {
  for (const char* filter : {"#", "sport/#", "sport/tennis/#", "/#", "+/#"}) EXPECT_TRUE(valid_topic_filter(filter)) << filter;
  for (const char* filter : {"sport/tennis#", "sport/tennis/#/ranking", "#/a", "##", "a/#/", "#a"}) EXPECT_FALSE(valid_topic_filter(filter)) << filter;
}

TEST(MqttTopic, MQTT_4_7_1_1_and_MQTT311_4_7_1_2_MultiLevelWildcardMatchesParentAndChildren) {
  EXPECT_TRUE(matches("sport/tennis/player1/#", "sport/tennis/player1"));
  EXPECT_TRUE(matches("sport/tennis/player1/#", "sport/tennis/player1/ranking"));
  EXPECT_TRUE(matches("sport/tennis/player1/#", "sport/tennis/player1/score/wimbledon"));
  EXPECT_TRUE(matches("sport/#", "sport"));
  EXPECT_TRUE(matches("#", "sport/tennis"));
  EXPECT_TRUE(matches("#", "/"));
  EXPECT_FALSE(matches("sport/tennis/player1/#", "sport/tennis/player2"));
  EXPECT_FALSE(matches("sport/#", "sports"));
}

TEST(MqttTopic, MQTT_4_7_1_2_and_MQTT311_4_7_1_3_SingleLevelWildcardOccupiesALevel) {
  for (const char* filter : {"+", "+/tennis/#", "sport/+/player1", "/+", "+/+", "sport/+"}) EXPECT_TRUE(valid_topic_filter(filter)) << filter;
  for (const char* filter : {"sport+", "+sport", "sport/+tennis", "a/b+/c", "++"}) EXPECT_FALSE(valid_topic_filter(filter)) << filter;
}

TEST(MqttTopic, MQTT_4_7_1_2_and_MQTT311_4_7_1_3_SingleLevelWildcardMatchesOneLevel) {
  EXPECT_TRUE(matches("sport/tennis/+", "sport/tennis/player1"));
  EXPECT_TRUE(matches("sport/tennis/+", "sport/tennis/player2"));
  EXPECT_FALSE(matches("sport/tennis/+", "sport/tennis/player1/ranking"));
  EXPECT_FALSE(matches("sport/+", "sport"));
  EXPECT_TRUE(matches("sport/+", "sport/"));
  EXPECT_TRUE(matches("+/+", "/finance"));
  EXPECT_TRUE(matches("/+", "/finance"));
  EXPECT_FALSE(matches("+", "/finance"));
  EXPECT_TRUE(matches("+", ""));
}

TEST(MqttTopic, MQTT_4_7_2_1_and_MQTT311_4_7_2_1_WildcardFirstNeverMatchesDollarTopics) {
  EXPECT_FALSE(matches("#", "$SYS/broker/clients"));
  EXPECT_FALSE(matches("#", "$SYS"));
  EXPECT_FALSE(matches("+/monitor/Clients", "$SYS/monitor/Clients"));
  EXPECT_FALSE(matches("+", "$lir"));
  EXPECT_TRUE(matches("$SYS/#", "$SYS/broker/clients"));
  EXPECT_TRUE(matches("$SYS/monitor/+", "$SYS/monitor/Clients"));
  // Only the first level is special.
  EXPECT_TRUE(matches("a/+", "a/$b"));
  EXPECT_TRUE(matches("a/#", "a/$b/c"));
}

TEST(MqttTopic, MQTT_4_7_3_1_and_MQTT311_4_7_3_1_AtLeastOneCharacter) {
  EXPECT_FALSE(valid_topic_name(""));
  EXPECT_FALSE(valid_topic_filter(""));
}

TEST(MqttTopic, MQTT_4_7_3_2_and_MQTT311_4_7_3_2_NoNullCharacter) {
  const std::string name("a\0b", 3);
  EXPECT_FALSE(valid_topic_name(name));
  EXPECT_FALSE(valid_topic_filter(name));
  EXPECT_FALSE(valid_topic_filter(std::string("a/\0/#", 5)));
}

TEST(MqttTopic, MQTT_4_7_3_3_and_MQTT311_4_7_3_3_AtMost65535Bytes) {
  EXPECT_TRUE(valid_topic_name(std::string(65535, 'a')));
  EXPECT_FALSE(valid_topic_name(std::string(65536, 'a')));
  EXPECT_TRUE(valid_topic_filter(std::string(65535, 'a')));
  EXPECT_FALSE(valid_topic_filter(std::string(65534, 'a') + "/#"));
}

TEST(MqttTopic, MQTT_4_7_3_4_and_MQTT311_4_7_3_4_NoNormalisation) {
  EXPECT_FALSE(matches("ACCOUNTS", "Accounts"));
  EXPECT_FALSE(matches("a/", "a"));
  EXPECT_FALSE(matches("a", "a/"));
  EXPECT_FALSE(matches("a//b", "a/b"));
  EXPECT_FALSE(matches(" a", "a"));
  // A precomposed e-acute and an e with a combining acute are different.
  EXPECT_FALSE(matches("caf\xC3\xA9", "cafe\xCC\x81"));
  EXPECT_TRUE(matches("caf\xC3\xA9", "caf\xC3\xA9"));
}

TEST(MqttTopic, MQTT_4_8_2_1_SharedFilterNeedsAShareName) {
  EXPECT_TRUE(is_shared("$share/g/a"));
  EXPECT_FALSE(is_shared("$shared/g/a"));
  EXPECT_FALSE(is_shared("share/g/a"));
  EXPECT_FALSE(parse_shared("$share//a").has_value());
  EXPECT_FALSE(parse_shared("$share/").has_value());
  EXPECT_EQ(parse_shared("$share/g/a"), (SharedFilter{"g", "a"}));
}

TEST(MqttTopic, MQTT_4_8_2_2_ShareNameRulesAndTheFilterAfterIt) {
  for (const char* bad : {"$share/g+/a", "$share/g#/a", "$share/+/a", "$share/#/a", "$share/g", "$share/g/", "$share/g/a#"}) {
    EXPECT_FALSE(parse_shared(bad).has_value()) << bad;
  }
  EXPECT_EQ(parse_shared("$share/consumer1/sport/tennis/+"), (SharedFilter{"consumer1", "sport/tennis/+"}));
  EXPECT_EQ(parse_shared("$share/g/#"), (SharedFilter{"g", "#"}));
  EXPECT_EQ(parse_shared("$share/g//a"), (SharedFilter{"g", "/a"}));
}

TEST(MqttTopic, HasWildcard) {
  EXPECT_TRUE(has_wildcard("a/+"));
  EXPECT_TRUE(has_wildcard("#"));
  EXPECT_FALSE(has_wildcard("a/b"));
}

TEST(MqttTopic, LevelsKeepEmptyOnes) {
  EXPECT_EQ(levels("a//b/"), (std::vector<std::string_view>{"a", "", "b", ""}));
  EXPECT_EQ(levels(""), (std::vector<std::string_view>{""}));
  EXPECT_EQ(levels("/"), (std::vector<std::string_view>{"", ""}));
}

TEST(MqttTopic, CoversIsMatchingBetweenFilters) {
  EXPECT_TRUE(covers("#", "a/b"));
  EXPECT_TRUE(covers("#", "a/#"));
  EXPECT_TRUE(covers("a/#", "a"));
  EXPECT_TRUE(covers("a/#", "a/+/c"));
  EXPECT_TRUE(covers("a/+", "a/b"));
  EXPECT_TRUE(covers("a/+", "a/+"));
  EXPECT_TRUE(covers("+/+", "a/+"));
  EXPECT_FALSE(covers("a/+", "a/#"));
  EXPECT_FALSE(covers("a/+", "a/b/c"));
  EXPECT_FALSE(covers("a/b", "a/+"));
  EXPECT_FALSE(covers("+", "a/b"));
  // The '$' rule holds between filters too.
  EXPECT_FALSE(covers("#", "$SYS/#"));
  EXPECT_FALSE(covers("+/x", "$SYS/x"));
  EXPECT_TRUE(covers("$SYS/#", "$SYS/+"));
}

}  // namespace
}  // namespace lir::mqtt
