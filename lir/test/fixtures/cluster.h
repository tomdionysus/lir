//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "dannan/test/fixtures/sim/nodes.h"
#include "lir/broker/delivery.h"

// A cluster of Dannan's simulated nodes in one world, each with its Delivery,
// joined: what every test of Lir past the codec runs on. One node is a cluster
// of one, never a mode.
namespace lir::test {

inline constexpr uint64_t max_steps = 2'000'000;

inline bool everyone_knows_everyone(dannan::test::sim::SimNodes& nodes) {
  for (size_t i = 0; i < nodes.size(); ++i) {
    const auto roster = nodes.node(i).context().nodes().active();
    for (size_t j = 0; j < nodes.size(); ++j)
      if (std::none_of(roster.begin(), roster.end(), [&](const auto& record) { return record.id == nodes.id(j); })) return false;
  }
  return true;
}

inline mqtt::Publish publish_of(std::string topic, std::string payload) {
  mqtt::Publish publish;
  publish.topic = std::move(topic);
  publish.payload = mqtt::Bytes(payload.begin(), payload.end());
  return publish;
}

// Delay but no loss, and links that delay without overtaking, as one
// Dannan session does (Dannan 0.11.1): Lir's channel is at most once and in
// order per origin, and the tests on it are about where a publish goes, not
// about loss. A test of what overtaking does passes its own faults.
struct Cluster {
  explicit Cluster(size_t size, uint64_t seed = 1, dannan::test::sim::NetworkFaults network = faults())
      : world(seed), nodes(world, size, network, 0, 0, false) {
    nodes.host = [this](size_t i, dannan::Node& node, bool) {
      if (deliveries.size() <= i) deliveries.resize(i + 1);
      deliveries[i] = std::make_unique<broker::Delivery>(node.context().channels());
    };
    nodes.start_all();
    joined = world.run_until([&] { return everyone_knows_everyone(nodes); }, max_steps);
  }
  static dannan::test::sim::NetworkFaults faults() {
    dannan::test::sim::NetworkFaults out;
    out.max_delay_ms = 20;
    out.in_order = true;
    return out;
  }

  // Runs the world until nothing is in flight.
  void settle() {
    world.run_until([] { return false; }, 200'000);
  }

  dannan::test::sim::World world;
  dannan::test::sim::SimNodes nodes;
  std::vector<std::unique_ptr<broker::Delivery>> deliveries;
  bool joined = false;
};

}  // namespace lir::test
