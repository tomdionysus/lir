//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "dannan/context.h"
#include "lir/edge/edge.h"

// The system topics: the substrate's and Lir's own state as MQTT messages,
// so dashboards and automation need no other feed (design, "In scope" and
// section 3). Each is configurable and on by default. All are JSON:
//
//   $lir/nodes/<node id>     retained: connections, sessions, retained,
//                            message counts. Each node its own, on change
//                            (checked every `every`); cleared by every node
//                            when the node leaves the roster.
//   $dannan/nodes/<node id>  retained: the node's record, its peers and
//                            their latency; as $lir/nodes.
//   $lir/cluster             retained: node count, connections, sessions,
//                            messages received a second, and whether the
//                            nodes' limits agree (section 4). Published by
//                            the Lir node with the lowest id, from the
//                            figures each Lir node advertises in its Dannan
//                            record.
//   $lir/clients/connected, $lir/clients/disconnected
//                            events: a client of this node connecting or
//                            its connection ending, and why.
//   $dannan/roster/joined, /left, /changed
//                            events: a node joining, leaving, or changing
//                            (a restart, or a new place, capability or path;
//                            not its adverts). Published by the Lir node
//                            with the lowest id.
//
// Clients may subscribe but never publish there (broker.h,
// reserved_prefixes).
namespace lir::system {

struct SystemConfig {
  std::chrono::milliseconds every{5000};
  bool nodes = true;
  bool cluster = true;
  bool clients = true;
  bool roster = true;
};

// The JSON a node publishes; pure, for tests.
std::string lir_json(const std::string& node, size_t connections, size_t sessions, size_t retained, const broker::Stats& stats);
std::string dannan_json(const dannan::membership::NodeRecord& self, const std::vector<dannan::membership::NodeRecord>& active,
                        const std::map<dannan::identity::NodeId, std::chrono::milliseconds>& latencies);
std::string escaped(const std::string& text);
std::string client_event_json(const std::string& node, const broker::ClientEvent& event);
std::string roster_event_json(const dannan::membership::NodeRecord& node);
// A node's limits as advertised ("name=value;..."), and which names differ
// between nodes' adverts, sorted.
std::string limits_advert(const broker::Config& config);
std::vector<std::string> differing_limits(const std::vector<std::string>& adverts);
// The cluster summary's JSON.
struct ClusterFigures {
  std::string publisher;
  size_t nodes = 0;
  uint64_t connections = 0;
  uint64_t sessions = 0;
  uint64_t received_per_second = 0;
  std::vector<std::string> differing;
};
std::string cluster_json(const ClusterFigures& figures);

class SystemTopics {
 public:
  // The context and the edge outlive it.
  SystemTopics(dannan::Context& context, edge::Edge& edge, SystemConfig config = {});
  ~SystemTopics();
  SystemTopics(const SystemTopics&) = delete;
  SystemTopics& operator=(const SystemTopics&) = delete;

  // Publishes now and then on change; watches the roster. Waits on the
  // edge's strand; never from an io thread.
  void start();
  void stop();

 private:
  struct State;
  std::shared_ptr<State> _state;
};

}  // namespace lir::system
