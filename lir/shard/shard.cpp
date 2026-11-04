//
// Lir - The Distributed Event System
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "lir/shard/shard.h"

#include <stdexcept>

#include "lir/mqtt/topic.h"

namespace lir::shard {

namespace {

constexpr std::string_view salt = "lir:";

void check(uint32_t count) {
  if (count == 0 || count > max_count) throw std::invalid_argument("shard count out of range");
}

}  // namespace

std::string_view ownership_prefix(std::string_view topic, uint32_t prefix_levels) noexcept {
  if (prefix_levels == 0) return topic;
  size_t position = 0;
  for (uint32_t level = 0; level < prefix_levels; ++level) {
    const size_t slash = topic.find('/', position);
    if (slash == std::string_view::npos) return topic;
    if (level + 1 == prefix_levels) return topic.substr(0, slash);
    position = slash + 1;
  }
  return topic;
}

ShardId shard_of(std::string_view topic, uint32_t prefix_levels, uint32_t count) {
  check(count);
  const auto prefix = ownership_prefix(topic, prefix_levels);
  dannan::identity::Sha256Hasher hasher;
  hasher.update({reinterpret_cast<const uint8_t*>(salt.data()), salt.size()});
  hasher.update({reinterpret_cast<const uint8_t*>(prefix.data()), prefix.size()});
  const auto digest = hasher.finish();
  uint64_t value = 0;
  for (size_t i = 0; i < 8; ++i) value = (value << 8) | digest.bytes[i];
  return static_cast<ShardId>(value % count);
}

std::optional<ShardId> shard_of_filter(std::string_view filter, uint32_t prefix_levels, uint32_t count) {
  check(count);
  if (!mqtt::has_wildcard(filter)) return shard_of(filter, prefix_levels, count);
  if (prefix_levels == 0) return std::nullopt;
  const auto parts = mqtt::levels(filter);
  // The first L levels must be literal, and there must be more levels than
  // L: "a/#" with L = 2 matches "a/x" and "a/y", whose prefixes differ.
  // "a/b/#" matches "a/b" too, whose prefix (all of it) is still "a/b".
  if (parts.size() <= prefix_levels) return std::nullopt;
  for (uint32_t i = 0; i < prefix_levels; ++i) {
    if (parts[i] == "+" || parts[i] == "#") return std::nullopt;
  }
  return shard_of(ownership_prefix(filter, prefix_levels), prefix_levels, count);
}

dannan::identity::Bytes placement_key(ShardId shard) {
  return {'l', 'i', 'r', '/', 's', 'h', 'a', 'r', 'd', '/', static_cast<uint8_t>(shard >> 8), static_cast<uint8_t>(shard & 0xFF)};
}

}  // namespace lir::shard
