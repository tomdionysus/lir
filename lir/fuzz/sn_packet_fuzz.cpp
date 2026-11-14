//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

#include "lir/sn/packet.h"

// Any datagram a sensor sends, over UDP or DTLS: the decoder either refuses it
// with Malformed or yields a packet that encodes back and decodes the same.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace lir::sn;
  Packet packet;
  try {
    packet = decode(std::span<const uint8_t>(data, size));
  } catch (const Malformed&) {
    return 0;
  }
  if (decode(encode(packet)) != packet) std::abort();
  return 0;
}
