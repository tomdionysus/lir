//
// Lir - The Distributed Event System
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "dannan/contract/time_source.h"
#include "dannan/status/log.h"
#include "lir/contract/authenticator.h"
#include "lir/contract/connection.h"
#include "lir/mqtt/packet.h"
#include "lir/subs/trie.h"

// The MQTT broker on one node: sessions, subscriptions, retained messages,
// wills, shared subscriptions and every QoS flow of 3.1, 3.1.1 and 5.0, over
// any transport (contract::Connection). It knows nothing of the network
// below or of other nodes; the cluster (step 4) feeds it publishes from
// elsewhere through publish() and takes its own through the route hook.
//
// Thread safety: none inside. A Broker and its Links are confined to one
// strand; every call, the transport's receive() and closed() included, is
// made on it. Nothing here waits: sends are queued by the transport, time is
// read from the injected source, and poll() does what time has made due.
namespace lir::broker {

using dannan::contract::TimePoint;
using mqtt::Bytes;

// A message as the broker holds it, independent of any one session's
// protocol version.
struct Message {
  std::string topic;
  Bytes payload;
  mqtt::QoS qos = mqtt::QoS::at_most_once;
  bool retain = false;
  // 5.0 properties carried from publisher to subscriber.
  std::optional<uint8_t> payload_format_indicator;
  std::optional<TimePoint> expires_at;
  std::optional<std::string> content_type;
  std::optional<std::string> response_topic;
  std::optional<Bytes> correlation_data;
  std::vector<std::pair<std::string, std::string>> user_properties;
  // The publishing client, for No Local; empty for the server's own.
  std::string origin;
};

struct Config {
  // Inbound limits, advertised in a 5.0 CONNACK.
  uint32_t maximum_packet_size = 1 << 20;
  uint16_t receive_maximum = 1024;
  uint16_t topic_alias_maximum = 64;
  mqtt::QoS maximum_qos = mqtt::QoS::exactly_once;
  bool retain_available = true;
  bool wildcard_subscription_available = true;
  bool subscription_identifier_available = true;
  bool shared_subscription_available = true;
  // A client's requested session expiry is capped at this (seconds;
  // 0xFFFFFFFF is never).
  uint32_t maximum_session_expiry = 0xFFFFFFFF;
  // Overrides the client's keep alive when set (5.0 Server Keep Alive).
  std::optional<uint16_t> server_keep_alive;
  // QoS 1/2 messages held per session while it is offline or at its
  // receive maximum; the oldest is dropped past it, and counted.
  size_t maximum_queued = 1000;
  // A connection that sends no CONNECT within this is closed.
  std::chrono::milliseconds connect_timeout{10000};
  // Topics clients may not publish to: the subscribe-only system prefixes.
  std::vector<std::string> reserved_prefixes{"$dannan/", "$lir/", "$share/"};
  std::string assigned_id_prefix = "lir-";
};

struct Stats {
  uint64_t connections_accepted = 0;
  uint64_t connections_refused = 0;
  uint64_t protocol_errors = 0;
  uint64_t messages_received = 0;
  uint64_t messages_delivered = 0;
  uint64_t dropped_offline_qos0 = 0;
  uint64_t dropped_queue_full = 0;
  uint64_t dropped_expired = 0;
  uint64_t dropped_too_large = 0;
  uint64_t wills_published = 0;
  uint64_t sessions_expired = 0;
};

class Broker;

// One client connection's protocol state: from the first byte to the close.
// The transport feeds it bytes and tells it of the close; the broker owns it
// and keeps it until then.
class Link : public std::enable_shared_from_this<Link> {
 public:
  Link(Broker& broker, std::shared_ptr<contract::Connection> connection);
  // Bytes in arrival order, any split. Waits on nothing.
  void receive(std::span<const uint8_t> bytes);
  // The transport closed (EOF, error). The will is published as the
  // specification says. Idempotent.
  void closed();
  bool open() const noexcept { return _state != State::closed; }
  const std::string& client_id() const noexcept { return _client_id; }

 private:
  friend class Broker;
  enum class State { awaiting_connect, authenticating, connected, closed };

  void handle(mqtt::Packet&& packet);
  void on_connect(mqtt::Connect&& connect);
  void finish_connect(std::optional<Bytes> auth_data);
  void on_publish(mqtt::Publish&& publish);
  void on_ack(const mqtt::Ack& ack);
  void on_subscribe(const mqtt::Subscribe& subscribe);
  void on_unsubscribe(const mqtt::Unsubscribe& unsubscribe);
  void on_disconnect(const mqtt::Disconnect& disconnect);
  void on_auth(const mqtt::Auth& auth);
  void refuse_connect(uint8_t reason);
  // Ends the connection: a 5.0 DISCONNECT with `reason` when connected,
  // then the close; the will as `publish_will` says.
  void shutdown(std::optional<uint8_t> reason, bool publish_will);
  void send(const mqtt::Packet& packet);
  bool v5() const noexcept { return _version == mqtt::Version::v5; }

  Broker& _broker;
  std::shared_ptr<contract::Connection> _connection;
  State _state = State::awaiting_connect;
  mqtt::Version _version = mqtt::Version::v5;
  Bytes _buffer;
  std::string _client_id;
  std::optional<mqtt::Connect> _connect;
  std::optional<mqtt::Will> _will;
  std::optional<std::string> _auth_method;
  uint16_t _keep_alive = 0;
  TimePoint _opened;
  TimePoint _last_received;
  std::map<uint16_t, std::string> _inbound_aliases;
};

class Broker {
 public:
  Broker(Config config, const dannan::contract::TimeSource& time, contract::Authenticator& authenticator, dannan::status::ScopedLogger logger);
  Broker(const Broker&) = delete;
  Broker& operator=(const Broker&) = delete;

  // A new client connection. The transport keeps the Link and feeds it.
  std::shared_ptr<Link> accept(std::shared_ptr<contract::Connection> connection);

  // A message from the server itself, or from another node: retained and
  // delivered as a client's would be, without the authorizer. Returns how
  // many sessions it was delivered or queued to.
  size_t publish(Message message);

  // Keep alive and connect timeouts, delayed wills, session expiry: what the
  // injected time has made due. The owner calls it on a timer.
  void poll();

  size_t sessions() const noexcept { return _sessions.size(); }
  size_t connections() const noexcept { return _links.size(); }
  size_t retained() const noexcept { return _retained.size(); }
  const Stats& stats() const noexcept { return _stats; }
  const Config& config() const noexcept { return _config; }

 private:
  friend class Link;

  struct Options {
    mqtt::QoS qos = mqtt::QoS::at_most_once;
    bool no_local = false;
    bool retain_as_published = false;
    uint8_t retain_handling = 0;
    std::optional<uint32_t> identifier;
    bool operator==(const Options&) const = default;
  };

  // One message on its way to one session.
  struct Delivery {
    Message message;
    mqtt::QoS qos = mqtt::QoS::at_most_once;
    bool retain = false;
    std::vector<uint32_t> identifiers;
  };

  struct Inflight {
    enum class State { awaiting_puback, awaiting_pubrec, awaiting_pubcomp };
    Delivery delivery;
    State state = State::awaiting_puback;
  };

  struct Session {
    std::string client_id;
    mqtt::Version version = mqtt::Version::v5;
    // Seconds; 0xFFFFFFFF never.
    uint32_t expiry_interval = 0;
    // As subscribed: a shared filter keeps its "$share/<group>/" prefix.
    std::map<std::string, Options> subscriptions;
    std::map<uint16_t, Inflight> outbound;
    std::deque<Delivery> queued;
    // QoS 2 packet identifiers PUBREC'd and awaiting PUBREL.
    std::set<uint16_t> inbound_qos2;
    uint16_t next_packet_id = 1;
    Link* link = nullptr;
    std::optional<TimePoint> expires_at;
    uint16_t client_receive_maximum = 65535;
    uint32_t client_maximum_packet_size = 0xFFFFFFFF;
  };

  struct SharedGroup {
    std::map<std::string, Options> members;
    size_t next = 0;
  };

  struct PendingWill {
    Message message;
    // A will's message expiry counts from when it is published.
    std::optional<uint32_t> expiry_seconds;
    TimePoint due;
  };

  bool reserved(std::string_view topic) const;
  // Delivers to every matching subscription; how many sessions took it.
  size_t route(const Message& message);
  void deliver(Session& session, Delivery delivery);
  // Sends queued messages while the client's receive maximum allows.
  void pump(Session& session);
  // Sends one delivery now: a QoS 0 straight out, a QoS 1/2 under a new
  // packet identifier. False if it was dropped (expired, too large).
  bool transmit(Session& session, Delivery delivery);
  mqtt::Publish build(const Session& session, const Delivery& delivery, uint16_t packet_id, bool dup) const;
  void resend(Session& session);
  void subscribe(Session& session, const std::string& filter, const Options& options);
  bool unsubscribe(Session& session, const std::string& filter);
  void send_retained(Session& session, const std::string& filter, const Options& options);
  void end_session(const std::string& client_id);
  void schedule_will(const std::string& client_id, const mqtt::Will& will, uint32_t session_expiry);
  void fire_will(const std::string& client_id);
  // The link has closed: its session goes offline (or ends, at expiry 0)
  // and its will is scheduled when `publish_will`.
  void detach(Link& link, bool publish_will);
  std::string assign_client_id();

  Config _config;
  const dannan::contract::TimeSource& _time;
  contract::Authenticator& _authenticator;
  dannan::status::ScopedLogger _logger;
  std::map<std::string, std::unique_ptr<Session>> _sessions;
  // Non-shared subscriptions keyed by client id.
  subs::Trie<std::string, Options> _subscriptions;
  // Shared: the inner filter, keyed by group, holding the inner filter
  // again so a match names its (group, filter).
  subs::Trie<std::string, std::string> _shared_filters;
  std::map<std::pair<std::string, std::string>, SharedGroup> _shared;
  std::map<std::string, Message> _retained;
  std::map<std::string, PendingWill> _wills;
  std::map<Link*, std::shared_ptr<Link>> _links;
  Stats _stats;
};

}  // namespace lir::broker
