//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "lir/client/client.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <future>
#include <mutex>
#include <vector>

#include "dannan/test/fixtures/child_process.h"
#include "dannan/test/fixtures/temp_dir.h"
#include "lir/edge/edge.h"
#include "lir/test/fixtures/mqtt_client.h"

// The client library against Lir's own broker, in process, over real
// sockets: what the stack's projects need of it (Tom, 2026-11-15).
namespace lir::client {
namespace {

using namespace std::chrono_literals;
using mqtt::QoS;
using mqtt::Version;
using test::eventually;
using test::MqttClient;

dannan::status::ScopedLogger quiet() { return {std::make_shared<dannan::status::ConsoleSink>(dannan::status::LogLevel::error), "client"}; }

mqtt::Bytes bytes(const std::string& text) { return mqtt::Bytes(text.begin(), text.end()); }
std::string text(const mqtt::Bytes& b) { return std::string(b.begin(), b.end()); }

// What a client told, kept for the test to read from its own thread. Shared
// with the callbacks, so one that runs after the test's end finds it alive.
struct Told {
  std::mutex mutex;
  std::vector<StateChange> states;
  std::vector<Message> messages;
  std::vector<Outcome> outcomes;
  std::vector<SubscribeOutcome> subscribes;
};

class Seen {
 public:
  Told* operator->() { return _told.get(); }

  void watch(Client& client) {
    client.on_state([told = _told](const StateChange& change) {
      std::lock_guard lock(told->mutex);
      told->states.push_back(change);
    });
  }
  std::function<void(const Message&)> on_message() {
    return [told = _told](const Message& m) {
      std::lock_guard lock(told->mutex);
      told->messages.push_back(m);
    };
  }
  std::function<void(Outcome)> on_outcome() {
    return [told = _told](Outcome o) {
      std::lock_guard lock(told->mutex);
      told->outcomes.push_back(std::move(o));
    };
  }
  std::function<void(SubscribeOutcome)> on_subscribe() {
    return [told = _told](SubscribeOutcome o) {
      std::lock_guard lock(told->mutex);
      told->subscribes.push_back(std::move(o));
    };
  }
  size_t connections() { return count_states(ConnectionState::connected); }
  size_t disconnections() { return count_states(ConnectionState::disconnected); }
  template <typename T>
  size_t count(std::vector<T>& of) {
    std::lock_guard lock(_told->mutex);
    return of.size();
  }

 private:
  size_t count_states(ConnectionState state) {
    std::lock_guard lock(_told->mutex);
    return static_cast<size_t>(std::count_if(_told->states.begin(), _told->states.end(), [state](const auto& s) { return s.state == state; }));
  }

  std::shared_ptr<Told> _told = std::make_shared<Told>();
};

class ClientTest : public ::testing::Test {
 protected:
  ClientTest() : edge(io.context, with_any_port(), dannan::contract::steady_time_source(), allow, quiet()) { edge.start(); }
  ~ClientTest() override {
    for (auto& client : clients) {
      std::promise<void> stopped;
      client->stop([&stopped] { stopped.set_value(); });
      stopped.get_future().wait_for(5s);
    }
  }

  static edge::EdgeConfig with_any_port() {
    edge::EdgeConfig config;
    config.listen_host = "127.0.0.1";
    config.port = 0;
    return config;
  }

  ClientConfig config_for(const std::string& id) {
    ClientConfig config;
    config.host = "127.0.0.1";
    config.port = edge.port();
    config.client_id = id;
    config.backoff_first = 20ms;
    config.backoff_most = 200ms;
    config.connect_timeout = 5s;
    return config;
  }

  // A started client, watched by `seen`.
  Client& start(ClientConfig config, Seen& seen) {
    clients.push_back(std::make_unique<Client>(io.context.get_executor(), std::move(config), dannan::contract::steady_time_source(), quiet()));
    seen.watch(*clients.back());
    clients.back()->start();
    return *clients.back();
  }

  test::Io io;
  contract::AllowAll allow;
  edge::Edge edge;
  std::vector<std::unique_ptr<Client>> clients;
};

TEST_F(ClientTest, ConnectsAndSaysSo) {
  Seen seen;
  start(config_for("a"), seen);
  ASSERT_TRUE(eventually([&] { return seen.connections() == 1; }));
  std::lock_guard lock(seen->mutex);
  EXPECT_FALSE(seen->states.front().session_present);
}

TEST_F(ClientTest, AQos1PublishIsAcknowledgedAndDelivered) {
  MqttClient sub(edge.port());
  sub.connect("sub");
  sub.subscribe("jobs/#", QoS::at_least_once);
  Seen seen;
  auto& client = start(config_for("pub"), seen);
  client.publish({"jobs/1/started", bytes("go"), QoS::at_least_once, false}, seen.on_outcome());
  ASSERT_TRUE(eventually([&] { return seen.count(seen->outcomes) == 1; }));
  {
    std::lock_guard lock(seen->mutex);
    EXPECT_TRUE(seen->outcomes.front().ok) << seen->outcomes.front().why;
  }
  const auto got = sub.next_message();
  ASSERT_TRUE(got);
  EXPECT_EQ(got->topic, "jobs/1/started");
  EXPECT_EQ(text(got->payload), "go");
}

// Published before the connection is up: queued, then sent.
TEST_F(ClientTest, APublishBeforeConnectingIsSentOnceConnected) {
  MqttClient sub(edge.port());
  sub.connect("sub");
  sub.subscribe("early", QoS::at_least_once);
  Seen seen;
  clients.push_back(std::make_unique<Client>(io.context.get_executor(), config_for("early"), dannan::contract::steady_time_source(), quiet()));
  clients.back()->publish({"early", bytes("1"), QoS::at_least_once, false}, seen.on_outcome());
  clients.back()->start();
  ASSERT_TRUE(eventually([&] { return seen.count(seen->outcomes) == 1; }));
  const auto got = sub.next_message();
  ASSERT_TRUE(got);
  EXPECT_EQ(text(got->payload), "1");
}

TEST_F(ClientTest, AWildcardSubscriptionReceivesInOrder) {
  Seen seen;
  auto& client = start(config_for("watcher"), seen);
  client.subscribe("room/+/temp", QoS::at_least_once, seen.on_message(), seen.on_subscribe());
  ASSERT_TRUE(eventually([&] { return seen.count(seen->subscribes) == 1; }));
  {
    std::lock_guard lock(seen->mutex);
    ASSERT_TRUE(seen->subscribes.front().ok);
    EXPECT_EQ(seen->subscribes.front().granted, QoS::at_least_once);
  }
  MqttClient pub(edge.port());
  pub.connect("pub");
  for (int i = 0; i < 5; ++i) pub.publish("room/" + std::to_string(i) + "/temp", std::to_string(i), QoS::at_least_once);
  pub.publish("room/9/humidity", "x", QoS::at_least_once);
  ASSERT_TRUE(eventually([&] { return seen.count(seen->messages) == 5; }));
  std::lock_guard lock(seen->mutex);
  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(seen->messages[static_cast<size_t>(i)].topic, "room/" + std::to_string(i) + "/temp");
    EXPECT_EQ(seen->messages[static_cast<size_t>(i)].qos, QoS::at_least_once);
  }
}

// 3.1.1 has no subscription identifiers: the client matches filters itself.
TEST_F(ClientTest, A311ClientMatchesItsOwnFilters) {
  Seen seen;
  auto config = config_for("old");
  config.version = Version::v3_1_1;
  auto& client = start(std::move(config), seen);
  Seen other;
  client.subscribe("a/#", QoS::at_most_once, seen.on_message(), seen.on_subscribe());
  client.subscribe("b/+", QoS::at_most_once, other.on_message(), other.on_subscribe());
  ASSERT_TRUE(eventually([&] { return seen.count(seen->subscribes) == 1 && other.count(other->subscribes) == 1; }));
  MqttClient pub(edge.port());
  pub.connect("pub");
  pub.publish("a/1", "x", QoS::at_least_once);
  pub.publish("b/1", "y", QoS::at_least_once);
  ASSERT_TRUE(eventually([&] { return seen.count(seen->messages) == 1 && other.count(other->messages) == 1; }));
  std::lock_guard lock(seen->mutex);
  EXPECT_EQ(seen->messages.front().topic, "a/1");
}

TEST_F(ClientTest, ARetainedPublishReachesALaterSubscriber) {
  Seen seen;
  auto& client = start(config_for("status"), seen);
  client.publish({"status/node/1", bytes("up"), QoS::at_least_once, true}, seen.on_outcome());
  ASSERT_TRUE(eventually([&] { return seen.count(seen->outcomes) == 1; }));
  MqttClient later(edge.port());
  later.connect("later");
  later.subscribe("status/#", QoS::at_least_once);
  const auto got = later.next_message();
  ASSERT_TRUE(got);
  EXPECT_TRUE(got->retain);
  EXPECT_EQ(text(got->payload), "up");
}

// The broker closes the connection (here by a takeover), so the will is
// published; the client says why it lost the connection.
TEST_F(ClientTest, TheWillIsPublishedWhenTheConnectionIsLost) {
  MqttClient watcher(edge.port());
  watcher.connect("watcher");
  watcher.subscribe("agents/+/gone", QoS::at_least_once);
  Seen seen;
  auto config = config_for("agent");
  config.will = Message{"agents/agent/gone", bytes("lost"), QoS::at_least_once, false};
  config.backoff_first = 10s;
  start(std::move(config), seen);
  ASSERT_TRUE(eventually([&] { return seen.connections() == 1; }));
  MqttClient taker(edge.port());
  taker.connect("agent");
  const auto got = watcher.next_message();
  ASSERT_TRUE(got);
  EXPECT_EQ(got->topic, "agents/agent/gone");
  ASSERT_TRUE(eventually([&] { return seen.disconnections() == 1; }));
  std::lock_guard lock(seen->mutex);
  EXPECT_EQ(seen->states.back().reason, mqtt::reason::session_taken_over);
}

TEST_F(ClientTest, StoppingPublishesNoWillAndRefusesWhatFollows) {
  MqttClient watcher(edge.port());
  watcher.connect("watcher");
  watcher.subscribe("agents/+/gone", QoS::at_least_once);
  Seen seen;
  auto config = config_for("agent");
  config.will = Message{"agents/agent/gone", bytes("lost"), QoS::at_least_once, false};
  auto& client = start(std::move(config), seen);
  ASSERT_TRUE(eventually([&] { return seen.connections() == 1; }));
  std::promise<void> stopped;
  client.stop([&stopped] { stopped.set_value(); });
  ASSERT_EQ(stopped.get_future().wait_for(5s), std::future_status::ready);
  client.publish({"after", bytes("x"), QoS::at_most_once, false}, seen.on_outcome());
  ASSERT_TRUE(eventually([&] { return seen.count(seen->outcomes) == 1; }));
  {
    std::lock_guard lock(seen->mutex);
    EXPECT_FALSE(seen->outcomes.front().ok);
    EXPECT_EQ(seen->outcomes.front().why, "stopped");
  }
  EXPECT_FALSE(watcher.next_message());
}

// Lost and back: the client reconnects by itself and subscribes again, since
// the broker kept no session for it.
TEST_F(ClientTest, ItReconnectsAndSubscribesAgain) {
  Seen seen;
  auto& client = start(config_for("back"), seen);
  client.subscribe("news", QoS::at_least_once, seen.on_message(), seen.on_subscribe());
  ASSERT_TRUE(eventually([&] { return seen.count(seen->subscribes) == 1; }));
  {
    MqttClient taker(edge.port());
    taker.connect("back");
    ASSERT_TRUE(eventually([&] { return seen.disconnections() >= 1; }));
  }
  ASSERT_TRUE(eventually([&] { return seen.connections() >= 2; }));
  MqttClient pub(edge.port());
  pub.connect("pub");
  ASSERT_TRUE(eventually(
      [&] {
        pub.publish("news", "again", QoS::at_least_once);
        return seen.count(seen->messages) >= 1;
      },
      5s));
  std::lock_guard lock(seen->mutex);
  EXPECT_EQ(text(seen->messages.front().payload), "again");
}

TEST_F(ClientTest, ABrokerThatIsNotThereIsTriedAgainWithAReason) {
  uint16_t closed_port = 0;
  {
    boost::asio::io_context scratch;
    boost::asio::ip::tcp::acceptor probe(scratch, {boost::asio::ip::make_address("127.0.0.1"), 0});
    closed_port = probe.local_endpoint().port();
  }
  Seen seen;
  auto config = config_for("nowhere");
  config.port = closed_port;
  start(std::move(config), seen);
  ASSERT_TRUE(eventually([&] { return seen.disconnections() >= 2; }));
  std::lock_guard lock(seen->mutex);
  EXPECT_NE(seen->states.front().why.find("cannot connect"), std::string::npos) << seen->states.front().why;
}

TEST_F(ClientTest, AnInvalidTopicFailsAtOnce) {
  Seen seen;
  auto& client = start(config_for("bad"), seen);
  client.publish({"a/+", bytes("x"), QoS::at_most_once, false}, seen.on_outcome());
  ASSERT_TRUE(eventually([&] { return seen.count(seen->outcomes) == 1; }));
  std::lock_guard lock(seen->mutex);
  EXPECT_FALSE(seen->outcomes.front().ok);
}

TEST_F(ClientTest, ThePasswordIsAskedForAtEachConnect) {
  auto asked = std::make_shared<std::atomic<int>>(0);
  Seen seen;
  auto config = config_for("token");
  config.password = [asked]() -> std::optional<std::string> { return "token-" + std::to_string(++*asked); };
  start(std::move(config), seen);
  ASSERT_TRUE(eventually([&] { return seen.connections() == 1; }));
  {
    MqttClient taker(edge.port());
    taker.connect("token");
    ASSERT_TRUE(eventually([&] { return seen.connections() >= 2; }));
  }
  EXPECT_GE(asked->load(), 2);
}

// MQTT over TLS: the server's certificate checked against the trust given
// and the address connected to (an IP address, so no SNI).
class ClientTlsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const std::string openssl = LIR_OPENSSL;
    if (openssl.find("NOTFOUND") != std::string::npos) GTEST_SKIP() << "no openssl command";
    for (const std::string name : {"server", "stranger"}) {
      dannan::test::ChildProcess make({openssl, "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes", "-keyout",
                                       (dir.path() / (name + "-key.pem")).string(), "-out", (dir.path() / (name + ".pem")).string(), "-days", "1", "-subj",
                                       "/CN=" + name, "-addext", "subjectAltName=IP:127.0.0.1"},
                                      dir.path() / "openssl.log");
      ASSERT_EQ(make.wait(std::chrono::steady_clock::now() + 20s), 0);
    }
    edge::EdgeConfig config;
    config.listen_host = "127.0.0.1";
    config.port = 0;
    config.tls = edge::TlsConfig{"127.0.0.1", 0, dir.path() / "server.pem", dir.path() / "server-key.pem"};
    edge.emplace(io.context, config, dannan::contract::steady_time_source(), allow, quiet());
    edge->start();
  }
  void TearDown() override {
    if (client) {
      std::promise<void> stopped;
      client->stop([&stopped] { stopped.set_value(); });
      stopped.get_future().wait_for(5s);
    }
  }

  void connect_trusting(const std::string& name) {
    ClientConfig config;
    config.host = "127.0.0.1";
    config.port = edge->tls_port();
    config.client_id = "secure";
    config.tls = Tls{dir.path() / (name + ".pem"), {}, {}, {}};
    config.backoff_first = 20ms;
    config.backoff_most = 200ms;
    client = std::make_unique<Client>(io.context.get_executor(), std::move(config), dannan::contract::steady_time_source(), quiet());
    seen.watch(*client);
    client->start();
  }

  dannan::test::TempDir dir;
  test::Io io;
  contract::AllowAll allow;
  std::optional<edge::Edge> edge;
  Seen seen;
  std::unique_ptr<Client> client;
};

TEST_F(ClientTlsTest, ATrustedServerConnects) {
  connect_trusting("server");
  ASSERT_TRUE(eventually([&] { return seen.connections() == 1; }));
}

TEST_F(ClientTlsTest, AnUntrustedServerIsRefusedAtTheHandshake) {
  connect_trusting("stranger");
  ASSERT_TRUE(eventually([&] { return seen.disconnections() >= 1; }));
  EXPECT_EQ(seen.connections(), 0u);
  std::lock_guard lock(seen->mutex);
  EXPECT_NE(seen->states.front().why.find("TLS handshake"), std::string::npos) << seen->states.front().why;
}

}  // namespace
}  // namespace lir::client
