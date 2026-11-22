//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "lir/client/client.h"

#include <gtest/gtest.h>

#include <array>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/thread_pool.hpp>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "dannan/listen/certificates.h"
#include "dannan/listen/listener.h"
#include "dannan/listen/stream.h"
#include "dannan/test/fixtures/recording_sink.h"
#include "dannan/test/fixtures/stepped_time.h"
#include "dannan/test/fixtures/temp_dir.h"
#include "lir/client/roster.h"
#include "lir/contract/endpoints.h"

// lir::client against a broker each test scripts: Dannan's listener on the
// loopback, each connection's packets decoded as they arrive, every answer
// the test's. Time is a stepped source the test moves, so backoff, keep-alive
// and the CONNACK deadline are never waited for on a real clock.
namespace lir::client {
namespace {

using namespace std::chrono_literals;
namespace reason = mqtt::reason;

class Broker {
 public:
  struct Session {
    std::shared_ptr<dannan::listen::Stream> stream;
    mqtt::Bytes input;
    std::vector<mqtt::Packet> received;
    bool closed = false;
    std::array<uint8_t, 4096> buffer{};
  };

  Broker(boost::asio::io_context& io, mqtt::Version version, std::shared_ptr<dannan::listen::Certificates> certificates = {})
      : _version(version), _listener(io, config(std::move(certificates))) {
    _listener.start(io.get_executor(), [this](std::shared_ptr<dannan::listen::Stream> stream) {
      auto session = std::make_shared<Session>();
      session->stream = std::move(stream);
      sessions.push_back(session);
      read(session);
    });
  }

  uint16_t port() const { return _listener.port(); }
  // From the io context's thread, so it is not waited for.
  void stop() { _listener.stop(); }

  void send(size_t session, const mqtt::Packet& packet) {
    const auto bytes = mqtt::encode(packet, _version);
    sessions.at(session)->stream->send(std::string(bytes.begin(), bytes.end()));
  }
  void raw(size_t session, std::string bytes) { sessions.at(session)->stream->send(std::move(bytes)); }
  void close(size_t session) { sessions.at(session)->stream->close(); }

  std::vector<std::shared_ptr<Session>> sessions;

 private:
  static dannan::listen::ListenerConfig config(std::shared_ptr<dannan::listen::Certificates> certificates) {
    dannan::listen::ListenerConfig out;
    out.name = "broker";
    out.host = "127.0.0.1";
    out.certificates = std::move(certificates);
    return out;
  }

  void read(const std::shared_ptr<Session>& session) {
    session->stream->async_read_some(boost::asio::buffer(session->buffer), [this, session](const boost::system::error_code& error, size_t size) {
      if (error) {
        // As a broker does: its side closes too, which a TLS close waits for.
        session->closed = true;
        session->stream->close();
        return;
      }
      session->input.insert(session->input.end(), session->buffer.begin(), session->buffer.begin() + static_cast<std::ptrdiff_t>(size));
      for (;;) {
        const auto frame = mqtt::read_frame(session->input, 1u << 20);
        if (!frame || session->input.size() < frame->size()) break;
        session->received.push_back(mqtt::decode(std::span<const uint8_t>(session->input).first(frame->size()), _version));
        session->input.erase(session->input.begin(), session->input.begin() + static_cast<std::ptrdiff_t>(frame->size()));
      }
      read(session);
    });
  }

  mqtt::Version _version;
  dannan::listen::StreamListener _listener;
};

class ClientTest : public ::testing::Test {
 protected:
  void make(mqtt::Version version = mqtt::Version::v5, std::shared_ptr<dannan::listen::Certificates> certificates = {}) {
    _version = version;
    broker = std::make_unique<Broker>(io, version, std::move(certificates));
    config.host = "127.0.0.1";
    config.port = broker->port();
    config.version = version;
    client = std::make_unique<Client>(io.get_executor(), config, time, dannan::status::ScopedLogger(sink, "lir.client"));
    client->on_state([this](const StateChange& change) { states.push_back(change); });
  }

  void SetUp() override { config.client_id = "c"; }

  // Stops the client and lets its last callbacks run. Each test calls it last,
  // while what its callbacks capture is still alive.
  void stop_client() {
    if (!client) return;
    bool stopped = false;
    client->stop([&] { stopped = true; });
    EXPECT_TRUE(run_until([&] { return stopped; }));
    client.reset();
  }

  void TearDown() override {
    stop_client();
    if (broker) {
      boost::asio::post(io, [this] { broker->stop(); });
      run_until([] { return false; }, 50ms);
    }
  }

  // Runs the io context until `done` holds or `limit` of real time passed.
  template <class Done>
  bool run_until(Done&& done, std::chrono::milliseconds limit = 5s) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!done() && std::chrono::steady_clock::now() < end) {
      io.restart();
      io.run_one_for(5ms);
    }
    return done();
  }
  // Moves the stepped clock, then lets the timers that poll it look.
  void advance(std::chrono::milliseconds by) {
    time.advance(by);
    run_until([] { return false; }, 30ms);
  }

  // The `index`th packet of type T the broker's session `session` received.
  template <class T>
  std::optional<T> received(size_t session, size_t index = 0) {
    if (!run_until([&] { return count<T>(session) > index; })) return std::nullopt;
    size_t seen = 0;
    for (const auto& packet : broker->sessions[session]->received) {
      if (const auto* out = std::get_if<T>(&packet)) {
        if (seen++ == index) return *out;
      }
    }
    return std::nullopt;
  }
  template <class T>
  size_t count(size_t session) {
    if (broker->sessions.size() <= session) return 0;
    size_t out = 0;
    for (const auto& packet : broker->sessions[session]->received) out += std::holds_alternative<T>(packet) ? 1 : 0;
    return out;
  }
  bool session_opens(size_t session) {
    return run_until([&] { return broker->sessions.size() > session; });
  }

  // Started, its CONNECT answered on session `session`.
  void connect(size_t session = 0, mqtt::Connack connack = {}) {
    if (session == 0) client->start();
    ASSERT_TRUE(received<mqtt::Connect>(session).has_value());
    broker->send(session, connack);
    ASSERT_TRUE(run_until([&] { return !states.empty() && states.back().state == ConnectionState::connected; }));
  }

  static Message message(std::string topic, std::string payload, mqtt::QoS qos = mqtt::QoS::at_most_once) {
    return Message{std::move(topic), mqtt::Bytes(payload.begin(), payload.end()), qos, false};
  }

  boost::asio::io_context io;
  dannan::test::SteppedTime time;
  std::shared_ptr<dannan::test::RecordingSink> sink = std::make_shared<dannan::test::RecordingSink>();
  ClientConfig config;
  std::unique_ptr<Broker> broker;
  std::unique_ptr<Client> client;
  std::vector<StateChange> states;
  mqtt::Version _version = mqtt::Version::v5;
};

// The roster

dannan::membership::NodeRecord lir_node(std::string endpoints) {
  dannan::membership::NodeRecord record;
  record.adverts[std::string(contract::lir_advert)] = {};
  record.adverts[std::string(contract::endpoints_advert_key)] = mqtt::Bytes(endpoints.begin(), endpoints.end());
  return record;
}

TEST(ClientRoster, ReadsLirNodesAtTheirEndpoints) {
  auto other = lir_node("tcp=192.0.2.9:1883");
  other.adverts.erase(std::string(contract::lir_advert));
  const std::vector<dannan::membership::NodeRecord> records{lir_node("tcp=192.0.2.1:1883;tls=192.0.2.1:8883"), other, lir_node("tls=[2001:db8::1]:8883"),
                                                            lir_node("tcp=192.0.2.3:1883"), lir_node("tcp=nonsense")};
  EXPECT_EQ(lir_addresses(records, "tcp"), (std::vector<Address>{{"192.0.2.1", 1883}, {"192.0.2.3", 1883}}));
  EXPECT_EQ(lir_addresses(records, "tls"), (std::vector<Address>{{"192.0.2.1", 8883}, {"2001:db8::1", 8883}}));
}

TEST(ClientRoster, ParsesAddresses) {
  EXPECT_EQ(parse_address("host:1883"), (Address{"host", 1883}));
  EXPECT_EQ(parse_address("[::1]:1883"), (Address{"::1", 1883}));
  EXPECT_EQ(parse_address("::1:1883"), (Address{"::1", 1883}));
  EXPECT_FALSE(parse_address("host"));
  EXPECT_FALSE(parse_address(":1883"));
  EXPECT_FALSE(parse_address("host:0"));
  EXPECT_FALSE(parse_address("host:70000"));
  EXPECT_FALSE(parse_address("host:18x3"));
}

// Connecting

TEST_F(ClientTest, ConnectsWithItsConfigurationAndAsksThePasswordEachTime) {
  int asked = 0;
  config.username = "louhi";
  config.password = [&]() -> std::optional<std::string> { return "token-" + std::to_string(++asked); };
  config.keep_alive = 20s;
  config.session_expiry = 600;
  config.clean_start = false;
  make();
  connect();
  const auto first = *received<mqtt::Connect>(0);
  EXPECT_EQ(first.client_id, "c");
  EXPECT_EQ(first.username, "louhi");
  EXPECT_EQ(first.password, (mqtt::Bytes{'t', 'o', 'k', 'e', 'n', '-', '1'}));
  EXPECT_EQ(first.keep_alive, 20);
  EXPECT_FALSE(first.clean_start);
  EXPECT_EQ(first.properties.session_expiry_interval, 600u);
  EXPECT_FALSE(states.back().session_present);
  broker->close(0);
  ASSERT_TRUE(session_opens(1));
  const auto second = *received<mqtt::Connect>(1);
  EXPECT_EQ(second.password, (mqtt::Bytes{'t', 'o', 'k', 'e', 'n', '-', '2'}));
  // A kept session is resumed.
  EXPECT_FALSE(second.clean_start);
  stop_client();
}

TEST_F(ClientTest, Speaks311) {
  config.username = "u";
  make(mqtt::Version::v3_1_1);
  connect();
  EXPECT_EQ(received<mqtt::Connect>(0)->version, mqtt::Version::v3_1_1);
  stop_client();
}

TEST_F(ClientTest, TheWillGoesInTheConnect) {
  config.will = message("status/c", "gone");
  make();
  client->start();
  const auto connect = *received<mqtt::Connect>(0);
  ASSERT_TRUE(connect.will.has_value());
  EXPECT_EQ(connect.will->topic, "status/c");
  EXPECT_EQ(connect.will->payload, (mqtt::Bytes{'g', 'o', 'n', 'e'}));
  stop_client();
}

TEST_F(ClientTest, AnAssignedClientIdIsKeptForTheNextConnect) {
  config.client_id = "";
  config.session_expiry = 60;
  make();
  mqtt::Connack connack;
  connack.properties.assigned_client_identifier = "assigned-7";
  connect(0, connack);
  broker->close(0);
  ASSERT_TRUE(session_opens(1));
  EXPECT_EQ(received<mqtt::Connect>(1)->client_id, "assigned-7");
  stop_client();
}

TEST_F(ClientTest, ARefusedConnectBacksOffThenTriesAgain) {
  config.backoff_first = 1000ms;
  config.backoff_most = 8000ms;
  make();
  client->start();
  ASSERT_TRUE(received<mqtt::Connect>(0).has_value());
  broker->send(0, mqtt::Connack{false, reason::not_authorized, {}});
  ASSERT_TRUE(run_until([&] { return !states.empty(); }));
  EXPECT_EQ(states.back().state, ConnectionState::disconnected);
  EXPECT_EQ(states.back().reason, reason::not_authorized);
  // A random part between half and all of the first backoff.
  advance(400ms);
  EXPECT_EQ(broker->sessions.size(), 1u);
  advance(700ms);
  EXPECT_TRUE(session_opens(1));
  stop_client();
}

TEST_F(ClientTest, NoConnackInTimeFailsTheAttempt) {
  config.connect_timeout = 10s;
  make();
  client->start();
  ASSERT_TRUE(received<mqtt::Connect>(0).has_value());
  advance(10s);
  ASSERT_TRUE(run_until([&] { return !states.empty(); }));
  EXPECT_EQ(states.back().state, ConnectionState::disconnected);
  EXPECT_EQ(states.back().why, "no CONNACK in time");
  stop_client();
}

TEST_F(ClientTest, ALostConnectionIsReportedWithTheBrokersReasonAndRetriedAtOnce) {
  make();
  connect();
  broker->send(0, mqtt::Disconnect{reason::server_shutting_down, {}});
  ASSERT_TRUE(run_until([&] { return states.back().state == ConnectionState::disconnected; }));
  EXPECT_EQ(states.back().reason, reason::server_shutting_down);
  EXPECT_TRUE(session_opens(1));
  stop_client();
}

TEST_F(ClientTest, KeepAlivePingsAndDropsADeadBroker) {
  config.keep_alive = 10s;
  make();
  connect();
  advance(10s);
  ASSERT_TRUE(received<mqtt::Pingreq>(0).has_value());
  broker->send(0, mqtt::Pingresp{});
  run_until([] { return false; }, 30ms);
  advance(10s);
  ASSERT_TRUE(received<mqtt::Pingreq>(0, 1).has_value());
  // No answer this time.
  advance(10s);
  ASSERT_TRUE(run_until([&] { return states.back().state == ConnectionState::disconnected; }));
  EXPECT_EQ(states.back().reason, reason::keep_alive_timeout);
  EXPECT_TRUE(session_opens(1));
  stop_client();
}

// Publishing

TEST_F(ClientTest, PublishesAtQoS0) {
  make();
  connect();
  std::optional<Outcome> outcome;
  client->publish(message("a/b", "x"), [&](Outcome out) { outcome = out; });
  const auto publish = *received<mqtt::Publish>(0);
  EXPECT_EQ(publish.topic, "a/b");
  EXPECT_EQ(publish.qos, mqtt::QoS::at_most_once);
  ASSERT_TRUE(run_until([&] { return outcome.has_value(); }));
  EXPECT_TRUE(outcome->ok);
  stop_client();
}

TEST_F(ClientTest, AQoS1PublishIsDoneAtItsPuback) {
  make();
  connect();
  std::vector<Outcome> outcomes;
  client->publish(message("t", "1", mqtt::QoS::at_least_once), [&](Outcome out) { outcomes.push_back(out); });
  client->publish(message("t", "2", mqtt::QoS::at_least_once), [&](Outcome out) { outcomes.push_back(out); });
  const auto first = *received<mqtt::Publish>(0);
  const auto second = *received<mqtt::Publish>(0, 1);
  EXPECT_NE(first.packet_id, second.packet_id);
  run_until([] { return false; }, 30ms);
  EXPECT_TRUE(outcomes.empty());
  broker->send(0, mqtt::Ack{mqtt::PacketType::puback, first.packet_id, reason::success, {}});
  broker->send(0, mqtt::Ack{mqtt::PacketType::puback, second.packet_id, reason::not_authorized, {}});
  ASSERT_TRUE(run_until([&] { return outcomes.size() == 2; }));
  EXPECT_TRUE(outcomes[0].ok);
  EXPECT_FALSE(outcomes[1].ok);
  EXPECT_EQ(outcomes[1].reason, reason::not_authorized);
  stop_client();
}

TEST_F(ClientTest, PublishesWhileDisconnectedAreQueuedInOrder) {
  make();
  client->publish(message("t", "1"));
  client->publish(message("t", "2", mqtt::QoS::at_least_once));
  client->publish(message("t", "3"));
  connect();
  ASSERT_TRUE(received<mqtt::Publish>(0, 2).has_value());
  EXPECT_EQ(received<mqtt::Publish>(0, 0)->payload, mqtt::Bytes{'1'});
  EXPECT_EQ(received<mqtt::Publish>(0, 1)->payload, mqtt::Bytes{'2'});
  EXPECT_EQ(received<mqtt::Publish>(0, 2)->payload, mqtt::Bytes{'3'});
  stop_client();
}

TEST_F(ClientTest, AFullQueueRefusesAtOnce) {
  config.maximum_queued_bytes = 4;
  make();
  std::vector<Outcome> outcomes;
  client->publish(message("t", "1234"), [&](Outcome out) { outcomes.push_back(out); });
  client->publish(message("t", "5"), [&](Outcome out) { outcomes.push_back(out); });
  ASSERT_TRUE(run_until([&] { return outcomes.size() == 1; }));
  EXPECT_FALSE(outcomes[0].ok);
  EXPECT_EQ(outcomes[0].reason, reason::quota_exceeded);
  stop_client();
}

TEST_F(ClientTest, TheInflightBoundHoldsBackQoS1) {
  config.maximum_inflight = 1;
  make();
  connect();
  client->publish(message("t", "1", mqtt::QoS::at_least_once));
  client->publish(message("t", "2", mqtt::QoS::at_least_once));
  const auto first = *received<mqtt::Publish>(0);
  run_until([] { return false; }, 30ms);
  EXPECT_EQ(count<mqtt::Publish>(0), 1u);
  broker->send(0, mqtt::Ack{mqtt::PacketType::puback, first.packet_id, reason::success, {}});
  EXPECT_EQ(received<mqtt::Publish>(0, 1)->payload, mqtt::Bytes{'2'});
  stop_client();
}

TEST_F(ClientTest, AnUnacknowledgedPublishGoesAgainWithDupWhenTheSessionWasKept) {
  config.session_expiry = 60;
  make();
  connect();
  std::optional<Outcome> outcome;
  client->publish(message("t", "x", mqtt::QoS::at_least_once), [&](Outcome out) { outcome = out; });
  const auto sent = *received<mqtt::Publish>(0);
  EXPECT_FALSE(sent.dup);
  broker->close(0);
  ASSERT_TRUE(session_opens(1));
  ASSERT_TRUE(received<mqtt::Connect>(1).has_value());
  broker->send(1, mqtt::Connack{true, reason::success, {}});
  const auto again = *received<mqtt::Publish>(1);
  EXPECT_TRUE(again.dup);
  EXPECT_EQ(again.packet_id, sent.packet_id);
  broker->send(1, mqtt::Ack{mqtt::PacketType::puback, again.packet_id, reason::success, {}});
  ASSERT_TRUE(run_until([&] { return outcome.has_value(); }));
  EXPECT_TRUE(outcome->ok);
  stop_client();
}

TEST_F(ClientTest, WithoutTheSessionItGoesAgainAsNew) {
  make();
  connect();
  client->publish(message("t", "x", mqtt::QoS::at_least_once));
  ASSERT_TRUE(received<mqtt::Publish>(0).has_value());
  broker->close(0);
  ASSERT_TRUE(session_opens(1));
  ASSERT_TRUE(received<mqtt::Connect>(1).has_value());
  broker->send(1, mqtt::Connack{});
  EXPECT_FALSE(received<mqtt::Publish>(1)->dup);
  stop_client();
}

TEST_F(ClientTest, WhatTheBrokerDoesNotTakeFailsWithItsReason) {
  make();
  mqtt::Connack connack;
  connack.properties.maximum_qos = 0;
  connack.properties.retain_available = 0;
  connect(0, connack);
  std::vector<Outcome> outcomes;
  client->publish(message("t", "x", mqtt::QoS::at_least_once), [&](Outcome out) { outcomes.push_back(out); });
  auto retained = message("t", "x");
  retained.retain = true;
  client->publish(retained, [&](Outcome out) { outcomes.push_back(out); });
  client->publish(message("a/+", "x"), [&](Outcome out) { outcomes.push_back(out); });
  client->publish(message("t", "x", mqtt::QoS::exactly_once), [&](Outcome out) { outcomes.push_back(out); });
  ASSERT_TRUE(run_until([&] { return outcomes.size() == 4; }));
  EXPECT_EQ(outcomes[0].reason, reason::qos_not_supported);
  EXPECT_EQ(outcomes[1].reason, reason::retain_not_supported);
  EXPECT_EQ(outcomes[2].reason, reason::topic_name_invalid);
  EXPECT_EQ(outcomes[3].reason, reason::qos_not_supported);
  for (const auto& outcome : outcomes) EXPECT_FALSE(outcome.ok);
  run_until([] { return false; }, 30ms);
  EXPECT_EQ(count<mqtt::Publish>(0), 0u);
  stop_client();
}

TEST_F(ClientTest, APacketOverTheBrokersMaximumIsRefused) {
  make();
  mqtt::Connack connack;
  connack.properties.maximum_packet_size = 32;
  connect(0, connack);
  std::optional<Outcome> outcome;
  client->publish(message("t", std::string(100, 'x')), [&](Outcome out) { outcome = out; });
  ASSERT_TRUE(run_until([&] { return outcome.has_value(); }));
  EXPECT_EQ(outcome->reason, reason::packet_too_large);
  stop_client();
}

// Subscribing

TEST_F(ClientTest, SubscribesAndReceivesWhatItsFilterMatches) {
  make();
  connect();
  std::vector<Message> got;
  std::optional<SubscribeOutcome> outcome;
  client->subscribe("sensors/+/t", mqtt::QoS::at_least_once, [&](const Message& m) { got.push_back(m); }, [&](SubscribeOutcome out) { outcome = out; });
  const auto subscribe = *received<mqtt::Subscribe>(0);
  ASSERT_EQ(subscribe.subscriptions.size(), 1u);
  EXPECT_EQ(subscribe.subscriptions[0].filter, "sensors/+/t");
  EXPECT_EQ(subscribe.subscriptions[0].qos, mqtt::QoS::at_least_once);
  broker->send(0, mqtt::Suback{subscribe.packet_id, {}, {reason::granted_qos_0}});
  ASSERT_TRUE(run_until([&] { return outcome.has_value(); }));
  EXPECT_TRUE(outcome->ok);
  EXPECT_EQ(outcome->granted, mqtt::QoS::at_most_once);
  mqtt::Publish publish;
  publish.topic = "sensors/kitchen/t";
  publish.payload = {'2', '1'};
  broker->send(0, publish);
  publish.topic = "sensors/kitchen/h";
  broker->send(0, publish);
  publish.topic = "sensors/hall/t";
  publish.qos = mqtt::QoS::at_least_once;
  publish.packet_id = 9;
  broker->send(0, publish);
  ASSERT_TRUE(run_until([&] { return got.size() == 2; }));
  EXPECT_EQ(got[0].topic, "sensors/kitchen/t");
  EXPECT_EQ(got[1].topic, "sensors/hall/t");
  // [MQTT-4.3.2-4]
  const auto puback = *received<mqtt::Ack>(0);
  EXPECT_EQ(puback.packet_id, 9);
  stop_client();
}

TEST_F(ClientTest, ARefusedSubscriptionIsReportedAndDropped) {
  make();
  connect();
  std::optional<SubscribeOutcome> outcome;
  client->subscribe("secret/#", mqtt::QoS::at_most_once, [](const Message&) {}, [&](SubscribeOutcome out) { outcome = out; });
  const auto subscribe = *received<mqtt::Subscribe>(0);
  broker->send(0, mqtt::Suback{subscribe.packet_id, {}, {reason::not_authorized}});
  ASSERT_TRUE(run_until([&] { return outcome.has_value(); }));
  EXPECT_FALSE(outcome->ok);
  EXPECT_EQ(outcome->reason, reason::not_authorized);
  broker->close(0);
  ASSERT_TRUE(session_opens(1));
  ASSERT_TRUE(received<mqtt::Connect>(1).has_value());
  broker->send(1, mqtt::Connack{});
  run_until([] { return false; }, 50ms);
  EXPECT_EQ(count<mqtt::Subscribe>(1), 0u);
  stop_client();
}

TEST_F(ClientTest, AnInvalidFilterIsRefusedAtOnce) {
  make();
  std::vector<SubscribeOutcome> outcomes;
  client->subscribe("a/#/b", mqtt::QoS::at_most_once, [](const Message&) {}, [&](SubscribeOutcome out) { outcomes.push_back(out); });
  client->subscribe("a", mqtt::QoS::exactly_once, [](const Message&) {}, [&](SubscribeOutcome out) { outcomes.push_back(out); });
  client->subscribe("$share//a", mqtt::QoS::at_most_once, [](const Message&) {}, [&](SubscribeOutcome out) { outcomes.push_back(out); });
  ASSERT_TRUE(run_until([&] { return outcomes.size() == 3; }));
  EXPECT_EQ(outcomes[0].reason, reason::topic_filter_invalid);
  EXPECT_EQ(outcomes[1].reason, reason::qos_not_supported);
  EXPECT_EQ(outcomes[2].reason, reason::topic_filter_invalid);
  stop_client();
}

TEST_F(ClientTest, SubscriptionsAreMadeAgainWhenTheBrokerHasNoSession) {
  make();
  client->subscribe("a/#", mqtt::QoS::at_most_once, [](const Message&) {});
  connect();
  const auto first = *received<mqtt::Subscribe>(0);
  broker->send(0, mqtt::Suback{first.packet_id, {}, {0}});
  run_until([] { return false; }, 30ms);
  broker->close(0);
  ASSERT_TRUE(session_opens(1));
  ASSERT_TRUE(received<mqtt::Connect>(1).has_value());
  broker->send(1, mqtt::Connack{});
  EXPECT_EQ(received<mqtt::Subscribe>(1)->subscriptions[0].filter, "a/#");
  // With the session kept, an answered subscription is not made again.
  broker->close(1);
  ASSERT_TRUE(session_opens(2));
  ASSERT_TRUE(received<mqtt::Connect>(2).has_value());
  broker->send(2, mqtt::Connack{true, reason::success, {}});
  run_until([] { return false; }, 50ms);
  EXPECT_EQ(count<mqtt::Subscribe>(2), 0u);
  stop_client();
}

TEST_F(ClientTest, AfterUnsubscribeNothingMoreArrives) {
  make();
  connect();
  int got = 0;
  const auto id = client->subscribe("t", mqtt::QoS::at_most_once, [&](const Message&) { ++got; });
  const auto subscribe = *received<mqtt::Subscribe>(0);
  broker->send(0, mqtt::Suback{subscribe.packet_id, {}, {0}});
  client->unsubscribe(id);
  const auto unsubscribe = *received<mqtt::Unsubscribe>(0);
  EXPECT_EQ(unsubscribe.filters, std::vector<std::string>{"t"});
  broker->send(0, mqtt::Unsuback{unsubscribe.packet_id, {}, {0}});
  mqtt::Publish publish;
  publish.topic = "t";
  broker->send(0, publish);
  run_until([] { return false; }, 50ms);
  EXPECT_EQ(got, 0);
  stop_client();
}

TEST_F(ClientTest, ASharedSubscriptionMatchesItsOwnFilter) {
  make();
  connect();
  int got = 0;
  client->subscribe("$share/g/a/+", mqtt::QoS::at_most_once, [&](const Message&) { ++got; });
  const auto subscribe = *received<mqtt::Subscribe>(0);
  EXPECT_EQ(subscribe.subscriptions[0].filter, "$share/g/a/+");
  broker->send(0, mqtt::Suback{subscribe.packet_id, {}, {0}});
  mqtt::Publish publish;
  publish.topic = "a/b";
  broker->send(0, publish);
  ASSERT_TRUE(run_until([&] { return got == 1; }));
  stop_client();
}

// What the broker must not send

TEST_F(ClientTest, AMalformedPacketFromTheBrokerEndsTheConnection) {
  make();
  connect();
  // PINGRESP with a body.
  broker->raw(0, std::string("\xD0\x01\x00", 3));
  ASSERT_TRUE(run_until([&] { return states.back().state == ConnectionState::disconnected; }));
  EXPECT_EQ(states.back().reason, reason::malformed_packet);
  EXPECT_EQ(received<mqtt::Disconnect>(0)->reason, reason::malformed_packet);
  EXPECT_TRUE(session_opens(1));
  stop_client();
}

TEST_F(ClientTest, PacketsABrokerDoesNotSendAreProtocolErrors) {
  make();
  connect();
  broker->send(0, mqtt::Ack{mqtt::PacketType::pubrec, 1, reason::success, {}});
  ASSERT_TRUE(run_until([&] { return states.back().state == ConnectionState::disconnected; }));
  EXPECT_EQ(states.back().reason, reason::protocol_error);
  ASSERT_TRUE(session_opens(1));
  ASSERT_TRUE(received<mqtt::Connect>(1).has_value());
  broker->send(1, mqtt::Connack{});
  ASSERT_TRUE(run_until([&] { return states.back().state == ConnectionState::connected; }));
  broker->send(1, mqtt::Pingreq{});
  ASSERT_TRUE(run_until([&] { return states.back().state == ConnectionState::disconnected; }));
  EXPECT_EQ(states.back().why, "a packet a broker does not send");
  ASSERT_TRUE(session_opens(2));
  ASSERT_TRUE(received<mqtt::Connect>(2).has_value());
  broker->send(2, mqtt::Connack{});
  ASSERT_TRUE(run_until([&] { return states.back().state == ConnectionState::connected; }));
  broker->send(2, mqtt::Connack{});
  ASSERT_TRUE(run_until([&] { return states.back().state == ConnectionState::disconnected; }));
  EXPECT_EQ(states.back().why, "a second CONNACK");
  stop_client();
}

TEST_F(ClientTest, APacketBeforeConnackFailsTheAttempt) {
  make();
  client->start();
  ASSERT_TRUE(received<mqtt::Connect>(0).has_value());
  broker->send(0, mqtt::Pingresp{});
  ASSERT_TRUE(run_until([&] { return !states.empty(); }));
  EXPECT_EQ(states.back().why, "a packet before CONNACK");
  stop_client();
}

// Connecting, the rest

TEST_F(ClientTest, WithNoNodeAndNoHostEachAttemptFails) {
  make();
  ClientConfig nowhere;
  nowhere.client_id = "c";
  std::vector<StateChange> seen;
  {
    Client lost(io.get_executor(), nowhere, time, dannan::status::ScopedLogger(sink, "lir.client"));
    lost.on_state([&](const StateChange& change) { seen.push_back(change); });
    lost.start();
    ASSERT_TRUE(run_until([&] { return !seen.empty(); }));
    bool stopped = false;
    lost.stop([&] { stopped = true; });
    ASSERT_TRUE(run_until([&] { return stopped; }));
  }
  EXPECT_EQ(seen.front().why, "no Lir node in the roster and no host configured");
  stop_client();
}

TEST_F(ClientTest, StoppedWhileConnectingNothingIsUsed) {
  make();
  client->start();
  bool stopped = false;
  client->stop([&] { stopped = true; });
  ASSERT_TRUE(run_until([&] { return stopped; }));
  run_until([] { return false; }, 50ms);
  for (const auto& session : broker->sessions) EXPECT_TRUE(session->received.empty());
  stop_client();
}

TEST_F(ClientTest, ATopicMqttCannotCarryIsRefused) {
  make();
  connect();
  std::optional<Outcome> outcome;
  client->publish(message("\xff", "x"), [&](Outcome out) { outcome = out; });
  ASSERT_TRUE(run_until([&] { return outcome.has_value(); }));
  EXPECT_FALSE(outcome->ok);
  EXPECT_EQ(outcome->reason, reason::packet_too_large);
  stop_client();
}

// TLS, with a certificate made for the test by the openssl command line.
TEST_F(ClientTest, ConnectsOverTls) {
  dannan::test::TempDir dir;
  const auto key = dir.path() / "key.pem";
  const auto certificate = dir.path() / "certificate.pem";
  const std::string command =
      "openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -days 1 -subj /CN=localhost"
      " -addext subjectAltName=IP:127.0.0.1 -keyout " +
      key.string() + " -out " + certificate.string() + " >/dev/null 2>&1";
  ASSERT_EQ(std::system(command.c_str()), 0);
  config.tls = Tls{certificate, "", certificate, key};
  make(mqtt::Version::v5, std::make_shared<dannan::listen::Certificates>(dannan::listen::CertificatePaths{certificate, key, {}}));
  connect();
  EXPECT_TRUE(broker->sessions[0]->stream->is_tls());
  stop_client();
}

TEST_F(ClientTest, AnUnreadableTrustBundleIsRefusedAtConstruction) {
  ClientConfig bad;
  bad.tls = Tls{"/nonexistent/trust.pem", "", "", ""};
  EXPECT_THROW(Client(io.get_executor(), bad, time, dannan::status::ScopedLogger(sink, "x")), std::runtime_error);
}

// Stopping

TEST_F(ClientTest, StopDisconnectsAndFailsWhatIsOpen) {
  make();
  connect();
  std::optional<Outcome> outcome;
  std::optional<SubscribeOutcome> subscribed;
  client->publish(message("t", "x", mqtt::QoS::at_least_once), [&](Outcome out) { outcome = out; });
  client->subscribe("t", mqtt::QoS::at_most_once, [](const Message&) {}, [&](SubscribeOutcome out) { subscribed = out; });
  ASSERT_TRUE(received<mqtt::Subscribe>(0).has_value());
  bool stopped = false;
  client->stop([&] { stopped = true; });
  ASSERT_TRUE(run_until([&] { return stopped; }));
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome->why, "stopped");
  ASSERT_TRUE(subscribed.has_value());
  EXPECT_FALSE(subscribed->ok);
  // A normal DISCONNECT, so the broker discards the will.
  const auto disconnect = *received<mqtt::Disconnect>(0);
  EXPECT_EQ(disconnect.reason, reason::normal_disconnection);
  EXPECT_EQ(states.back().state, ConnectionState::disconnected);
  // Nothing more is tried.
  advance(60s);
  EXPECT_EQ(broker->sessions.size(), 1u);
  std::optional<Outcome> after;
  client->publish(message("t", "x"), [&](Outcome out) { after = out; });
  ASSERT_TRUE(run_until([&] { return after.has_value(); }));
  EXPECT_FALSE(after->ok);
  stop_client();
}

TEST_F(ClientTest, AnExecutorThatIsNotAnIoContextsIsRefused) {
  boost::asio::thread_pool pool(1);
  EXPECT_THROW(Client(pool.get_executor(), ClientConfig{}, time, dannan::status::ScopedLogger(sink, "x")), std::invalid_argument);
  pool.join();
  stop_client();
}

}  // namespace
}  // namespace lir::client
