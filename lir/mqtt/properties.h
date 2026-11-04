//
// Lir - The Distributed Event System
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "lir/mqtt/wire.h"

// MQTT 5.0 properties (section 2.2.2). One struct for every packet: a
// property a packet may not carry is refused on decode and on encode, by the
// table in properties.cpp, so a packet built wrong never reaches a peer.
namespace lir::mqtt {

// Where a property block sits. A will's properties are their own place.
enum class PropertyPlace : uint8_t {
  connect,
  connack,
  publish,
  will,
  puback,
  pubrec,
  pubrel,
  pubcomp,
  subscribe,
  suback,
  unsubscribe,
  unsuback,
  disconnect,
  auth,
};

namespace property {
inline constexpr uint8_t payload_format_indicator = 0x01;
inline constexpr uint8_t message_expiry_interval = 0x02;
inline constexpr uint8_t content_type = 0x03;
inline constexpr uint8_t response_topic = 0x08;
inline constexpr uint8_t correlation_data = 0x09;
inline constexpr uint8_t subscription_identifier = 0x0B;
inline constexpr uint8_t session_expiry_interval = 0x11;
inline constexpr uint8_t assigned_client_identifier = 0x12;
inline constexpr uint8_t server_keep_alive = 0x13;
inline constexpr uint8_t authentication_method = 0x15;
inline constexpr uint8_t authentication_data = 0x16;
inline constexpr uint8_t request_problem_information = 0x17;
inline constexpr uint8_t will_delay_interval = 0x18;
inline constexpr uint8_t request_response_information = 0x19;
inline constexpr uint8_t response_information = 0x1A;
inline constexpr uint8_t server_reference = 0x1C;
inline constexpr uint8_t reason_string = 0x1F;
inline constexpr uint8_t receive_maximum = 0x21;
inline constexpr uint8_t topic_alias_maximum = 0x22;
inline constexpr uint8_t topic_alias = 0x23;
inline constexpr uint8_t maximum_qos = 0x24;
inline constexpr uint8_t retain_available = 0x25;
inline constexpr uint8_t user_property = 0x26;
inline constexpr uint8_t maximum_packet_size = 0x27;
inline constexpr uint8_t wildcard_subscription_available = 0x28;
inline constexpr uint8_t subscription_identifier_available = 0x29;
inline constexpr uint8_t shared_subscription_available = 0x2A;
}  // namespace property

inline constexpr uint32_t subscription_identifier_max = varint_max;

struct Properties {
  std::optional<uint8_t> payload_format_indicator;
  std::optional<uint32_t> message_expiry_interval;
  std::optional<std::string> content_type;
  std::optional<std::string> response_topic;
  std::optional<Bytes> correlation_data;
  // Several on a PUBLISH to a client; at most one on a SUBSCRIBE.
  std::vector<uint32_t> subscription_identifiers;
  std::optional<uint32_t> session_expiry_interval;
  std::optional<std::string> assigned_client_identifier;
  std::optional<uint16_t> server_keep_alive;
  std::optional<std::string> authentication_method;
  std::optional<Bytes> authentication_data;
  std::optional<uint8_t> request_problem_information;
  std::optional<uint32_t> will_delay_interval;
  std::optional<uint8_t> request_response_information;
  std::optional<std::string> response_information;
  std::optional<std::string> server_reference;
  std::optional<std::string> reason_string;
  std::optional<uint16_t> receive_maximum;
  std::optional<uint16_t> topic_alias_maximum;
  std::optional<uint16_t> topic_alias;
  std::optional<uint8_t> maximum_qos;
  std::optional<uint8_t> retain_available;
  // In order, repeats allowed (section 3.1.2.11.8).
  std::vector<std::pair<std::string, std::string>> user_properties;
  std::optional<uint32_t> maximum_packet_size;
  std::optional<uint8_t> wildcard_subscription_available;
  std::optional<uint8_t> subscription_identifier_available;
  std::optional<uint8_t> shared_subscription_available;

  bool empty() const noexcept;
  bool operator==(const Properties&) const = default;
};

// The length-prefixed block. Decode throws MalformedPacket (protocol error)
// for a property not allowed at `place`, one repeated that may not be, or a
// value out of range; encode throws std::invalid_argument for the same.
Properties read_properties(dannan::net::Reader& reader, PropertyPlace place);
void write_properties(dannan::net::Writer& writer, const Properties& properties, PropertyPlace place);
// The encoded size, length prefix included.
size_t properties_size(const Properties& properties);

}  // namespace lir::mqtt
