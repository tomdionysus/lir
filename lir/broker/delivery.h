//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "dannan/context.h"
#include "dannan/membership/node_record.h"
#include "lir/mqtt/packet.h"

// Delivery across the cluster (the rewrite's design, "Delivery"): a publish
// reaches every node with a matching subscription, and each node matches its
// own subscribers' filters. It is Dannan's channel and nothing of Lir's: one
// channel, `channel_name`, opened as ordered (at most once and in order per
// origin, QoS 0, so every topic is ordered [MQTT-4.6.0-6]); each
// subscription subscribes its filter's literal prefix, so Dannan's gossiped
// interest sends a publish only where a filter could match it, and the filter
// itself decides here. On the channel a publish is the publishing client's
// id, as an MQTT string, then its 5.0 PUBLISH packet: the id is what No Local
// compares [MQTT-3.8.3-3].
namespace lir::broker {

inline constexpr std::string_view channel_name = "lir";

// The literal front of a valid filter, up to its first wildcard level: every
// topic the filter matches starts with it. A trailing "/#" also matches the
// parent level (MQTT 5.0 4.7.1.2), so its separator is left off.
std::string interest_prefix(std::string_view filter);

// One node's. Thread-safe as Dannan's Channels is; it holds no state of its
// own.
class Delivery {
 public:
  // Each publish whose topic the subscription's filter matches, with the
  // client id of the connection that published it.
  using Handler = std::function<void(const mqtt::Publish&, const std::string& publisher)>;

  // Opens the channel on this node. Throws std::invalid_argument if it is
  // already open: one Delivery a node.
  explicit Delivery(dannan::Channels& channels);
  Delivery(const Delivery&) = delete;
  Delivery& operator=(const Delivery&) = delete;

  // `handler` runs on `executor`, in order per origin when it is a strand.
  // `held` is posted to `done_executor` once the interest is held by the
  // peers it names, or `deadline` passed: a publish made after that reaches
  // this subscription, so SUBACK waits on it. Throws std::invalid_argument for
  // an invalid filter or a shared one (a group's, not a node's).
  uint64_t subscribe(std::string filter, boost::asio::any_io_executor executor, Handler handler, std::chrono::milliseconds deadline,
                     boost::asio::any_io_executor done_executor, std::function<void(dannan::membership::Pushed)> held);
  void unsubscribe(uint64_t id);

  // Sent now, never waiting: where it went is the evidence. `publisher` is
  // the publishing connection's client id. Throws std::invalid_argument for a
  // topic that is not a valid name, a topic alias (one connection's, resolved
  // before here), QoS above 0 (Maximum QoS 0 until Dagda's log), or a
  // publisher that is not an MQTT string.
  dannan::net::Published publish(const mqtt::Publish& publish, std::string_view publisher);

 private:
  dannan::Channels& _channels;
};

}  // namespace lir::broker
