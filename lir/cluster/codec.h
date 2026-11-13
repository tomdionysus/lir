//
// Lir - The Message Broker
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

// The answer: the session, if this node held one, and whether it was the
// live session or a standby's mirror of it.
struct Taken {
  std::optional<broker::SessionImage> image;
  bool mirror = false;
};
Bytes encode_taken(const Taken& taken, TimePoint now);
Taken decode_taken(std::span<const uint8_t> bytes, TimePoint now);

// A session's owner to its standbys: the session as it now is, or that it
// has ended (or left the owner).
struct Mirror {
  std::string client_id;
  std::optional<broker::SessionImage> image;
};
Bytes encode_mirror(const Mirror& mirror, TimePoint now);
Mirror decode_mirror(std::span<const uint8_t> bytes, TimePoint now);

// A message for one member of a shared group on the receiving node.
struct SharedDelivery {
  std::string group;
  std::string filter;
  broker::Message message;
};
Bytes encode_shared(const SharedDelivery& delivery, TimePoint now);
SharedDelivery decode_shared(std::span<const uint8_t> bytes, TimePoint now);

// What Lir keeps in Dagda's logs (step 5 design, "The mapping"), under a
// format number and kind of their own. These outlive the node and process
// that wrote them, and a steady clock does not survive a restart, so an
// expiry is kept as wall-clock time: `Clocks` pairs the steady time a
// message is decided by with the wall clock, read at the same moment.
inline constexpr uint8_t stored_format = 1;

struct Clocks {
  TimePoint now;
  uint64_t wall_ms = 0;
};

// A message's identity: the publishing node and its sequence (sequence.h).
struct Dot {
  std::string origin;
  uint64_t seq = 0;
  auto operator<=>(const Dot&) const = default;
};

// A QoS 1 or 2 message in its topic shard's inflight log, appended under the
// publishing node's dot before PUBACK. For QoS 2 the publishing client's
// packet id is recorded with it, so a resend after that node's death is
// known for what it is.
struct InflightEntry {
  broker::Message message;
  std::optional<uint16_t> packet_id;
};
Bytes encode_inflight(const InflightEntry& entry, Clocks clocks);
InflightEntry decode_inflight(std::span<const uint8_t> bytes, Clocks clocks);

// A delivery held for an offline session in its session shard's queue log.
// It is appended under the holding node's own dot, since a log refuses a dot
// whose origin is not the appender; the publisher's dot rides here, for the
// session's dedup.
struct QueueEntry {
  Dot publisher;
  broker::Delivery delivery;
};
Bytes encode_queued(const QueueEntry& entry, Clocks clocks);
QueueEntry decode_queued(std::span<const uint8_t> bytes, Clocks clocks);

}  // namespace lir::cluster
