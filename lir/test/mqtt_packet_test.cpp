//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

#include "lir/mqtt/packet.h"

// Packets are built here from the leading members only; the rest keep their
// defaults, which is the point of the defaults.
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

// The control packets' own bytes: MQTT 5.0 sections 2 and 3 and 3.1.1's,
// each test named by the statements it proves. What a statement asks of a
// session (packet ids in use, aliases, receive maximum, the order of
// packets) is the connection's, and is tested there.
namespace lir::mqtt {
namespace {

constexpr Version v5 = Version::v5;
constexpr Version v311 = Version::v3_1_1;
constexpr Version v31 = Version::v3_1;

uint8_t refusal(const Bytes& bytes, Version version) {
  try {
    decode(bytes, version);
  } catch (const MalformedPacket& error) {
    return error.reason();
  }
  return reason::success;
}

// A whole packet: the first byte, the remaining length, then the body.
Bytes packet(uint8_t first, const Bytes& body) {
  Bytes out{first};
  uint32_t length = static_cast<uint32_t>(body.size());
  do {
    auto byte = static_cast<uint8_t>(length & 0x7F);
    length >>= 7;
    if (length > 0) byte |= 0x80;
    out.push_back(byte);
  } while (length > 0);
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

Bytes str(const std::string& text) {
  Bytes out{static_cast<uint8_t>(text.size() >> 8), static_cast<uint8_t>(text.size())};
  out.insert(out.end(), text.begin(), text.end());
  return out;
}

Bytes bytes_of_text(const std::string& text) { return Bytes(text.begin(), text.end()); }

Bytes cat(std::initializer_list<Bytes> parts) {
  Bytes out;
  for (const auto& part : parts) out.insert(out.end(), part.begin(), part.end());
  return out;
}

// A CONNECT's variable header and payload for MQTT 5.0, with no properties.
Bytes connect5(uint8_t flags, const Bytes& payload) { return packet(0x10, cat({str("MQTT"), {0x05, flags, 0x00, 0x3C, 0x00}, payload})); }
Bytes connect311(uint8_t flags, const Bytes& payload) { return packet(0x10, cat({str("MQTT"), {0x04, flags, 0x00, 0x3C}, payload})); }

// ---- Fixed header -------------------------------------------------------

TEST(MqttPacket, MQTT_2_1_3_1_and_MQTT311_2_2_2_1_and_2_FixedHeaderFlags) {
  // PUBREL, SUBSCRIBE and UNSUBSCRIBE carry 0010; every other type but PUBLISH 0000.
  EXPECT_EQ(refusal(packet(0x60, {0x00, 0x01}), v5), reason::malformed_packet);
  EXPECT_EQ(refusal(packet(0x60, {0x00, 0x01}), v311), reason::malformed_packet);
  EXPECT_NO_THROW(decode(packet(0x62, {0x00, 0x01}), v311));
  EXPECT_EQ(refusal(packet(0x80, cat({{0x00, 0x01}, str("a"), {0x00}})), v311), reason::malformed_packet);
  EXPECT_EQ(refusal(packet(0xA0, cat({{0x00, 0x01}, str("a")})), v311), reason::malformed_packet);
  EXPECT_EQ(refusal(packet(0x41, {0x00, 0x01}), v311), reason::malformed_packet);
  EXPECT_EQ(refusal(packet(0xC1, {}), v5), reason::malformed_packet);
  EXPECT_EQ(refusal(packet(0xE8, {}), v311), reason::malformed_packet);
  EXPECT_EQ(refusal(packet(0x21, {0x00, 0x00}), v311), reason::malformed_packet);
}

TEST(MqttPacket, ReservedTypesAreMalformed) {
  EXPECT_EQ(refusal(packet(0x00, {}), v5), reason::malformed_packet);
  // AUTH is 5.0's alone.
  EXPECT_EQ(refusal(packet(0xF0, {}), v311), reason::malformed_packet);
}

TEST(MqttPacket, MQTT_3_6_1_1_and_MQTT311_3_6_1_1_PubrelFlags) { EXPECT_EQ(refusal(packet(0x63, {0x00, 0x01}), v5), reason::malformed_packet); }

TEST(MqttPacket, MQTT_3_8_1_1_and_MQTT311_3_8_1_1_SubscribeFlags) {
  EXPECT_EQ(refusal(packet(0x80, cat({{0x00, 0x01, 0x00}, str("a"), {0x00}})), v5), reason::malformed_packet);
}

TEST(MqttPacket, MQTT_3_10_1_1_and_MQTT311_3_10_1_1_UnsubscribeFlags) {
  EXPECT_EQ(refusal(packet(0xA0, cat({{0x00, 0x01, 0x00}, str("a")})), v5), reason::malformed_packet);
}

TEST(MqttPacket, MQTT_3_14_1_1_and_MQTT311_3_14_1_1_DisconnectReservedBits) {
  EXPECT_EQ(refusal(packet(0xE1, {}), v5), reason::malformed_packet);
  EXPECT_EQ(refusal(packet(0xE1, {}), v311), reason::malformed_packet);
}

TEST(MqttPacket, MQTT_3_15_1_1_AuthReservedBits) { EXPECT_EQ(refusal(packet(0xF1, {}), v5), reason::malformed_packet); }

TEST(MqttPacket, RemainingLengthMustBeTheBody) {
  Bytes short_body = packet(0xC0, {});
  short_body[1] = 0x01;
  EXPECT_EQ(refusal(short_body, v5), reason::malformed_packet);
  Bytes long_body = packet(0xC0, {});
  long_body.push_back(0x00);
  EXPECT_EQ(refusal(long_body, v5), reason::malformed_packet);
}

TEST(MqttPacket, ReadFrameWaitsForTheHeaderAndBoundsTheSize) {
  EXPECT_FALSE(read_frame(Bytes{}, 1024).has_value());
  EXPECT_FALSE(read_frame(Bytes{0x30}, 1024).has_value());
  EXPECT_FALSE(read_frame(Bytes{0x30, 0x80}, 1024).has_value());
  const auto frame = read_frame(Bytes{0x30, 0x80, 0x01}, 1024);
  ASSERT_TRUE(frame.has_value());
  EXPECT_EQ(frame->first_byte, 0x30);
  EXPECT_EQ(frame->header_size, 3u);
  EXPECT_EQ(frame->remaining_length, 128u);
  EXPECT_EQ(frame->size(), 131u);
  try {
    read_frame(Bytes{0x30, 0x80, 0x08}, 1024);
    FAIL() << "a packet over the maximum was framed";
  } catch (const MalformedPacket& error) {
    EXPECT_EQ(error.reason(), reason::packet_too_large);
  }
  EXPECT_THROW(read_frame(Bytes{0x30, 0x80, 0x80, 0x80, 0x80, 0x01}, varint_max), MalformedPacket);
  EXPECT_THROW(read_frame(Bytes{0x30, 0x80, 0x00}, 1024), MalformedPacket);
}

// ---- Packet identifiers -------------------------------------------------

TEST(MqttPacket, MQTT_2_2_1_2_and_MQTT311_2_3_1_5_NoPacketIdentifierAtQoS0) {
  const Publish qos0{false, QoS::at_most_once, false, "t", 0, {}, Bytes{'x'}};
  EXPECT_EQ(encode(qos0, v311), packet(0x30, cat({str("t"), {'x'}})));
  Publish with_id = qos0;
  with_id.packet_id = 7;
  EXPECT_THROW(encode(with_id, v5), std::invalid_argument);
  // Read at QoS 0, the two bytes after the topic are payload.
  const auto read = std::get<Publish>(decode(packet(0x30, cat({str("t"), {0x00, 0x07}})), v311));
  EXPECT_EQ(read.packet_id, 0);
  EXPECT_EQ(read.payload, (Bytes{0x00, 0x07}));
}

TEST(MqttPacket, MQTT311_2_3_1_1_PacketIdentifiersAreNonZero) {
  EXPECT_EQ(refusal(packet(0x32, cat({str("t"), {0x00, 0x00}})), v311), reason::malformed_packet);
  EXPECT_EQ(refusal(packet(0x82, cat({{0x00, 0x00}, str("a"), {0x00}})), v311), reason::malformed_packet);
  EXPECT_EQ(refusal(packet(0xA2, cat({{0x00, 0x00}, str("a")})), v311), reason::malformed_packet);
  EXPECT_EQ(refusal(packet(0x40, {0x00, 0x00}), v311), reason::malformed_packet);
  EXPECT_THROW(encode(Subscribe{0, {}, {Subscription{"a"}}}, v5), std::invalid_argument);
  EXPECT_THROW(encode(Ack{PacketType::puback, 0}, v5), std::invalid_argument);
}

TEST(MqttPacket, MQTT_2_2_1_5_and_6_AcknowledgementsCarryTheIdentifier) {
  // The codec carries the identifier through both ways, byte for byte.
  EXPECT_EQ(encode(Ack{PacketType::puback, 0x1234}, v5), packet(0x40, {0x12, 0x34}));
  EXPECT_EQ(encode(Ack{PacketType::pubrec, 0x1234}, v5), packet(0x50, {0x12, 0x34}));
  EXPECT_EQ(encode(Ack{PacketType::pubrel, 0x1234}, v5), packet(0x62, {0x12, 0x34}));
  EXPECT_EQ(encode(Ack{PacketType::pubcomp, 0x1234}, v5), packet(0x70, {0x12, 0x34}));
  EXPECT_EQ(encode(Suback{0x1234, {}, {0x00}}, v5), packet(0x90, {0x12, 0x34, 0x00, 0x00}));
  EXPECT_EQ(encode(Unsuback{0x1234, {}, {0x00}}, v5), packet(0xB0, {0x12, 0x34, 0x00, 0x00}));
  EXPECT_EQ(encode(Unsuback{0x1234, {}, {}}, v311), packet(0xB0, {0x12, 0x34}));
  EXPECT_EQ(std::get<Ack>(decode(packet(0x62, {0xBE, 0xEF}), v311)).packet_id, 0xBEEF);
}

// ---- CONNECT ------------------------------------------------------------

TEST(MqttPacket, MQTT_3_1_2_1_and_MQTT311_3_1_2_1_ProtocolName) {
  const auto connect = std::get<Connect>(decode(connect5(0x02, str("c")), v5));
  EXPECT_EQ(connect.version, v5);
  EXPECT_EQ(connect.client_id, "c");
  try {
    decode(packet(0x10, cat({str("MQTX"), {0x05, 0x02, 0x00, 0x3C, 0x00}, str("c")})), v5);
    FAIL() << "a wrong protocol name was read";
  } catch (const UnsupportedVersion& error) {
    EXPECT_EQ(error.reason(), reason::unsupported_protocol_version);
  }
  // MQIsdp names level 3 only.
  EXPECT_THROW(decode(packet(0x10, cat({str("MQIsdp"), {0x04, 0x02, 0x00, 0x3C}, str("c")})), v5), UnsupportedVersion);
}

TEST(MqttPacket, MQTT_3_1_2_2_and_MQTT311_3_1_2_2_UnsupportedLevel) {
  try {
    decode(packet(0x10, cat({str("MQTT"), {0x06, 0x02, 0x00, 0x3C}, str("c")})), v5);
    FAIL() << "level 6 was read";
  } catch (const UnsupportedVersion& error) {
    EXPECT_EQ(error.level(), 6);
  }
  EXPECT_EQ(std::get<Connect>(decode(connect311(0x02, str("c")), v5)).version, v311);
  EXPECT_EQ(std::get<Connect>(decode(packet(0x10, cat({str("MQIsdp"), {0x03, 0x02, 0x00, 0x3C}, str("c")})), v5)).version, v31);
}

TEST(MqttPacket, MQTT_3_1_2_3_and_MQTT311_3_1_2_3_ReservedFlag) {
  EXPECT_EQ(refusal(connect5(0x03, str("c")), v5), reason::malformed_packet);
  EXPECT_EQ(refusal(connect311(0x03, str("c")), v5), reason::malformed_packet);
}

TEST(MqttPacket, MQTT_3_1_2_9_and_MQTT311_3_1_2_9_WillFieldsPresent) {
  // Will Flag 1: will properties, topic and payload follow the client id.
  const auto connect = std::get<Connect>(decode(connect5(0x06, cat({str("c"), {0x00}, str("w"), str("bye")})), v5));
  ASSERT_TRUE(connect.will.has_value());
  EXPECT_EQ(connect.will->topic, "w");
  EXPECT_EQ(connect.will->payload, (Bytes{'b', 'y', 'e'}));
  EXPECT_EQ(refusal(connect5(0x06, cat({str("c"), {0x00}, str("w")})), v5), reason::malformed_packet);
  EXPECT_EQ(refusal(connect311(0x06, str("c")), v5), reason::malformed_packet);
}

TEST(MqttPacket, MQTT_3_1_2_11_and_13_and_MQTT311_3_1_2_11_and_13_and_15_NoWillMeansNoWillQoSOrRetain) {
  EXPECT_EQ(refusal(connect5(0x0A, str("c")), v5), reason::malformed_packet);    // will QoS 1
  EXPECT_EQ(refusal(connect5(0x22, str("c")), v5), reason::malformed_packet);    // will retain
  EXPECT_EQ(refusal(connect311(0x12, str("c")), v5), reason::malformed_packet);  // will QoS 2
  EXPECT_EQ(refusal(connect311(0x22, str("c")), v5), reason::malformed_packet);
}

TEST(MqttPacket, MQTT_3_1_2_12_and_MQTT311_3_1_2_14_WillQoSIsZeroToTwo) {
  for (const uint8_t flags : {uint8_t{0x06}, uint8_t{0x0E}, uint8_t{0x16}}) {
    EXPECT_NO_THROW(decode(connect311(flags, cat({str("c"), str("w"), str("p")})), v5));
  }
  EXPECT_EQ(refusal(connect311(0x1E, cat({str("c"), str("w"), str("p")})), v5), reason::malformed_packet);
  EXPECT_EQ(refusal(connect5(0x1E, cat({str("c"), {0x00}, str("w"), str("p")})), v5), reason::malformed_packet);
  Connect bad{v5, true, 0, "c", Will{"w", {}, static_cast<QoS>(3)}};
  EXPECT_THROW(encode(bad, v5), std::invalid_argument);
}

TEST(MqttPacket, MQTT_3_1_2_16_to_19_and_MQTT311_3_1_2_18_to_21_UserNameAndPasswordFlags) {
  const auto both = std::get<Connect>(decode(connect5(0xC2, cat({str("c"), str("u"), str("p")})), v5));
  EXPECT_EQ(both.username, "u");
  EXPECT_EQ(both.password, (Bytes{'p'}));
  const auto neither = std::get<Connect>(decode(connect5(0x02, str("c")), v5));
  EXPECT_FALSE(neither.username.has_value());
  EXPECT_FALSE(neither.password.has_value());
  // A flag set with its field missing, and a field present with its flag clear.
  EXPECT_EQ(refusal(connect5(0x82, str("c")), v5), reason::malformed_packet);
  EXPECT_EQ(refusal(connect5(0x42, str("c")), v5), reason::malformed_packet);
  EXPECT_EQ(refusal(connect5(0x02, cat({str("c"), str("u")})), v5), reason::malformed_packet);
  EXPECT_EQ(refusal(connect311(0x82, str("c")), v5), reason::malformed_packet);
  EXPECT_EQ(refusal(connect311(0x02, cat({str("c"), str("u")})), v5), reason::malformed_packet);
  // 5.0 allows a password with no user name.
  EXPECT_EQ(std::get<Connect>(decode(connect5(0x42, cat({str("c"), str("p")})), v5)).password, (Bytes{'p'}));
}

TEST(MqttPacket, MQTT311_3_1_2_22_PasswordNeedsAUserName) {
  EXPECT_EQ(refusal(connect311(0x42, cat({str("c"), str("p")})), v5), reason::malformed_packet);
  Connect bad{v311, true, 0, "c", std::nullopt, std::nullopt, Bytes{'p'}};
  EXPECT_THROW(encode(bad, v311), std::invalid_argument);
}

TEST(MqttPacket, MQTT_3_1_3_3_and_4_and_MQTT311_3_1_3_3_and_4_ClientIdFirstAndUtf8) {
  EXPECT_EQ(refusal(connect5(0x02, {}), v5), reason::malformed_packet);
  EXPECT_EQ(refusal(connect5(0x02, Bytes{0x00, 0x01, 0xFF}), v5), reason::malformed_packet);
  EXPECT_EQ(refusal(connect311(0x02, Bytes{0x00, 0x01, 0x00}), v5), reason::malformed_packet);
  // An empty client id is the session's to judge, not malformed.
  EXPECT_EQ(std::get<Connect>(decode(connect311(0x02, str("")), v5)).client_id, "");
}

TEST(MqttPacket, MQTT_3_1_3_11_and_12_and_MQTT311_3_1_3_10_and_11_WillTopicAndUserNameAreUtf8) {
  EXPECT_EQ(refusal(connect311(0x06, cat({str("c"), Bytes{0x00, 0x01, 0xC0}, str("p")})), v5), reason::malformed_packet);
  EXPECT_EQ(refusal(connect5(0x82, cat({str("c"), Bytes{0x00, 0x01, 0xFF}})), v5), reason::malformed_packet);
}

TEST(MqttPacket, MQTT_3_1_4_1_and_MQTT311_3_1_4_1_TrailingBytesAreMalformed) {
  EXPECT_EQ(refusal(connect5(0x02, cat({str("c"), {0x00}})), v5), reason::malformed_packet);
}

TEST(MqttPacket, ConnectWithEverythingMatchesTheSpecificationsBytes) {
  // MQTT 5.0 section 3.1.2.12's example variable header, then a payload.
  Connect c;
  c.version = v5;
  c.clean_start = true;
  c.keep_alive = 10;
  c.client_id = "c";
  c.properties.session_expiry_interval = 10;
  c.username = "u";
  c.password = Bytes{'p'};
  const Bytes expected = packet(0x10, cat({str("MQTT"), {0x05, 0xC2, 0x00, 0x0A, 0x05, 0x11, 0x00, 0x00, 0x00, 0x0A}, str("c"), str("u"), str("p")}));
  EXPECT_EQ(encode(c, v5), expected);
  EXPECT_EQ(std::get<Connect>(decode(expected, v5)), c);
}

TEST(MqttPacket, AuthenticationDataWithoutAMethodIsAProtocolError) {
  const Bytes data_only = packet(0x10, cat({str("MQTT"), {0x05, 0x02, 0x00, 0x3C, 0x04, 0x16, 0x00, 0x01, 'd'}, str("c")}));
  EXPECT_EQ(refusal(data_only, v5), reason::protocol_error);
}

TEST(MqttPacket, PropertiesOnlyOn5) {
  Connect c{v311, true, 0, "c"};
  c.properties.session_expiry_interval = 1;
  EXPECT_THROW(encode(c, v311), std::invalid_argument);
}

// ---- CONNACK ------------------------------------------------------------

TEST(MqttPacket, MQTT_3_2_2_1_ConnackReservedFlagBits) {
  EXPECT_EQ(refusal(packet(0x20, {0x02, 0x00, 0x00}), v5), reason::malformed_packet);
  EXPECT_EQ(refusal(packet(0x20, {0x80, 0x00}), v311), reason::malformed_packet);
  EXPECT_TRUE(std::get<Connack>(decode(packet(0x20, {0x01, 0x00}), v311)).session_present);
}

TEST(MqttPacket, MQTT_3_2_2_6_SessionPresentOnlyWithSuccess) {
  EXPECT_THROW(encode(Connack{true, reason::not_authorized}, v5), std::invalid_argument);
  EXPECT_EQ(refusal(packet(0x20, {0x01, 0x87, 0x00}), v5), reason::protocol_error);
}

TEST(MqttPacket, MQTT_3_2_2_7_and_MQTT311_3_2_2_5_ConnectReasonCodes) {
  EXPECT_EQ(refusal(packet(0x20, {0x00, 0x01, 0x00}), v5), reason::malformed_packet);
  EXPECT_EQ(refusal(packet(0x20, {0x00, 0x06}), v311), reason::malformed_packet);
  EXPECT_EQ(std::get<Connack>(decode(packet(0x20, {0x00, 0x05}), v311)).reason, 5);
  EXPECT_THROW(encode(Connack{false, 0x06}, v311), std::invalid_argument);
  EXPECT_THROW(encode(Connack{false, 0x01}, v5), std::invalid_argument);
  EXPECT_EQ(encode(Connack{false, reason::banned}, v5), packet(0x20, {0x00, 0x8A, 0x00}));
}

// ---- PUBLISH ------------------------------------------------------------

TEST(MqttPacket, MQTT_3_3_1_2_and_MQTT311_3_3_1_2_NoDupAtQoS0) {
  EXPECT_EQ(refusal(packet(0x38, cat({str("t")})), v311), reason::malformed_packet);
  Publish dup{true, QoS::at_most_once, false, "t"};
  EXPECT_THROW(encode(dup, v5), std::invalid_argument);
  const auto again = std::get<Publish>(decode(packet(0x3A, cat({str("t"), {0x00, 0x01}})), v311));
  EXPECT_TRUE(again.dup);
}

TEST(MqttPacket, MQTT_3_3_1_4_and_MQTT311_3_3_1_4_QoSThreeIsMalformed) {
  EXPECT_EQ(refusal(packet(0x36, cat({str("t"), {0x00, 0x01}})), v5), reason::malformed_packet);
  EXPECT_EQ(refusal(packet(0x36, cat({str("t"), {0x00, 0x01}})), v311), reason::malformed_packet);
}

TEST(MqttPacket, MQTT_3_3_2_1_and_MQTT311_3_3_2_1_TopicFirstAndUtf8) {
  const auto publish = std::get<Publish>(decode(packet(0x33, cat({str("a/b"), {0x00, 0x05}, {0x00}, {'x'}})), v5));
  EXPECT_EQ(publish.topic, "a/b");
  EXPECT_EQ(publish.qos, QoS::at_least_once);
  EXPECT_TRUE(publish.retain);
  EXPECT_EQ(publish.packet_id, 5);
  EXPECT_EQ(publish.payload, (Bytes{'x'}));
  EXPECT_EQ(refusal(packet(0x30, Bytes{0x00, 0x01, 0xFF}), v311), reason::malformed_packet);
  EXPECT_EQ(refusal(packet(0x30, Bytes{0x00}), v311), reason::malformed_packet);
}

TEST(MqttPacket, MQTT_3_3_2_2_and_MQTT311_3_3_2_2_NoWildcardInThePublishTopic) {
  EXPECT_EQ(refusal(packet(0x30, cat({str("a/+"), {0x00}})), v5), reason::topic_name_invalid);
  EXPECT_EQ(refusal(packet(0x30, str("a/#")), v311), reason::topic_name_invalid);
  EXPECT_THROW(encode(Publish{false, QoS::at_most_once, false, "a/#"}, v5), std::invalid_argument);
}

TEST(MqttPacket, AnEmptyTopicNeedsATopicAliasOn5AndIsInvalidOn3) {
  EXPECT_EQ(refusal(packet(0x30, cat({str(""), {0x00}})), v5), reason::protocol_error);
  EXPECT_EQ(std::get<Publish>(decode(packet(0x30, cat({str(""), {0x03, 0x23, 0x00, 0x01}})), v5)).properties.topic_alias, 1);
  EXPECT_EQ(refusal(packet(0x30, str("")), v311), reason::topic_name_invalid);
}

TEST(MqttPacket, PublishRoundTripsWithPropertiesAndAnEmptyPayload) {
  Publish p{false, QoS::exactly_once, false, "t", 9};
  p.properties.user_properties = {{"k", "v"}};
  EXPECT_EQ(std::get<Publish>(decode(encode(p, v5), v5)), p);
}

// ---- PUBACK, PUBREC, PUBREL, PUBCOMP ------------------------------------

TEST(MqttPacket, MQTT_3_4_2_1_PubackReasonCodes) {
  EXPECT_EQ(std::get<Ack>(decode(packet(0x40, {0x00, 0x01, 0x10}), v5)).reason, reason::no_matching_subscribers);
  EXPECT_EQ(refusal(packet(0x40, {0x00, 0x01, 0x92}), v5), reason::malformed_packet);
  EXPECT_THROW(encode(Ack{PacketType::puback, 1, reason::packet_identifier_not_found}, v5), std::invalid_argument);
}

TEST(MqttPacket, MQTT_3_5_2_1_PubrecReasonCodes) {
  EXPECT_EQ(std::get<Ack>(decode(packet(0x50, {0x00, 0x01, 0x97, 0x00}), v5)).reason, reason::quota_exceeded);
  EXPECT_EQ(refusal(packet(0x50, {0x00, 0x01, 0x92}), v5), reason::malformed_packet);
}

TEST(MqttPacket, MQTT_3_6_2_1_PubrelReasonCodes) {
  EXPECT_EQ(std::get<Ack>(decode(packet(0x62, {0x00, 0x01, 0x92}), v5)).reason, reason::packet_identifier_not_found);
  EXPECT_EQ(refusal(packet(0x62, {0x00, 0x01, 0x80}), v5), reason::malformed_packet);
}

TEST(MqttPacket, MQTT_3_7_2_1_PubcompReasonCodes) {
  EXPECT_EQ(refusal(packet(0x70, {0x00, 0x01, 0x10}), v5), reason::malformed_packet);
  EXPECT_THROW(encode(Ack{PacketType::pubcomp, 1, reason::unspecified_error}, v5), std::invalid_argument);
}

TEST(MqttPacket, AcknowledgementShortForms) {
  // 5.0: two bytes is Success with no properties; three adds the reason alone.
  EXPECT_EQ(std::get<Ack>(decode(packet(0x40, {0x00, 0x01}), v5)).reason, reason::success);
  EXPECT_EQ(encode(Ack{PacketType::puback, 1, reason::no_matching_subscribers}, v5), packet(0x40, {0x00, 0x01, 0x10}));
  Ack with_reason_string{PacketType::puback, 1, reason::success};
  with_reason_string.properties.reason_string = "r";
  EXPECT_EQ(encode(with_reason_string, v5), packet(0x40, {0x00, 0x01, 0x00, 0x04, 0x1F, 0x00, 0x01, 'r'}));
  // 3.x: exactly the identifier.
  EXPECT_EQ(refusal(packet(0x40, {0x00, 0x01, 0x00}), v311), reason::malformed_packet);
  EXPECT_THROW(encode(Ack{PacketType::puback, 1, reason::unspecified_error}, v311), std::invalid_argument);
}

// ---- SUBSCRIBE and SUBACK -----------------------------------------------

TEST(MqttPacket, MQTT_3_8_3_1_and_MQTT311_3_8_3_1_FiltersAreUtf8) {
  EXPECT_EQ(refusal(packet(0x82, cat({{0x00, 0x01}, {0x00, 0x01, 0xFF}, {0x00}})), v311), reason::malformed_packet);
}

TEST(MqttPacket, MQTT_3_8_3_2_and_MQTT311_3_8_3_3_AtLeastOneFilter) {
  EXPECT_EQ(refusal(packet(0x82, {0x00, 0x01, 0x00}), v5), reason::protocol_error);
  EXPECT_EQ(refusal(packet(0x82, {0x00, 0x01}), v311), reason::protocol_error);
  EXPECT_THROW(encode(Subscribe{1}, v5), std::invalid_argument);
}

TEST(MqttPacket, MQTT_3_8_3_4_NoLocalOnASharedSubscription) {
  EXPECT_EQ(refusal(packet(0x82, cat({{0x00, 0x01, 0x00}, str("$share/g/a"), {0x04}})), v5), reason::protocol_error);
  EXPECT_NO_THROW(decode(packet(0x82, cat({{0x00, 0x01, 0x00}, str("a"), {0x04}})), v5));
}

TEST(MqttPacket, MQTT_3_8_3_5_ReservedOptionBits) {
  EXPECT_EQ(refusal(packet(0x82, cat({{0x00, 0x01, 0x00}, str("a"), {0x40}})), v5), reason::malformed_packet);
  EXPECT_EQ(refusal(packet(0x82, cat({{0x00, 0x01, 0x00}, str("a"), {0x03}})), v5), reason::malformed_packet);
  EXPECT_EQ(refusal(packet(0x82, cat({{0x00, 0x01, 0x00}, str("a"), {0x30}})), v5), reason::protocol_error);
}

TEST(MqttPacket, MQTT311_3_8_3_4_ReservedBitsAndQoS) {
  EXPECT_EQ(refusal(packet(0x82, cat({{0x00, 0x01}, str("a"), {0x04}})), v311), reason::malformed_packet);
  EXPECT_EQ(refusal(packet(0x82, cat({{0x00, 0x01}, str("a"), {0x03}})), v311), reason::malformed_packet);
  EXPECT_THROW(encode(Subscribe{1, {}, {Subscription{"a", QoS::at_most_once, true}}}, v311), std::invalid_argument);
}

TEST(MqttPacket, SubscribeOptionsRoundTrip) {
  Subscribe s{3};
  s.properties.subscription_identifiers = {42};
  s.subscriptions = {Subscription{"a/+", QoS::exactly_once, true, true, 2}, Subscription{"b", QoS::at_least_once}};
  const Bytes bytes = encode(s, v5);
  EXPECT_EQ(bytes, packet(0x82, cat({{0x00, 0x03, 0x02, 0x0B, 0x2A}, str("a/+"), {0x2E}, str("b"), {0x01}})));
  EXPECT_EQ(std::get<Subscribe>(decode(bytes, v5)), s);
}

TEST(MqttPacket, MQTT_3_9_3_2_and_MQTT311_3_9_3_2_SubackReasonCodes) {
  EXPECT_EQ(refusal(packet(0x90, {0x00, 0x01, 0x00, 0x03}), v5), reason::malformed_packet);
  EXPECT_EQ(refusal(packet(0x90, {0x00, 0x01, 0x87}), v311), reason::malformed_packet);
  EXPECT_EQ(std::get<Suback>(decode(packet(0x90, {0x00, 0x01, 0x80}), v311)).reasons, (std::vector<uint8_t>{0x80}));
  EXPECT_EQ(std::get<Suback>(decode(packet(0x90, {0x00, 0x01, 0x00, 0xA2, 0x02}), v5)).reasons, (std::vector<uint8_t>{0xA2, 0x02}));
  EXPECT_THROW(encode(Suback{1, {}, {reason::not_authorized}}, v311), std::invalid_argument);
}

// ---- UNSUBSCRIBE and UNSUBACK -------------------------------------------

TEST(MqttPacket, MQTT_3_10_3_1_and_MQTT311_3_10_3_1_FiltersAreUtf8) {
  EXPECT_EQ(refusal(packet(0xA2, cat({{0x00, 0x01}, {0x00, 0x01, 0xFF}})), v311), reason::malformed_packet);
}

TEST(MqttPacket, MQTT_3_10_3_2_and_MQTT311_3_10_3_2_AtLeastOneFilter) {
  EXPECT_EQ(refusal(packet(0xA2, {0x00, 0x01, 0x00}), v5), reason::protocol_error);
  EXPECT_EQ(refusal(packet(0xA2, {0x00, 0x01}), v311), reason::protocol_error);
  EXPECT_EQ(std::get<Unsubscribe>(decode(packet(0xA2, cat({{0x00, 0x01}, str("a"), str("b/#")})), v311)).filters, (std::vector<std::string>{"a", "b/#"}));
}

TEST(MqttPacket, MQTT_3_11_3_2_UnsubackReasonCodes) {
  EXPECT_EQ(std::get<Unsuback>(decode(packet(0xB0, {0x00, 0x01, 0x00, 0x11}), v5)).reasons, (std::vector<uint8_t>{0x11}));
  EXPECT_EQ(refusal(packet(0xB0, {0x00, 0x01, 0x00, 0x01}), v5), reason::malformed_packet);
  EXPECT_EQ(refusal(packet(0xB0, {0x00, 0x01, 0x00}), v5), reason::protocol_error);
  EXPECT_EQ(refusal(packet(0xB0, {0x00, 0x01, 0x00}), v311), reason::malformed_packet);
}

// ---- PINGREQ, PINGRESP, DISCONNECT, AUTH --------------------------------

TEST(MqttPacket, PingsAreTwoBytes) {
  EXPECT_EQ(encode(Pingreq{}, v311), (Bytes{0xC0, 0x00}));
  EXPECT_EQ(encode(Pingresp{}, v5), (Bytes{0xD0, 0x00}));
  EXPECT_EQ(refusal(Bytes{0xC0, 0x01, 0x00}, v5), reason::malformed_packet);
}

TEST(MqttPacket, MQTT_3_14_2_1_DisconnectReasonCodes) {
  EXPECT_EQ(std::get<Disconnect>(decode(Bytes{0xE0, 0x00}, v5)).reason, reason::normal_disconnection);
  EXPECT_EQ(std::get<Disconnect>(decode(Bytes{0xE0, 0x01, 0x8E}, v5)).reason, reason::session_taken_over);
  EXPECT_EQ(refusal(Bytes{0xE0, 0x01, 0x01}, v5), reason::malformed_packet);
  EXPECT_THROW(encode(Disconnect{reason::continue_authentication}, v5), std::invalid_argument);
  EXPECT_EQ(refusal(Bytes{0xE0, 0x01, 0x00}, v311), reason::malformed_packet);
  EXPECT_EQ(encode(Disconnect{}, v311), (Bytes{0xE0, 0x00}));
  EXPECT_EQ(encode(Disconnect{}, v5), (Bytes{0xE0, 0x00}));
  EXPECT_EQ(encode(Disconnect{reason::server_busy}, v5), (Bytes{0xE0, 0x01, 0x89}));
}

TEST(MqttPacket, MQTT_3_15_2_1_AuthReasonCodes) {
  Auth a{reason::continue_authentication};
  a.properties.authentication_method = "m";
  const Bytes bytes = encode(a, v5);
  EXPECT_EQ(bytes, packet(0xF0, {0x18, 0x04, 0x15, 0x00, 0x01, 'm'}));
  EXPECT_EQ(std::get<Auth>(decode(bytes, v5)), a);
  EXPECT_EQ(std::get<Auth>(decode(Bytes{0xF0, 0x00}, v5)).reason, reason::success);
  EXPECT_EQ(refusal(packet(0xF0, {0x80, 0x00}), v5), reason::malformed_packet);
  EXPECT_THROW(encode(Auth{reason::unspecified_error}, v5), std::invalid_argument);
  EXPECT_THROW(encode(Auth{}, v311), std::invalid_argument);
}

// ---- Encoding what the tests above only decode ---------------------------

TEST(MqttPacket, AWillIsEncodedInTheFlagsAndThePayloadAtEachVersion) {
  for (const Version version : {v5, v311, v31}) {
    Connect c{version, false, 60, "c"};
    c.will = Will{"gone", bytes_of_text("bye"), QoS::exactly_once, true};
    if (version == v5) {
      c.will->properties.will_delay_interval = 5;
      c.will->properties.content_type = "text/plain";
    }
    const Bytes bytes = encode(c, version);
    // Will flag 0x04, will QoS 2 in bits 3-4, will retain 0x20; clean start off.
    const size_t flags_at = version == v31 ? 2 + 8 + 1 : 2 + 6 + 1;
    EXPECT_EQ(bytes[flags_at], 0x34);
    EXPECT_EQ(std::get<Connect>(decode(bytes, version)), c);
  }
}

TEST(MqttPacket, UnsubscribeIsEncodedAtEachVersion) {
  Unsubscribe u{9, {}, {"a/#", "b"}};
  EXPECT_EQ(encode(u, v311), packet(0xA2, cat({{0x00, 0x09}, str("a/#"), str("b")})));
  EXPECT_EQ(encode(u, v5), packet(0xA2, cat({{0x00, 0x09, 0x00}, str("a/#"), str("b")})));
  u.properties.user_properties = {{"k", "v"}};
  EXPECT_EQ(std::get<Unsubscribe>(decode(encode(u, v5), v5)), u);
  EXPECT_THROW(encode(Unsubscribe{9}, v5), std::invalid_argument);
  EXPECT_THROW(encode(Unsubscribe{0, {}, {"a"}}, v5), std::invalid_argument);
}

TEST(MqttPacket, PingsDecode) {
  EXPECT_TRUE(std::holds_alternative<Pingreq>(decode(Bytes{0xC0, 0x00}, v311)));
  EXPECT_TRUE(std::holds_alternative<Pingresp>(decode(Bytes{0xD0, 0x00}, v5)));
  EXPECT_EQ(type_of(Packet{Unsubscribe{1, {}, {"a"}}}), PacketType::unsubscribe);
}

TEST(MqttPacket, PubrelAndPubcompCarryTheirProperties) {
  for (const PacketType type : {PacketType::pubrel, PacketType::pubcomp}) {
    Ack a{type, 3, reason::packet_identifier_not_found};
    a.properties.reason_string = "gone";
    a.properties.user_properties = {{"k", "v"}};
    EXPECT_EQ(std::get<Ack>(decode(encode(a, v5), v5)), a);
  }
}

// ---- MQTT 3.1, which numbers no statements (docs/conformance.md) ---------

TEST(MqttPacket, MQTT31_ConnackHasNoSessionPresentFlag) {
  EXPECT_EQ(encode(Connack{false, reason::success}, v31), packet(0x20, {0x00, 0x00}));
  EXPECT_THROW(encode(Connack{true, reason::success}, v31), std::invalid_argument);
  EXPECT_EQ(refusal(packet(0x20, {0x01, 0x00}), v31), reason::malformed_packet);
}

TEST(MqttPacket, MQTT31_SubackHasNoFailureCode) {
  EXPECT_EQ(encode(Suback{1, {}, {0x00, 0x02}}, v31), packet(0x90, {0x00, 0x01, 0x00, 0x02}));
  EXPECT_THROW(encode(Suback{1, {}, {0x80}}, v31), std::invalid_argument);
  EXPECT_EQ(refusal(packet(0x90, {0x00, 0x01, 0x80}), v31), reason::malformed_packet);
  // 3.1.1 has it.
  EXPECT_EQ(encode(Suback{1, {}, {0x80}}, v311), packet(0x90, {0x00, 0x01, 0x80}));
}

TEST(MqttPacket, MQTT31_ConnectSpeaksMQIsdpLevel3) {
  const Connect c{v31, true, 10, "old-client"};
  const Bytes bytes = encode(c, v31);
  EXPECT_EQ(bytes, packet(0x10, cat({str("MQIsdp"), {0x03, 0x02, 0x00, 0x0A}, str("old-client")})));
  EXPECT_EQ(std::get<Connect>(decode(bytes, v5)), c);
}

TEST(MqttPacket, TypeOf) {
  EXPECT_EQ(type_of(Packet{Ack{PacketType::pubrel, 1}}), PacketType::pubrel);
  EXPECT_EQ(type_of(Packet{Connect{}}), PacketType::connect);
  EXPECT_EQ(type_of(Packet{Auth{}}), PacketType::auth);
}

}  // namespace
}  // namespace lir::mqtt
