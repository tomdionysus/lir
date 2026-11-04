//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "dannan/identity/crypto.h"

// Topics are not placed one by one: each maps to one of `count` shards, and
// shards are what get holders (design, "Topics, shards and ownership").
// shard(topic) = SHA-256("lir:" || prefix(topic, L)) mod count, where the
// prefix is the topic's first L levels (L = 0: the whole topic), so topics
// sharing a prefix share a shard and a filter wild only beyond it can be
// placed with that shard.
namespace lir::shard {

using ShardId = uint16_t;

inline constexpr uint32_t default_count = 256;
inline constexpr uint32_t max_count = 65536;

// The first `prefix_levels` levels of `topic`, or all of it with fewer
// levels or prefix_levels 0.
std::string_view ownership_prefix(std::string_view topic, uint32_t prefix_levels) noexcept;

// Throws std::invalid_argument for a count of 0 or over max_count.
ShardId shard_of(std::string_view topic, uint32_t prefix_levels, uint32_t count);

// The shard a filter's matches all fall in, when its wildcards lie beyond
// the ownership prefix (and so it is shard-local); nothing when it may match
// topics of several shards and goes in the global wildcard index. With
// prefix_levels 0 only a filter with no wildcard is shard-local.
std::optional<ShardId> shard_of_filter(std::string_view filter, uint32_t prefix_levels, uint32_t count);

// The placement key for a shard: what Dannan's rendezvous hashes with each
// node id.
dannan::identity::Bytes placement_key(ShardId shard);

}  // namespace lir::shard
