//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <string_view>

// The system topics (TODO/2026-11-22-system-topics.md): Lir reserves `$lir/`
// and `$dannan/`, which only Lir writes (the design's section 3). Any other
// `$` topic is a client's, held only by its grant.
namespace lir::system {

inline constexpr std::string_view lir_root = "$lir";
inline constexpr std::string_view dannan_root = "$dannan";

// Whether `topic` is under one of the two: its first level is "$lir" or
// "$dannan".
constexpr bool reserved(std::string_view topic) noexcept {
  const auto first = topic.substr(0, topic.find('/'));
  return first == lir_root || first == dannan_root;
}

}  // namespace lir::system
