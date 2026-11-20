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

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

#include "lir/mqtt/properties.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace lir::mqtt;
  if (size == 0) return 0;
  const auto place = static_cast<PropertyPlace>(data[0] % (static_cast<uint8_t>(PropertyPlace::auth) + 1));
  const std::span<const uint8_t> block(data + 1, size - 1);

  Properties properties;
  try {
    dannan::net::Reader reader(block);
    properties = read_properties(reader, place);
  } catch (const MalformedPacket&) {
    return 0;
  }
  dannan::net::Writer writer;
  write_properties(writer, properties, place);
  const Bytes written = writer.take();
  if (written.size() != properties_size(properties)) std::abort();
  dannan::net::Reader again(written);
  if (!(read_properties(again, place) == properties) || again.remaining() != 0) std::abort();
  return 0;
}
