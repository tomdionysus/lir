//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <gtest/gtest.h>

#include "lir/cluster/codec.h"

// What Lir keeps in Dagda's logs (step 5 design, "The mapping"): entries
// that outlive the node and the process that wrote them.
namespace lir::cluster {
namespace {

using namespace std::chrono_literals;

broker::Message qos2_message(TimePoint now) {
  broker::Message m;
  m.topic = "sensors/room1/temp";
  m.payload = {'2', '1', '.', '5'};
  m.qos = mqtt::QoS::exactly_once;
  m.expires_at = now + 30s;
  m.user_properties = {{"unit", "C"}};
  m.origin = "client-1";
  return m;
}

TEST(ClusterStored, AnInflightEntryRoundTripsWithItsPacketId) {
  const Clocks clocks{TimePoint{} + 5s, 1'700'000'000'000};
  const InflightEntry entry{qos2_message(clocks.now), 4242};
  const auto back = decode_inflight(encode_inflight(entry, clocks), clocks);
  EXPECT_EQ(back.message.topic, entry.message.topic);
  EXPECT_EQ(back.message.payload, entry.message.payload);
  EXPECT_EQ(back.message.qos, entry.message.qos);
  EXPECT_EQ(back.message.user_properties, entry.message.user_properties);
  EXPECT_EQ(back.message.origin, entry.message.origin);
  EXPECT_EQ(back.message.expires_at, entry.message.expires_at);
  EXPECT_EQ(back.packet_id, std::optional<uint16_t>(4242));

  const InflightEntry qos1{broker::Message{}, std::nullopt};
  EXPECT_FALSE(decode_inflight(encode_inflight(qos1, clocks), clocks).packet_id);
}

// A steady clock does not survive a restart, and a log entry does. So a
// stored expiry is wall-clock time: read back by a later process, whose
// steady clock has nothing to do with the writer's, a message expires
// when it would have.
TEST(ClusterStored, AStoredExpiryIsWallClockAndSurvivesARestart) {
  const Clocks writer{TimePoint{} + 100s, 1'700'000'000'000};
  const InflightEntry entry{qos2_message(writer.now), std::nullopt};
  const auto bytes = encode_inflight(entry, writer);

  // Ten seconds later by the wall, in a process whose steady clock started
  // afresh.
  const Clocks reader{TimePoint{} + 3s, writer.wall_ms + 10'000};
  const auto back = decode_inflight(bytes, reader);
  ASSERT_TRUE(back.message.expires_at);
  EXPECT_EQ(*back.message.expires_at, reader.now + 20s);
}

TEST(ClusterStored, AnExpiryThatPassedWhileStoredIsDueAtOnce) {
  const Clocks writer{TimePoint{} + 100s, 1'700'000'000'000};
  const auto bytes = encode_inflight({qos2_message(writer.now), std::nullopt}, writer);
  const Clocks reader{TimePoint{} + 7s, writer.wall_ms + 60'000};
  const auto back = decode_inflight(bytes, reader);
  ASSERT_TRUE(back.message.expires_at);
  EXPECT_EQ(*back.message.expires_at, reader.now);
}

// A queue entry is appended under the holding node's own dot (a log refuses
// a dot whose origin is not the appender); the publisher's rides inside, for
// the session's dedup.
TEST(ClusterStored, AQueueEntryCarriesThePublishersDot) {
  const Clocks clocks{TimePoint{} + 5s, 1'700'000'000'000};
  broker::Delivery delivery;
  delivery.message = qos2_message(clocks.now);
  delivery.qos = mqtt::QoS::at_least_once;
  delivery.retain = false;
  delivery.identifiers = {7, 9};
  const QueueEntry entry{Dot{"node-a", 1'234'567}, delivery};

  const auto back = decode_queued(encode_queued(entry, clocks), clocks);
  EXPECT_EQ(back.publisher, entry.publisher);
  EXPECT_EQ(back.delivery.qos, mqtt::QoS::at_least_once);
  EXPECT_EQ(back.delivery.identifiers, (std::vector<uint32_t>{7, 9}));
  EXPECT_EQ(back.delivery.message.topic, delivery.message.topic);
  EXPECT_EQ(back.delivery.message.expires_at, delivery.message.expires_at);
}

TEST(ClusterStored, DotsOrderByOriginThenSequence) {
  EXPECT_LT((Dot{"a", 9}), (Dot{"b", 1}));
  EXPECT_LT((Dot{"a", 1}), (Dot{"a", 2}));
  EXPECT_EQ((Dot{"a", 1}), (Dot{"a", 1}));
}

TEST(ClusterStored, ATruncatedOrForeignEntryIsRefused) {
  const Clocks clocks{TimePoint{}, 1'700'000'000'000};
  auto bytes = encode_inflight({qos2_message(clocks.now), 1}, clocks);
  const Bytes truncated(bytes.begin(), bytes.end() - 1);
  EXPECT_THROW(decode_inflight(truncated, clocks), dannan::net::DecodeError);

  auto queued = encode_queued({Dot{"n", 1}, broker::Delivery{}}, clocks);
  // An inflight entry is not a queue entry, nor the reverse.
  EXPECT_THROW(decode_queued(bytes, clocks), dannan::net::DecodeError);
  EXPECT_THROW(decode_inflight(queued, clocks), dannan::net::DecodeError);

  bytes[0] = 0xFF;
  EXPECT_THROW(decode_inflight(bytes, clocks), dannan::net::DecodeError);
}

}  // namespace
}  // namespace lir::cluster
