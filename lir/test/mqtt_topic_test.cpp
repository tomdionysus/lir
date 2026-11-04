//
// Lir - The Distributed Event System
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <gtest/gtest.h>

#include "lir/mqtt/topic.h"

namespace lir::mqtt {
namespace {

TEST(MqttTopic, NamesHaveNoWildcards) {
  EXPECT_TRUE(valid_topic_name("a/b"));
  EXPECT_TRUE(valid_topic_name("/"));
  EXPECT_TRUE(valid_topic_name("$SYS/x"));
  EXPECT_FALSE(valid_topic_name(""));
  EXPECT_FALSE(valid_topic_name("a/+"));
  EXPECT_FALSE(valid_topic_name("a/#"));
  EXPECT_FALSE(valid_topic_name("a+b"));
}

// Section 4.7.1: the specification's examples.
TEST(MqttTopic, FilterRules) {
  for (const auto* f : {"#", "+", "sport/#", "sport/tennis/#", "+/tennis/#", "sport/+/player1", "+/+", "/+", "a//b", "/"})
    EXPECT_TRUE(valid_topic_filter(f)) << f;
  for (const auto* f : {"", "sport/tennis#", "sport/tennis/#/ranking", "sport+", "#/a", "a/b+", "++"}) EXPECT_FALSE(valid_topic_filter(f)) << f;
}

// Section 4.7.1.2 and 4.7.1.3.
TEST(MqttTopic, MatchingExamples) {
  EXPECT_TRUE(matches("sport/tennis/player1/#", "sport/tennis/player1"));
  EXPECT_TRUE(matches("sport/tennis/player1/#", "sport/tennis/player1/ranking"));
  EXPECT_TRUE(matches("sport/tennis/player1/#", "sport/tennis/player1/score/wimbledon"));
  EXPECT_TRUE(matches("sport/#", "sport"));
  EXPECT_TRUE(matches("sport/tennis/+", "sport/tennis/player1"));
  EXPECT_FALSE(matches("sport/tennis/+", "sport/tennis/player1/ranking"));
  EXPECT_TRUE(matches("sport/+", "sport/"));
  EXPECT_FALSE(matches("sport/+", "sport"));
  EXPECT_TRUE(matches("+/+", "/finance"));
  EXPECT_TRUE(matches("/+", "/finance"));
  EXPECT_FALSE(matches("+", "/finance"));
  EXPECT_TRUE(matches("#", "a/b/c"));
  EXPECT_TRUE(matches("a/b", "a/b"));
  EXPECT_FALSE(matches("a/b", "a/b/c"));
  EXPECT_FALSE(matches("a/b/c", "a/b"));
  EXPECT_FALSE(matches("a/+", "a"));
}

// Section 4.7.2: '$' topics and leading wildcards.
TEST(MqttTopic, DollarTopicsAndLeadingWildcards) {
  EXPECT_FALSE(matches("#", "$SYS/monitor/Clients"));
  EXPECT_FALSE(matches("+/monitor/Clients", "$SYS/monitor/Clients"));
  EXPECT_TRUE(matches("$SYS/#", "$SYS/monitor/Clients"));
  EXPECT_TRUE(matches("$SYS/monitor/+", "$SYS/monitor/Clients"));
}

TEST(MqttTopic, SharedSubscriptions) {
  EXPECT_TRUE(is_shared("$share/g/a/b"));
  EXPECT_FALSE(is_shared("$SYS/a"));
  EXPECT_EQ(parse_shared("$share/consumers/sport/+"), (SharedFilter{"consumers", "sport/+"}));
  EXPECT_EQ(parse_shared("$share/g/#"), (SharedFilter{"g", "#"}));
  EXPECT_FALSE(parse_shared("$share/g"));
  EXPECT_FALSE(parse_shared("$share//a"));
  EXPECT_FALSE(parse_shared("$share/g+/a"));
  EXPECT_FALSE(parse_shared("$share/g/"));
  EXPECT_FALSE(parse_shared("$share/g/a#"));
  EXPECT_FALSE(parse_shared("a/b"));
}

TEST(MqttTopic, Levels) {
  EXPECT_EQ(levels("a/b"), (std::vector<std::string_view>{"a", "b"}));
  EXPECT_EQ(levels("/a/"), (std::vector<std::string_view>{"", "a", ""}));
  EXPECT_EQ(levels("x"), (std::vector<std::string_view>{"x"}));
}

}  // namespace
}  // namespace lir::mqtt
