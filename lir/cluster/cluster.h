//
// Lir - The Distributed Event System
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <chrono>
#include <memory>

#include "dannan/context.h"
#include "lir/edge/edge.h"

// The cluster: one MQTT broker across every node advertising "lir", over
// Dannan and nothing else (step 4, before Dagda; ACTIVE.md says what is
// interim).
//
// - A message published on a node goes to the nodes with matching
//   subscribers in one hop, on Dannan channels: QoS 0 on lir.notify (at
//   most once), QoS 1 and 2 on lir.seq (at least once, in order per origin,
//   deduplicated here by sequence). Each node subscribes there by the
//   literal prefix of its filters, so Dannan sends a node only what it may
//   want; the broker matches the filters.
// - Retained values are held on every node, versioned (broker.h), spread
//   on lir.retained and fetched whole from each node that joins.
// - Shared subscriptions: each node publishes its groups on lir.groups;
//   the publishing node gives each message to one member node, in turn,
//   which gives it to one member session.
// - Sessions live where their client is connected and move when it
//   connects elsewhere: the new node asks every other to let go
//   (lir.session_take), and takes the session if one had it.
//
// Thread safety: every member is touched on the edge's strand; the route
// handlers Dannan calls on its own threads reach the broker through the
// edge.
namespace lir::cluster {

// Lir's message types (Dannan user types, from 512, registered freely).
inline constexpr dannan::net::MessageType snapshot_type{0x4C00};
inline constexpr dannan::net::MessageType take_type{0x4C01};
inline constexpr dannan::net::MessageType shared_type{0x4C02};

struct ClusterConfig {
  // How long a node waits for a peer's answer.
  std::chrono::milliseconds call_deadline{2000};
  // How often this node republishes its shared groups, besides on change.
  std::chrono::milliseconds resync_every{30000};
  // Each stream channel's replay buffer.
  size_t replay_bytes = 4u << 20;
};

class Cluster {
 public:
  // Registers Lir's routes: before the node starts. The context and the
  // edge outlive the cluster.
  Cluster(dannan::Context& context, edge::Edge& edge, ClusterConfig config = {});
  // Stops if running.
  ~Cluster();
  Cluster(const Cluster&) = delete;
  Cluster& operator=(const Cluster&) = delete;

  // After the node has started: opens the channels, advertises "lir",
  // watches the roster and attaches to the broker. Waits on the edge's
  // strand; never from one of the io context's threads.
  void start();
  // Detaches from the broker and the roster and unbinds the routes. Waits
  // on the strand and on route calls in flight; never from an io thread.
  void stop();

 private:
  struct State;
  std::shared_ptr<State> _state;
};

}  // namespace lir::cluster
