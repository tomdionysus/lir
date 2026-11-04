//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "lir/mqtt/wire.h"

// MQTT-SN 1.2 packets (MQTT-SN Protocol Specification Version 1.2, section 5),
// for sensor networks over UDP. One datagram is one packet: a length (one
// byte, or 0x01 and two bytes), the type, then the fields. Written here, as
// the MQTT codec is.
namespace lir::sn {

using mqtt::Bytes;

// A datagram that is not a well-formed MQTT-SN 1.2 packet.
struct Malformed : std::runtime_error {
  using std::runtime_error::runtime_error;
};

enum class Type : uint8_t {
  advertise = 0x00,
  searchgw = 0x01,
  gwinfo = 0x02,
  connect = 0x04,
  connack = 0x05,
  willtopicreq = 0x06,
  willtopic = 0x07,
  willmsgreq = 0x08,
  willmsg = 0x09,
  register_ = 0x0A,
  regack = 0x0B,
  publish = 0x0C,
  puback = 0x0D,
  pubcomp = 0x0E,
  pubrec = 0x0F,
  pubrel = 0x10,
  subscribe = 0x12,
  suback = 0x13,
  unsubscribe = 0x14,
  unsuback = 0x15,
  pingreq = 0x16,
  pingresp = 0x17,
  disconnect = 0x18,
  willtopicupd = 0x1A,
  willtopicresp = 0x1B,
  willmsgupd = 0x1C,
  willmsgresp = 0x1D,
  encapsulated = 0xFE,
};

// Return codes (section 5.3.10).
namespace rc {
inline constexpr uint8_t accepted = 0x00;
inline constexpr uint8_t congestion = 0x01;
inline constexpr uint8_t invalid_topic_id = 0x02;
inline constexpr uint8_t not_supported = 0x03;
}  // namespace rc

enum class TopicIdType : uint8_t { normal = 0, predefined = 1, short_name = 2 };

// The flags byte (section 5.3.4). QoS -1 (binary 11) is a publish with no
// connection, to a predefined or short topic.
struct Flags {
  bool dup = false;
  int8_t qos = 0;
  bool retain = false;
  bool will = false;
  bool clean_session = false;
  TopicIdType topic_id_type = TopicIdType::normal;
  bool operator==(const Flags&) const = default;
};

// A two-character topic name carried in the topic id field.
uint16_t short_topic(std::string_view name);
std::string short_name(uint16_t id);

struct Advertise {
  uint8_t gateway_id = 0;
  uint16_t duration = 0;
  bool operator==(const Advertise&) const = default;
};
struct SearchGw {
  uint8_t radius = 0;
  bool operator==(const SearchGw&) const = default;
};
struct GwInfo {
  uint8_t gateway_id = 0;
  // Present only when a client answers for a gateway.
  Bytes gateway_address;
  bool operator==(const GwInfo&) const = default;
};
struct Connect {
  Flags flags;
  uint8_t protocol_id = 0x01;
  uint16_t duration = 0;
  std::string client_id;
  bool operator==(const Connect&) const = default;
};
struct Connack {
  uint8_t code = rc::accepted;
  bool operator==(const Connack&) const = default;
};
struct WillTopicReq {
  bool operator==(const WillTopicReq&) const = default;
};
struct WillTopic {
  Flags flags;
  // Empty: no will (and, as an update, delete it).
  std::string topic;
  bool operator==(const WillTopic&) const = default;
};
struct WillMsgReq {
  bool operator==(const WillMsgReq&) const = default;
};
struct WillMsg {
  Bytes message;
  bool operator==(const WillMsg&) const = default;
};
struct Register {
  uint16_t topic_id = 0;
  uint16_t msg_id = 0;
  std::string topic;
  bool operator==(const Register&) const = default;
};
struct Regack {
  uint16_t topic_id = 0;
  uint16_t msg_id = 0;
  uint8_t code = rc::accepted;
  bool operator==(const Regack&) const = default;
};
struct Publish {
  Flags flags;
  uint16_t topic_id = 0;
  uint16_t msg_id = 0;
  Bytes data;
  bool operator==(const Publish&) const = default;
};
struct Puback {
  uint16_t topic_id = 0;
  uint16_t msg_id = 0;
  uint8_t code = rc::accepted;
  bool operator==(const Puback&) const = default;
};
// PUBREC, PUBREL and PUBCOMP share one shape.
struct PubFlow {
  Type type = Type::pubrec;
  uint16_t msg_id = 0;
  bool operator==(const PubFlow&) const = default;
};
struct Subscribe {
  Flags flags;
  uint16_t msg_id = 0;
  // normal: the name (wildcards allowed); short_name: its two characters
  // (topic_id 0); predefined: topic_id.
  std::string topic;
  uint16_t topic_id = 0;
  bool operator==(const Subscribe&) const = default;
};
struct Suback {
  Flags flags;
  uint16_t topic_id = 0;
  uint16_t msg_id = 0;
  uint8_t code = rc::accepted;
  bool operator==(const Suback&) const = default;
};
struct Unsubscribe {
  Flags flags;
  uint16_t msg_id = 0;
  std::string topic;
  uint16_t topic_id = 0;
  bool operator==(const Unsubscribe&) const = default;
};
struct Unsuback {
  uint16_t msg_id = 0;
  bool operator==(const Unsuback&) const = default;
};
struct Pingreq {
  // A sleeping client waking: its id.
  std::string client_id;
  bool operator==(const Pingreq&) const = default;
};
struct Pingresp {
  bool operator==(const Pingresp&) const = default;
};
struct Disconnect {
  // A client going to sleep: for how many seconds.
  std::optional<uint16_t> duration;
  bool operator==(const Disconnect&) const = default;
};
struct WillTopicUpd {
  Flags flags;
  std::string topic;
  bool operator==(const WillTopicUpd&) const = default;
};
struct WillMsgUpd {
  Bytes message;
  bool operator==(const WillMsgUpd&) const = default;
};
// WILLTOPICRESP and WILLMSGRESP share one shape.
struct WillResp {
  Type type = Type::willtopicresp;
  uint8_t code = rc::accepted;
  bool operator==(const WillResp&) const = default;
};
// A packet a forwarder relays (section 5.5): its radius and the wireless
// node it came from, around the packet itself.
struct Encapsulated {
  uint8_t radius = 0;
  Bytes wireless_node_id;
  Bytes packet;
  bool operator==(const Encapsulated&) const = default;
};

using Packet = std::variant<Advertise, SearchGw, GwInfo, Connect, Connack, WillTopicReq, WillTopic, WillMsgReq, WillMsg, Register, Regack, Publish, Puback,
                            PubFlow, Subscribe, Suback, Unsubscribe, Unsuback, Pingreq, Pingresp, Disconnect, WillTopicUpd, WillMsgUpd, WillResp, Encapsulated>;

// One datagram to one packet; throws Malformed.
Packet decode(std::span<const uint8_t> datagram);
// One packet to one datagram; throws std::invalid_argument for a packet over
// 65535 bytes.
Bytes encode(const Packet& packet);

}  // namespace lir::sn
