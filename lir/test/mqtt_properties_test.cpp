//
// Lir - The Distributed Event System
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <gtest/gtest.h>

#include "lir/mqtt/properties.h"

namespace lir::mqtt {
namespace {

using dannan::net::Reader;
using dannan::net::Writer;

Properties round_trip(const Properties& in, PropertyPlace place) {
  Writer w;
  write_properties(w, in, place);
  EXPECT_EQ(w.data().size(), properties_size(in));
  Reader r(w.data());
  auto out = read_properties(r, place);
  EXPECT_EQ(r.remaining(), 0u);
  return out;
}

Properties decode_block(const Bytes& block, PropertyPlace place) {
  Reader r(block);
  return read_properties(r, place);
}

TEST(MqttProperties, EmptyIsOneZeroByte) {
  Writer w;
  write_properties(w, {}, PropertyPlace::publish);
  EXPECT_EQ(w.data(), (Bytes{0x00}));
}

TEST(MqttProperties, ConnackRoundTrip) {
  Properties p;
  p.session_expiry_interval = 3600;
  p.assigned_client_identifier = "auto-1";
  p.server_keep_alive = 30;
  p.receive_maximum = 100;
  p.topic_alias_maximum = 10;
  p.maximum_qos = 1;
  p.retain_available = 1;
  p.maximum_packet_size = 1 << 20;
  p.wildcard_subscription_available = 1;
  p.subscription_identifier_available = 1;
  p.shared_subscription_available = 1;
  p.reason_string = "ok";
  p.server_reference = "other:1883";
  p.response_information = "resp/";
  p.authentication_method = "SCRAM";
  p.authentication_data = Bytes{1, 2, 3};
  p.user_properties = {{"a", "1"}, {"a", "2"}, {"b", ""}};
  EXPECT_EQ(round_trip(p, PropertyPlace::connack), p);
}

TEST(MqttProperties, PublishRoundTripWithSeveralSubscriptionIdentifiers) {
  Properties p;
  p.payload_format_indicator = 1;
  p.message_expiry_interval = 60;
  p.content_type = "application/json";
  p.response_topic = "reply/here";
  p.correlation_data = Bytes{9, 9};
  p.subscription_identifiers = {1, 268435455};
  p.topic_alias = 5;
  EXPECT_EQ(round_trip(p, PropertyPlace::publish), p);
}

TEST(MqttProperties, WillRoundTrip) {
  Properties p;
  p.will_delay_interval = 10;
  p.message_expiry_interval = 20;
  EXPECT_EQ(round_trip(p, PropertyPlace::will), p);
}

// Section 2.2.2.2: a property not valid for the packet is a protocol error.
TEST(MqttProperties, NotAllowedHereIsProtocolError) {
  // Topic alias (0x23) inside a CONNECT.
  try {
    decode_block(Bytes{0x03, 0x23, 0x00, 0x01}, PropertyPlace::connect);
    FAIL();
  } catch (const MalformedPacket& e) {
    EXPECT_EQ(e.reason(), reason::protocol_error);
  }
  Properties p;
  p.will_delay_interval = 1;
  Writer w;
  EXPECT_THROW(write_properties(w, p, PropertyPlace::publish), std::invalid_argument);
}

TEST(MqttProperties, RepeatedPropertyIsProtocolError) {
  // Two message expiry intervals.
  EXPECT_THROW(decode_block(Bytes{0x0A, 0x02, 0, 0, 0, 1, 0x02, 0, 0, 0, 2}, PropertyPlace::publish), MalformedPacket);
}

TEST(MqttProperties, UserPropertyMayRepeat) {
  const auto p = decode_block(Bytes{0x0B, 0x26, 0, 1, 'k', 0, 0, 0x26, 0, 0, 0, 0}, PropertyPlace::publish);
  ASSERT_EQ(p.user_properties.size(), 2u);
}

TEST(MqttProperties, OneSubscriptionIdentifierPerSubscribe) {
  EXPECT_THROW(decode_block(Bytes{0x04, 0x0B, 0x01, 0x0B, 0x02}, PropertyPlace::subscribe), MalformedPacket);
  Properties p;
  p.subscription_identifiers = {1, 2};
  Writer w;
  EXPECT_THROW(write_properties(w, p, PropertyPlace::subscribe), std::invalid_argument);
}

TEST(MqttProperties, ValuesOutOfRange) {
  EXPECT_THROW(decode_block(Bytes{0x02, 0x0B, 0x00}, PropertyPlace::subscribe), MalformedPacket);      // subscription id 0
  EXPECT_THROW(decode_block(Bytes{0x03, 0x21, 0x00, 0x00}, PropertyPlace::connect), MalformedPacket);  // receive maximum 0
  EXPECT_THROW(decode_block(Bytes{0x05, 0x27, 0, 0, 0, 0}, PropertyPlace::connect), MalformedPacket);  // maximum packet size 0
  EXPECT_THROW(decode_block(Bytes{0x03, 0x23, 0x00, 0x00}, PropertyPlace::publish), MalformedPacket);  // topic alias 0
  EXPECT_THROW(decode_block(Bytes{0x02, 0x01, 0x02}, PropertyPlace::publish), MalformedPacket);        // payload format 2
  EXPECT_THROW(decode_block(Bytes{0x02, 0x24, 0x02}, PropertyPlace::connack), MalformedPacket);        // maximum QoS 2
}

TEST(MqttProperties, UnknownPropertyIsProtocolError) { EXPECT_THROW(decode_block(Bytes{0x02, 0x7F, 0x00}, PropertyPlace::publish), MalformedPacket); }

TEST(MqttProperties, BlockPastPacketIsMalformed) {
  try {
    decode_block(Bytes{0x05, 0x02, 0x00}, PropertyPlace::publish);
    FAIL();
  } catch (const MalformedPacket& e) {
    EXPECT_EQ(e.reason(), reason::malformed_packet);
  }
}

TEST(MqttProperties, ValueRunningPastBlockIsMalformed) {
  // The block says 2 bytes; a message expiry interval needs 1 + 4.
  EXPECT_THROW(decode_block(Bytes{0x02, 0x02, 0x00, 0x00, 0x00, 0x00}, PropertyPlace::publish), MalformedPacket);
}

}  // namespace
}  // namespace lir::mqtt
