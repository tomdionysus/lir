//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "lir/client/client.h"

#include <algorithm>
#include <atomic>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <charconv>
#include <deque>
#include <map>
#include <random>
#include <set>
#include <utility>
#include <vector>

#include "lir/mqtt/topic.h"

namespace lir::client {

namespace asio = boost::asio;
using dannan::contract::TimePoint;
using mqtt::QoS;
using mqtt::Version;
namespace reason = mqtt::reason;

namespace {

constexpr size_t kReadChunk = 16 * 1024;
// What the broker may send us: as much as MQTT can carry.
constexpr uint32_t kMaximumIncoming = mqtt::varint_max;

mqtt::Will to_will(const Message& message) {
  mqtt::Will will;
  will.topic = message.topic;
  will.payload = message.payload;
  will.qos = message.qos;
  will.retain = message.retain;
  return will;
}

// "host:port", the form Lir's Server Reference takes; nothing for another.
std::optional<std::pair<std::string, uint16_t>> parse_reference(const std::string& reference) {
  const auto colon = reference.rfind(':');
  if (colon == std::string::npos || colon == 0) return std::nullopt;
  uint16_t port = 0;
  const auto* first = reference.data() + colon + 1;
  const auto* last = reference.data() + reference.size();
  const auto [end, error] = std::from_chars(first, last, port);
  if (error != std::errc{} || end != last || port == 0) return std::nullopt;
  return std::pair{reference.substr(0, colon), port};
}

}  // namespace

struct Client::State : std::enable_shared_from_this<State> {
  enum class Phase : uint8_t { idle, connecting, awaiting_connack, connected, waiting, stopped };

  struct Pending {
    Message message;
    std::function<void(Outcome)> done;
    uint16_t packet_id = 0;
  };

  struct Subscribed {
    std::string filter;
    QoS qos = QoS::at_most_once;
    std::shared_ptr<std::function<void(const Message&)>> on_message;
    std::function<void(SubscribeOutcome)> done;
    // Whether the broker has answered for it on this session.
    bool acknowledged = false;
  };

  struct Outgoing {
    mqtt::Bytes bytes;
    // Told whether it was written; nothing for packets that need no word.
    std::function<void(bool)> after;
  };

  State(asio::any_io_executor executor, ClientConfig given, const dannan::contract::TimeSource& clock, dannan::status::ScopedLogger log)
      : strand(asio::make_strand(executor)), config(std::move(given)), time(clock), logger(std::move(log)), timer(strand), random(std::random_device{}()) {
    backoff = config.backoff_first;
  }

  // ---- posted callbacks, in order, on the strand

  template <typename F>
  void tell(F&& f) {
    asio::post(strand, std::forward<F>(f));
  }

  void tell_outcome(std::function<void(Outcome)> done, Outcome outcome) {
    if (done) tell([done = std::move(done), outcome = std::move(outcome)] { done(outcome); });
  }

  void tell_state(StateChange change) {
    if (on_state) tell([handler = on_state, change = std::move(change)] { handler(change); });
  }

  // ---- connecting

  void attempt() {
    if (phase == Phase::stopped) return;
    phase = Phase::connecting;
    const uint64_t attempt_generation = ++generation;
    connect_deadline = time.now() + config.connect_timeout;
    auto host = config.host;
    auto port = config.port;
    if (redirect) {
      host = redirect->first;
      port = redirect->second;
      redirect.reset();
    }
    client::connect(strand, host, port, config.tls, config.connect_timeout,
                    [self = shared_from_this(), attempt_generation](std::shared_ptr<Transport> made, std::string why) {
                      if (self->generation != attempt_generation || self->phase != Phase::connecting) {
                        if (made) made->close();
                        return;
                      }
                      if (!made) return self->lost(std::move(why), reason::success);
                      self->transport = std::move(made);
                      self->phase = Phase::awaiting_connack;
                      self->incoming.clear();
                      self->read();
                      self->send_connect();
                    });
    arm();
  }

  void send_connect() {
    mqtt::Connect connect;
    connect.version = config.version;
    // A reconnect asks for the session back; 3.1.1's clean session is the
    // caller's choice every time, as it decides whether one is kept at all.
    connect.clean_start = (ever_connected && config.version == Version::v5) ? false : config.clean_start;
    connect.keep_alive = static_cast<uint16_t>(std::min<int64_t>(config.keep_alive.count(), UINT16_MAX));
    connect.client_id = config.client_id;
    if (config.will) connect.will = to_will(*config.will);
    connect.username = config.username;
    if (config.password) {
      if (auto password = config.password()) connect.password = mqtt::Bytes(password->begin(), password->end());
    }
    if (config.version == Version::v5 && config.session_expiry > 0) connect.properties.session_expiry_interval = config.session_expiry;
    write_packet(connect, {});
  }

  void on_connack(const mqtt::Connack& connack) {
    if (connack.reason != reason::success) {
      std::string why = "the broker refused the connection (reason " + std::to_string(connack.reason) + ")";
      if (connack.properties.reason_string) why += ": " + *connack.properties.reason_string;
      if ((connack.reason == reason::use_another_server || connack.reason == reason::server_moved) && connack.properties.server_reference) {
        redirect = parse_reference(*connack.properties.server_reference);
        if (redirect) why = "the broker sent the client to " + *connack.properties.server_reference;
      }
      return lost(std::move(why), connack.reason);
    }
    phase = Phase::connected;
    ever_connected = true;
    backoff = config.backoff_first;
    const auto& p = connack.properties;
    receive_maximum = p.receive_maximum.value_or(UINT16_MAX);
    maximum_packet_size = p.maximum_packet_size.value_or(0);
    maximum_qos = p.maximum_qos.value_or(1);
    retain_available = p.retain_available.value_or(1) != 0;
    keep_alive = p.server_keep_alive ? std::chrono::seconds(*p.server_keep_alive) : config.keep_alive;
    if (p.assigned_client_identifier && config.client_id.empty()) config.client_id = *p.assigned_client_identifier;
    ping_sent.reset();
    last_sent = time.now();

    // What the session does not hold is sent again: subscriptions it lost or
    // never had, and every QoS 1 publish not yet acknowledged.
    for (auto& [id, sub] : subscriptions) {
      if (!connack.session_present || !sub.acknowledged) send_subscribe(id, sub);
    }
    for (auto& [packet_id, pending] : inflight) send_publish(pending, connack.session_present);
    tell_state({ConnectionState::connected, connack.session_present, reason::success, {}});
    pump();
    arm();
  }

  // The connection is gone, or an attempt failed: everything bound to it ends,
  // and the next attempt waits its turn.
  void lost(std::string why, uint8_t broker_reason) {
    if (phase == Phase::stopped || phase == Phase::idle || phase == Phase::waiting) return;
    ++generation;
    if (transport) {
      transport->close();
      transport.reset();
    }
    for (auto& outgoing : outgoing_queue) {
      if (outgoing.after) outgoing.after(false);
    }
    outgoing_queue.clear();
    writing = false;
    suback_waiting.clear();
    for (auto& [id, sub] : subscriptions) sub.acknowledged = false;
    logger.info("disconnected from the broker: " + why);
    tell_state({ConnectionState::disconnected, false, broker_reason, std::move(why)});
    phase = Phase::waiting;
    if (redirect) {
      retry_at = time.now();
    } else {
      const auto most = backoff.count();
      std::uniform_int_distribution<int64_t> part(most / 2, std::max<int64_t>(most, 1));
      retry_at = time.now() + std::chrono::milliseconds(part(random));
      backoff = std::min(backoff * 2, config.backoff_most);
    }
    arm();
  }

  // ---- the wire

  void read() {
    const uint64_t read_generation = generation;
    chunk.resize(kReadChunk);
    transport->read_some(chunk, [self = shared_from_this(), read_generation](boost::system::error_code error, size_t n) {
      if (self->generation != read_generation) return;
      if (error) return self->lost("the connection closed: " + error.message(), reason::success);
      self->incoming.insert(self->incoming.end(), self->chunk.begin(), self->chunk.begin() + static_cast<std::ptrdiff_t>(n));
      if (!self->take_packets()) return;
      if (self->generation == read_generation) self->read();
    });
  }

  // Whether the connection is still up after what arrived.
  bool take_packets() {
    const uint64_t take_generation = generation;
    try {
      for (;;) {
        const auto frame = mqtt::read_frame(incoming, kMaximumIncoming);
        if (!frame || incoming.size() < frame->size()) return true;
        auto packet = mqtt::decode(std::span<const uint8_t>(incoming).first(frame->size()), config.version);
        incoming.erase(incoming.begin(), incoming.begin() + static_cast<std::ptrdiff_t>(frame->size()));
        handle(packet);
        if (generation != take_generation) return false;
      }
    } catch (const mqtt::MalformedPacket& e) {
      protocol_failure(e.reason(), std::string("the broker sent a malformed packet: ") + e.what());
      return false;
    }
  }

  void protocol_failure(uint8_t code, std::string why) {
    if (config.version == Version::v5 && transport && phase == Phase::connected) {
      // Said before closing, as a 5.0 client should; not waited for.
      try {
        transport->write(mqtt::encode(mqtt::Disconnect{code, {}}, config.version), [](auto) {});
      } catch (const std::exception&) {
      }
    }
    lost(std::move(why), code);
  }

  void handle(const mqtt::Packet& packet) {
    if (phase == Phase::awaiting_connack) {
      if (const auto* connack = std::get_if<mqtt::Connack>(&packet)) return on_connack(*connack);
      return protocol_failure(reason::protocol_error, "the broker sent something before CONNACK");
    }
    if (const auto* publish = std::get_if<mqtt::Publish>(&packet)) return on_publish(*publish);
    if (const auto* ack = std::get_if<mqtt::Ack>(&packet)) {
      if (ack->type != mqtt::PacketType::puback) return protocol_failure(reason::protocol_error, "the broker sent a QoS 2 acknowledgement");
      return on_puback(*ack);
    }
    if (const auto* suback = std::get_if<mqtt::Suback>(&packet)) return on_suback(*suback);
    if (std::holds_alternative<mqtt::Unsuback>(packet)) return;
    if (std::holds_alternative<mqtt::Pingresp>(packet)) {
      ping_sent.reset();
      return;
    }
    if (const auto* disconnect = std::get_if<mqtt::Disconnect>(&packet)) {
      std::string why = "the broker disconnected (reason " + std::to_string(disconnect->reason) + ")";
      if (disconnect->properties.reason_string) why += ": " + *disconnect->properties.reason_string;
      if ((disconnect->reason == reason::use_another_server || disconnect->reason == reason::server_moved) && disconnect->properties.server_reference) {
        redirect = parse_reference(*disconnect->properties.server_reference);
      }
      return lost(std::move(why), disconnect->reason);
    }
    protocol_failure(reason::protocol_error, "the broker sent a packet a client does not take");
  }

  void on_publish(const mqtt::Publish& publish) {
    if (publish.qos == QoS::exactly_once) return protocol_failure(reason::qos_not_supported, "the broker sent QoS 2, which this client never asks for");
    if (publish.topic.empty()) return protocol_failure(reason::topic_alias_invalid, "the broker sent a topic alias, which this client never allows");
    auto message = std::make_shared<Message>(Message{publish.topic, publish.payload, publish.qos, publish.retain});
    // 5.0 names the subscriptions a message is for; 3.1.1 is matched here. A
    // second subscription to the same filter replaced the first's identifier
    // at the broker, so a named one stands for every subscription to its filter.
    std::set<std::string> named;
    for (const auto identifier : publish.properties.subscription_identifiers) {
      if (const auto it = subscriptions.find(identifier); it != subscriptions.end()) named.insert(it->second.filter);
    }
    for (const auto& [id, sub] : subscriptions) {
      bool wanted = false;
      if (!publish.properties.subscription_identifiers.empty()) {
        wanted = named.contains(sub.filter);
      } else if (mqtt::is_shared(sub.filter)) {
        const auto shared = mqtt::parse_shared(sub.filter);
        wanted = shared && mqtt::matches(shared->filter, publish.topic);
      } else {
        wanted = mqtt::matches(sub.filter, publish.topic);
      }
      if (wanted) tell([on_message = sub.on_message, message] { (*on_message)(*message); });
    }
    if (publish.qos == QoS::at_least_once) write_packet(mqtt::Ack{mqtt::PacketType::puback, publish.packet_id, reason::success, {}}, {});
  }

  void on_puback(const mqtt::Ack& ack) {
    const auto it = inflight.find(ack.packet_id);
    if (it == inflight.end()) return;
    auto done = std::move(it->second.done);
    inflight.erase(it);
    if (ack.reason >= 0x80) {
      std::string why = "the broker refused the publish (reason " + std::to_string(ack.reason) + ")";
      if (ack.properties.reason_string) why += ": " + *ack.properties.reason_string;
      tell_outcome(std::move(done), {false, ack.reason, std::move(why)});
    } else {
      tell_outcome(std::move(done), {true, ack.reason, {}});
    }
    pump();
  }

  void on_suback(const mqtt::Suback& suback) {
    const auto it = suback_waiting.find(suback.packet_id);
    if (it == suback_waiting.end()) return;
    const SubscriptionId id = it->second;
    suback_waiting.erase(it);
    const auto sub = subscriptions.find(id);
    if (sub == subscriptions.end()) return;
    const uint8_t code = suback.reasons.empty() ? reason::unspecified_error : suback.reasons.front();
    auto done = std::move(sub->second.done);
    if (code >= 0x80) {
      // Refused: not asked for again.
      subscriptions.erase(sub);
      if (done)
        tell([done = std::move(done), code] {
          done({false, QoS::at_most_once, code, "the broker refused the subscription (reason " + std::to_string(code) + ")"});
        });
      return;
    }
    sub->second.acknowledged = true;
    if (done) tell([done = std::move(done), code] { done({true, static_cast<QoS>(code), code, {}}); });
  }

  uint16_t next_packet_id() {
    for (;;) {
      if (++last_packet_id == 0) last_packet_id = 1;
      if (!inflight.contains(last_packet_id) && !suback_waiting.contains(last_packet_id)) return last_packet_id;
    }
  }

  void send_subscribe(SubscriptionId id, Subscribed& sub) {
    mqtt::Subscribe subscribe;
    subscribe.packet_id = next_packet_id();
    if (config.version == Version::v5) subscribe.properties.subscription_identifiers.push_back(static_cast<uint32_t>(id));
    subscribe.subscriptions.push_back({sub.filter, sub.qos});
    suback_waiting[subscribe.packet_id] = id;
    write_packet(subscribe, {});
  }

  void send_publish(Pending& pending, bool dup) {
    mqtt::Publish publish;
    publish.dup = dup;
    publish.qos = pending.message.qos;
    publish.retain = pending.message.retain;
    publish.topic = pending.message.topic;
    publish.packet_id = pending.packet_id;
    publish.payload = pending.message.payload;
    write_packet(publish, {});
  }

  // Why the broker cannot take `message` as it said at CONNACK, or nothing.
  std::optional<std::string> refused_by_broker(const Message& message, size_t encoded) const {
    if (message.qos == QoS::at_least_once && maximum_qos == 0) return "the broker takes QoS 0 only";
    if (message.retain && !retain_available) return "the broker keeps no retained messages";
    if (maximum_packet_size != 0 && encoded > maximum_packet_size) return "the publish is larger than the broker takes";
    return std::nullopt;
  }

  // Sends what is queued, as far as the broker's receive maximum and ours allow.
  void pump() {
    const size_t limit = std::max<size_t>(1, std::min<size_t>(config.maximum_inflight, receive_maximum));
    while (phase == Phase::connected && !queued.empty()) {
      if (queued.front().message.qos == QoS::at_least_once && inflight.size() >= limit) return;
      Pending pending = std::move(queued.front());
      queued.pop_front();
      queued_bytes -= pending.message.payload.size();
      if (pending.message.qos == QoS::at_least_once) pending.packet_id = next_packet_id();
      mqtt::Publish publish{false, pending.message.qos, pending.message.retain, pending.message.topic, pending.packet_id, {}, pending.message.payload};
      mqtt::Bytes bytes;
      try {
        bytes = mqtt::encode(publish, config.version);
      } catch (const std::exception& e) {
        tell_outcome(std::move(pending.done), {false, reason::unspecified_error, std::string("the publish cannot be sent: ") + e.what()});
        continue;
      }
      if (auto why = refused_by_broker(pending.message, bytes.size())) {
        tell_outcome(std::move(pending.done), {false, reason::unspecified_error, std::move(*why)});
        continue;
      }
      if (pending.message.qos == QoS::at_least_once) {
        const uint16_t id = pending.packet_id;
        inflight.emplace(id, std::move(pending));
        write_bytes(std::move(bytes), {});
      } else {
        write_bytes(std::move(bytes), [self = shared_from_this(), done = std::move(pending.done)](bool written) mutable {
          self->tell_outcome(std::move(done),
                             written ? Outcome{true, reason::success, {}} : Outcome{false, reason::unspecified_error, "the connection was lost first"});
        });
      }
    }
  }

  void write_packet(const mqtt::Packet& packet, std::function<void(bool)> after) {
    mqtt::Bytes bytes;
    try {
      bytes = mqtt::encode(packet, config.version);
    } catch (const std::exception& e) {
      logger.error(std::string("a packet could not be encoded: ") + e.what());
      if (after) after(false);
      return;
    }
    write_bytes(std::move(bytes), std::move(after));
  }

  void write_bytes(mqtt::Bytes bytes, std::function<void(bool)> after) {
    outgoing_queue.push_back({std::move(bytes), std::move(after)});
    write_next();
  }

  void write_next() {
    if (writing || outgoing_queue.empty() || !transport) return;
    writing = true;
    last_sent = time.now();
    const uint64_t write_generation = generation;
    transport->write(outgoing_queue.front().bytes, [self = shared_from_this(), write_generation](boost::system::error_code error) {
      if (self->generation != write_generation) return;
      self->writing = false;
      if (error) return self->lost("writing failed: " + error.message(), reason::success);
      auto after = std::move(self->outgoing_queue.front().after);
      self->outgoing_queue.pop_front();
      if (after) after(true);
      self->write_next();
    });
  }

  // ---- time

  // One timer, set for the earliest thing due.
  void arm() {
    std::optional<TimePoint> due;
    const auto consider = [&due](TimePoint at) { due = due ? std::min(*due, at) : at; };
    switch (phase) {
      case Phase::connecting:
      case Phase::awaiting_connack:
        consider(connect_deadline);
        break;
      case Phase::connected:
        if (keep_alive.count() > 0) consider(ping_sent ? *ping_sent + keep_alive : last_sent + keep_alive);
        break;
      case Phase::waiting:
        consider(retry_at);
        break;
      default:
        break;
    }
    if (!due) {
      timer.cancel();
      return;
    }
    const auto remaining = std::max(std::chrono::duration_cast<std::chrono::milliseconds>(*due - time.now()), std::chrono::milliseconds(0));
    timer.expires_after(time.wake_within(remaining));
    timer.async_wait([self = shared_from_this()](boost::system::error_code error) {
      if (!error) self->tick();
    });
  }

  void tick() {
    const auto now = time.now();
    switch (phase) {
      case Phase::connecting:
      case Phase::awaiting_connack:
        if (now >= connect_deadline) return lost("the broker did not answer in time", reason::success);
        break;
      case Phase::connected:
        if (keep_alive.count() > 0) {
          if (ping_sent && now >= *ping_sent + keep_alive) return lost("the broker stopped answering", reason::keep_alive_timeout);
          if (!ping_sent && now >= last_sent + keep_alive) {
            ping_sent = now;
            write_packet(mqtt::Pingreq{}, {});
          }
        }
        break;
      case Phase::waiting:
        if (now >= retry_at) return attempt();
        break;
      default:
        return;
    }
    arm();
  }

  // ---- ending

  void fail_everything(const std::string& why) {
    for (auto& pending : queued) tell_outcome(std::move(pending.done), {false, reason::unspecified_error, why});
    queued.clear();
    queued_bytes = 0;
    for (auto& [id, pending] : inflight) tell_outcome(std::move(pending.done), {false, reason::unspecified_error, why});
    inflight.clear();
    for (auto& [id, sub] : subscriptions) {
      if (sub.done) tell([done = std::move(sub.done), why] { done({false, QoS::at_most_once, reason::unspecified_error, why}); });
    }
  }

  void stop(std::function<void()> done) {
    if (phase == Phase::stopped) {
      if (done) tell(std::move(done));
      return;
    }
    const bool was_connected = phase == Phase::connected && transport;
    phase = Phase::stopped;
    ++generation;
    timer.cancel();
    fail_everything("stopped");
    for (auto& outgoing : outgoing_queue) {
      if (outgoing.after) outgoing.after(false);
    }
    outgoing_queue.clear();
    writing = false;
    auto ending = std::move(transport);
    if (!was_connected) {
      if (ending) ending->close();
      if (done) tell(std::move(done));
      return;
    }
    // DISCONNECT, normal: the will is not published. Then close.
    mqtt::Bytes bytes;
    try {
      bytes = mqtt::encode(mqtt::Disconnect{}, config.version);
    } catch (const std::exception&) {
    }
    ending->write(std::move(bytes), [self = shared_from_this(), ending, done = std::move(done)](auto) mutable {
      ending->close();
      if (done) self->tell(std::move(done));
    });
  }

  asio::strand<asio::any_io_executor> strand;
  ClientConfig config;
  const dannan::contract::TimeSource& time;
  dannan::status::ScopedLogger logger;
  asio::steady_timer timer;
  std::mt19937_64 random;

  Phase phase = Phase::idle;
  // Moves on with each attempt and each loss, so a handler of an older
  // connection does nothing.
  uint64_t generation = 0;
  std::shared_ptr<Transport> transport;
  std::vector<uint8_t> chunk;
  mqtt::Bytes incoming;
  std::deque<Outgoing> outgoing_queue;
  bool writing = false;

  std::deque<Pending> queued;
  size_t queued_bytes = 0;
  std::map<uint16_t, Pending> inflight;
  uint16_t last_packet_id = 0;

  std::atomic<SubscriptionId> next_subscription{1};
  std::map<SubscriptionId, Subscribed> subscriptions;
  std::map<uint16_t, SubscriptionId> suback_waiting;

  std::function<void(const StateChange&)> on_state;

  bool ever_connected = false;
  std::optional<std::pair<std::string, uint16_t>> redirect;
  std::chrono::milliseconds backoff{0};
  TimePoint connect_deadline{};
  TimePoint retry_at{};
  TimePoint last_sent{};
  std::optional<TimePoint> ping_sent;

  // What the broker said at CONNACK.
  uint32_t receive_maximum = UINT16_MAX;
  uint32_t maximum_packet_size = 0;
  uint8_t maximum_qos = 1;
  bool retain_available = true;
  std::chrono::seconds keep_alive{0};
};

Client::Client(asio::any_io_executor executor, ClientConfig config, const dannan::contract::TimeSource& time, dannan::status::ScopedLogger logger)
    : _state(std::make_shared<State>(std::move(executor), std::move(config), time, std::move(logger))) {}

Client::~Client() {
  asio::post(_state->strand, [state = _state] { state->stop({}); });
}

void Client::start() {
  asio::post(_state->strand, [state = _state] {
    if (state->phase == State::Phase::idle) state->attempt();
  });
}

void Client::stop(std::function<void()> done) {
  asio::post(_state->strand, [state = _state, done = std::move(done)]() mutable { state->stop(std::move(done)); });
}

void Client::publish(Message message, std::function<void(Outcome)> done) {
  asio::post(_state->strand, [state = _state, message = std::move(message), done = std::move(done)]() mutable {
    const auto refuse = [&](std::string why) { state->tell_outcome(std::move(done), {false, reason::unspecified_error, std::move(why)}); };
    if (state->phase == State::Phase::stopped) return refuse("stopped");
    if (message.qos == QoS::exactly_once) return refuse("QoS 2 is not supported by this client");
    if (!mqtt::valid_utf8(message.topic) || !mqtt::valid_topic_name(message.topic)) return refuse("not a valid topic name: " + message.topic);
    if (state->queued_bytes + message.payload.size() > state->config.maximum_queued_bytes) return refuse("too much is queued already");
    state->queued_bytes += message.payload.size();
    state->queued.push_back({std::move(message), std::move(done), 0});
    state->pump();
  });
}

SubscriptionId Client::subscribe(std::string filter, QoS qos, std::function<void(const Message&)> on_message, std::function<void(SubscribeOutcome)> done) {
  const SubscriptionId id = _state->next_subscription++;
  asio::post(_state->strand, [state = _state, id, filter = std::move(filter), qos, on_message = std::move(on_message), done = std::move(done)]() mutable {
    const auto valid = mqtt::valid_utf8(filter) && (mqtt::is_shared(filter) ? mqtt::parse_shared(filter).has_value() : mqtt::valid_topic_filter(filter));
    if (state->phase == State::Phase::stopped || !valid || qos == QoS::exactly_once || !on_message) {
      const std::string why = state->phase == State::Phase::stopped ? "stopped"
                              : !valid                              ? "not a valid topic filter: " + filter
                              : !on_message                         ? "no handler"
                                                                    : "QoS 2 is not supported by this client";
      if (done) state->tell([done = std::move(done), why] { done({false, QoS::at_most_once, reason::unspecified_error, why}); });
      return;
    }
    auto& sub = state->subscriptions[id];
    sub.filter = std::move(filter);
    sub.qos = qos;
    sub.on_message = std::make_shared<std::function<void(const Message&)>>(std::move(on_message));
    sub.done = std::move(done);
    if (state->phase == State::Phase::connected) state->send_subscribe(id, sub);
  });
  return id;
}

void Client::unsubscribe(SubscriptionId id) {
  asio::post(_state->strand, [state = _state, id] {
    const auto it = state->subscriptions.find(id);
    if (it == state->subscriptions.end()) return;
    const std::string filter = it->second.filter;
    state->subscriptions.erase(it);
    for (auto waiting = state->suback_waiting.begin(); waiting != state->suback_waiting.end();) {
      waiting = waiting->second == id ? state->suback_waiting.erase(waiting) : std::next(waiting);
    }
    // The broker stops sending only when no other subscription shares the filter.
    const bool shared = std::any_of(state->subscriptions.begin(), state->subscriptions.end(), [&](const auto& entry) { return entry.second.filter == filter; });
    if (!shared && state->phase == State::Phase::connected) {
      mqtt::Unsubscribe unsubscribe;
      unsubscribe.packet_id = state->next_packet_id();
      unsubscribe.filters.push_back(filter);
      state->write_packet(unsubscribe, {});
    }
  });
}

void Client::on_state(std::function<void(const StateChange&)> handler) {
  asio::post(_state->strand, [state = _state, handler = std::move(handler)]() mutable { state->on_state = std::move(handler); });
}

}  // namespace lir::client
