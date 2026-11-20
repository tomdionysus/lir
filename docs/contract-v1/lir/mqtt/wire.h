//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "dannan/net/codec.h"

// MQTT's data representations (MQTT 5.0 section 1.5, 3.1.1 section 1.5) over
// Dannan's byte codec: big-endian integers come from Dannan; the variable
// byte integer and the two-byte-length string and binary are MQTT's own. A
// decode that cannot be completed throws MalformedPacket with the reason code
// a server sends for it; nothing is guessed.
namespace lir::mqtt {

using Bytes = dannan::net::Bytes;

enum class Version : uint8_t { v3_1 = 3, v3_1_1 = 4, v5 = 5 };

enum class QoS : uint8_t { at_most_once = 0, at_least_once = 1, exactly_once = 2 };

// The reason codes a decode can end in (MQTT 5.0 section 2.4).
namespace reason {
inline constexpr uint8_t success = 0x00;
inline constexpr uint8_t normal_disconnection = 0x00;
inline constexpr uint8_t granted_qos_0 = 0x00;
inline constexpr uint8_t granted_qos_1 = 0x01;
inline constexpr uint8_t granted_qos_2 = 0x02;
inline constexpr uint8_t disconnect_with_will = 0x04;
inline constexpr uint8_t no_matching_subscribers = 0x10;
inline constexpr uint8_t no_subscription_existed = 0x11;
inline constexpr uint8_t continue_authentication = 0x18;
inline constexpr uint8_t reauthenticate = 0x19;
inline constexpr uint8_t unspecified_error = 0x80;
inline constexpr uint8_t malformed_packet = 0x81;
inline constexpr uint8_t protocol_error = 0x82;
inline constexpr uint8_t implementation_specific_error = 0x83;
inline constexpr uint8_t unsupported_protocol_version = 0x84;
inline constexpr uint8_t client_identifier_not_valid = 0x85;
inline constexpr uint8_t bad_user_name_or_password = 0x86;
inline constexpr uint8_t not_authorized = 0x87;
inline constexpr uint8_t server_unavailable = 0x88;
inline constexpr uint8_t server_busy = 0x89;
inline constexpr uint8_t banned = 0x8A;
inline constexpr uint8_t server_shutting_down = 0x8B;
inline constexpr uint8_t bad_authentication_method = 0x8C;
inline constexpr uint8_t keep_alive_timeout = 0x8D;
inline constexpr uint8_t session_taken_over = 0x8E;
inline constexpr uint8_t topic_filter_invalid = 0x8F;
inline constexpr uint8_t topic_name_invalid = 0x90;
inline constexpr uint8_t packet_identifier_in_use = 0x91;
inline constexpr uint8_t packet_identifier_not_found = 0x92;
inline constexpr uint8_t receive_maximum_exceeded = 0x93;
inline constexpr uint8_t topic_alias_invalid = 0x94;
inline constexpr uint8_t packet_too_large = 0x95;
inline constexpr uint8_t message_rate_too_high = 0x96;
inline constexpr uint8_t quota_exceeded = 0x97;
inline constexpr uint8_t administrative_action = 0x98;
inline constexpr uint8_t payload_format_invalid = 0x99;
inline constexpr uint8_t retain_not_supported = 0x9A;
inline constexpr uint8_t qos_not_supported = 0x9B;
inline constexpr uint8_t use_another_server = 0x9C;
inline constexpr uint8_t server_moved = 0x9D;
inline constexpr uint8_t shared_subscriptions_not_supported = 0x9E;
inline constexpr uint8_t connection_rate_exceeded = 0x9F;
inline constexpr uint8_t maximum_connect_time = 0xA0;
inline constexpr uint8_t subscription_identifiers_not_supported = 0xA1;
inline constexpr uint8_t wildcard_subscriptions_not_supported = 0xA2;
}  // namespace reason

// A packet that breaks the specification. `reason` is what a v5 server
// sends in its DISCONNECT (or CONNACK) before closing; a 3.x server closes.
class MalformedPacket : public dannan::net::DecodeError {
 public:
  MalformedPacket(uint8_t reason, const std::string& what) : dannan::net::DecodeError(what), _reason(reason) {}
  uint8_t reason() const noexcept { return _reason; }

 private:
  uint8_t _reason;
};

// A CONNECT naming a protocol level or name this server does not speak. The
// server answers CONNACK 0x01 (3.x) or 0x84 (5.0) and closes.
class UnsupportedVersion : public MalformedPacket {
 public:
  UnsupportedVersion(uint8_t level, const std::string& what) : MalformedPacket(reason::unsupported_protocol_version, what), _level(level) {}
  uint8_t level() const noexcept { return _level; }

 private:
  uint8_t _level;
};

[[noreturn]] void malformed(const std::string& what);
[[noreturn]] void protocol_error(const std::string& what);

// The variable byte integer: at most four bytes, at most 268,435,455.
inline constexpr uint32_t varint_max = 268435455;
void write_varint(dannan::net::Writer& writer, uint32_t value);
uint32_t read_varint(dannan::net::Reader& reader);
size_t varint_size(uint32_t value) noexcept;

// Well-formed UTF-8 with no U+0000 and no surrogate code points: the MUSTs
// of section 1.5.4. Code points the specification only discourages pass.
bool valid_utf8(std::string_view text) noexcept;

// A UTF-8 string or binary data behind a two-byte length. Writes throw
// std::length_error past 65,535 bytes and std::invalid_argument for a string
// that is not valid UTF-8 by the rule above.
void write_string(dannan::net::Writer& writer, std::string_view value);
std::string read_string(dannan::net::Reader& reader);
void write_binary(dannan::net::Writer& writer, std::span<const uint8_t> value);
Bytes read_binary(dannan::net::Reader& reader);

}  // namespace lir::mqtt
