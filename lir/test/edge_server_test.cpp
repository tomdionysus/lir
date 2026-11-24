//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "dannan/test/fixtures/child_process.h"
#include "dannan/test/fixtures/sim/nodes.h"
#include "dannan/test/fixtures/sim/world.h"
#include "lir/client/roster.h"
#include "lir/edge/server.h"
#include "lir/test/fixtures/cluster.h"
#include "lir/test/fixtures/servers.h"

// Lir on each node of a cluster (the rewrite's step 2): every node runs the
// same server, and says where it serves in Dannan's roster, which is the one
// list a client or another node finds Lir in. None is special.
namespace lir::edge {
namespace {

using std::chrono::steady_clock;
using test::max_steps;
using namespace std::chrono_literals;

struct Cluster : test::Servers {
  using Servers::Servers;

  // What node `from` reads in its roster: where each Lir node serves.
  std::vector<client::Address> addresses(size_t from) { return client::lir_addresses(nodes.node(from).context().nodes().active(), "tcp"); }
  bool sees(size_t from, std::vector<uint16_t> ports) {
    std::vector<uint16_t> seen;
    for (const auto& address : addresses(from)) seen.push_back(address.port);
    std::sort(seen.begin(), seen.end());
    std::sort(ports.begin(), ports.end());
    return seen == ports;
  }
};

TEST(EdgeServer, EveryNodeSaysWhereItServes) {
  Cluster cluster(3);
  ASSERT_TRUE(cluster.joined);
  std::vector<uint16_t> ports;
  for (const auto& server : cluster.servers) ports.push_back(server->port());
  for (size_t i = 0; i < 3; ++i) EXPECT_TRUE(cluster.world.run_until([&] { return cluster.sees(i, ports); }, max_steps)) << "node " << i;
  for (const auto& address : cluster.addresses(0)) EXPECT_EQ(address.host, "127.0.0.1");
}

// A node that stops serving withdraws its adverts first, so no client is sent
// to it; the others keep theirs.
TEST(EdgeServer, AStoppedServerIsNoLongerFound) {
  Cluster cluster(3);
  ASSERT_TRUE(cluster.joined);
  const auto kept = std::vector<uint16_t>{cluster.servers[0]->port(), cluster.servers[2]->port()};
  cluster.stop_server(1);
  for (const size_t i : {size_t{0}, size_t{2}}) EXPECT_TRUE(cluster.world.run_until([&] { return cluster.sees(i, kept); }, max_steps)) << "node " << i;
}

// A node that leaves the cluster leaves the list with it, and one that comes
// back is found again at its new port.
TEST(EdgeServer, ARestartedNodeIsFoundAgain) {
  Cluster cluster(3);
  ASSERT_TRUE(cluster.joined);
  cluster.nodes.restart(2);
  ASSERT_TRUE(cluster.servers[2]);
  const auto ports = std::vector<uint16_t>{cluster.servers[0]->port(), cluster.servers[1]->port(), cluster.servers[2]->port()};
  EXPECT_TRUE(cluster.world.run_until([&] { return cluster.sees(0, ports); }, max_steps));
}

// wss is WebSocket on TLS: without its pair there is no wss to serve. A
// transport not served has no listener and no port.
TEST(EdgeServer, WssWithoutTlsIsRefusedAndAnUnservedTransportHasNoListener) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  ServerConfig wss_alone;
  wss_alone.wss = Endpoint{"127.0.0.1", 0, std::nullopt};
  EXPECT_THROW(Server(cluster.world.io, cluster.nodes.node(0).context(), cluster.authenticator, wss_alone, &cluster.time), std::invalid_argument);
  EXPECT_EQ(cluster.servers[0]->listener_of("ws"), nullptr);
  EXPECT_EQ(cluster.servers[0]->port_of("wss"), 0);
  EXPECT_EQ(cluster.servers[0]->tls_port(), 0);
}

// Runs the world no faster than real time, for a real client in a process of
// its own to keep pace with, until `done` holds or `patience` of real time has
// passed: real sockets are served whenever the world's io is polled.
template <class Done>
bool run_in_real_time(dannan::test::sim::World& world, Done&& done, std::chrono::milliseconds patience) {
  const auto start = steady_clock::now();
  const auto simulated = world.now_ms();
  while (steady_clock::now() - start < patience) {
    if (done()) return true;
    const auto real = std::chrono::duration_cast<std::chrono::milliseconds>(steady_clock::now() - start).count();
    if (world.now_ms() - simulated <= static_cast<uint64_t>(real) && world.step()) continue;
    world.io.restart();
    if (world.io.poll() == 0) std::this_thread::sleep_for(1ms);
  }
  return done();
}

// On a terminal, a client writes each line as it goes; into a pipe it holds
// them until it exits.
dannan::test::ChildOptions terminal() {
  dannan::test::ChildOptions out;
  out.terminal = true;
  return out;
}

std::filesystem::path log_for(const std::string& name) {
  const auto dir = std::filesystem::path(DANNAN_TEST_TEMP_ROOT) / "edge-server";
  std::filesystem::create_directories(dir);
  return dir / (name + ".log");
}

// Real MQTT clients (the maintainer, 2026-11-20: "You will test with a real
// MQTT client"): mosquitto_sub on one node, connected over TCP to the port its
// roster advert names, receives what mosquitto_pub sends to another node. The
// publisher repeats until the subscriber has one: at QoS 0 nothing tells a
// publisher when the subscription is held across the cluster.
TEST(EdgeServer, RealClientsOnDifferentNodesReachEachOther) {
  Cluster cluster(3);
  ASSERT_TRUE(cluster.joined);
  for (const std::string version : {"5", "311"}) {
    SCOPED_TRACE("MQTT " + version);
    // Both with -d: their account of each packet, shown if the test fails.
    dannan::test::ChildProcess subscriber({LIR_MOSQUITTO_SUB, "-d", "-h", "127.0.0.1", "-p", std::to_string(cluster.servers[0]->port()), "-V", version, "-i",
                                           "lir-sub-" + version, "-t", "sensors/#", "-C", "1", "-W", "30"},
                                          log_for("sub-" + version), terminal());
    std::optional<dannan::test::ChildProcess> publisher;
    std::vector<std::string> said;
    std::string published;
    bool received = false;
    auto next_publish = steady_clock::now();
    auto next_look = steady_clock::now();
    run_in_real_time(
        cluster.world,
        [&] {
          // Lines are looked for every 50 ms, with 2 ms to arrive in: a read
          // counts whole milliseconds left, and with none does not look.
          if (steady_clock::now() >= next_look) {
            while (const auto line = subscriber.line(steady_clock::now() + 2ms)) {
              said.push_back(*line);
              received = received || *line == "21";
            }
            next_look = steady_clock::now() + 50ms;
          }
          if (received) return true;
          if ((!publisher || publisher->wait(steady_clock::now())) && steady_clock::now() >= next_publish) {
            if (publisher) published = publisher->read_all(steady_clock::now() + 2ms);
            publisher.reset();
            publisher.emplace(std::vector<std::string>{LIR_MOSQUITTO_PUB, "-d", "-h", "127.0.0.1", "-p", std::to_string(cluster.servers[2]->port()), "-V",
                                                       version, "-i", "lir-pub-" + version, "-t", "sensors/kitchen", "-m", "21"},
                              log_for("pub-" + version), terminal());
            next_publish = steady_clock::now() + 500ms;
          }
          return false;
        },
        30s);
    std::string account;
    for (const auto& line : said) account += line + "\n";
    EXPECT_TRUE(received) << "mosquitto_sub said:\n" << account << "the last mosquitto_pub said:\n" << published;
    EXPECT_EQ(subscriber.wait(steady_clock::now() + 5s), 0);
  }
}

}  // namespace
}  // namespace lir::edge
