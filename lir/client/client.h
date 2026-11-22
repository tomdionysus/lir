//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "dannan/context.h"
#include "dannan/contract/time_source.h"
#include "dannan/status/log.h"
#include "lir/mqtt/packet.h"

// A small MQTT client of Lir (the maintainer, 2026-11-15), for the stack's projects that
// publish or subscribe on it as any MQTT client would (Louhi, Lugh's events,
// Mimir): MQTT 5.0 or 3.1.1 over TCP or TLS, a token as the password, QoS 0
// and 1 publish and receive, retained publish, a will, wildcard subscribe,
// and reconnecting with backoff. QoS 2 and 5.0's topic aliases and user
// properties are left out until someone needs them.
//
// Thread-safe: every call may come from any thread and returns at once; the
// client runs on its own strand of the executor it was given, and every
// callback is posted there, never called inline, in the order things happened.
// The connection is Dannan's (dannan::listen::StreamConnector): TCP_NODELAY,
// the TLS checks, the bounded send queue and the draining close are its.
namespace lir::client {

// The server's certificate is checked against `trust` (a PEM bundle; empty:
// the system's store) and its name against `server_name` (empty: the host
// connected to; an IP address is checked against the certificate's IP
// addresses, and sent as no SNI). A client certificate is offered when both
// of its paths are given. Read when the client is made.
struct Tls {
  std::filesystem::path trust;
  std::string server_name;
  std::filesystem::path certificate_chain;
  std::filesystem::path private_key;
};

struct Message {
  std::string topic;
  mqtt::Bytes payload;
  // 0 or 1.
  mqtt::QoS qos = mqtt::QoS::at_most_once;
  bool retain = false;
};

struct ClientConfig {
  // The process's Dannan roster, the one list of Lir's nodes (the maintainer,
  // 2026-11-15): each attempt connects to a node advertising Lir, at its
  // "tcp" endpoint ("tls" with `tls`), in turn, so a lost node is passed over
  // at once; the backoff applies once each has failed since the last
  // connection. Nothing: `host` and `port`, for a process not on Dannan.
  // Outlives the client.
  const dannan::Nodes* nodes = nullptr;
  // Without `nodes`, or while the roster shows no Lir node.
  std::string host;
  uint16_t port = 1883;
  // Nothing: plain TCP.
  std::optional<Tls> tls;
  std::string client_id;
  mqtt::Version version = mqtt::Version::v5;
  std::optional<std::string> username;
  // Asked at every CONNECT, so a Lugh token can be fresh each time; nothing:
  // no password. Called on the client's strand; must not block.
  std::function<std::optional<std::string>()> password;
  std::chrono::seconds keep_alive{30};
  // Whether the first CONNECT starts clean; a reconnect resumes the session
  // when `session_expiry` kept it (5.0) or `clean_start` is false (3.1.1).
  bool clean_start = true;
  // 5.0: seconds the broker keeps the session after a disconnect.
  uint32_t session_expiry = 0;
  std::optional<Message> will;
  // Between attempts: doubling from the first to the most, each a random
  // part of it between half and all.
  std::chrono::milliseconds backoff_first{500};
  std::chrono::milliseconds backoff_most{30000};
  // A connection and its CONNACK within this, or the attempt has failed.
  std::chrono::milliseconds connect_timeout{10000};
  // QoS 1 publishes awaiting PUBACK at once; less when the broker's receive
  // maximum is less.
  uint16_t maximum_inflight = 64;
  // Payload bytes held for publishes not yet sent, while disconnected or past
  // the inflight bound; a publish past it fails at once.
  size_t maximum_queued_bytes = 4u << 20;
};

// What became of a publish, a subscribe or the connection. `reason` is the
// broker's reason code where it gave one; `why` says it in words.
struct Outcome {
  bool ok = false;
  uint8_t reason = mqtt::reason::success;
  std::string why;
};

struct SubscribeOutcome {
  bool ok = false;
  // The QoS granted, when ok.
  mqtt::QoS granted = mqtt::QoS::at_most_once;
  uint8_t reason = mqtt::reason::success;
  std::string why;
};

enum class ConnectionState : uint8_t { connected, disconnected };

struct StateChange {
  ConnectionState state = ConnectionState::disconnected;
  // On connected: whether the broker resumed the session.
  bool session_present = false;
  // On disconnected: the broker's reason code, where it gave one, and why.
  uint8_t reason = mqtt::reason::success;
  std::string why;
};

// Names a subscription for unsubscribe; zero is none.
using SubscriptionId = uint64_t;

class Client {
 public:
  // The time source and the executor's context outlive the client. The
  // executor is an io context's or a strand of one. Throws std::runtime_error
  // for a trust bundle, certificate or key that cannot be read.
  Client(boost::asio::any_io_executor executor, ClientConfig config, const dannan::contract::TimeSource& time, dannan::status::ScopedLogger logger);
  // Stops without waiting; callbacks already queued may still run. Call stop()
  // and wait for its done first when they capture what is being destroyed.
  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  // Connects, and reconnects whenever the connection is lost, until stop().
  void start();
  // Sends DISCONNECT (so the will is not published) and closes. Every
  // publish and subscribe still open fails ("stopped"); then `done`.
  void stop(std::function<void()> done = {});

  // QoS 0: done once the packet is handed to the connection. QoS 1: once the broker's PUBACK
  // arrives, sent again after a reconnect until it does; fails for a refusal
  // (reason 0x80 or more), a topic or payload the broker or MQTT cannot take,
  // a full queue, or stop(). Queued while disconnected.
  void publish(Message message, std::function<void(Outcome)> done = {});

  // `filter` may hold wildcards; each matching message goes to `on_message`,
  // in order. Kept across reconnects: subscribed again whenever the broker
  // has no session. `done` once the first SUBACK answers.
  SubscriptionId subscribe(std::string filter, mqtt::QoS qos, std::function<void(const Message&)> on_message, std::function<void(SubscribeOutcome)> done = {});
  void unsubscribe(SubscriptionId id);

  // Each change of the connection, and each attempt that failed, in order.
  void on_state(std::function<void(const StateChange&)> handler);

 private:
  struct State;
  std::shared_ptr<State> _state;
};

}  // namespace lir::client
