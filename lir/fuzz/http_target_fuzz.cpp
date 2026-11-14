//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>

#include "lir/http/http.h"

// Any request target a client sends: parsed, or refused with
// std::invalid_argument for a malformed escape. Nothing else.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  try {
    (void)lir::http::parse_target(std::string_view(reinterpret_cast<const char*>(data), size));
  } catch (const std::invalid_argument&) {
  }
  return 0;
}
