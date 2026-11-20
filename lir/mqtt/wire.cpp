//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "lir/mqtt/wire.h"

#include <stdexcept>

namespace lir::mqtt {

namespace {

// Dannan's reader throws its own DecodeError on a short buffer; MQTT names
// that a malformed packet, so every read here checks first.
void need(const dannan::net::Reader& reader, size_t count) {
  if (reader.remaining() < count) malformed("packet ends inside a field");
}

uint16_t length_of(dannan::net::Reader& reader) {
  need(reader, 2);
  const uint16_t length = reader.u16();
  need(reader, length);
  return length;
}

void write_length(dannan::net::Writer& writer, size_t size) {
  if (size > 65535) throw std::length_error("MQTT string or binary data over 65,535 bytes");
  writer.u16(static_cast<uint16_t>(size));
}

}  // namespace

void malformed(const std::string& what) { throw MalformedPacket(reason::malformed_packet, what); }
void protocol_error(const std::string& what) { throw MalformedPacket(reason::protocol_error, what); }

void write_varint(dannan::net::Writer& writer, uint32_t value) {
  if (value > varint_max) throw std::invalid_argument("variable byte integer over 268,435,455");
  do {
    auto byte = static_cast<uint8_t>(value & 0x7F);
    value >>= 7;
    if (value > 0) byte |= 0x80;
    writer.u8(byte);
  } while (value > 0);
}

uint32_t read_varint(dannan::net::Reader& reader) {
  uint32_t value = 0;
  for (int index = 0; index < 4; ++index) {
    need(reader, 1);
    const uint8_t byte = reader.u8();
    value |= static_cast<uint32_t>(byte & 0x7F) << (7 * index);
    if ((byte & 0x80) == 0) {
      // A last byte of zero after the first adds nothing: not the minimum
      // number of bytes [MQTT-1.5.5-1].
      if (index > 0 && byte == 0) malformed("variable byte integer not in its shortest form");
      return value;
    }
  }
  malformed("variable byte integer longer than four bytes");
}

size_t varint_size(uint32_t value) noexcept {
  if (value < 128) return 1;
  if (value < 16384) return 2;
  if (value < 2097152) return 3;
  return 4;
}

bool valid_utf8(std::string_view text) noexcept {
  const auto* bytes = reinterpret_cast<const uint8_t*>(text.data());
  const size_t size = text.size();
  size_t index = 0;
  while (index < size) {
    const uint8_t lead = bytes[index];
    if (lead == 0x00) return false;
    if (lead < 0x80) {
      ++index;
      continue;
    }
    size_t count = 0;
    // The bounds of the second byte, which rule out overlong forms, the
    // surrogates and anything past U+10FFFF (RFC 3629, section 4).
    uint8_t low = 0x80;
    uint8_t high = 0xBF;
    if (lead >= 0xC2 && lead <= 0xDF) {
      count = 1;
    } else if (lead == 0xE0) {
      count = 2;
      low = 0xA0;
    } else if (lead == 0xED) {
      count = 2;
      high = 0x9F;
    } else if ((lead >= 0xE1 && lead <= 0xEC) || lead == 0xEE || lead == 0xEF) {
      count = 2;
    } else if (lead == 0xF0) {
      count = 3;
      low = 0x90;
    } else if (lead >= 0xF1 && lead <= 0xF3) {
      count = 3;
    } else if (lead == 0xF4) {
      count = 3;
      high = 0x8F;
    } else {
      return false;
    }
    if (size - index - 1 < count) return false;
    if (bytes[index + 1] < low || bytes[index + 1] > high) return false;
    for (size_t next = 2; next <= count; ++next) {
      if ((bytes[index + next] & 0xC0) != 0x80) return false;
    }
    index += count + 1;
  }
  return true;
}

void write_string(dannan::net::Writer& writer, std::string_view value) {
  if (value.size() > 65535) throw std::length_error("MQTT string over 65,535 bytes");
  if (!valid_utf8(value)) throw std::invalid_argument("MQTT string is not well-formed UTF-8 without U+0000");
  write_length(writer, value.size());
  writer.raw({reinterpret_cast<const uint8_t*>(value.data()), value.size()});
}

std::string read_string(dannan::net::Reader& reader) {
  const uint16_t length = length_of(reader);
  const auto view = reader.view(length);
  std::string out(reinterpret_cast<const char*>(view.data()), view.size());
  if (!valid_utf8(out)) malformed("string is not well-formed UTF-8 without U+0000");
  return out;
}

void write_binary(dannan::net::Writer& writer, std::span<const uint8_t> value) {
  write_length(writer, value.size());
  writer.raw(value);
}

Bytes read_binary(dannan::net::Reader& reader) {
  const uint16_t length = length_of(reader);
  return reader.raw(length);
}

}  // namespace lir::mqtt
