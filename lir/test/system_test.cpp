//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "lir/system/system.h"

#include <gtest/gtest.h>

namespace lir::system {
namespace {

TEST(SystemJson, ConnectedEvent) {
  broker::ClientEvent e;
  e.client_id = "dev\"1";
  e.version = mqtt::Version::v3_1_1;
  e.peer = "127.0.0.1:5";
  e.username = "u";
  e.session_present = true;
  EXPECT_EQ(client_event_json("n1", e), R"({"client":"dev\"1","node":"n1","version":"3.1.1","peer":"127.0.0.1:5","username":"u","session_present":true})");
}

TEST(SystemJson, DisconnectedEventSaysWhoAndWhy) {
  broker::ClientEvent e;
  e.kind = broker::ClientEvent::Kind::disconnected;
  e.client_id = "c";
  e.peer = "p";
  e.by = broker::ClientEvent::By::server;
  e.reason = 0x8E;
  EXPECT_EQ(client_event_json("n", e), R"({"client":"c","node":"n","version":"5.0","peer":"p","by":"server","reason":142})");
  e.by = broker::ClientEvent::By::network;
  e.reason.reset();
  EXPECT_EQ(client_event_json("n", e), R"({"client":"c","node":"n","version":"5.0","peer":"p","by":"network"})");
}

TEST(SystemJson, LimitsAgreeOrNameTheDifference) {
  broker::Config one;
  broker::Config other;
  other.maximum_packet_size = 4096;
  other.receive_maximum = 10;
  EXPECT_TRUE(differing_limits({limits_advert(one), limits_advert(one)}).empty());
  EXPECT_EQ(differing_limits({limits_advert(one), limits_advert(other)}), (std::vector<std::string>{"maximum_packet_size", "receive_maximum"}));
  EXPECT_TRUE(differing_limits({}).empty());
}

TEST(SystemJson, ALimitOneNodeLacksDiffers) { EXPECT_EQ(differing_limits({"a=1;b=2", "a=1"}), (std::vector<std::string>{"b"})); }

TEST(SystemJson, ClusterSummary) {
  ClusterFigures f;
  f.publisher = "n";
  f.nodes = 3;
  f.connections = 7;
  f.sessions = 9;
  f.received_per_second = 120;
  EXPECT_EQ(cluster_json(f),
            R"({"publisher":"n","nodes":3,"connections":7,"sessions":9,"received_per_second":120,"limits":{"consistent":true,"differing":[]}})");
  f.differing = {"receive_maximum"};
  EXPECT_NE(cluster_json(f).find(R"("limits":{"consistent":false,"differing":["receive_maximum"]})"), std::string::npos);
}

}  // namespace
}  // namespace lir::system
