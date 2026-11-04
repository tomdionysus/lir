//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <gtest/gtest.h>

#include "lir/mqtt/wire.h"

namespace lir::mqtt {
namespace {

using dannan::net::Reader;
using dannan::net::Writer;

Bytes varint(uint32_t value) {
  Writer w;
  write_varint(w, value);
  return w.take();
}

uint32_t read_back(const Bytes& bytes) {
  Reader r(bytes);
  const uint32_t value = read_varint(r);
  EXPECT_EQ(r.remaining(), 0u);
  return value;
}

// MQTT 5.0 table 1-1: the size of the variable byte integer at each edge.
TEST(MqttWire, VarintBoundaries) {
  const std::pair<uint32_t, size_t> edges[] = {{0, 1}, {127, 1}, {128, 2}, {16383, 2}, {16384, 3}, {2097151, 3}, {2097152, 4}, {268435455, 4}};
  for (const auto& [value, size] : edges) {
    const Bytes bytes = varint(value);
    EXPECT_EQ(bytes.size(), size) << value;
    EXPECT_EQ(varint_size(value), size) << value;
    EXPECT_EQ(read_back(bytes), value);
  }
  EXPECT_EQ(varint(268435455), (Bytes{0xFF, 0xFF, 0xFF, 0x7F}));
}

TEST(MqttWire, VarintOverMaximumRefusedOnWrite) {
  Writer w;
  EXPECT_THROW(write_varint(w, 268435456), std::length_error);
}

TEST(MqttWire, VarintLongerThanFourBytesIsMalformed) {
  const Bytes bytes{0x80, 0x80, 0x80, 0x80, 0x01};
  Reader r(bytes);
  EXPECT_THROW(read_varint(r), MalformedPacket);
}

TEST(MqttWire, VarintNotInShortestFormIsMalformed) {
  const Bytes bytes{0x80, 0x00};
  Reader r(bytes);
  EXPECT_THROW(read_varint(r), MalformedPacket);
}

TEST(MqttWire, VarintTruncatedIsMalformed) {
  const Bytes bytes{0x80};
  Reader r(bytes);
  EXPECT_THROW(read_varint(r), MalformedPacket);
}

// Section 1.5.4: U+0000 and surrogates MUST NOT appear; ill-formed UTF-8
// MUST be refused.
TEST(MqttWire, Utf8Rules) {
  EXPECT_TRUE(valid_utf8(""));
  EXPECT_TRUE(valid_utf8("a/b/c"));
  // The specification's own example: "A" then U+2A6D4 (figure 1-1).
  EXPECT_TRUE(valid_utf8("A\xF0\xAA\x9B\x94"));
  EXPECT_TRUE(valid_utf8("\xC3\xA9"));
  EXPECT_FALSE(valid_utf8(std::string("a\0b", 3)));
  EXPECT_FALSE(valid_utf8("\xED\xA0\x80"));      // U+D800
  EXPECT_FALSE(valid_utf8("\xC0\x80"));          // overlong NUL
  EXPECT_FALSE(valid_utf8("\xE0\x80\xAF"));      // overlong '/'
  EXPECT_FALSE(valid_utf8("\xF4\x90\x80\x80"));  // past U+10FFFF
  EXPECT_FALSE(valid_utf8("\xC3"));              // truncated
  EXPECT_FALSE(valid_utf8("\x80"));              // stray continuation
  EXPECT_FALSE(valid_utf8("\xFF"));
}

TEST(MqttWire, StringRoundTrip) {
  Writer w;
  write_string(w, "hello");
  EXPECT_EQ(w.data(), (Bytes{0x00, 0x05, 'h', 'e', 'l', 'l', 'o'}));
  Reader r(w.data());
  EXPECT_EQ(read_string(r), "hello");
}

TEST(MqttWire, InvalidStringRefusedBothWays) {
  Writer w;
  EXPECT_THROW(write_string(w, "\xED\xA0\x80"), std::invalid_argument);
  const Bytes bytes{0x00, 0x03, 0xED, 0xA0, 0x80};
  Reader r(bytes);
  EXPECT_THROW(read_string(r), MalformedPacket);
}

TEST(MqttWire, StringPastPacketIsMalformed) {
  const Bytes bytes{0x00, 0x05, 'a'};
  Reader r(bytes);
  EXPECT_THROW(read_string(r), MalformedPacket);
}

TEST(MqttWire, BinaryOverLimitRefused) {
  Writer w;
  const Bytes big(65536, 0);
  EXPECT_THROW(write_binary(w, big), std::length_error);
}

}  // namespace
}  // namespace lir::mqtt
