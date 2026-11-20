//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "lir/mqtt/topic.h"

namespace lir::mqtt {

namespace {

constexpr std::string_view share_prefix = "$share/";

// What every name and filter must be [MQTT-4.7.3-1] to [MQTT-4.7.3-3].
bool plausible(std::string_view text) noexcept { return !text.empty() && text.size() <= 65535 && text.find('\0') == std::string_view::npos; }

// A filter whose first character is a wildcard never matches, or covers, a
// name or filter whose first character is '$' [MQTT-4.7.2-1].
bool hidden_from(std::string_view filter, std::string_view other) noexcept {
  return !other.empty() && other.front() == '$' && !filter.empty() && (filter.front() == '+' || filter.front() == '#');
}

// The next level of `text` from `position`, and where the one after starts
// (npos past the last level).
std::string_view next_level(std::string_view text, size_t& position) noexcept {
  const size_t slash = text.find('/', position);
  const std::string_view level = text.substr(position, slash == std::string_view::npos ? std::string_view::npos : slash - position);
  position = slash == std::string_view::npos ? std::string_view::npos : slash + 1;
  return level;
}

}  // namespace

bool valid_topic_name(std::string_view name) noexcept { return plausible(name) && !has_wildcard(name); }

bool valid_topic_filter(std::string_view filter) noexcept {
  if (!plausible(filter)) return false;
  size_t position = 0;
  while (position != std::string_view::npos) {
    const std::string_view level = next_level(filter, position);
    if (level == "#") {
      if (position != std::string_view::npos) return false;  // '#' is the last level [MQTT-4.7.1-1]
    } else if (level != "+" && has_wildcard(level)) {
      return false;  // a wildcard occupies its whole level [MQTT-4.7.1-2]
    }
  }
  return true;
}

bool has_wildcard(std::string_view filter) noexcept { return filter.find_first_of("+#") != std::string_view::npos; }

bool is_shared(std::string_view filter) noexcept { return filter.starts_with(share_prefix); }

std::optional<SharedFilter> parse_shared(std::string_view filter) {
  if (!is_shared(filter)) return std::nullopt;
  const std::string_view rest = filter.substr(share_prefix.size());
  const size_t slash = rest.find('/');
  if (slash == std::string_view::npos) return std::nullopt;  // the ShareName is followed by '/' [MQTT-4.8.2-2]
  const std::string_view group = rest.substr(0, slash);
  if (group.empty() || has_wildcard(group)) return std::nullopt;  // [MQTT-4.8.2-1], [MQTT-4.8.2-2]
  const std::string_view inner = rest.substr(slash + 1);
  if (!valid_topic_filter(inner)) return std::nullopt;
  return SharedFilter{std::string(group), std::string(inner)};
}

bool matches(std::string_view filter, std::string_view name) noexcept {
  if (hidden_from(filter, name)) return false;
  size_t f = 0;
  size_t n = 0;
  while (f != std::string_view::npos) {
    const std::string_view wanted = next_level(filter, f);
    if (wanted == "#") return true;  // the parent and every level below it
    if (n == std::string_view::npos) return false;
    const std::string_view level = next_level(name, n);
    if (wanted != "+" && wanted != level) return false;  // byte for byte [MQTT-4.7.3-4]
  }
  return n == std::string_view::npos;
}

bool covers(std::string_view granted, std::string_view requested) {
  if (hidden_from(granted, requested)) return false;
  size_t g = 0;
  size_t r = 0;
  while (g != std::string_view::npos) {
    const std::string_view grant = next_level(granted, g);
    if (grant == "#") return true;
    if (r == std::string_view::npos) return false;
    const std::string_view asked = next_level(requested, r);
    if (asked == "#") return false;  // only '#' covers '#'
    if (grant == "+") continue;      // '+' covers one level, '+' included
    if (asked == "+" || grant != asked) return false;
  }
  return r == std::string_view::npos;
}

std::vector<std::string_view> levels(std::string_view topic) {
  std::vector<std::string_view> out;
  size_t position = 0;
  while (position != std::string_view::npos) out.push_back(next_level(topic, position));
  return out;
}

}  // namespace lir::mqtt
