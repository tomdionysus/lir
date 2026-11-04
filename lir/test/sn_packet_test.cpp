//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <gtest/gtest.h>

#include "lir/sn/packet.h"

namespace lir::sn {
namespace {

Bytes b(std::initializer_list<int> values) {
  Bytes out;
  for (const int v : values) out.push_back(static_cast<uint8_t>(v));
  return out;
}

TEST(SnPacket, EveryPacketRoundTrips) {
  Flags f;
  f.qos = 1;
  f.retain = true;
  f.topic_id_type = TopicIdType::predefined;
  const std::vector<Packet> all{
      Advertise{7, 900},
      SearchGw{1},
      GwInfo{7, {}},
      GwInfo{7, b({10, 0, 0, 1})},
      Connect{Flags{false, 0, false, true, true, TopicIdType::normal}, 1, 60, "sensor-1"},
      Connack{rc::accepted},
      WillTopicReq{},
      WillTopic{Flags{false, 1, true, false, false, TopicIdType::normal}, "gone/sensor-1"},
      WillTopic{},
      WillMsgReq{},
      WillMsg{b({'b', 'y', 'e'})},
      Register{0, 5, "sensors/temp"},
      Regack{12, 5, rc::accepted},
      Publish{f, 3, 9, b({1, 2, 3})},
      Puback{3, 9, rc::invalid_topic_id},
      PubFlow{Type::pubrec, 9},
      PubFlow{Type::pubrel, 9},
      PubFlow{Type::pubcomp, 9},
      Subscribe{Flags{}, 4, "sensors/#", 0},
      Subscribe{Flags{false, 0, false, false, false, TopicIdType::predefined}, 4, "", 77},
      Subscribe{Flags{false, 0, false, false, false, TopicIdType::short_name}, 4, "ab", 0},
      Suback{Flags{false, 1, false, false, false, TopicIdType::normal}, 12, 4, rc::accepted},
      Unsubscribe{Flags{}, 6, "sensors/#", 0},
      Unsuback{6},
      Pingreq{},
      Pingreq{"sleepy"},
      Pingresp{},
      Disconnect{},
      Disconnect{600},
      WillTopicUpd{Flags{false, 0, false, false, false, TopicIdType::normal}, "new/will"},
      WillMsgUpd{b({'x'})},
      WillResp{Type::willtopicresp, rc::accepted},
      WillResp{Type::willmsgresp, rc::not_supported},
      Encapsulated{1, b({0xAA, 0xBB}), encode(Pingreq{})},
  };
  for (const auto& p : all) {
    const auto bytes = encode(p);
    EXPECT_EQ(decode(bytes), p) << "type " << p.index();
  }
}

TEST(SnPacket, WireFormatOfAConnect) {
  // Length 12, CONNECT, flags (will, clean), protocol 1, duration 60, "sensor".
  const Connect c{Flags{false, 0, false, true, true, TopicIdType::normal}, 1, 60, "sensor"};
  EXPECT_EQ(encode(c), b({0x0C, 0x04, 0x0C, 0x01, 0x00, 0x3C, 's', 'e', 'n', 's', 'o', 'r'}));
}

TEST(SnPacket, ALongPacketTakesAThreeByteLength) {
  Publish p;
  p.data.assign(300, 'x');
  const auto bytes = encode(p);
  ASSERT_EQ(bytes[0], 0x01);
  EXPECT_EQ(static_cast<size_t>((bytes[1] << 8) | bytes[2]), bytes.size());
  EXPECT_EQ(decode(bytes), Packet(p));
}

TEST(SnPacket, QoSMinusOneAndShortTopics) {
  Publish p;
  p.flags.qos = -1;
  p.flags.topic_id_type = TopicIdType::short_name;
  p.topic_id = short_topic("tt");
  const auto bytes = encode(p);
  EXPECT_EQ(bytes[2] & 0x60, 0x60);
  const auto back = std::get<Publish>(decode(bytes));
  EXPECT_EQ(back.flags.qos, -1);
  EXPECT_EQ(short_name(back.topic_id), "tt");
}

TEST(SnPacket, MalformedDatagramsAreRefused) {
  EXPECT_THROW(decode(b({})), Malformed);
  EXPECT_THROW(decode(b({0x02})), Malformed);
  // The length disagrees with the datagram.
  EXPECT_THROW(decode(b({0x05, 0x16})), Malformed);
  // An unknown type.
  EXPECT_THROW(decode(b({0x02, 0x03})), Malformed);
  // A CONNECT cut short.
  EXPECT_THROW(decode(b({0x04, 0x04, 0x04, 0x01})), Malformed);
  // A reserved topic id type.
  EXPECT_THROW(decode(b({0x05, 0x12, 0x03, 0x00, 0x01})), Malformed);
  // Trailing bytes after a fixed-size packet.
  EXPECT_THROW(decode(b({0x03, 0x17, 0x00})), Malformed);
  // REGISTER with no topic.
  EXPECT_THROW(decode(b({0x06, 0x0A, 0x00, 0x00, 0x00, 0x01})), Malformed);
}

TEST(SnPacket, EncapsulatedLengthEndsAtTheNodeId) {
  const Encapsulated e{0, b({1, 2, 3}), encode(Pingresp{})};
  const auto bytes = encode(e);
  // Length 6: itself, type, ctrl and three bytes of node id.
  EXPECT_EQ(bytes[0], 6);
  EXPECT_EQ(bytes.size(), 6u + 2u);
}

}  // namespace
}  // namespace lir::sn
