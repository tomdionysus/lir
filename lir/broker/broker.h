//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
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
// any transport (contract::Connection). Alone it is a whole broker. Attached
// to a cluster (attach()), it hands what its clients publish to the cluster,
// takes what other nodes' clients publish through inject(), keeps the
// cluster's retained set through apply_retained(), lets the cluster choose
// which node's member a shared subscription's message goes to, and gives up
// a session when its client connects elsewhere.
//
// Thread safety: none inside. A Broker and its Links are confined to one
// strand; every call, the transport's receive() and closed() and the
// cluster's hooks and their completions included, is made on it. Nothing
// here waits: sends are queued by the transport, time is read from the
// injected source, and poll() does what time has made due.
namespace lir::rules {
class RuleSet;
}  // namespace lir::rules

namespace lir::broker {

using dannan::contract::TimePoint;
using mqtt::Bytes;

// Orders a retained topic's values across nodes with no shared clock: a
// Lamport counter, then the node id. Every node reaches the same value for a
// topic whatever order the updates arrive in.
struct RetainedVersion {
  uint64_t lamport = 0;
  std::string node;
  auto operator<=>(const RetainedVersion&) const = default;
};

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
  // Set on a retained message once the broker has stored it.
  std::optional<RetainedVersion> version;
  // How many rules produced it, one from another (rules/rules.h); never
  // carried between nodes, since rules run where a message is published.
  uint8_t hops = 0;
};

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
  enum class State : uint8_t { awaiting_puback = 0, awaiting_pubrec = 1, awaiting_pubcomp = 2 };
  Delivery delivery;
  State state = State::awaiting_puback;
};

// A session as it moves between nodes: everything the specification says
// the server keeps for it (section 4.1).
struct SessionImage {
  std::string client_id;
  mqtt::Version version = mqtt::Version::v5;
  uint32_t expiry_interval = 0;
  std::vector<std::pair<std::string, Options>> subscriptions;
  std::vector<std::pair<uint16_t, Inflight>> outbound;
  std::vector<Delivery> queued;
  std::vector<uint16_t> inbound_qos2;
  uint16_t next_packet_id = 1;
};

// The defaults are the design's moderate set (section 4, the maintainer 2026-11-04),
// sized for about 10,000 messages a second across a cluster, mostly under
// 1 KiB; every one is configurable.
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
  uint32_t maximum_session_expiry = 7 * 24 * 3600;
  // Overrides the client's keep alive when set (5.0 Server Keep Alive).
  std::optional<uint16_t> server_keep_alive;
  // QoS 1/2 messages held per session while it is offline or at its
  // receive maximum; the oldest is dropped past it, and counted.
  size_t maximum_queued = 10000;
  // Clients connected to this node past which a new one is refused with
  // 0x89 (Server busy); the existing are never evicted (section 4).
  size_t maximum_connections = 10000;
  // A connection that sends no CONNECT within this is closed.
  std::chrono::milliseconds connect_timeout{10000};
  // Topics clients may not publish to: the subscribe-only system prefixes.
  std::vector<std::string> reserved_prefixes{"$dannan/", "$lir/", "$share/"};
  std::string assigned_id_prefix = "lir-";
  // 5.0 Response Information, sent when a client asks for it; "{client}"
  // becomes its client id. Nothing: not sent.
  std::optional<std::string> response_information;
  // QoS 0 messages queued for an offline session too (the specification
  // allows it; off, they are dropped and counted).
  bool queue_qos0_offline = false;
  // The bytes of messages this node holds (queued, in flight, retained)
  // before it gives way, in the design's order (section 4; the maintainer 2026-11-04,
  // all configurable): at `shed_percent` QoS 0 is no longer queued and what
  // is queued is dropped, oldest first; at `refuse_publish_percent` QoS 1
  // and 2 publishes are refused (0x97; a 3.x client is closed, and resends
  // when it returns); at `refuse_connect_percent` new clients are refused
  // (0x89), as they are while the node's Dannan ledger has no room for
  // Lir. Existing sessions are never evicted. Zero: no budget.
  uint64_t memory_budget = 512ull << 20;
  uint32_t shed_percent = 80;
  uint32_t refuse_publish_percent = 90;
  uint32_t refuse_connect_percent = 100;
};

// How hard the memory budget presses, in the order it gives way.
enum class Pressure : uint8_t { none, shed_qos0, refuse_publish, refuse_connect };
const char* pressure_name(Pressure pressure) noexcept;

struct Stats {
  uint64_t connections_accepted = 0;
  uint64_t connections_refused = 0;
  uint64_t protocol_errors = 0;
  uint64_t messages_received = 0;
  uint64_t messages_delivered = 0;
  uint64_t messages_injected = 0;
  uint64_t dropped_offline_qos0 = 0;
  uint64_t dropped_queue_full = 0;
  uint64_t dropped_expired = 0;
  uint64_t dropped_too_large = 0;
  uint64_t wills_published = 0;
  uint64_t sessions_expired = 0;
  uint64_t sessions_taken = 0;
  uint64_t sessions_surrendered = 0;
  uint64_t dropped_pressure = 0;
  uint64_t refused_pressure_publishes = 0;
  uint64_t refused_pressure_connections = 0;
  uint64_t connections_redirected = 0;
  // The rules engine (rules/rules.h).
  uint64_t rules_dropped = 0;
  uint64_t rules_republished = 0;
  uint64_t rules_skipped = 0;
  uint64_t rules_too_deep = 0;
  uint64_t rules_invalid = 0;
};

// A client connected here, or its connection here ended: for the
// $lir/clients events (design, section 3).
struct ClientEvent {
  enum class Kind { connected, disconnected };
  Kind kind = Kind::connected;
  std::string client_id;
  mqtt::Version version = mqtt::Version::v5;
  std::string peer;
  std::optional<std::string> username;
  // connected: whether a session was resumed.
  bool session_present = false;
  // disconnected: who ended it, and the reason code the DISCONNECT carried
  // (sent by the server, or received from the client). Nothing from the
  // network: the connection was lost.
  enum class By { client, server, network };
  By by = By::network;
  std::optional<uint8_t> reason;
};

// What the broker asks of the cluster it is attached to. Every hook is
// called on the broker's strand and must not call back into the broker
// before returning; a completion it is handed is called on the strand later.
struct ClusterHooks {
  // This node's id, which versions its retained values.
  std::string node;
  // A message published on this node (by a client, a will or publish()),
  // already delivered to this node's non-shared subscribers: the cluster
  // carries it to the others, and to one member of each matching shared
  // group anywhere. Returns how many other nodes took it.
  std::function<size_t(const Message&)> published;
  // This node's subscriptions changed (filters or shared groups).
  std::function<void()> interest_changed;
  // A client is connecting here: every other node must close its
  // connection, and hand over its session when `want_state`. `done` gets
  // the session, if any node had one.
  std::function<void(const std::string& client_id, bool want_state, std::function<void(std::optional<SessionImage>)> done)> take;
  // A 5.0 client is connecting on `transport`: another node's address to
  // send it to (CONNACK 0x9C, Server Reference), or nothing to serve it
  // here. Empty: never.
  std::function<std::optional<std::string>(const std::string& transport)> redirect;
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
  // `taking`: the CONNECT is accepted and other nodes are being asked to let
  // go of the client; packets that arrive meanwhile wait in the buffer.
  enum class State { awaiting_connect, authenticating, taking, connected, closed };

  void process();
  void handle(mqtt::Packet&& packet);
  void dispatch(mqtt::Packet&& packet);
  void on_connect(mqtt::Connect&& connect);
  void begin_connect(const contract::AuthDecision& accepted);
  void finish_connect(std::optional<Bytes> auth_data, std::optional<SessionImage> image);
  void on_publish(mqtt::Publish&& publish);
  void on_ack(const mqtt::Ack& ack);
  void on_subscribe(const mqtt::Subscribe& subscribe);
  void on_unsubscribe(const mqtt::Unsubscribe& unsubscribe);
  void on_disconnect(const mqtt::Disconnect& disconnect);
  void on_auth(const mqtt::Auth& auth);
  // Who the client is, as the transport knows it.
  contract::Peer peer() const;
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
  bool _assigned = false;
  std::optional<mqtt::Connect> _connect;
  std::optional<mqtt::Will> _will;
  std::optional<std::string> _auth_method;
  // What this connection may do; set on accept, replaced on re-authentication.
  std::shared_ptr<const contract::Grant> _grant;
  // How the connection ended, for the disconnected event.
  ClientEvent::By _ended_by = ClientEvent::By::network;
  std::optional<uint8_t> _end_reason;
  uint16_t _keep_alive = 0;
  TimePoint _opened;
  TimePoint _last_received;
  std::map<uint16_t, std::string> _inbound_aliases;
  // Topic aliases this server has given the client, up to its maximum.
  std::map<std::string, uint16_t> _outbound_aliases;
  uint16_t _client_alias_maximum = 0;
};

class Broker {
 public:
  Broker(Config config, const dannan::contract::TimeSource& time, contract::Authenticator& authenticator, dannan::status::ScopedLogger logger);
  ~Broker();
  Broker(const Broker&) = delete;
  Broker& operator=(const Broker&) = delete;

  // A new client connection. The transport keeps the Link and feeds it.
  std::shared_ptr<Link> accept(std::shared_ptr<contract::Connection> connection);

  // A message from the server itself: retained and delivered as a client's
  // would be (to the cluster too, when attached), without the authorizer.
  // Returns how many sessions here, and other nodes, took it.
  size_t publish(Message message);

  // Keep alive and connect timeouts, delayed wills, session expiry: what the
  // injected time has made due. The owner calls it on a timer.
  void poll();

  // `observer` is told of each ClientEvent, on the broker's strand; it must
  // not call back into the broker before returning. Empty: none.
  void observe(std::function<void(const ClientEvent&)> observer);

  // ---- the cluster's side

  // Attaches to a cluster (or, with empty hooks, detaches). Shared
  // subscriptions are then the cluster's to choose a member for.
  void attach(ClusterHooks hooks);
  // A message another node published: delivered to this node's non-shared
  // subscribers, never stored or carried further. How many sessions took it.
  size_t inject(const Message& message);
  // Another node's retained update (an empty payload clears): kept when its
  // version is newer than what is held here. Whether it was.
  bool apply_retained(const Message& message);
  // Every retained value and every clearing, versioned, for a node catching
  // up.
  std::vector<Message> retained_entries() const;
  // The retained values whose topics start with `prefix`, as (topic,
  // payload).
  std::vector<std::pair<std::string, Bytes>> retained_under(const std::string& prefix) const;
  // Delivers to one member of the shared group (group, filter) here. False
  // when this node has none.
  bool deliver_shared(const Message& message, const std::string& group, const std::string& filter);
  // The client connected elsewhere: its connection here is closed (session
  // taken over) and its session given up, as an image when `want_state`,
  // or ended (its will sent) otherwise.
  std::optional<SessionImage> surrender(const std::string& client_id, bool want_state);
  // This node's non-shared filters, and its shared groups as (group,
  // filter).
  std::set<std::string> local_filters() const;
  std::set<std::pair<std::string, std::string>> local_shared() const;

  // Persistent sessions (expiry above 0) changed since the last call, as
  // images, and those that ended or left this node: what the cluster
  // mirrors to standbys.
  struct Changes {
    std::vector<SessionImage> changed;
    std::vector<std::string> ended;
  };
  Changes take_changes();
  // Every persistent session counts as changed (the standbys moved).
  void touch_all();
  // A session whose node is gone, taken up from a standby's mirror: held
  // here offline, receiving and queueing, until its client connects. False
  // when this node already has the client.
  bool adopt(SessionImage image);

  size_t sessions() const noexcept { return _sessions.size(); }
  size_t connections() const noexcept { return _links.size(); }
  // Connections past CONNECT: connected, or on their way to it.
  size_t clients() const noexcept;
  size_t retained() const noexcept { return _retained.size(); }
  // The bytes of messages held, as counted, and the pressure they make.
  uint64_t held_bytes() const noexcept { return _held; }
  Pressure pressure() const noexcept;
  // The same bytes counted afresh from every queue and the retained set;
  // for tests, which check that the running count never drifts.
  uint64_t recount_held_bytes() const;
  // Whether the node's memory ledger (Dannan's, shared by every system on
  // the node) refused Lir room: while it does, the pressure is at least
  // refuse_connect.
  void ledger_full(bool full) noexcept { _ledger_full = full; }
  const Stats& stats() const noexcept { return _stats; }
  const Config& config() const noexcept { return _config; }
  // The rules held, from the retained $lir/rules/ messages.
  size_t rules() const noexcept;

 private:
  friend class Link;

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
    // Bytes of its queued and in-flight messages.
    uint64_t held = 0;
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

  bool clustered() const noexcept { return static_cast<bool>(_hooks.published); }
  bool reserved(std::string_view topic) const;
  // A message published on this node: stored if retained, delivered here,
  // and handed to the cluster. How many sessions and nodes took it.
  size_t route(Message message);
  size_t route_here(Message message);
  void route_produced(Message message);
  // Delivers to this node's subscribers: non-shared ones, and shared groups
  // too when `shared`.
  size_t deliver_here(const Message& message, bool shared);
  bool deliver_to_group(const Message& message, SharedGroup& group);
  void deliver(Session& session, Delivery delivery);
  // Sends queued messages while the client's receive maximum allows.
  void pump(Session& session);
  // Sends one delivery now: a QoS 0 straight out, a QoS 1/2 under a new
  // packet identifier. False if it was dropped (expired, too large).
  bool transmit(Session& session, Delivery delivery);
  mqtt::Publish build(const Session& session, const Delivery& delivery, uint16_t packet_id, bool dup) const;
  // The bytes for a PUBLISH to the session's client, with a topic alias
  // where it may have one; nothing when over the client's maximum size.
  std::optional<Bytes> outgoing(Session& session, mqtt::Publish publish);
  void resend(Session& session);
  void subscribe(Session& session, const std::string& filter, const Options& options);
  bool unsubscribe(Session& session, const std::string& filter);
  // Removes the subscriptions `grant` no longer allows (design, section 1:
  // a narrower grant on re-authentication or at reconnect); how many.
  size_t prune(Session& session, const contract::Grant& grant);
  void send_retained(Session& session, const std::string& filter, const Options& options);
  // Ends a session: its pending will goes, then the session.
  void end_session(const std::string& client_id);
  // Removes a session and its subscriptions, its will untouched.
  void drop_session(const std::string& client_id);
  SessionImage image_of(const Session& session) const;
  Session& import(SessionImage image);
  void schedule_will(const std::string& client_id, const mqtt::Will& will, uint32_t session_expiry);
  void fire_will(const std::string& client_id);
  // The link has closed: its session goes offline (or ends, at expiry 0)
  // and its will is scheduled when `publish_will`.
  void detach(Link& link, bool publish_will);
  void interest_changed();
  void tell(ClientEvent event);
  void charge(Session& session, int64_t bytes);
  void charge_retained(int64_t bytes);
  // Drops queued QoS 0 messages, oldest first, until the pressure eases.
  void shed();
  // Marks a persistent session as changed, for the mirror.
  void touch(const std::string& client_id);
  std::string assign_client_id();

  Config _config;
  const dannan::contract::TimeSource& _time;
  contract::Authenticator& _authenticator;
  dannan::status::ScopedLogger _logger;
  ClusterHooks _hooks;
  std::function<void(const ClientEvent&)> _observer;
  uint64_t _held = 0;
  std::unique_ptr<rules::RuleSet> _rules;
  bool _ledger_full = false;
  std::map<std::string, std::unique_ptr<Session>> _sessions;
  // Non-shared subscriptions keyed by client id.
  subs::Trie<std::string, Options> _subscriptions;
  // Shared: the inner filter, keyed by group, holding the inner filter
  // again so a match names its (group, filter).
  subs::Trie<std::string, std::string> _shared_filters;
  std::map<std::pair<std::string, std::string>, SharedGroup> _shared;
  std::map<std::string, Message> _retained;
  // Clearings, kept so an older value arriving late cannot return.
  std::map<std::string, RetainedVersion> _cleared;
  uint64_t _lamport = 0;
  std::map<std::string, PendingWill> _wills;
  std::map<Link*, std::shared_ptr<Link>> _links;
  std::set<std::string> _changed;
  std::set<std::string> _ended;
  Stats _stats;
};

}  // namespace lir::broker
