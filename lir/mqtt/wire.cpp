//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "lir/mqtt/wire.h"

#include <stdexcept>

namespace lir::mqtt {

void malformed(const std::string& what) { throw MalformedPacket(reason::malformed_packet, what); }

void protocol_error(const std::string& what) { throw MalformedPacket(reason::protocol_error, what); }

void write_varint(dannan::net::Writer& writer, uint32_t value) {
  if (value > varint_max) throw std::length_error("variable byte integer over 268,435,455");
  do {
    auto byte = static_cast<uint8_t>(value % 128);
    value /= 128;
    if (value > 0) byte |= 0x80;
    writer.u8(byte);
  } while (value > 0);
}

uint32_t read_varint(dannan::net::Reader& reader) {
  uint32_t value = 0;
  uint32_t multiplier = 1;
  for (int i = 0; i < 4; ++i) {
    if (reader.remaining() == 0) malformed("variable byte integer runs past the packet");
    const uint8_t byte = reader.u8();
    value += static_cast<uint32_t>(byte & 0x7F) * multiplier;
    if ((byte & 0x80) == 0) {
      // The encoding is the minimum number of bytes (section 1.5.5).
      if (i > 0 && byte == 0) malformed("variable byte integer not in its shortest form");
      return value;
    }
    multiplier *= 128;
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
  size_t i = 0;
  while (i < text.size()) {
    const auto lead = static_cast<uint8_t>(text[i]);
    uint32_t code = 0;
    size_t length = 0;
    if (lead < 0x80) {
      if (lead == 0) return false;
      ++i;
      continue;
    } else if ((lead & 0xE0) == 0xC0) {
      code = lead & 0x1Fu;
      length = 2;
    } else if ((lead & 0xF0) == 0xE0) {
      code = lead & 0x0Fu;
      length = 3;
    } else if ((lead & 0xF8) == 0xF0) {
      code = lead & 0x07u;
      length = 4;
    } else {
      return false;
    }
    if (i + length > text.size()) return false;
    for (size_t k = 1; k < length; ++k) {
      const auto next = static_cast<uint8_t>(text[i + k]);
      if ((next & 0xC0) != 0x80) return false;
      code = (code << 6) | (next & 0x3Fu);
    }
    // Overlong forms, surrogates and anything past U+10FFFF are not UTF-8.
    if ((length == 2 && code < 0x80) || (length == 3 && code < 0x800) || (length == 4 && code < 0x10000)) return false;
    if (code >= 0xD800 && code <= 0xDFFF) return false;
    if (code > 0x10FFFF) return false;
    i += length;
  }
  return true;
}

void write_string(dannan::net::Writer& writer, std::string_view value) {
  if (!valid_utf8(value)) throw std::invalid_argument("string is not valid MQTT UTF-8");
  write_binary(writer, {reinterpret_cast<const uint8_t*>(value.data()), value.size()});
}

std::string read_string(dannan::net::Reader& reader) {
  if (reader.remaining() < 2) malformed("string length runs past the packet");
  const uint16_t length = reader.u16();
  if (reader.remaining() < length) malformed("string runs past the packet");
  const auto view = reader.view(length);
  std::string value(reinterpret_cast<const char*>(view.data()), view.size());
  if (!valid_utf8(value)) malformed("string is not valid UTF-8");
  return value;
}

void write_binary(dannan::net::Writer& writer, std::span<const uint8_t> value) {
  if (value.size() > 65535) throw std::length_error("string or binary data over 65,535 bytes");
  writer.u16(static_cast<uint16_t>(value.size()));
  writer.raw(value);
}

Bytes read_binary(dannan::net::Reader& reader) {
  if (reader.remaining() < 2) malformed("binary length runs past the packet");
  const uint16_t length = reader.u16();
  if (reader.remaining() < length) malformed("binary data runs past the packet");
  return reader.raw(length);
}

}  // namespace lir::mqtt
