//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "lir/mqtt/wire.h"

// MQTT 5.0 section 1.5 and 3.1.1 section 1.5, statement by statement. Each
// test is named by the identifier it proves (docs/conformance.md).
namespace lir::mqtt {
namespace {

using dannan::net::Reader;
using dannan::net::Writer;

Bytes string_bytes(const std::string& text) {
  Bytes out{static_cast<uint8_t>(text.size() >> 8), static_cast<uint8_t>(text.size())};
  out.insert(out.end(), text.begin(), text.end());
  return out;
}

std::string read_string_from(const Bytes& bytes) {
  Reader reader(bytes);
  return read_string(reader);
}

uint8_t malformed_reason_reading(const Bytes& bytes) {
  try {
    read_string_from(bytes);
  } catch (const MalformedPacket& error) {
    return error.reason();
  }
  return reason::success;
}

// Ill-formed UTF-8 by RFC 3629: each must be refused on read and on write.
const std::vector<std::string> ill_formed = {
    std::string("\x80", 1),              // a continuation byte alone
    std::string("\xC3", 1),              // a lead byte with nothing after it
    std::string("\xC3\x28", 2),          // a lead byte followed by a non-continuation
    std::string("\xC0\x80", 2),          // an overlong form of U+0000
    std::string("\xC1\xBF", 2),          // an overlong two-byte form
    std::string("\xE0\x80\xAF", 3),      // an overlong three-byte form
    std::string("\xF0\x80\x80\xAF", 4),  // an overlong four-byte form
    std::string("\xF4\x90\x80\x80", 4),  // past U+10FFFF
    std::string("\xF5\x80\x80\x80", 4),  // a lead byte RFC 3629 forbids
    std::string("\xFF", 1),
};

const std::vector<std::string> surrogates = {
    std::string("\xED\xA0\x80", 3),  // U+D800
    std::string("\xED\xBF\xBF", 3),  // U+DFFF
    std::string("\xED\xAD\xBF", 3),  // U+DB7F
};

TEST(MqttWire, MQTT_1_5_4_1_IllFormedUtf8IsRefused) {
  for (const auto& text : ill_formed) {
    EXPECT_FALSE(valid_utf8(text)) << "accepted ill-formed bytes";
    EXPECT_EQ(malformed_reason_reading(string_bytes(text)), reason::malformed_packet);
    Writer writer;
    EXPECT_THROW(write_string(writer, text), std::invalid_argument);
  }
}

TEST(MqttWire, MQTT_1_5_4_1_SurrogatesAreRefused) {
  for (const auto& text : surrogates) {
    EXPECT_FALSE(valid_utf8(text));
    EXPECT_EQ(malformed_reason_reading(string_bytes(text)), reason::malformed_packet);
    Writer writer;
    EXPECT_THROW(write_string(writer, text), std::invalid_argument);
  }
}

TEST(MqttWire, MQTT_1_5_4_1_WellFormedUtf8IsAccepted) {
  // One code point of each length, the largest scalar value, and the last
  // before and first after the surrogate range.
  const std::vector<std::string> good = {
      "a", "\xC3\xA9", "\xE2\x82\xAC", "\xF0\x9F\x98\x80", "\xF1\x80\x80\x80", "\xF4\x8F\xBF\xBF", "\xED\x9F\xBF", "\xEE\x80\x80", "",
  };
  for (const auto& text : good) {
    EXPECT_TRUE(valid_utf8(text));
    EXPECT_EQ(read_string_from(string_bytes(text)), text);
  }
}

TEST(MqttWire, MQTT_1_5_4_2_NullCharacterIsRefused) {
  const std::string with_null("a\0b", 3);
  EXPECT_FALSE(valid_utf8(with_null));
  EXPECT_EQ(malformed_reason_reading(string_bytes(with_null)), reason::malformed_packet);
  Writer writer;
  EXPECT_THROW(write_string(writer, with_null), std::invalid_argument);
}

TEST(MqttWire, MQTT_1_5_4_3_ByteOrderMarkIsKept) {
  const std::string bom = "\xEF\xBB\xBF";
  for (const auto& text : {bom + "topic", "to" + bom + "pic", "topic" + bom, bom}) {
    EXPECT_EQ(read_string_from(string_bytes(text)), text);
    Writer writer;
    write_string(writer, text);
    EXPECT_EQ(writer.data(), string_bytes(text));
  }
}

// 3.1.1 asks the same of its strings, with the null character's close.
TEST(MqttWire, MQTT311_1_5_3_1_IllFormedUtf8AndSurrogatesAreRefused) {
  for (const auto& text : ill_formed) EXPECT_EQ(malformed_reason_reading(string_bytes(text)), reason::malformed_packet);
  for (const auto& text : surrogates) EXPECT_EQ(malformed_reason_reading(string_bytes(text)), reason::malformed_packet);
}

TEST(MqttWire, MQTT311_1_5_3_2_NullCharacterIsRefused) { EXPECT_EQ(malformed_reason_reading(string_bytes(std::string("\0", 1))), reason::malformed_packet); }

// A SHOULD NOT with a MAY close: Lir takes the discouraged code points.
TEST(MqttWire, MQTT311_1_5_3_3_DiscouragedCodePointsPass) {
  for (const std::string text : {"\x01", "\x1F", "\x7F", "\xC2\x80", "\xC2\x9F", "\xEF\xBF\xBF"}) {
    EXPECT_TRUE(valid_utf8(text));
    EXPECT_EQ(read_string_from(string_bytes(text)), text);
  }
}

TEST(MqttWire, StringLengthIsTwoBytesAndBounded) {
  Writer writer;
  write_string(writer, std::string(65535, 'a'));
  EXPECT_EQ(writer.data().size(), 65537u);
  EXPECT_EQ(writer.data()[0], 0xFF);
  EXPECT_EQ(writer.data()[1], 0xFF);
  Writer over;
  EXPECT_THROW(write_string(over, std::string(65536, 'a')), std::length_error);
  // A length that runs past the packet.
  EXPECT_EQ(malformed_reason_reading(Bytes{0x00, 0x05, 'a', 'b'}), reason::malformed_packet);
  EXPECT_EQ(malformed_reason_reading(Bytes{0x00}), reason::malformed_packet);
}

TEST(MqttWire, BinaryDataRoundTrips) {
  const Bytes data{0x00, 0xFF, 0xC0, 0x80};
  Writer writer;
  write_binary(writer, data);
  EXPECT_EQ(writer.data(), (Bytes{0x00, 0x04, 0x00, 0xFF, 0xC0, 0x80}));
  Reader reader(writer.data());
  EXPECT_EQ(read_binary(reader), data);
  Writer over;
  EXPECT_THROW(write_binary(over, Bytes(65536, 0)), std::length_error);
  const Bytes short_binary{0x00, 0x03, 0x01};
  Reader short_reader(short_binary);
  EXPECT_THROW(read_binary(short_reader), MalformedPacket);
}

// The specification's own table of boundaries (section 1.5.5).
TEST(MqttWire, MQTT_1_5_5_1_VariableByteIntegerBoundaries) {
  const std::vector<std::pair<uint32_t, Bytes>> table = {
      {0, {0x00}},
      {127, {0x7F}},
      {128, {0x80, 0x01}},
      {16383, {0xFF, 0x7F}},
      {16384, {0x80, 0x80, 0x01}},
      {2097151, {0xFF, 0xFF, 0x7F}},
      {2097152, {0x80, 0x80, 0x80, 0x01}},
      {268435455, {0xFF, 0xFF, 0xFF, 0x7F}},
  };
  for (const auto& [value, bytes] : table) {
    Writer writer;
    write_varint(writer, value);
    EXPECT_EQ(writer.data(), bytes) << value;
    EXPECT_EQ(varint_size(value), bytes.size()) << value;
    Reader reader(bytes);
    EXPECT_EQ(read_varint(reader), value);
    EXPECT_EQ(reader.remaining(), 0u);
  }
  Writer over;
  EXPECT_THROW(write_varint(over, varint_max + 1), std::invalid_argument);
}

TEST(MqttWire, MQTT_1_5_5_1_MoreThanFourBytesIsMalformed) {
  const Bytes five{0x80, 0x80, 0x80, 0x80, 0x01};
  Reader reader(five);
  try {
    read_varint(reader);
    FAIL() << "a five-byte integer was read";
  } catch (const MalformedPacket& error) {
    EXPECT_EQ(error.reason(), reason::malformed_packet);
  }
}

TEST(MqttWire, MQTT_1_5_5_1_NotTheMinimumNumberOfBytesIsMalformed) {
  for (const Bytes& bytes : {Bytes{0x80, 0x00}, Bytes{0xFF, 0x00}, Bytes{0x80, 0x80, 0x00}, Bytes{0x80, 0x80, 0x80, 0x00}}) {
    Reader reader(bytes);
    EXPECT_THROW(read_varint(reader), MalformedPacket);
  }
}

TEST(MqttWire, MQTT_1_5_5_1_TruncatedIsMalformed) {
  const Bytes truncated{0x80, 0x80};
  Reader reader(truncated);
  EXPECT_THROW(read_varint(reader), MalformedPacket);
}

TEST(MqttWire, ProtocolErrorCarriesItsReason) {
  try {
    protocol_error("x");
  } catch (const MalformedPacket& error) {
    EXPECT_EQ(error.reason(), reason::protocol_error);
  }
  try {
    malformed("x");
  } catch (const MalformedPacket& error) {
    EXPECT_EQ(error.reason(), reason::malformed_packet);
  }
}

}  // namespace
}  // namespace lir::mqtt
