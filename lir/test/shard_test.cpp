//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "lir/shard/shard.h"

#include <gtest/gtest.h>

#include <set>

#include "lir/mqtt/topic.h"

namespace lir::shard {
namespace {

TEST(Shard, OwnershipPrefix) {
  EXPECT_EQ(ownership_prefix("a/b/c", 0), "a/b/c");
  EXPECT_EQ(ownership_prefix("a/b/c", 1), "a");
  EXPECT_EQ(ownership_prefix("a/b/c", 2), "a/b");
  EXPECT_EQ(ownership_prefix("a/b/c", 3), "a/b/c");
  EXPECT_EQ(ownership_prefix("a/b", 5), "a/b");
  EXPECT_EQ(ownership_prefix("/x", 1), "");
}

TEST(Shard, DeterministicAndInRange) {
  EXPECT_EQ(shard_of("sensors/room1/temp", 0, 256), shard_of("sensors/room1/temp", 0, 256));
  std::set<ShardId> seen;
  for (int i = 0; i < 2000; ++i) {
    const auto s = shard_of("t/" + std::to_string(i), 0, 256);
    EXPECT_LT(s, 256);
    seen.insert(s);
  }
  // 2000 topics over 256 shards leave none empty in practice.
  EXPECT_EQ(seen.size(), 256u);
}

// The design's example: with L = 2 the two room1 topics share a shard.
TEST(Shard, PrefixLevelsGroupTopics) {
  EXPECT_EQ(shard_of("sensors/room1/temp", 2, 256), shard_of("sensors/room1/hum", 2, 256));
  EXPECT_EQ(shard_of("sensors/room1", 2, 256), shard_of("sensors/room1/hum", 2, 256));
}

TEST(Shard, FilterLocality) {
  EXPECT_EQ(shard_of_filter("a/b", 0, 256), shard_of("a/b", 0, 256));
  EXPECT_FALSE(shard_of_filter("a/+", 0, 256));
  EXPECT_EQ(shard_of_filter("sensors/room1/+", 2, 256), shard_of("sensors/room1/x", 2, 256));
  EXPECT_EQ(shard_of_filter("sensors/room1/#", 2, 256), shard_of("sensors/room1", 2, 256));
  EXPECT_FALSE(shard_of_filter("sensors/+/temp", 2, 256));
  EXPECT_FALSE(shard_of_filter("sensors/#", 2, 256));
  EXPECT_FALSE(shard_of_filter("#", 2, 256));
}

// Every topic a shard-local filter matches lies in the filter's shard.
TEST(Shard, ShardLocalFilterMatchesOnlyItsShard) {
  const std::vector<std::string> topics{"s/r/a", "s/r", "s/r/a/b", "s/q/a", "t/r/a"};
  for (const auto* filter : {"s/r/+", "s/r/#", "s/r/+/b"}) {
    const auto shard = shard_of_filter(filter, 2, 64);
    ASSERT_TRUE(shard) << filter;
    for (const auto& topic : topics) {
      if (mqtt::matches(filter, topic)) EXPECT_EQ(shard_of(topic, 2, 64), *shard) << filter << " " << topic;
    }
  }
}

TEST(Shard, CountChecked) {
  EXPECT_THROW(shard_of("a", 0, 0), std::invalid_argument);
  EXPECT_THROW(shard_of("a", 0, 65537), std::invalid_argument);
}

// A session's shard (plan 103): SHA-256("lir-sess:" || client id) mod S,
// the first eight bytes of the digest big-endian, as a topic's is under its
// own salt.
TEST(Shard, ASessionsShardIsItsClientIdHashedUnderItsOwnSalt) {
  for (const std::string id : {"client-1", "", "lir-0f3a", "a much longer client identifier than most"}) {
    dannan::identity::Sha256Hasher hasher;
    const std::string salted = "lir-sess:" + id;
    hasher.update({reinterpret_cast<const uint8_t*>(salted.data()), salted.size()});
    const auto digest = hasher.finish();
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) value = (value << 8) | digest.bytes[i];
    EXPECT_EQ(session_shard_of(id, 256), static_cast<ShardId>(value % 256)) << id;
  }
}

TEST(Shard, SessionShardsSpreadAndStayInRange) {
  std::set<ShardId> seen;
  for (int i = 0; i < 2000; ++i) {
    const auto s = session_shard_of("client-" + std::to_string(i), 256);
    EXPECT_LT(s, 256);
    seen.insert(s);
  }
  EXPECT_EQ(seen.size(), 256u);
  EXPECT_THROW(session_shard_of("a", 0), std::invalid_argument);
}

TEST(Shard, PlacementKeyNamesTheShard) {
  EXPECT_NE(placement_key(1), placement_key(2));
  EXPECT_EQ(placement_key(258).back(), 2);
}

}  // namespace
}  // namespace lir::shard
