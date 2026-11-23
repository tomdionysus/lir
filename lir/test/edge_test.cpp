//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "lir/edge/edge.h"

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "dannan/listen/listener.h"
#include "dannan/listen/memory.h"
#include "dannan/test/fixtures/sim/world.h"
#include "lir/test/fixtures/cluster.h"
#include "lir/test/fixtures/stream_client.h"

// The edge (the rewrite's step 2, the transport): MQTT clients on Dannan's
// streams, served by each node's listener exactly as over TCP but carried in
// memory inside the simulation's world, with publishes crossing the cluster
// on Delivery. One node is a cluster of one, never a mode.
namespace lir::edge {
namespace {

using namespace std::chrono_literals;
using mqtt::Version;
using test::max_steps;
namespace reason = mqtt::reason;
namespace listen = dannan::listen;

// Accepts everyone, and remembers who it was asked about.
class Recording final : public contract::Authenticator {
 public:
  contract::AuthDecision connect(const mqtt::Connect& connect, const contract::Peer& peer) override {
    peers.push_back(peer.address);
    return _allow.connect(connect, peer);
  }
  contract::AuthDecision auth(const std::string& id, const std::string& method, const mqtt::Bytes& data, const contract::Peer& peer) override {
    return _allow.auth(id, method, data, peer);
  }
  std::vector<std::string> peers;

 private:
  contract::AllowAll _allow;
};

// Each node with its edge on a listener of Dannan's, reached in memory.
struct Cluster : test::Cluster {
  explicit Cluster(size_t size) : test::Cluster(size), time(world) {
    for (size_t i = 0; i < size; ++i) {
      auto& context = nodes.node(i).context();
      edges.push_back(std::make_unique<Edge>(*deliveries.at(i), authenticator, context.time(), context.logger("lir.link")));
      listen::ListenerConfig config;
      config.name = "mqtt";
      config.host = "127.0.0.1";
      listeners.push_back(std::make_unique<listen::StreamListener>(world.io, config, &time));
      listeners.back()->start(world.io.get_executor(), edges.back()->accepted());
    }
  }
  ~Cluster() {
    for (auto& listener : listeners) listener->stop();
    settle();
  }

  dannan::test::sim::Time time;
  Recording authenticator;
  std::vector<std::unique_ptr<Edge>> edges;
  std::vector<std::unique_ptr<listen::StreamListener>> listeners;
};

// A client on node `node`, in memory.
class Client : public test::StreamClient {
 public:
  Client(Cluster& cluster, size_t node, Version version = Version::v5, listen::MemoryOptions options = {})
      : StreamClient(cluster.world, *cluster.listeners.at(node), version, options) {}
};

TEST(Edge, AStreamIsServedAsOneClient) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  listen::MemoryOptions options;
  options.client_address = {boost::asio::ip::make_address("192.0.2.7"), 51000};
  Client client(cluster, 0, Version::v5, options);
  EXPECT_EQ(client.connect().reason, reason::success);
  // The authenticator is told who connected, as the stream knows it.
  ASSERT_EQ(cluster.authenticator.peers.size(), 1u);
  EXPECT_EQ(cluster.authenticator.peers[0], "192.0.2.7:51000");
  EXPECT_EQ(cluster.listeners[0]->counters().accepted, 1u);
}

TEST(Edge, APublishOnOneNodeReachesASubscriberOnAnother) {
  Cluster cluster(3);
  ASSERT_TRUE(cluster.joined);
  Client subscriber(cluster, 0);
  Client publisher(cluster, 2, Version::v3_1_1);
  subscriber.connect();
  publisher.connect();
  ASSERT_EQ(subscriber.subscribe("sensors/+").reasons, std::vector<uint8_t>{reason::granted_qos_0});
  publisher.send(test::publish_of("sensors/kitchen", "21"));
  const auto publish = subscriber.expect<mqtt::Publish>();
  EXPECT_EQ(publish.topic, "sensors/kitchen");
  EXPECT_EQ(publish.payload, (mqtt::Bytes{'2', '1'}));
}

// A packet is read whole however the stream splits it, one byte at a time
// included, both ways.
TEST(Edge, PacketsSplitAtEveryByteAreReadWhole) {
  Cluster cluster(2);
  ASSERT_TRUE(cluster.joined);
  Client subscriber(cluster, 0);
  Client publisher(cluster, 1);
  for (auto* client : {&subscriber, &publisher}) {
    client->link->set_pieces(listen::End::client, [](size_t) { return size_t{1}; });
    client->link->set_pieces(listen::End::server, [](size_t) { return size_t{1}; });
  }
  subscriber.connect();
  publisher.connect();
  ASSERT_EQ(subscriber.subscribe("a/#").reasons, std::vector<uint8_t>{reason::granted_qos_0});
  publisher.send(test::publish_of("a/b/c", std::string(300, 'x')));
  const auto publish = subscriber.expect<mqtt::Publish>();
  EXPECT_EQ(publish.topic, "a/b/c");
  EXPECT_EQ(publish.payload.size(), 300u);
}

// A refusal closes the stream after its CONNACK [MQTT-3.2.2-7]: here an
// authentication method the authenticator does not know.
TEST(Edge, ARefusedConnectEndsTheStream) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0);
  mqtt::Connect connect;
  connect.properties.authentication_method = "unknown";
  EXPECT_EQ(client.connect(connect).reason, reason::bad_authentication_method);
  EXPECT_TRUE(client.ends());
}

// A client that goes silent on a connection that stays open is closed once
// one and a half keep-alives pass [MQTT-3.1.2-22]: the half-open connection
// a stalled link is.
TEST(Edge, AStalledClientIsClosedByItsKeepAlive) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  Client client(cluster, 0);
  mqtt::Connect connect;
  connect.keep_alive = 2;
  client.connect(connect);
  client.link->stall();
  EXPECT_TRUE(cluster.world.run_until([&] { return cluster.listeners[0]->counters().open == 0; }, max_steps));
}

// A client that goes away is dropped: its stream is closed, and what is
// published after reaches the clients still there.
TEST(Edge, ASeveredClientIsDroppedAndTheOthersCarryOn) {
  Cluster cluster(2);
  ASSERT_TRUE(cluster.joined);
  Client gone(cluster, 0);
  Client staying(cluster, 0);
  Client publisher(cluster, 1);
  gone.connect();
  staying.connect();
  publisher.connect();
  gone.subscribe("t");
  staying.subscribe("t");
  gone.link->sever();
  EXPECT_TRUE(cluster.world.run_until([&] { return cluster.listeners[0]->counters().open == 1; }, max_steps));
  publisher.send(test::publish_of("t", "after"));
  EXPECT_EQ(staying.expect<mqtt::Publish>().payload, (mqtt::Bytes{'a', 'f', 't', 'e', 'r'}));
}

}  // namespace
}  // namespace lir::edge
