//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <gtest/gtest.h>

#include "lir/mqtt/packet.h"

namespace lir::mqtt {
namespace {

constexpr Version all_versions[] = {Version::v3_1, Version::v3_1_1, Version::v5};

Packet round_trip(const Packet& in, Version version) {
  const Bytes bytes = encode(in, version);
  const auto frame = read_frame(bytes, UINT32_MAX);
  EXPECT_TRUE(frame.has_value());
  if (frame) {
    EXPECT_EQ(frame->size(), bytes.size());
  }
  return decode(bytes, version);
}

uint8_t reason_of(const Bytes& bytes, Version version) {
  try {
    decode(bytes, version);
  } catch (const MalformedPacket& e) {
    return e.reason();
  }
  ADD_FAILURE() << "decoded";
  return 0;
}

// ---- known bytes

TEST(MqttPacket, MinimalConnect311Bytes) {
  Connect c;
  c.version = Version::v3_1_1;
  c.keep_alive = 60;
  c.client_id = "c";
  const Bytes expected{0x10, 0x0D, 0x00, 0x04, 'M', 'Q', 'T', 'T', 0x04, 0x02, 0x00, 0x3C, 0x00, 0x01, 'c'};
  EXPECT_EQ(encode(c, Version::v3_1_1), expected);
  EXPECT_EQ(std::get<Connect>(decode(expected, Version::v3_1_1)), c);
}

TEST(MqttPacket, PingsAreTwoBytes) {
  for (const auto version : all_versions) {
    EXPECT_EQ(encode(Pingreq{}, version), (Bytes{0xC0, 0x00}));
    EXPECT_EQ(encode(Pingresp{}, version), (Bytes{0xD0, 0x00}));
  }
}

TEST(MqttPacket, V5ShortFormsOmitSuccessAndEmptyProperties) {
  EXPECT_EQ(encode(Ack{PacketType::puback, 7, reason::success, {}}, Version::v5), (Bytes{0x40, 0x02, 0x00, 0x07}));
  EXPECT_EQ(encode(Disconnect{}, Version::v5), (Bytes{0xE0, 0x00}));
  EXPECT_EQ(encode(Auth{}, Version::v5), (Bytes{0xF0, 0x00}));
  // A reason code with no properties: three bytes, accepted.
  EXPECT_EQ(std::get<Ack>(decode(Bytes{0x40, 0x03, 0x00, 0x07, 0x10}, Version::v5)).reason, reason::no_matching_subscribers);
}

TEST(MqttPacket, PubrelSubscribeUnsubscribeCarryFlags0010) {
  EXPECT_EQ(encode(Ack{PacketType::pubrel, 1, reason::success, {}}, Version::v3_1_1)[0], 0x62);
  EXPECT_EQ(encode(Subscribe{1, {}, {{"a", QoS::at_most_once}}}, Version::v3_1_1)[0], 0x82);
  EXPECT_EQ(encode(Unsubscribe{1, {}, {"a"}}, Version::v3_1_1)[0], 0xA2);
}

// ---- round trips, every packet, every version that carries it

TEST(MqttPacket, ConnectWithEverythingRoundTrips) {
  for (const auto version : all_versions) {
    Connect c;
    c.version = version;
    c.clean_start = false;
    c.keep_alive = 15;
    c.client_id = "client-1";
    c.will = Will{"last/will", Bytes{'b', 'y', 'e'}, QoS::exactly_once, true, {}};
    c.username = "user";
    c.password = Bytes{'p', 'w'};
    if (version == Version::v5) {
      c.properties.session_expiry_interval = 120;
      c.properties.receive_maximum = 20;
      c.properties.authentication_method = "PLAIN";
      c.will->properties.will_delay_interval = 5;
    }
    EXPECT_EQ(std::get<Connect>(round_trip(c, version)), c);
  }
}

TEST(MqttPacket, PublishRoundTripsAtEveryQoS) {
  for (const auto version : all_versions) {
    for (const auto qos : {QoS::at_most_once, QoS::at_least_once, QoS::exactly_once}) {
      Publish p;
      p.qos = qos;
      p.retain = true;
      p.dup = qos != QoS::at_most_once;
      p.topic = "a/b";
      p.packet_id = qos == QoS::at_most_once ? 0 : 42;
      p.payload = Bytes{1, 2, 3};
      if (version == Version::v5) p.properties.message_expiry_interval = 9;
      EXPECT_EQ(std::get<Publish>(round_trip(p, version)), p);
    }
  }
}

TEST(MqttPacket, EmptyPayloadIsAllowed) {
  Publish p;
  p.topic = "clear/me";
  p.retain = true;
  EXPECT_EQ(std::get<Publish>(round_trip(p, Version::v3_1_1)), p);
}

TEST(MqttPacket, AcksRoundTrip) {
  for (const auto version : all_versions) {
    for (const auto type : {PacketType::puback, PacketType::pubrec, PacketType::pubrel, PacketType::pubcomp}) {
      Ack a{type, 513, reason::success, {}};
      EXPECT_EQ(std::get<Ack>(round_trip(a, version)), a);
    }
  }
  Ack a{PacketType::pubrec, 1, reason::quota_exceeded, {}};
  a.properties.reason_string = "slow down";
  EXPECT_EQ(std::get<Ack>(round_trip(a, Version::v5)), a);
  Ack rel{PacketType::pubrel, 1, reason::packet_identifier_not_found, {}};
  EXPECT_EQ(std::get<Ack>(round_trip(rel, Version::v5)), rel);
}

TEST(MqttPacket, SubscribeFamilyRoundTrips) {
  for (const auto version : all_versions) {
    Subscribe s{9, {}, {{"a/+", QoS::at_least_once}, {"b/#", QoS::exactly_once}}};
    Suback sa{9, {}, {reason::granted_qos_1, 0x80}};
    Unsubscribe u{10, {}, {"a/+", "b/#"}};
    Unsuback ua{10, {}, {}};
    if (version == Version::v5) {
      s.subscriptions[0].no_local = true;
      s.subscriptions[0].retain_as_published = true;
      s.subscriptions[1].retain_handling = 2;
      s.properties.subscription_identifiers = {77};
      ua.reasons = {reason::success, reason::no_subscription_existed};
    }
    EXPECT_EQ(std::get<Subscribe>(round_trip(s, version)), s);
    EXPECT_EQ(std::get<Suback>(round_trip(sa, version)), sa);
    EXPECT_EQ(std::get<Unsubscribe>(round_trip(u, version)), u);
    EXPECT_EQ(std::get<Unsuback>(round_trip(ua, version)), ua);
  }
}

TEST(MqttPacket, ConnackDisconnectAuthRoundTrip) {
  for (const auto version : all_versions) {
    Connack c{true, reason::success, {}};
    EXPECT_EQ(std::get<Connack>(round_trip(c, version)), c);
    EXPECT_EQ(std::get<Disconnect>(round_trip(Disconnect{}, version)), Disconnect{});
  }
  Connack refused{false, reason::not_authorized, {}};
  refused.properties.reason_string = "no";
  EXPECT_EQ(std::get<Connack>(round_trip(refused, Version::v5)), refused);
  Disconnect d{reason::disconnect_with_will, {}};
  d.properties.session_expiry_interval = 0;
  EXPECT_EQ(std::get<Disconnect>(round_trip(d, Version::v5)), d);
  Auth a{reason::continue_authentication, {}};
  a.properties.authentication_method = "SCRAM";
  a.properties.authentication_data = Bytes{1};
  EXPECT_EQ(std::get<Auth>(round_trip(a, Version::v5)), a);
}

TEST(MqttPacket, V5PublishByTopicAliasAlone) {
  Publish p;
  p.properties.topic_alias = 3;
  EXPECT_EQ(std::get<Publish>(round_trip(p, Version::v5)), p);
}

// ---- framing

TEST(MqttPacket, FrameWaitsForTheWholeFixedHeader) {
  EXPECT_FALSE(read_frame(Bytes{}, 1024));
  EXPECT_FALSE(read_frame(Bytes{0x30}, 1024));
  EXPECT_FALSE(read_frame(Bytes{0x30, 0x80}, 1024));
  const auto frame = read_frame(Bytes{0x30, 0x80, 0x01}, 1024);
  ASSERT_TRUE(frame);
  EXPECT_EQ(frame->header_size, 3u);
  EXPECT_EQ(frame->remaining_length, 128u);
}

TEST(MqttPacket, FrameOverMaximumIsPacketTooLargeBeforeTheBody) {
  try {
    read_frame(Bytes{0x30, 0xFF, 0x7F}, 1024);
    FAIL();
  } catch (const MalformedPacket& e) {
    EXPECT_EQ(e.reason(), reason::packet_too_large);
  }
}

TEST(MqttPacket, LengthMismatchIsMalformed) { EXPECT_EQ(reason_of(Bytes{0xC0, 0x01}, Version::v3_1_1), reason::malformed_packet); }

// ---- the MUSTs, each refused

TEST(MqttPacket, ReservedTypeIsMalformed) {
  EXPECT_EQ(reason_of(Bytes{0x00, 0x00}, Version::v5), reason::malformed_packet);
  // AUTH does not exist before 5.0.
  EXPECT_EQ(reason_of(Bytes{0xF0, 0x00}, Version::v3_1_1), reason::malformed_packet);
  EXPECT_THROW(encode(Auth{}, Version::v3_1_1), std::invalid_argument);
}

TEST(MqttPacket, WrongFixedHeaderFlagsAreMalformed) {
  EXPECT_EQ(reason_of(Bytes{0x80, 0x06, 0x00, 0x01, 0x00, 0x01, 'a', 0x00}, Version::v3_1_1), reason::malformed_packet);  // SUBSCRIBE flags 0
  EXPECT_EQ(reason_of(Bytes{0xC1, 0x00}, Version::v3_1_1), reason::malformed_packet);                                     // PINGREQ flags 1
}

TEST(MqttPacket, PublishQoS3AndDupAtQoS0AreMalformed) {
  EXPECT_EQ(reason_of(Bytes{0x36, 0x03, 0x00, 0x01, 'a'}, Version::v3_1_1), reason::malformed_packet);
  EXPECT_EQ(reason_of(Bytes{0x38, 0x03, 0x00, 0x01, 'a'}, Version::v3_1_1), reason::malformed_packet);
  Publish p;
  p.topic = "a";
  p.dup = true;
  EXPECT_THROW(encode(p, Version::v3_1_1), std::invalid_argument);
}

TEST(MqttPacket, PacketIdentifierZeroIsRefused) {
  EXPECT_EQ(reason_of(Bytes{0x32, 0x05, 0x00, 0x01, 'a', 0x00, 0x00}, Version::v3_1_1), reason::protocol_error);
  EXPECT_EQ(reason_of(Bytes{0x40, 0x02, 0x00, 0x00}, Version::v3_1_1), reason::protocol_error);
  EXPECT_THROW(encode(Ack{PacketType::puback, 0, reason::success, {}}, Version::v5), std::invalid_argument);
}

TEST(MqttPacket, EmptyTopicWithoutAliasIsRefused) {
  EXPECT_EQ(reason_of(Bytes{0x30, 0x02, 0x00, 0x00}, Version::v3_1_1), reason::protocol_error);
  EXPECT_EQ(reason_of(Bytes{0x30, 0x03, 0x00, 0x00, 0x00}, Version::v5), reason::protocol_error);
}

TEST(MqttPacket, ConnectReservedFlagIsMalformed) {
  Bytes bytes = encode(Connect{Version::v3_1_1, true, 0, "c", {}, {}, {}, {}}, Version::v3_1_1);
  bytes[9] |= 0x01;
  EXPECT_EQ(reason_of(bytes, Version::v3_1_1), reason::malformed_packet);
}

TEST(MqttPacket, WillFlagsWithoutWillAreMalformed) {
  Bytes bytes = encode(Connect{Version::v3_1_1, true, 0, "c", {}, {}, {}, {}}, Version::v3_1_1);
  bytes[9] |= 0x08;  // will QoS 1, no will
  EXPECT_EQ(reason_of(bytes, Version::v3_1_1), reason::malformed_packet);
  bytes[9] = static_cast<uint8_t>((bytes[9] & ~0x08) | 0x20);  // will retain, no will
  EXPECT_EQ(reason_of(bytes, Version::v3_1_1), reason::malformed_packet);
}

TEST(MqttPacket, PasswordWithoutUserNameRefusedOn3xOnly) {
  Connect c{Version::v3_1_1, true, 0, "c", {}, {}, Bytes{'p'}, {}};
  EXPECT_THROW(encode(c, Version::v3_1_1), std::invalid_argument);
  c.version = Version::v5;
  EXPECT_EQ(std::get<Connect>(round_trip(c, Version::v5)), c);
  // Hand-built 3.1.1: flags 0x42, password with no user name.
  const Bytes bytes{0x10, 0x10, 0x00, 0x04, 'M', 'Q', 'T', 'T', 0x04, 0x42, 0x00, 0x00, 0x00, 0x01, 'c', 0x00, 0x01, 'p'};
  EXPECT_EQ(reason_of(bytes, Version::v3_1_1), reason::malformed_packet);
}

TEST(MqttPacket, UnsupportedVersionNamesTheLevel) {
  const Bytes bytes{0x10, 0x0D, 0x00, 0x04, 'M', 'Q', 'T', 'T', 0x06, 0x02, 0x00, 0x3C, 0x00, 0x01, 'c'};
  try {
    decode(bytes, Version::v5);
    FAIL();
  } catch (const UnsupportedVersion& e) {
    EXPECT_EQ(e.level(), 6);
    EXPECT_EQ(e.reason(), reason::unsupported_protocol_version);
  }
}

TEST(MqttPacket, V31SpeaksMQIsdp) {
  Connect c{Version::v3_1, true, 10, "old-client", {}, {}, {}, {}};
  const Bytes bytes = encode(c, Version::v3_1);
  EXPECT_EQ(bytes[3], 0x06);
  EXPECT_EQ(std::string(bytes.begin() + 4, bytes.begin() + 10), "MQIsdp");
  EXPECT_EQ(std::get<Connect>(decode(bytes, Version::v5)).version, Version::v3_1);
}

TEST(MqttPacket, SubscribeNeedsASubscriptionAndUnsubscribeAFilter) {
  EXPECT_EQ(reason_of(Bytes{0x82, 0x02, 0x00, 0x01}, Version::v3_1_1), reason::protocol_error);
  EXPECT_EQ(reason_of(Bytes{0xA2, 0x02, 0x00, 0x01}, Version::v3_1_1), reason::protocol_error);
  EXPECT_THROW(encode(Subscribe{1, {}, {}}, Version::v3_1_1), std::invalid_argument);
}

TEST(MqttPacket, SubscriptionOptionReservedBitsAreMalformed) {
  // 3.1.1: anything above the QoS bits.
  EXPECT_EQ(reason_of(Bytes{0x82, 0x06, 0x00, 0x01, 0x00, 0x01, 'a', 0x04}, Version::v3_1_1), reason::malformed_packet);
  // 5.0: the top two bits, and retain handling 3.
  EXPECT_EQ(reason_of(Bytes{0x82, 0x07, 0x00, 0x01, 0x00, 0x00, 0x01, 'a', 0x40}, Version::v5), reason::malformed_packet);
  EXPECT_EQ(reason_of(Bytes{0x82, 0x07, 0x00, 0x01, 0x00, 0x00, 0x01, 'a', 0x30}, Version::v5), reason::malformed_packet);
  Subscribe s{1, {}, {{"a", QoS::at_most_once, true, false, 0}}};
  EXPECT_THROW(encode(s, Version::v3_1_1), std::invalid_argument);
}

TEST(MqttPacket, PropertiesRefusedOn3x) {
  Publish p;
  p.topic = "a";
  p.properties.message_expiry_interval = 1;
  EXPECT_THROW(encode(p, Version::v3_1_1), std::invalid_argument);
}

TEST(MqttPacket, ReasonCodesOutsideTheirPacketsTableAreRefused) {
  // PUBREL may only carry 0x00 or 0x92.
  EXPECT_EQ(reason_of(Bytes{0x62, 0x03, 0x00, 0x01, 0x10}, Version::v5), reason::protocol_error);
  EXPECT_THROW(encode(Ack{PacketType::pubrel, 1, reason::no_matching_subscribers, {}}, Version::v5), std::invalid_argument);
  // A 3.x CONNACK return code over 5.
  EXPECT_EQ(reason_of(Bytes{0x20, 0x02, 0x00, 0x06}, Version::v3_1_1), reason::protocol_error);
  // Session present on a refusal.
  EXPECT_EQ(reason_of(Bytes{0x20, 0x02, 0x01, 0x05}, Version::v3_1_1), reason::protocol_error);
}

TEST(MqttPacket, ThreeXUnsubackHasNoReasons) {
  EXPECT_EQ(encode(Unsuback{5, {}, {}}, Version::v3_1_1), (Bytes{0xB0, 0x02, 0x00, 0x05}));
  EXPECT_THROW(encode(Unsuback{5, {}, {0x00}}, Version::v3_1_1), std::invalid_argument);
  EXPECT_THROW(encode(Unsuback{5, {}, {}}, Version::v5), std::invalid_argument);
}

TEST(MqttPacket, TrailingBytesAreMalformed) {
  EXPECT_EQ(reason_of(Bytes{0x40, 0x03, 0x00, 0x01, 0x00}, Version::v3_1_1), reason::malformed_packet);
  EXPECT_EQ(reason_of(Bytes{0xE0, 0x01, 0x00}, Version::v3_1_1), reason::malformed_packet);
}

}  // namespace
}  // namespace lir::mqtt
