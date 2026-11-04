//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <gtest/gtest.h>

#include "lir/cluster/balance.h"

namespace lir::cluster {
namespace {

const Endpoints everywhere{{"tcp", "h:1883"}, {"ws", "h:8080"}};

Load node(std::string id, uint64_t clients, Endpoints endpoints = everywhere) { return {std::move(id), clients, std::move(endpoints)}; }

TEST(ClusterBalance, EndpointsRoundTrip) {
  const Endpoints e{{"tcp", "10.0.0.1:1883"}, {"wss", "10.0.0.1:8081"}};
  EXPECT_EQ(endpoints_advert(e), "tcp=10.0.0.1:1883;wss=10.0.0.1:8081");
  EXPECT_EQ(parse_endpoints(endpoints_advert(e)), e);
  EXPECT_TRUE(parse_endpoints("").empty());
  EXPECT_EQ(parse_endpoints("tcp=a:1;junk;=x;ws="), (Endpoints{{"tcp", "a:1"}}));
}

TEST(ClusterBalance, ABalancedClusterServesHere) {
  EXPECT_FALSE(choose_redirect(node("a", 10), {node("b", 10), node("c", 9)}, "tcp", 0.2));
  // 12 against an average of 10.3: within 20%.
  EXPECT_FALSE(choose_redirect(node("a", 12), {node("b", 10), node("c", 9)}, "tcp", 0.2));
}

TEST(ClusterBalance, ABusyNodeSendsToTheLeastLoaded) {
  const auto chosen = choose_redirect(node("a", 30), {node("b", 10, {{"tcp", "b:1883"}}), node("c", 5, {{"tcp", "c:1883"}})}, "tcp", 0.2);
  ASSERT_TRUE(chosen);
  EXPECT_EQ(chosen->node, "c");
  EXPECT_EQ(chosen->address, "c:1883");
}

TEST(ClusterBalance, OnlyANodeServingTheTransport) {
  const auto chosen = choose_redirect(node("a", 30), {node("b", 10, {{"ws", "b:8080"}}), node("c", 5, {{"tcp", "c:1883"}})}, "ws", 0.2);
  ASSERT_TRUE(chosen);
  EXPECT_EQ(chosen->address, "b:8080");
  EXPECT_FALSE(choose_redirect(node("a", 30), {node("b", 10, {{"tcp", "b:1"}})}, "wss", 0.2));
}

TEST(ClusterBalance, TiesGoToTheLowestId) {
  const auto chosen = choose_redirect(node("m", 30), {node("z", 5), node("b", 5)}, "tcp", 0.2);
  ASSERT_TRUE(chosen);
  EXPECT_EQ(chosen->node, "b");
}

TEST(ClusterBalance, NeverToANodeNearlyAsBusy) {
  // One client apart: moving it would only swap the load.
  EXPECT_FALSE(choose_redirect(node("a", 2), {node("b", 1)}, "tcp", 0.2));
  EXPECT_TRUE(choose_redirect(node("a", 3), {node("b", 1)}, "tcp", 0.2));
}

TEST(ClusterBalance, OffOrAloneNeverRedirects) {
  EXPECT_FALSE(choose_redirect(node("a", 100), {node("b", 0)}, "tcp", -1));
  EXPECT_FALSE(choose_redirect(node("a", 100), {}, "tcp", 0.2));
}

}  // namespace
}  // namespace lir::cluster
