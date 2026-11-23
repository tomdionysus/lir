//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "lir/broker/delivery.h"
#include "lir/test/fixtures/cluster.h"

// Delivery across the cluster (the rewrite's step 2): a publish made on any
// node reaches every subscription whose filter matches it, on whichever node
// holds it, over Dannan's notify channel. Every test runs on Dannan's
// simulated nodes in one world; one node is a cluster of one, never a mode.
namespace lir::broker {
namespace {

using namespace std::chrono_literals;
using test::max_steps;
using test::publish_of;

struct Cluster : test::Cluster {
  using test::Cluster::Cluster;

  // Subscribes `filter` on node `i`, recording what reaches it, and runs the
  // world until the interest is held (what SUBACK waits on).
  std::shared_ptr<std::vector<mqtt::Publish>> subscribe(size_t i, const std::string& filter, uint64_t* id = nullptr) {
    auto received = std::make_shared<std::vector<mqtt::Publish>>();
    std::optional<dannan::membership::Pushed> held;
    const auto executor = world.io.get_executor();
    const auto made = deliveries.at(i)->subscribe(
        filter, executor, [received](const mqtt::Publish& publish, const std::string&) { received->push_back(publish); }, 5s, executor,
        [&held](dannan::membership::Pushed pushed) { held = std::move(pushed); });
    if (id) *id = made;
    EXPECT_TRUE(world.run_until([&] { return held.has_value(); }, max_steps)) << filter;
    if (held) {
      EXPECT_TRUE(held->missed.empty()) << filter;
    }
    return received;
  }
};

std::vector<std::string> topics_of(const std::vector<mqtt::Publish>& received) {
  std::vector<std::string> out;
  for (const auto& publish : received) out.push_back(publish.topic);
  return out;
}

TEST(InterestPrefix, IsTheFilterUpToItsFirstWildcard) {
  EXPECT_EQ(interest_prefix("a/b/c"), "a/b/c");
  EXPECT_EQ(interest_prefix("a/+/c"), "a/");
  EXPECT_EQ(interest_prefix("+/b"), "");
  EXPECT_EQ(interest_prefix("#"), "");
  // "sport/#" also matches "sport" itself (4.7.1.2), so the separator goes.
  EXPECT_EQ(interest_prefix("sport/#"), "sport");
  EXPECT_EQ(interest_prefix("$SYS/#"), "$SYS");
  EXPECT_EQ(interest_prefix("a//+"), "a//");
}

TEST(Delivery, APublishOnOneNodeReachesASubscriberOnAnother) {
  Cluster cluster(3);
  ASSERT_TRUE(cluster.joined);
  const auto received = cluster.subscribe(2, "sensors/kitchen");
  const auto published = cluster.deliveries[0]->publish(publish_of("sensors/kitchen", "21.5"), "p");
  EXPECT_NE(std::find(published.to.begin(), published.to.end(), cluster.nodes.id(2)), published.to.end());
  ASSERT_TRUE(cluster.world.run_until([&] { return !received->empty(); }, max_steps));
  cluster.settle();
  ASSERT_EQ(received->size(), 1u);
  EXPECT_EQ(received->front(), publish_of("sensors/kitchen", "21.5"));
}

TEST(Delivery, ASubscriberOnThePublishingNodeGetsItToo) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  const auto received = cluster.subscribe(0, "a/b");
  (void)cluster.deliveries[0]->publish(publish_of("a/b", "x"), "p");
  cluster.settle();
  EXPECT_EQ(topics_of(*received), std::vector<std::string>{"a/b"});
}

// Every node holds a different filter; each gets exactly the topics its filter
// matches, wherever the publish was made.
TEST(Delivery, EachSubscriptionGetsWhatItsFilterMatches) {
  for (uint64_t seed = 1; seed <= 5; ++seed) {
    SCOPED_TRACE("seed " + std::to_string(seed));
    Cluster cluster(4, seed);
    ASSERT_TRUE(cluster.joined);
    const auto plus = cluster.subscribe(0, "a/+/c");
    const auto hash = cluster.subscribe(1, "a/#");
    const auto all = cluster.subscribe(2, "#");
    const auto system = cluster.subscribe(3, "$SYS/#");
    const auto exact = cluster.subscribe(3, "a/b");
    const std::vector<std::string> topics{"a/b/c", "a", "a/b", "a/bc", "b/c", "$SYS/nodes"};
    for (size_t i = 0; i < topics.size(); ++i) (void)cluster.deliveries[i % 4]->publish(publish_of(topics[i], topics[i]), "p");
    cluster.settle();
    auto sorted = [](std::vector<std::string> v) {
      std::sort(v.begin(), v.end());
      return v;
    };
    EXPECT_EQ(sorted(topics_of(*plus)), (std::vector<std::string>{"a/b/c"}));
    EXPECT_EQ(sorted(topics_of(*hash)), sorted({"a/b/c", "a", "a/b", "a/bc"}));
    // '#' never matches a topic starting with '$' (4.7.2).
    EXPECT_EQ(sorted(topics_of(*all)), sorted({"a/b/c", "a", "a/b", "a/bc", "b/c"}));
    EXPECT_EQ(sorted(topics_of(*system)), (std::vector<std::string>{"$SYS/nodes"}));
    // "a/bc" shares the prefix "a/b" on the wire; the filter still refuses it.
    EXPECT_EQ(sorted(topics_of(*exact)), (std::vector<std::string>{"a/b"}));
  }
}

TEST(Delivery, ThePublishArrivesAsItWasMade) {
  Cluster cluster(2);
  ASSERT_TRUE(cluster.joined);
  const auto received = cluster.subscribe(1, "t");
  auto publish = publish_of("t", std::string("\0\xff payload", 10));
  publish.retain = true;
  publish.properties.content_type = "text/plain";
  publish.properties.message_expiry_interval = 60;
  publish.properties.correlation_data = mqtt::Bytes{1, 2};
  publish.properties.user_properties = {{"k", "v"}, {"k", "w"}};
  (void)cluster.deliveries[0]->publish(publish, "p");
  cluster.settle();
  ASSERT_EQ(received->size(), 1u);
  EXPECT_EQ(received->front(), publish);
}

TEST(Delivery, AfterUnsubscribeNothingMoreArrives) {
  Cluster cluster(2);
  ASSERT_TRUE(cluster.joined);
  uint64_t id = 0;
  const auto received = cluster.subscribe(1, "t", &id);
  (void)cluster.deliveries[0]->publish(publish_of("t", "1"), "p");
  cluster.settle();
  cluster.deliveries[1]->unsubscribe(id);
  (void)cluster.deliveries[0]->publish(publish_of("t", "2"), "p");
  cluster.settle();
  ASSERT_EQ(received->size(), 1u);
  EXPECT_EQ(received->front().payload, mqtt::Bytes{'1'});
}

// The publisher's client id travels with each publish, for No Local.
TEST(Delivery, ThePublisherArrivesWithThePublish) {
  Cluster cluster(2);
  ASSERT_TRUE(cluster.joined);
  std::vector<std::string> publishers;
  std::optional<dannan::membership::Pushed> held;
  const auto executor = cluster.world.io.get_executor();
  (void)cluster.deliveries[1]->subscribe(
      "t", executor, [&](const mqtt::Publish&, const std::string& publisher) { publishers.push_back(publisher); }, 5s, executor,
      [&](dannan::membership::Pushed pushed) { held = std::move(pushed); });
  ASSERT_TRUE(cluster.world.run_until([&] { return held.has_value(); }, max_steps));
  (void)cluster.deliveries[0]->publish(publish_of("t", "x"), "sensor-7");
  (void)cluster.deliveries[1]->publish(publish_of("t", "y"), "");
  cluster.settle();
  std::sort(publishers.begin(), publishers.end());
  EXPECT_EQ(publishers, (std::vector<std::string>{"", "sensor-7"}));
}

// The client id, then a 5.0 PUBLISH of the channel message's own topic, is
// all that is delivered: what another build of Lir might put on the channel
// is dropped, never guessed at.
TEST(Delivery, WhatIsNotAPublishOfItsOwnTopicIsDropped) {
  Cluster cluster(2);
  ASSERT_TRUE(cluster.joined);
  const auto received = cluster.subscribe(1, "t/#");
  auto& channels = cluster.nodes.node(0).context().channels();
  const std::string channel(channel_name);
  auto framed = [](const mqtt::Packet& packet) {
    dannan::net::Writer writer;
    mqtt::write_string(writer, "p");
    writer.raw(mqtt::encode(packet, mqtt::Version::v5));
    return writer.take();
  };
  (void)channels.publish(channel, "t", mqtt::Bytes{0xff, 0x00});
  (void)channels.publish(channel, "t", mqtt::Bytes{0x00, 0x05, 'p'});
  (void)channels.publish(channel, "t", mqtt::encode(publish_of("t", "unframed"), mqtt::Version::v5));
  (void)channels.publish(channel, "t", framed(mqtt::Pingreq{}));
  (void)channels.publish(channel, "t/other", framed(publish_of("t", "x")));
  (void)channels.publish(channel, "t", framed(publish_of("t", "kept")));
  cluster.settle();
  ASSERT_EQ(received->size(), 1u);
  EXPECT_EQ(received->front(), publish_of("t", "kept"));
}

TEST(Delivery, RefusesWhatItCannotCarry) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.joined);
  auto& delivery = *cluster.deliveries[0];
  const auto executor = cluster.world.io.get_executor();
  auto subscribe = [&](const std::string& filter) {
    return delivery.subscribe(filter, executor, [](const mqtt::Publish&, const std::string&) {}, 1s, executor, [](dannan::membership::Pushed) {});
  };
  EXPECT_THROW(subscribe("a/#/b"), std::invalid_argument);
  EXPECT_THROW(subscribe(""), std::invalid_argument);
  // A shared subscription is a group's, not a node's (a later piece).
  EXPECT_THROW(subscribe("$share/g/a"), std::invalid_argument);
  EXPECT_THROW((void)delivery.publish(publish_of("a/+", "x"), "p"), std::invalid_argument);
  EXPECT_THROW((void)delivery.publish(publish_of("", "x"), "p"), std::invalid_argument);
  // An alias is one connection's; the connection resolves it first.
  auto aliased = publish_of("a", "x");
  aliased.properties.topic_alias = 1;
  EXPECT_THROW((void)delivery.publish(aliased, "p"), std::invalid_argument);
  // Maximum QoS 0 until Dagda's log (the design, "What may be acknowledged").
  auto qos1 = publish_of("a", "x");
  qos1.qos = mqtt::QoS::at_least_once;
  qos1.packet_id = 1;
  EXPECT_THROW((void)delivery.publish(qos1, "p"), std::invalid_argument);
}

}  // namespace
}  // namespace lir::broker
