//
// Lir - The Distributed Event System
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <gtest/gtest.h>

#include "lir/cluster/codec.h"

namespace lir::cluster {
namespace {

using namespace std::chrono_literals;

broker::Message full_message(TimePoint now) {
  broker::Message m;
  m.topic = "a/b";
  m.payload = {1, 2, 3};
  m.qos = mqtt::QoS::exactly_once;
  m.retain = true;
  m.payload_format_indicator = 1;
  m.expires_at = now + 30s;
  m.content_type = "text/plain";
  m.response_topic = "reply";
  m.correlation_data = Bytes{9};
  m.user_properties = {{"k", "v"}, {"k", "w"}};
  m.origin = "client-1";
  m.version = broker::RetainedVersion{42, "node-a"};
  return m;
}

void expect_same(const broker::Message& x, const broker::Message& y) {
  EXPECT_EQ(x.topic, y.topic);
  EXPECT_EQ(x.payload, y.payload);
  EXPECT_EQ(x.qos, y.qos);
  EXPECT_EQ(x.retain, y.retain);
  EXPECT_EQ(x.payload_format_indicator, y.payload_format_indicator);
  EXPECT_EQ(x.expires_at, y.expires_at);
  EXPECT_EQ(x.content_type, y.content_type);
  EXPECT_EQ(x.response_topic, y.response_topic);
  EXPECT_EQ(x.correlation_data, y.correlation_data);
  EXPECT_EQ(x.user_properties, y.user_properties);
  EXPECT_EQ(x.origin, y.origin);
  EXPECT_EQ(x.version, y.version);
}

TEST(ClusterCodec, MessageRoundTrips) {
  const TimePoint now{};
  const auto m = full_message(now);
  expect_same(decode_message(encode_message(m, now), now), m);
  expect_same(decode_message(encode_message(broker::Message{}, now), now), broker::Message{});
}

// An expiry travels as what is left, so a receiver whose clock reads
// differently still expires it after the same interval.
TEST(ClusterCodec, ExpiryTravelsAsWhatIsLeft) {
  const TimePoint sender{};
  const TimePoint receiver = sender + 1000s;
  auto m = full_message(sender);
  const auto got = decode_message(encode_message(m, sender), receiver);
  EXPECT_EQ(*got.expires_at, receiver + 30s);
}

TEST(ClusterCodec, MessagesRoundTrip) {
  const TimePoint now{};
  const auto out = decode_messages(encode_messages({full_message(now), broker::Message{}}, now), now);
  ASSERT_EQ(out.size(), 2u);
  expect_same(out[0], full_message(now));
}

TEST(ClusterCodec, SessionImageRoundTrips) {
  const TimePoint now{};
  broker::SessionImage image;
  image.client_id = "roamer";
  image.version = mqtt::Version::v3_1_1;
  image.expiry_interval = 0xFFFFFFFF;
  image.subscriptions = {{"a/#", broker::Options{mqtt::QoS::at_least_once, true, true, 1, 7}}, {"$share/g/b", broker::Options{}}};
  image.outbound = {{5, broker::Inflight{broker::Delivery{full_message(now), mqtt::QoS::exactly_once, true, {7}}, broker::Inflight::State::awaiting_pubcomp}}};
  image.queued = {broker::Delivery{full_message(now), mqtt::QoS::at_least_once, false, {}}};
  image.inbound_qos2 = {3, 4};
  image.next_packet_id = 6;
  const auto got = decode_image(encode_image(image, now), now);
  EXPECT_EQ(got.client_id, image.client_id);
  EXPECT_EQ(got.version, image.version);
  EXPECT_EQ(got.expiry_interval, image.expiry_interval);
  EXPECT_EQ(got.subscriptions, image.subscriptions);
  ASSERT_EQ(got.outbound.size(), 1u);
  EXPECT_EQ(got.outbound[0].first, 5);
  EXPECT_EQ(got.outbound[0].second.state, broker::Inflight::State::awaiting_pubcomp);
  EXPECT_EQ(got.outbound[0].second.delivery.identifiers, (std::vector<uint32_t>{7}));
  ASSERT_EQ(got.queued.size(), 1u);
  expect_same(got.queued[0].message, full_message(now));
  EXPECT_EQ(got.inbound_qos2, image.inbound_qos2);
  EXPECT_EQ(got.next_packet_id, 6);
}

TEST(ClusterCodec, SmallMessagesRoundTrip) {
  const TimePoint now{};
  const Groups groups{{"g", "a/+"}, {"h", "#"}};
  EXPECT_EQ(decode_groups(encode_groups(groups)), groups);
  const auto take = decode_take(encode_take(Take{"c", true}));
  EXPECT_EQ(take.client_id, "c");
  EXPECT_TRUE(take.want_state);
  EXPECT_FALSE(decode_taken(encode_taken(std::nullopt, now), now));
  const auto shared = decode_shared(encode_shared(SharedDelivery{"g", "a/+", full_message(now)}, now), now);
  EXPECT_EQ(shared.group, "g");
  EXPECT_EQ(shared.filter, "a/+");
}

TEST(ClusterCodec, UnknownFormatAndTrailingBytesAreRefused) {
  Bytes bytes = encode_groups({});
  bytes[0] = 99;
  EXPECT_THROW(decode_groups(bytes), dannan::net::DecodeError);
  Bytes longer = encode_groups({});
  longer.push_back(0);
  EXPECT_THROW(decode_groups(longer), dannan::net::DecodeError);
}

TEST(ClusterCodec, CountsAreBoundedBeforeAllocation) {
  Bytes bytes{format, 0xFF, 0xFF, 0xFF, 0xFF};
  EXPECT_THROW(decode_groups(bytes), dannan::net::DecodeError);
}

}  // namespace
}  // namespace lir::cluster
