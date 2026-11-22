//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// A 5.0 property block at each place a packet carries one: the first byte
// picks the place, the rest is the block. A read may throw only
// MalformedPacket. What reads must write at the same place, to the size
// properties_size gives, and read back the same.

#include <span>
#include <stdexcept>

#include "dannan/fuzz/fuzz.h"
#include "lir/fuzz/seeds.h"
#include "lir/mqtt/properties.h"

namespace {

using namespace lir::mqtt;

void check(std::span<const uint8_t> input) {
  if (input.empty()) return;
  const auto place = static_cast<PropertyPlace>(input[0] % (static_cast<uint8_t>(PropertyPlace::auth) + 1));
  const auto block = input.subspan(1);

  Properties properties;
  try {
    dannan::net::Reader reader(block);
    properties = read_properties(reader, place);
  } catch (const MalformedPacket&) {
    return;
  }
  dannan::net::Writer writer;
  write_properties(writer, properties, place);
  const Bytes written = writer.take();
  if (written.size() != properties_size(properties)) throw std::logic_error("properties: written size is not properties_size");
  dannan::net::Reader again(written);
  if (!(read_properties(again, place) == properties) || again.remaining() != 0) throw std::logic_error("properties: what was written reads back otherwise");
}

const dannan::fuzz::Registration registration("properties", check, [] { return lir::fuzz::seeds("properties"); });

}  // namespace
