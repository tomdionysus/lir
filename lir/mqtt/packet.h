//
// Lir - The Distributed Event System
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "lir/mqtt/properties.h"
#include "lir/mqtt/wire.h"

// The MQTT control packets of 3.1, 3.1.1 and 5.0, one struct each, and the
// codec between them and bytes. Every rule the specification states as a MUST
// about a packet's own bytes is checked here; rules about a session (packet
// identifiers in use, topic alias ranges, receive maximum) are the session's.
// Properties are carried only on 5.0 and must be empty on 3.x.
namespace lir::mqtt {

enum class PacketType : uint8_t {
  connect = 1,
  connack = 2,
  publish = 3,
  puback = 4,
  pubrec = 5,
  pubrel = 6,
  pubcomp = 7,
  subscribe = 8,
  suback = 9,
  unsubscribe = 10,
  unsuback = 11,
  pingreq = 12,
  pingresp = 13,
  disconnect = 14,
  auth = 15,
};

struct Will {
  std::string topic;
  Bytes payload;
  QoS qos = QoS::at_most_once;
  bool retain = false;
  Properties properties;
  bool operator==(const Will&) const = default;
};

struct Connect {
  Version version = Version::v5;
  // 3.x: clean session.
  bool clean_start = true;
  uint16_t keep_alive = 0;
  std::string client_id;
  std::optional<Will> will;
  std::optional<std::string> username;
  std::optional<Bytes> password;
  Properties properties;
  bool operator==(const Connect&) const = default;
};

struct Connack {
  bool session_present = false;
  // 3.x: the return code (0 to 5).
  uint8_t reason = reason::success;
  Properties properties;
  bool operator==(const Connack&) const = default;
};

struct Publish {
  bool dup = false;
  QoS qos = QoS::at_most_once;
  bool retain = false;
  // Empty on 5.0 when a topic alias stands for it.
  std::string topic;
  // Zero at QoS 0.
  uint16_t packet_id = 0;
  Properties properties;
  Bytes payload;
  bool operator==(const Publish&) const = default;
};

// PUBACK, PUBREC, PUBREL and PUBCOMP share one shape.
struct Ack {
  PacketType type = PacketType::puback;
  uint16_t packet_id = 0;
  uint8_t reason = reason::success;
  Properties properties;
  bool operator==(const Ack&) const = default;
};

struct Subscription {
  std::string filter;
  QoS qos = QoS::at_most_once;
  // 5.0 subscription options.
  bool no_local = false;
  bool retain_as_published = false;
  uint8_t retain_handling = 0;
  bool operator==(const Subscription&) const = default;
};

struct Subscribe {
  uint16_t packet_id = 0;
  Properties properties;
  std::vector<Subscription> subscriptions;
  bool operator==(const Subscribe&) const = default;
};

struct Suback {
  uint16_t packet_id = 0;
  Properties properties;
  std::vector<uint8_t> reasons;
  bool operator==(const Suback&) const = default;
};

struct Unsubscribe {
  uint16_t packet_id = 0;
  Properties properties;
  std::vector<std::string> filters;
  bool operator==(const Unsubscribe&) const = default;
};

struct Unsuback {
  uint16_t packet_id = 0;
  Properties properties;
  // 5.0 only; a 3.x UNSUBACK has none.
  std::vector<uint8_t> reasons;
  bool operator==(const Unsuback&) const = default;
};

struct Pingreq {
  bool operator==(const Pingreq&) const = default;
};

struct Pingresp {
  bool operator==(const Pingresp&) const = default;
};

struct Disconnect {
  uint8_t reason = reason::normal_disconnection;
  Properties properties;
  bool operator==(const Disconnect&) const = default;
};

// 5.0 only.
struct Auth {
  uint8_t reason = reason::success;
  Properties properties;
  bool operator==(const Auth&) const = default;
};

using Packet = std::variant<Connect, Connack, Publish, Ack, Subscribe, Suback, Unsubscribe, Unsuback, Pingreq, Pingresp, Disconnect, Auth>;

PacketType type_of(const Packet& packet) noexcept;

// The fixed header, read from the front of a stream: how long the packet in
// front is, once enough bytes have arrived to say.
struct Frame {
  uint8_t first_byte = 0;
  size_t header_size = 0;
  uint32_t remaining_length = 0;
  size_t size() const noexcept { return header_size + remaining_length; }
};

// Nothing until the fixed header is complete. Throws MalformedPacket for a
// bad length, and (packet_too_large) for a packet over `maximum_size` bytes,
// before any of its body has to be held.
std::optional<Frame> read_frame(std::span<const uint8_t> stream, uint32_t maximum_size);

// One whole packet, fixed header first. A CONNECT names its own version; any
// other packet is read as `version`, the session's. Throws MalformedPacket,
// or UnsupportedVersion for a CONNECT whose name or level is not one of the
// three.
Packet decode(std::span<const uint8_t> packet, Version version);

// Throws std::invalid_argument for a packet that `version` cannot carry or
// that breaks a MUST of the specification, so a bad packet is never sent.
Bytes encode(const Packet& packet, Version version);

}  // namespace lir::mqtt
