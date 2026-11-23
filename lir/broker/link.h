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
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "dannan/context.h"
#include "dannan/status/log.h"
#include "lir/broker/delivery.h"
#include "lir/contract/authenticator.h"
#include "lir/contract/connection.h"
#include "lir/mqtt/packet.h"
#include "lir/system/views.h"

// One client's protocol (the rewrite's design, "Connection"): fed the bytes
// its transport read, it answers through contract::Connection, and has no I/O
// of its own. What a cluster needs it takes from below: delivery is Dannan's
// channel (Delivery), time is the node's (dannan::Time). Until a noun ships,
// it says so in MQTT's own terms (the design, "What may be acknowledged"):
// Maximum QoS 0, Retain Available 0, no shared subscriptions or
// subscription identifiers, no wills, and a session that ends with its
// connection.
namespace lir::broker {

// The nearest 3.1 and 3.1.1 CONNACK return code for a 5.0 refusal.
uint8_t connack_code_3x(uint8_t reason) noexcept;

class Link : public std::enable_shared_from_this<Link> {
 public:
  struct Limits {
    // The largest packet taken from the client, told to it in a 5.0 CONNACK.
    uint32_t maximum_packet_size = 1024 * 1024;
    // How long a SUBACK waits for the cluster to hold a new subscription.
    std::chrono::milliseconds subscribe_deadline{5000};
    // The most topic aliases a 5.0 client may set on its connection, told
    // to it in CONNACK; 0: none.
    uint16_t topic_alias_maximum = 64;
  };

  // Everything runs on `strand`: every call here is made on it, and every
  // completion Link waits for is posted to it. `delivery`, `authenticator`,
  // `time` and `views` outlive the link. With `views`, a subscription to
  // `$lir/` or `$dannan/` is answered from them, and the link publishes its
  // client's connection and disconnection (TODO/2026-11-22-system-topics.md);
  // without, neither.
  static std::shared_ptr<Link> create(boost::asio::any_io_executor strand, Delivery& delivery, contract::Authenticator& authenticator, dannan::Time& time,
                                      std::shared_ptr<contract::Connection> connection, dannan::status::ScopedLogger logger, Limits limits,
                                      system::Views* views = nullptr);
  Link(const Link&) = delete;
  Link& operator=(const Link&) = delete;

  // Bytes the transport read, in order. Waits on nothing.
  void receive(std::span<const uint8_t> bytes);
  // The transport ended: the link drops what it holds. Idempotent.
  void closed();

  bool connected() const noexcept { return _state == State::connected; }
  const std::string& client_id() const noexcept { return _client_id; }

 private:
  enum class State { awaiting_connect, authenticating, connected, closed };

  // One filter this connection holds. `delivery` is Delivery's id, `token`
  // this link's, so a late answer for a filter since dropped is told apart;
  // `held` once the cluster holds it, and until then the SUBACKs waiting on
  // it are listed (SUBACK, filter index).
  struct Held {
    uint64_t delivery = 0;
    // The views' subscription, for a system filter; zero for none.
    uint64_t view = 0;
    uint64_t token = 0;
    mqtt::Subscription options;
    bool held = false;
    std::vector<std::pair<uint64_t, size_t>> waiting;
  };
  // A SUBACK not yet sent: one reason per filter, and how many still wait.
  struct PendingSuback {
    uint16_t packet_id = 0;
    std::vector<uint8_t> reasons;
    size_t outstanding = 0;
  };

  Link(boost::asio::any_io_executor strand, Delivery& delivery, contract::Authenticator& authenticator, dannan::Time& time,
       std::shared_ptr<contract::Connection> connection, dannan::status::ScopedLogger logger, Limits limits, system::Views* views);

  void handle(const mqtt::Packet& packet);
  void on_connect(const mqtt::Connect& connect);
  void on_auth(const mqtt::Auth& auth);
  void decided(const contract::AuthDecision& decision);
  void accept(std::shared_ptr<const contract::Grant> grant, std::optional<mqtt::Bytes> data);
  void refuse_connect(uint8_t reason);
  void on_subscribe(const mqtt::Subscribe& subscribe);
  void on_held(uint64_t token, const std::string& filter, bool missed);
  void answer(uint64_t suback, size_t index, uint8_t reason);
  void on_unsubscribe(const mqtt::Unsubscribe& unsubscribe);
  void on_publish(const mqtt::Publish& publish);
  void on_disconnect(const mqtt::Disconnect& disconnect);
  void deliver(uint64_t token, const std::string& filter, const mqtt::Publish& publish, const std::string& publisher);
  // A view's state or change, or a roster event, for a system filter.
  void deliver_view(uint64_t token, const std::string& filter, const mqtt::Publish& publish);
  // The views' current state under `filter`, as Retain Handling allows.
  void send_views(uint64_t token, const std::string& filter);
  // `$lir/clients/<what>` on the channel, from this node.
  void announce(const std::string& what, std::optional<uint8_t> code, const std::string& why);
  // Drops a filter, answering the SUBACKs still waiting on it with `reason`.
  void drop(std::map<std::string, Held>::iterator it, uint8_t reason);

  void send(const mqtt::Packet& packet);
  // A 5.0 DISCONNECT with `reason`, then close; a 3.x connection just closes.
  void fail(uint8_t reason, const std::string& why);
  void close();

  void arm_keep_alive();
  void check_keep_alive();
  void arm_expiry();
  // Runs `fn` on the strand after `delay`, while the link lives.
  void after(std::chrono::milliseconds delay, std::function<void(Link&)> fn);

  boost::asio::any_io_executor _strand;
  Delivery& _delivery;
  contract::Authenticator& _authenticator;
  dannan::Time& _time;
  std::shared_ptr<contract::Connection> _connection;
  dannan::status::ScopedLogger _logger;
  Limits _limits;
  system::Views* _views;

  // All below: the strand's.
  State _state = State::awaiting_connect;
  mqtt::Bytes _input;
  mqtt::Version _version = mqtt::Version::v5;
  std::string _client_id;
  contract::Peer _peer;
  std::optional<mqtt::Connect> _connect;
  std::shared_ptr<const contract::Grant> _grant;
  uint64_t _grant_generation = 0;
  std::optional<uint32_t> _client_maximum_packet_size;
  std::chrono::milliseconds _keep_alive{0};
  dannan::contract::TimePoint _last_heard{};
  std::map<std::string, Held> _filters;
  // The client's topic aliases, this connection's only.
  std::map<uint16_t, std::string> _aliases;
  std::map<uint64_t, PendingSuback> _subacks;
  // Set at the CONNACK that accepted it: a disconnection is announced only
  // for a connection that was. And how the connection ended, for that.
  bool _announced = false;
  std::optional<uint8_t> _end_reason;
  std::string _end_why;
  uint64_t _next_suback = 1;
  uint64_t _next_token = 1;
};

}  // namespace lir::broker
