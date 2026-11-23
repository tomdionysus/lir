//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <memory>
#include <vector>

#include "dannan/test/fixtures/sim/nodes.h"
#include "dannan/test/fixtures/sim/world.h"
#include "lir/contract/authenticator.h"
#include "lir/edge/server.h"
#include "lir/test/fixtures/cluster.h"

// A cluster of Dannan's simulated nodes, each running Lir's server as a
// lir-node does, joined: started with its node, and stopped before it.
namespace lir::test {

struct Servers {
  // `configure` adjusts each node's server before it starts.
  explicit Servers(size_t size, uint64_t seed = 1, std::function<void(edge::ServerConfig&)> configure = {})
      : world(seed), time(world), nodes(world, size, Cluster::faults(), 0, 0, false) {
    servers.resize(size);
    nodes.host = [this, configure](size_t i, dannan::Node& node, bool) {
      edge::ServerConfig config;
      config.address = "127.0.0.1";
      config.port = 0;
      if (configure) configure(config);
      servers[i] = std::make_unique<edge::Server>(world.io, node.context(), authenticator, config, &time);
      servers[i]->start();
    };
    nodes.stopping.push_back([this](size_t i) { stop_server(i); });
    nodes.start_all();
    joined = world.run_until([&] { return everyone_knows_everyone(nodes); }, max_steps);
  }
  // The nodes go after the servers, and must not reach back for them.
  ~Servers() {
    for (size_t i = 0; i < servers.size(); ++i) stop_server(i);
    nodes.stopping.clear();
  }

  void stop_server(size_t i) {
    if (!servers[i]) return;
    bool done = false;
    servers[i]->stop(world.io.get_executor(), [&] { done = true; });
    world.run_until([&] { return done; }, max_steps);
    servers[i].reset();
  }
  std::string id(size_t i) { return dannan::identity::to_string(nodes.id(i)); }

  dannan::test::sim::World world;
  dannan::test::sim::Time time;
  dannan::test::sim::SimNodes nodes;
  contract::AllowAll authenticator;
  std::vector<std::unique_ptr<edge::Server>> servers;
  bool joined = false;
};

}  // namespace lir::test
