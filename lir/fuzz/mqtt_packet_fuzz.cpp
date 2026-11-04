//
// Lir - The Distributed Event System
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

#include "lir/mqtt/packet.h"

// Any bytes a client sends: the decoder either refuses them with
// MalformedPacket or yields a packet that encodes back and decodes the same.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace lir::mqtt;
  if (size < 1) return 0;
  const auto version = static_cast<Version>(3 + data[0] % 3);
  const std::span<const uint8_t> bytes(data + 1, size - 1);
  Packet packet;
  try {
    const auto frame = read_frame(bytes, 1 << 20);
    if (!frame || frame->size() > bytes.size()) return 0;
    packet = decode(bytes.first(frame->size()), version);
  } catch (const MalformedPacket&) {
    return 0;
  }
  const Version as = std::holds_alternative<Connect>(packet) ? std::get<Connect>(packet).version : version;
  if (decode(encode(packet, as), as) != packet) std::abort();
  return 0;
}
