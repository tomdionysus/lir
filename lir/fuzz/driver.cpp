//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// libFuzzer's entry: each input through the target registered in this
// binary (one a binary), by Dannan's harness.

#include <cstddef>
#include <cstdint>

#include "dannan/fuzz/fuzz.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  dannan::fuzz::run_all(dannan::fuzz::targets(), {data, size});
  return 0;
}
