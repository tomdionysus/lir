//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "lir/mqtt/topic.h"

#include <algorithm>

namespace lir::mqtt {

namespace {

constexpr std::string_view share_prefix = "$share/";
constexpr size_t topic_max = 65535;

}  // namespace

std::vector<std::string_view> levels(std::string_view topic) {
  std::vector<std::string_view> out;
  size_t start = 0;
  while (true) {
    const size_t slash = topic.find('/', start);
    if (slash == std::string_view::npos) {
      out.push_back(topic.substr(start));
      return out;
    }
    out.push_back(topic.substr(start, slash - start));
    start = slash + 1;
  }
}

bool has_wildcard(std::string_view filter) noexcept { return filter.find_first_of("+#") != std::string_view::npos; }

bool valid_topic_name(std::string_view name) noexcept { return !name.empty() && name.size() <= topic_max && !has_wildcard(name); }

bool valid_topic_filter(std::string_view filter) noexcept {
  if (filter.empty() || filter.size() > topic_max) return false;
  const auto parts = levels(filter);
  for (size_t i = 0; i < parts.size(); ++i) {
    const auto level = parts[i];
    if (level.find('#') != std::string_view::npos && (level != "#" || i + 1 != parts.size())) return false;
    if (level.find('+') != std::string_view::npos && level != "+") return false;
  }
  return true;
}

bool is_shared(std::string_view filter) noexcept { return filter.starts_with(share_prefix); }

std::optional<SharedFilter> parse_shared(std::string_view filter) {
  if (!is_shared(filter)) return std::nullopt;
  const auto rest = filter.substr(share_prefix.size());
  const size_t slash = rest.find('/');
  if (slash == std::string_view::npos || slash == 0) return std::nullopt;
  const auto group = rest.substr(0, slash);
  if (group.find_first_of("+#") != std::string_view::npos) return std::nullopt;
  const auto inner = rest.substr(slash + 1);
  if (!valid_topic_filter(inner)) return std::nullopt;
  return SharedFilter{std::string(group), std::string(inner)};
}

bool matches(std::string_view filter, std::string_view name) noexcept {
  if (!name.empty() && name.front() == '$' && !filter.empty() && (filter.front() == '+' || filter.front() == '#')) return false;
  size_t f = 0;
  size_t n = 0;
  while (true) {
    const size_t f_end = std::min(filter.find('/', f), filter.size());
    const size_t n_end = std::min(name.find('/', n), name.size());
    const auto f_level = filter.substr(f, f_end - f);
    // '#' matches this level and everything under it, and the parent too.
    if (f_level == "#") return true;
    if (f_level != "+" && f_level != name.substr(n, n_end - n)) return false;
    const bool f_last = f_end == filter.size();
    const bool n_last = n_end == name.size();
    if (f_last && n_last) return true;
    if (n_last) {
      // "a/#" matches "a": the only filter level left may be '#'.
      return filter.substr(f_end + 1) == "#";
    }
    if (f_last) return false;
    f = f_end + 1;
    n = n_end + 1;
  }
}

}  // namespace lir::mqtt
