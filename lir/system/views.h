//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <atomic>
#include <boost/asio/any_io_executor.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "dannan/context.h"
#include "lir/mqtt/packet.h"

// The system topics' views (TODO/2026-11-22-system-topics.md): what one node
// renders for its own subscribers from what Dannan already gossips to every
// node, and from nothing else. No node publishes a view for another, nothing
// goes on Lir's channel and nothing is stored. Every payload is JSON and says
// which node rendered it ("from"): during a split, or while gossip converges,
// two nodes may answer differently, and each says whose answer it is.
//
// - `$lir/nodes/<id>`: each Lir node's figures, from its "lir.figures" advert;
//   an empty message once it is withdrawn or the node leaves.
// - `$lir/cluster`: the same figures summed, computed alike on every node.
// - `$dannan/nodes/<id>`: each node of the roster, as this node sees it; empty
//   once it leaves.
// - `$dannan/roster/joined`, `/left`, `/changed`: each roster change as this
//   node saw it, never kept.
//
// The connection events (`$lir/clients/...`) are not views: the node that saw
// one publishes it on the channel (broker::Link).
namespace lir::system {

// The advert each Lir node keeps its figures in.
inline constexpr std::string_view figures_advert = "lir.figures";

class Views {
 public:
  // A message for a subscriber: a view's current state (RETAIN 1, sent
  // because of a new subscription), a change to one, or an event.
  using Handler = std::function<void(const mqtt::Publish&)>;

  // Watches the roster and the figures adverts on a strand of the node's
  // executor. `context` outlives the views.
  explicit Views(dannan::Context& context, std::chrono::milliseconds advert_every = std::chrono::seconds(1));
  ~Views();
  Views(const Views&) = delete;
  Views& operator=(const Views&) = delete;

  // This node's id, as every payload's "from" says it.
  const std::string& self() const noexcept { return _self; }

  // Each change and event under `filter` from now on, posted to `executor`
  // in order when it is a strand, until unsubscribe. Waits on nothing.
  uint64_t subscribe(std::string filter, boost::asio::any_io_executor executor, Handler handler);
  void unsubscribe(uint64_t id);
  // Every current view under `filter`, each with RETAIN 1, posted to
  // `executor`; ordered with the changes subscribe() sends to the same
  // executor. Waits on nothing.
  void current(std::string filter, boost::asio::any_io_executor executor, Handler handler);

  // A client's publish this node accepted, for its figures. Thread-safe.
  void published() noexcept { _published.fetch_add(1, std::memory_order_relaxed); }
  uint64_t published_count() const noexcept { return _published.load(std::memory_order_relaxed); }

  // This node's own figures, as it advertises them (nothing: withdrawn).
  // Dannan's advert watch tells a node its peers' adverts, never its own.
  // Waits on nothing.
  void own_figures(std::optional<mqtt::Bytes> figures);

 private:
  struct Inner;

  const std::string _self;
  std::shared_ptr<Inner> _inner;
  std::atomic<uint64_t> _published{0};
};

}  // namespace lir::system
