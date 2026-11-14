//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <cstddef>
#include <cstdint>
#include <span>

#include "lir/cluster/codec.h"

// Any payload another node sends: each decoder either refuses it with
// dannan::net::DecodeError or yields a value that encodes again. The first
// byte picks the decoder.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace lir::cluster;
  if (size < 1) return 0;
  const std::span<const uint8_t> bytes(data + 1, size - 1);
  const TimePoint now{};
  try {
    switch (data[0] % 8) {
      case 0:
        (void)encode_message(decode_message(bytes, now), now);
        break;
      case 1:
        (void)encode_messages(decode_messages(bytes, now), now);
        break;
      case 2:
        (void)encode_image(decode_image(bytes, now), now);
        break;
      case 3:
        (void)encode_groups(decode_groups(bytes));
        break;
      case 4:
        (void)encode_take(decode_take(bytes));
        break;
      case 5:
        (void)encode_taken(decode_taken(bytes, now), now);
        break;
      case 6:
        (void)encode_mirror(decode_mirror(bytes, now), now);
        break;
      default:
        (void)encode_shared(decode_shared(bytes, now), now);
        break;
    }
  } catch (const dannan::net::DecodeError&) {
  }
  return 0;
}
