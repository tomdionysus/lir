//
// Lir - The Distributed Event System
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Topic names, topic filters and shared subscriptions (MQTT 5.0 section 4.7
// and 4.8.2; 3.1.1 section 4.7). Strings arrive here already valid UTF-8.
namespace lir::mqtt {

// At least one character, no wildcard.
bool valid_topic_name(std::string_view name) noexcept;

// At least one character; '#' alone in the last level; '+' alone in its
// level. A "$share/" filter is checked by parse_shared, not here.
bool valid_topic_filter(std::string_view filter) noexcept;

bool has_wildcard(std::string_view filter) noexcept;

struct SharedFilter {
  std::string group;
  std::string filter;
  bool operator==(const SharedFilter&) const = default;
};

// "$share/<group>/<filter>": a group of at least one character with no '/',
// '+' or '#', then a valid filter. Nothing for a filter that does not start
// "$share/"; std::nullopt is also the answer for a malformed shared filter,
// so callers test is_shared first.
bool is_shared(std::string_view filter) noexcept;
std::optional<SharedFilter> parse_shared(std::string_view filter);

// Whether `filter` matches `name`. A name starting with '$' is not matched
// by a filter whose first level is '+' or '#' (section 4.7.2).
bool matches(std::string_view filter, std::string_view name) noexcept;

// The levels between '/' separators; empty levels kept.
std::vector<std::string_view> levels(std::string_view topic);

}  // namespace lir::mqtt
