//
// Lir - The Distributed Event System
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "lir/broker/broker.h"

// What Lir's nodes say to each other, in Dannan's byte codec. Each payload
// starts with a format number; a decode that cannot be completed, or meets a
// format it does not know, throws dannan::net::DecodeError. An expiry
// travels as what is left of it, since nodes share no clock; `now` is the
// sender's or the receiver's own.
namespace lir::cluster {

using broker::Bytes;
using broker::TimePoint;

inline constexpr uint8_t format = 1;

Bytes encode_message(const broker::Message& message, TimePoint now);
broker::Message decode_message(std::span<const uint8_t> bytes, TimePoint now);

Bytes encode_messages(const std::vector<broker::Message>& messages, TimePoint now);
std::vector<broker::Message> decode_messages(std::span<const uint8_t> bytes, TimePoint now);

Bytes encode_image(const broker::SessionImage& image, TimePoint now);
broker::SessionImage decode_image(std::span<const uint8_t> bytes, TimePoint now);

// A node's shared groups, as (group, filter).
using Groups = std::set<std::pair<std::string, std::string>>;
Bytes encode_groups(const Groups& groups);
Groups decode_groups(std::span<const uint8_t> bytes);

// "Let go of this client": its id, and whether its session is wanted.
struct Take {
  std::string client_id;
  bool want_state = false;
};
Bytes encode_take(const Take& take);
Take decode_take(std::span<const uint8_t> bytes);

// The answer: the session, if this node held one.
Bytes encode_taken(const std::optional<broker::SessionImage>& image, TimePoint now);
std::optional<broker::SessionImage> decode_taken(std::span<const uint8_t> bytes, TimePoint now);

// A message for one member of a shared group on the receiving node.
struct SharedDelivery {
  std::string group;
  std::string filter;
  broker::Message message;
};
Bytes encode_shared(const SharedDelivery& delivery, TimePoint now);
SharedDelivery decode_shared(std::span<const uint8_t> bytes, TimePoint now);

}  // namespace lir::cluster
