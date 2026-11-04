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

#include "dannan/context.h"
#include "lir/edge/edge.h"

// The system topics: the substrate's and Lir's own state as retained MQTT
// messages, so dashboards and automation need no other feed (design,
// "In scope"). Each node publishes its own, as JSON, whenever it changes
// (checked every `every`), and every node clears a node's topics when it
// leaves the roster:
//
//   $lir/nodes/<node id>     connections, sessions, retained, message counts
//   $dannan/nodes/<node id>  the node's record, its peers and their latency
//
// Clients may subscribe but never publish there (broker.h,
// reserved_prefixes).
namespace lir::system {

struct SystemConfig {
  std::chrono::milliseconds every{5000};
};

// The JSON a node publishes; pure, for tests.
std::string lir_json(const std::string& node, size_t connections, size_t sessions, size_t retained, const broker::Stats& stats);
std::string dannan_json(const dannan::membership::NodeRecord& self, const std::vector<dannan::membership::NodeRecord>& active,
                        const std::map<dannan::identity::NodeId, std::chrono::milliseconds>& latencies);
std::string escaped(const std::string& text);

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
