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

// Any bytes read back from a Dagda log (step 5): an inflight or queue entry
// is either refused with dannan::net::DecodeError or decoded to a value that
// encodes again. The first byte picks the form.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace lir::cluster;
  if (size < 1) return 0;
  const std::span<const uint8_t> bytes(data + 1, size - 1);
  const Clocks clocks{TimePoint{}, 1'700'000'000'000};
  try {
    if (data[0] % 2 == 0) {
      (void)encode_inflight(decode_inflight(bytes, clocks), clocks);
    } else {
      (void)encode_queued(decode_queued(bytes, clocks), clocks);
    }
  } catch (const dannan::net::DecodeError&) {
  }
  return 0;
}
