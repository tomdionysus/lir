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

#include "lir/mqtt/properties.h"

// MQTT 5.0 section 2.2.2: the property block. Bytes are written out as the
// specification lays them down, never only round-tripped through the codec.
namespace lir::mqtt {
namespace {

using dannan::net::Reader;
using dannan::net::Writer;

Properties read(const Bytes& bytes, PropertyPlace place) {
  Reader reader(bytes);
  Properties out = read_properties(reader, place);
  EXPECT_EQ(reader.remaining(), 0u);
  return out;
}

uint8_t refusal(const Bytes& bytes, PropertyPlace place) {
  try {
    read(bytes, place);
  } catch (const MalformedPacket& error) {
    return error.reason();
  }
  return reason::success;
}

Bytes write(const Properties& properties, PropertyPlace place) {
  Writer writer;
  write_properties(writer, properties, place);
  return writer.take();
}

TEST(MqttProperties, MQTT_2_2_2_1_LengthIsAVariableByteIntegerOfWhatFollows) {
  EXPECT_EQ(read({0x00}, PropertyPlace::publish), Properties{});
  // Message Expiry Interval 10: length 5, then 0x02 and four bytes.
  const Properties expiry = read({0x05, 0x02, 0x00, 0x00, 0x00, 0x0A}, PropertyPlace::publish);
  EXPECT_EQ(expiry.message_expiry_interval, 10u);
  EXPECT_EQ(write(expiry, PropertyPlace::publish), (Bytes{0x05, 0x02, 0x00, 0x00, 0x00, 0x0A}));
  EXPECT_EQ(properties_size(expiry), 6u);
  // A length past what follows, or short of a whole property.
  EXPECT_EQ(refusal({0x06, 0x02, 0x00, 0x00, 0x00, 0x0A}, PropertyPlace::publish), reason::malformed_packet);
  EXPECT_EQ(refusal({0x03, 0x02, 0x00, 0x00}, PropertyPlace::publish), reason::malformed_packet);
  // A length that needs two bytes: 130 bytes of correlation data.
  Properties big;
  big.correlation_data = Bytes(127, 0x55);
  const Bytes encoded = write(big, PropertyPlace::publish);
  EXPECT_EQ(encoded[0], 0x82);
  EXPECT_EQ(encoded[1], 0x01);
  EXPECT_EQ(read(encoded, PropertyPlace::publish), big);
}

TEST(MqttProperties, MQTT_1_5_7_1_StringPairsAreBothUtf8) {
  // User Property "a" = "b".
  const Properties pair = read({0x07, 0x26, 0x00, 0x01, 'a', 0x00, 0x01, 'b'}, PropertyPlace::publish);
  ASSERT_EQ(pair.user_properties.size(), 1u);
  EXPECT_EQ(pair.user_properties[0], (std::pair<std::string, std::string>{"a", "b"}));
  EXPECT_EQ(refusal({0x07, 0x26, 0x00, 0x01, 0xFF, 0x00, 0x01, 'b'}, PropertyPlace::publish), reason::malformed_packet);
  EXPECT_EQ(refusal({0x07, 0x26, 0x00, 0x01, 'a', 0x00, 0x01, 0x00}, PropertyPlace::publish), reason::malformed_packet);
  Properties bad;
  bad.user_properties = {{"a", std::string("\xC0\x80", 2)}};
  Writer writer;
  EXPECT_THROW(write_properties(writer, bad, PropertyPlace::publish), std::invalid_argument);
}

TEST(MqttProperties, UserPropertiesKeepOrderAndRepeats) {
  Properties many;
  many.user_properties = {{"k", "1"}, {"a", "2"}, {"k", "3"}};
  EXPECT_EQ(read(write(many, PropertyPlace::publish), PropertyPlace::publish).user_properties, many.user_properties);
}

TEST(MqttProperties, APropertyOutOfItsPlaceIsAProtocolError) {
  // Topic Alias belongs to PUBLISH only; Session Expiry Interval never to PUBLISH.
  EXPECT_EQ(refusal({0x03, 0x23, 0x00, 0x01}, PropertyPlace::connect), reason::protocol_error);
  EXPECT_EQ(refusal({0x05, 0x11, 0x00, 0x00, 0x00, 0x01}, PropertyPlace::publish), reason::protocol_error);
  EXPECT_EQ(refusal({0x02, 0x24, 0x01}, PropertyPlace::connect), reason::protocol_error);
  EXPECT_EQ(refusal({0x05, 0x18, 0x00, 0x00, 0x00, 0x01}, PropertyPlace::connect), reason::protocol_error);
  // Will Delay Interval is a will's.
  EXPECT_EQ(read({0x05, 0x18, 0x00, 0x00, 0x00, 0x01}, PropertyPlace::will).will_delay_interval, 1u);
  Properties alias;
  alias.topic_alias = 1;
  Writer writer;
  EXPECT_THROW(write_properties(writer, alias, PropertyPlace::connect), std::invalid_argument);
}

TEST(MqttProperties, AnUnknownIdentifierIsMalformed) { EXPECT_EQ(refusal({0x02, 0x7F, 0x00}, PropertyPlace::publish), reason::malformed_packet); }

TEST(MqttProperties, AnyPropertyButTheRepeatableOnesTwiceIsAProtocolError) {
  EXPECT_EQ(refusal({0x04, 0x01, 0x00, 0x01, 0x01}, PropertyPlace::publish), reason::protocol_error);
  // A Subscription Identifier: at most one on a SUBSCRIBE, several on a PUBLISH.
  EXPECT_EQ(refusal({0x04, 0x0B, 0x01, 0x0B, 0x02}, PropertyPlace::subscribe), reason::protocol_error);
  EXPECT_EQ(read({0x04, 0x0B, 0x01, 0x0B, 0x02}, PropertyPlace::publish).subscription_identifiers, (std::vector<uint32_t>{1, 2}));
}

TEST(MqttProperties, ValuesOutOfRangeAreProtocolErrors) {
  // Payload Format Indicator, Maximum QoS and the CONNACK flags are 0 or 1.
  EXPECT_EQ(refusal({0x02, 0x01, 0x02}, PropertyPlace::publish), reason::protocol_error);
  EXPECT_EQ(refusal({0x02, 0x24, 0x02}, PropertyPlace::connack), reason::protocol_error);
  EXPECT_EQ(refusal({0x02, 0x25, 0x02}, PropertyPlace::connack), reason::protocol_error);
  EXPECT_EQ(refusal({0x02, 0x17, 0x02}, PropertyPlace::connect), reason::protocol_error);
  EXPECT_EQ(refusal({0x02, 0x19, 0x02}, PropertyPlace::connect), reason::protocol_error);
  // Receive Maximum and Maximum Packet Size may not be zero.
  EXPECT_EQ(refusal({0x03, 0x21, 0x00, 0x00}, PropertyPlace::connect), reason::protocol_error);
  EXPECT_EQ(refusal({0x05, 0x27, 0x00, 0x00, 0x00, 0x00}, PropertyPlace::connect), reason::protocol_error);
  // A Subscription Identifier of 0.
  EXPECT_EQ(refusal({0x02, 0x0B, 0x00}, PropertyPlace::subscribe), reason::protocol_error);
}

TEST(MqttProperties, MQTT_3_3_2_8_TopicAliasZeroIsRefused) {
  EXPECT_EQ(refusal({0x03, 0x23, 0x00, 0x00}, PropertyPlace::publish), reason::topic_alias_invalid);
  Properties zero;
  zero.topic_alias = 0;
  Writer writer;
  EXPECT_THROW(write_properties(writer, zero, PropertyPlace::publish), std::invalid_argument);
}

TEST(MqttProperties, MQTT_3_3_2_13_and_14_ResponseTopicIsATopicName) {
  EXPECT_EQ(read({0x04, 0x08, 0x00, 0x01, 'r'}, PropertyPlace::publish).response_topic, "r");
  EXPECT_EQ(refusal({0x04, 0x08, 0x00, 0x01, '#'}, PropertyPlace::publish), reason::protocol_error);
  EXPECT_EQ(refusal({0x04, 0x08, 0x00, 0x01, '+'}, PropertyPlace::will), reason::protocol_error);
  EXPECT_EQ(refusal({0x04, 0x08, 0x00, 0x01, 0xFF}, PropertyPlace::publish), reason::malformed_packet);
}

TEST(MqttProperties, MQTT_3_3_2_19_ContentTypeIsUtf8) {
  EXPECT_EQ(read({0x04, 0x03, 0x00, 0x01, 't'}, PropertyPlace::publish).content_type, "t");
  EXPECT_EQ(refusal({0x04, 0x03, 0x00, 0x01, 0xFE}, PropertyPlace::publish), reason::malformed_packet);
}

TEST(MqttProperties, EveryPropertyRoundTripsAtItsPlace) {
  Properties connack;
  connack.session_expiry_interval = 60;
  connack.assigned_client_identifier = "id";
  connack.server_keep_alive = 30;
  connack.authentication_method = "m";
  connack.authentication_data = Bytes{1, 2};
  connack.response_information = "ri";
  connack.server_reference = "other:1883";
  connack.reason_string = "why";
  connack.receive_maximum = 10;
  connack.topic_alias_maximum = 5;
  connack.maximum_qos = 1;
  connack.retain_available = 0;
  connack.user_properties = {{"x", "y"}};
  connack.maximum_packet_size = 1024;
  connack.wildcard_subscription_available = 1;
  connack.subscription_identifier_available = 0;
  connack.shared_subscription_available = 1;
  EXPECT_EQ(read(write(connack, PropertyPlace::connack), PropertyPlace::connack), connack);

  Properties publish;
  publish.payload_format_indicator = 1;
  publish.message_expiry_interval = 100;
  publish.content_type = "text/plain";
  publish.response_topic = "reply/here";
  publish.correlation_data = Bytes{9, 8, 7};
  publish.subscription_identifiers = {1, varint_max};
  publish.topic_alias = 3;
  EXPECT_EQ(read(write(publish, PropertyPlace::publish), PropertyPlace::publish), publish);
  EXPECT_FALSE(publish.empty());
  EXPECT_TRUE(Properties{}.empty());

  Properties connect;
  connect.session_expiry_interval = 0;
  connect.authentication_method = "m";
  connect.request_problem_information = 0;
  connect.request_response_information = 1;
  connect.receive_maximum = 100;
  connect.maximum_packet_size = 65536;
  connect.topic_alias_maximum = 0;
  EXPECT_EQ(read(write(connect, PropertyPlace::connect), PropertyPlace::connect), connect);

  // The size is what is written, for every property.
  for (const auto& [properties, place] :
       {std::pair{connack, PropertyPlace::connack}, std::pair{publish, PropertyPlace::publish}, std::pair{connect, PropertyPlace::connect}}) {
    EXPECT_EQ(properties_size(properties), write(properties, place).size());
  }
  EXPECT_EQ(properties_size(Properties{}), 1u);
}

}  // namespace
}  // namespace lir::mqtt
