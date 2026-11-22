//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "lir/client/client.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <charconv>
#include <deque>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

#include "dannan/listen/certificates.h"
#include "dannan/listen/connector.h"
#include "dannan/listen/stream.h"
#include "dannan/sched/retry.h"
#include "lir/client/roster.h"
#include "lir/contract/endpoints.h"
#include "lir/mqtt/topic.h"

namespace lir::client {

namespace {

using dannan::contract::TimePoint;
using mqtt::QoS;
namespace reason = mqtt::reason;

constexpr uint32_t protocol_maximum_packet_size = 5 + mqtt::varint_max;

bool is5(mqtt::Version version) noexcept { return version == mqtt::Version::v5; }

std::chrono::milliseconds until(TimePoint due, TimePoint now) {
  return std::max(std::chrono::ceil<std::chrono::milliseconds>(due - now), std::chrono::milliseconds(0));
}

// The executor's io context, as the connector needs it: an io context's
// executor, or a strand of one.
boost::asio::strand<boost::asio::io_context::executor_type> strand_of(const boost::asio::any_io_executor& executor) {
  using IoExecutor = boost::asio::io_context::executor_type;
  if (const auto* io = executor.target<IoExecutor>()) return boost::asio::make_strand(*io);
  if (const auto* strand = executor.target<boost::asio::strand<IoExecutor>>()) return boost::asio::make_strand(strand->get_inner_executor());
  throw std::invalid_argument("lir::client::Client: the executor is not an io context's or a strand of one");
}

}  // namespace

std::optional<Address> parse_address(std::string_view address) {
  const size_t colon = address.rfind(':');
  if (colon == std::string_view::npos || colon == 0) return std::nullopt;
  auto host = address.substr(0, colon);
  const auto digits = address.substr(colon + 1);
  uint16_t port = 0;
  const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), port);
  if (error != std::errc{} || end != digits.data() + digits.size() || port == 0) return std::nullopt;
  if (host.size() > 2 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
  return Address{std::string(host), port};
}

std::vector<Address> lir_addresses(const std::vector<dannan::membership::NodeRecord>& records, std::string_view transport) {
  std::vector<Address> out;
  for (const auto& record : records) {
    if (!record.adverts.contains(std::string(contract::lir_advert))) continue;
    const auto found = record.adverts.find(std::string(contract::endpoints_advert_key));
    if (found == record.adverts.end()) continue;
    const auto endpoints = contract::parse_endpoints(std::string_view(reinterpret_cast<const char*>(found->second.data()), found->second.size()));
    const auto served = endpoints.find(std::string(transport));
    if (served == endpoints.end()) continue;
    if (auto address = parse_address(served->second)) out.push_back(std::move(*address));
  }
  return out;
}

// Everything below runs on `strand`; only the public calls cross onto it.
struct Client::State : std::enable_shared_from_this<State> {
  struct Pending {
    Message message;
    std::function<void(Outcome)> done;
    uint16_t packet_id = 0;
  };
  struct Subscription {
    std::string filter;
    // What it matches: the filter, or a shared one's own filter.
    std::string matches;
    QoS qos = QoS::at_most_once;
    std::function<void(const Message&)> on_message;
    // Until the first SUBACK.
    std::function<void(SubscribeOutcome)> done;
  };

  State(boost::asio::any_io_executor executor, ClientConfig configured, const dannan::contract::TimeSource& source, dannan::status::ScopedLogger log)
      : strand(strand_of(executor)),
        config(std::move(configured)),
        time(source),
        logger(std::move(log)),
        retry_timer(strand),
        connack_timer(strand),
        keep_alive_timer(strand),
        policy(dannan::sched::RetryPolicy::unbounded(config.backoff_first, config.backoff_most)),
        jitter(std::random_device{}()),
        client_id(config.client_id) {
    if (config.tls) {
      trust = std::make_shared<dannan::listen::ClientTrust>(config.tls->trust);
      if (!config.tls->certificate_chain.empty() && !config.tls->private_key.empty()) {
        certificate =
            std::make_shared<dannan::listen::Certificates>(dannan::listen::CertificatePaths{config.tls->certificate_chain, config.tls->private_key, {}});
      }
    }
    dannan::listen::ConnectorConfig connector_config;
    connector_config.name = "lir.client";
    // The stream shares the client's strand with its timers and callbacks.
    connector_config.strand_per_stream = false;
    connector = std::make_unique<dannan::listen::StreamConnector>(boost::asio::any_io_executor(strand), connector_config, &time);
  }

  template <class Fn>
  void post(Fn fn) {
    boost::asio::post(strand, [self = shared_from_this(), fn = std::move(fn)]() mutable { fn(*self); });
  }

  // Runs `fn` once the injected time reaches `due`, looking again as the
  // source asks; re-arming `timer` drops what it waited for before.
  void wait(boost::asio::steady_timer& timer, TimePoint due, std::function<void(State&)> fn) {
    timer.expires_after(time.wake_within(until(due, time.now())));
    timer.async_wait([self = shared_from_this(), &timer, due, fn = std::move(fn)](const boost::system::error_code& error) mutable {
      if (error) return;
      if (self->time.now() < due) return self->wait(timer, due, std::move(fn));
      fn(*self);
    });
  }

  void emit(const StateChange& change) {
    for (const auto& handler : state_handlers) handler(change);
  }

  // Connecting

  std::vector<Address> addresses() const {
    std::vector<Address> out;
    if (config.nodes) out = lir_addresses(config.nodes->active(), config.tls ? "tls" : "tcp");
    if (out.empty() && !config.host.empty()) out.push_back({config.host, config.port});
    return out;
  }

  void attempt() {
    if (stopping || stream) return;
    const auto list = addresses();
    ++generation;
    if (list.empty()) {
      round_size = 1;
      return failed("no Lir node in the roster and no host configured", reason::success);
    }
    round_size = list.size();
    const auto& address = list[next_address++ % list.size()];
    attempt_started = time.now();
    std::optional<dannan::listen::ClientTls> tls;
    if (config.tls) {
      tls.emplace();
      tls->trust = trust;
      tls->server_name = config.tls->server_name;
      tls->certificate = certificate;
    }
    logger.debug("connecting to " + address.host + ":" + std::to_string(address.port));
    connector->connect(strand, address.host, address.port, std::move(tls), config.connect_timeout,
                       [self = shared_from_this(), tried = generation](std::shared_ptr<dannan::listen::Stream> made, std::string why) {
                         self->on_stream(tried, std::move(made), std::move(why));
                       });
  }

  void on_stream(uint64_t tried, std::shared_ptr<dannan::listen::Stream> made, std::string why) {
    if (tried != generation || stopping) {
      if (made) made->close();
      return;
    }
    if (!made) return failed(why, reason::success);
    stream = std::move(made);
    input.clear();
    broker_maximum_packet_size = protocol_maximum_packet_size;
    mqtt::Connect connect;
    connect.version = config.version;
    connect.client_id = client_id;
    // A reconnect resumes the session the broker was asked to keep.
    connect.clean_start = first_connect ? config.clean_start : (is5(config.version) ? config.session_expiry == 0 : config.clean_start);
    connect.keep_alive = static_cast<uint16_t>(std::min<int64_t>(config.keep_alive.count(), 65535));
    connect.username = config.username;
    if (config.password) {
      if (auto password = config.password()) connect.password = mqtt::Bytes(password->begin(), password->end());
    }
    if (is5(config.version) && config.session_expiry != 0) connect.properties.session_expiry_interval = config.session_expiry;
    if (config.will) connect.will = mqtt::Will{config.will->topic, config.will->payload, config.will->qos, config.will->retain, {}};
    if (!send(connect)) return failed("the CONNECT could not be encoded", reason::success);
    wait(connack_timer, attempt_started + config.connect_timeout, [tried](State& self) {
      if (tried == self.generation && !self.connected) self.failed("no CONNACK in time", reason::success);
    });
    read();
  }

  void read() {
    stream->async_read_some(boost::asio::buffer(buffer), [self = shared_from_this(), tried = generation](const boost::system::error_code& error, size_t size) {
      self->on_read(tried, error, size);
    });
  }

  void on_read(uint64_t tried, const boost::system::error_code& error, size_t size) {
    if (tried != generation || !stream) return;
    if (error) return lost("connection: " + error.message(), reason::success);
    input.insert(input.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(size));
    size_t used = 0;
    while (tried == generation) {
      const std::span<const uint8_t> rest(input.data() + used, input.size() - used);
      mqtt::Packet packet;
      try {
        const auto frame = mqtt::read_frame(rest, protocol_maximum_packet_size);
        if (!frame || rest.size() < frame->size()) break;
        packet = mqtt::decode(rest.first(frame->size()), config.version);
        used += frame->size();
      } catch (const mqtt::MalformedPacket& malformed) {
        return violated(malformed.reason(), std::string("the broker sent a malformed packet: ") + malformed.what());
      }
      handle(packet);
    }
    if (tried != generation) return;
    input.erase(input.begin(), input.begin() + static_cast<std::ptrdiff_t>(used));
    read();
  }

  void handle(const mqtt::Packet& packet) {
    if (const auto* connack = std::get_if<mqtt::Connack>(&packet)) {
      if (connected) return violated(reason::protocol_error, "a second CONNACK");
      connack_timer.cancel();
      if (connack->reason != reason::success) return failed("the broker refused the connection", connack->reason);
      return on_connected(*connack);
    }
    if (!connected) return violated(reason::protocol_error, "a packet before CONNACK");
    if (const auto* incoming = std::get_if<mqtt::Publish>(&packet)) return on_publish(*incoming);
    if (const auto* ack = std::get_if<mqtt::Ack>(&packet)) {
      if (ack->type == mqtt::PacketType::puback) return on_puback(*ack);
      return violated(reason::protocol_error, "a QoS 2 acknowledgement this client never asked for");
    }
    if (const auto* suback = std::get_if<mqtt::Suback>(&packet)) return on_suback(*suback);
    if (const auto* unsuback = std::get_if<mqtt::Unsuback>(&packet)) {
      unsubacks.erase(unsuback->packet_id);
      return;
    }
    if (std::holds_alternative<mqtt::Pingresp>(packet)) {
      ping_outstanding = false;
      return;
    }
    if (const auto* disconnect = std::get_if<mqtt::Disconnect>(&packet)) return lost("the broker disconnected", disconnect->reason);
    violated(reason::protocol_error, "a packet a broker does not send");
  }

  void on_connected(const mqtt::Connack& connack) {
    connected = true;
    first_connect = false;
    retry.succeeded();
    round_failures = 0;
    const auto& p = connack.properties;
    maximum_qos = p.maximum_qos.value_or(2);
    retain_available = p.retain_available.value_or(1) != 0;
    broker_maximum_packet_size = p.maximum_packet_size.value_or(protocol_maximum_packet_size);
    receive_maximum = p.receive_maximum.value_or(65535);
    if (p.assigned_client_identifier) client_id = *p.assigned_client_identifier;
    keep_alive = p.server_keep_alive ? std::chrono::seconds(*p.server_keep_alive) : config.keep_alive;
    ping_outstanding = false;
    logger.debug(std::string("connected") + (connack.session_present ? ", session resumed" : ""));
    emit({ConnectionState::connected, connack.session_present, reason::success, {}});
    if (!connected) return;
    // Without the session the broker holds no subscription: each is made
    // again. With it, only those never answered.
    for (auto& [id, subscription] : subscriptions) {
      if (!connack.session_present || subscription.done) send_subscribe(id);
    }
    // QoS 1 publishes not yet acknowledged: again with their ids and DUP
    // when the session was kept, otherwise as new, ahead of the queue.
    auto unacknowledged = std::move(inflight);
    inflight.clear();
    if (connack.session_present) {
      for (auto& pending : unacknowledged) send_publish(std::move(pending), true);
    } else {
      for (auto it = unacknowledged.rbegin(); it != unacknowledged.rend(); ++it) {
        it->packet_id = 0;
        queued_bytes += it->message.payload.size();
        queue.push_front(std::move(*it));
      }
    }
    arm_keep_alive();
    flush();
  }

  // A protocol error by the broker: told, where 5.0 lets it be, and dropped.
  void violated(uint8_t code, const std::string& why) {
    if (stream && connected && is5(config.version)) send(mqtt::Disconnect{code, {}});
    if (connected) return lost(why, code);
    failed(why, code);
  }

  // The connection ended after its CONNACK.
  void lost(const std::string& why, uint8_t code) {
    if (!connected) return failed(why, code);
    drop_stream();
    logger.debug("disconnected: " + why);
    emit({ConnectionState::disconnected, false, code, why});
    if (stopping) return;
    // The next node is tried at once; the backoff starts once each has failed.
    post([](State& self) { self.attempt(); });
  }

  // An attempt that ended before its CONNACK.
  void failed(const std::string& why, uint8_t code) {
    drop_stream();
    logger.debug("connection attempt failed: " + why);
    emit({ConnectionState::disconnected, false, code, why});
    if (stopping) return;
    if (++round_failures < round_size) return post([](State& self) { self.attempt(); });
    round_failures = 0;
    auto delay = retry.failed(policy, time.now()).value_or(policy.max_backoff);
    // A random part of it, between half and all.
    const auto half = delay.count() / 2;
    delay = std::chrono::milliseconds(half + std::uniform_int_distribution<int64_t>(0, delay.count() - half)(jitter));
    wait(retry_timer, time.now() + delay, [](State& self) { self.attempt(); });
  }

  void drop_stream() {
    connected = false;
    ping_outstanding = false;
    ++generation;
    connack_timer.cancel();
    keep_alive_timer.cancel();
    subacks.clear();
    unsubacks.clear();
    if (stream) {
      stream->close();
      stream.reset();
    }
  }

  // Keep-alive: a PINGREQ once a keep-alive passes with nothing sent, and
  // the connection dropped when its PINGRESP has not come a keep-alive later.
  void arm_keep_alive() {
    if (keep_alive.count() == 0 || !connected) return;
    const auto due = ping_outstanding ? ping_sent + keep_alive : last_sent + keep_alive;
    wait(keep_alive_timer, due, [tried = generation](State& self) {
      if (tried != self.generation || !self.connected) return;
      const auto now = self.time.now();
      if (self.ping_outstanding) {
        if (now >= self.ping_sent + self.keep_alive) return self.lost("no PINGRESP within the keep-alive", reason::keep_alive_timeout);
      } else if (now >= self.last_sent + self.keep_alive) {
        self.send(mqtt::Pingreq{});
        self.ping_outstanding = true;
        self.ping_sent = now;
      }
      self.arm_keep_alive();
    });
  }

  // Sending

  // Whether it went: false for a packet MQTT cannot carry or over the
  // broker's maximum size.
  bool send(const mqtt::Packet& packet) {
    if (!stream) return false;
    mqtt::Bytes bytes;
    try {
      bytes = mqtt::encode(packet, config.version);
    } catch (const std::exception&) {
      return false;
    }
    if (bytes.size() > broker_maximum_packet_size) return false;
    stream->send(std::string(bytes.begin(), bytes.end()));
    last_sent = time.now();
    return true;
  }

  static void finish(std::function<void(Outcome)>& done, bool ok, uint8_t code, std::string why) {
    if (done) done(Outcome{ok, code, std::move(why)});
    done = nullptr;
  }

  size_t inflight_bound() const { return std::max<size_t>(1, std::min<size_t>(config.maximum_inflight, receive_maximum)); }

  uint16_t take_packet_id() {
    for (;;) {
      const uint16_t id = next_packet_id++;
      if (next_packet_id == 0) next_packet_id = 1;
      const bool used = subacks.contains(id) || unsubacks.contains(id) ||
                        std::any_of(inflight.begin(), inflight.end(), [id](const Pending& pending) { return pending.packet_id == id; });
      if (!used) return id;
    }
  }

  void send_publish(Pending pending, bool again) {
    const auto& message = pending.message;
    // [MQTT-3.2.2-11], [MQTT-3.2.2-14]: what the broker said it takes.
    if (static_cast<uint8_t>(message.qos) > maximum_qos)
      return finish(pending.done, false, reason::qos_not_supported, "the broker takes QoS " + std::to_string(maximum_qos) + " at most");
    if (message.retain && !retain_available) return finish(pending.done, false, reason::retain_not_supported, "the broker keeps no retained messages");
    mqtt::Publish packet;
    packet.qos = message.qos;
    packet.retain = message.retain;
    packet.topic = message.topic;
    packet.payload = message.payload;
    if (message.qos == QoS::at_least_once) {
      if (!again) pending.packet_id = take_packet_id();
      packet.packet_id = pending.packet_id;
      packet.dup = again;
    }
    if (!send(packet)) return finish(pending.done, false, reason::packet_too_large, "too large for the broker, or not a packet MQTT can carry");
    if (message.qos == QoS::at_most_once) return finish(pending.done, true, reason::success, {});
    inflight.push_back(std::move(pending));
  }

  void flush() {
    while (connected && !queue.empty()) {
      if (queue.front().message.qos == QoS::at_least_once && inflight.size() >= inflight_bound()) return;
      auto pending = std::move(queue.front());
      queue.pop_front();
      queued_bytes -= pending.message.payload.size();
      send_publish(std::move(pending), false);
    }
  }

  void publish(Message message, std::function<void(Outcome)> done) {
    Pending pending{std::move(message), std::move(done), 0};
    if (stopping) return finish(pending.done, false, reason::unspecified_error, "stopped");
    if (!mqtt::valid_topic_name(pending.message.topic)) return finish(pending.done, false, reason::topic_name_invalid, "not a topic name");
    if (pending.message.qos == QoS::exactly_once) return finish(pending.done, false, reason::qos_not_supported, "QoS 2 is not supported by this client");
    if (connected && queue.empty() && (pending.message.qos == QoS::at_most_once || inflight.size() < inflight_bound()))
      return send_publish(std::move(pending), false);
    if (queued_bytes + pending.message.payload.size() > config.maximum_queued_bytes)
      return finish(pending.done, false, reason::quota_exceeded, "the queue of unsent publishes is full");
    queued_bytes += pending.message.payload.size();
    queue.push_back(std::move(pending));
  }

  void on_puback(const mqtt::Ack& ack) {
    auto it = std::find_if(inflight.begin(), inflight.end(), [&](const Pending& pending) { return pending.packet_id == ack.packet_id; });
    if (it == inflight.end()) return;
    auto pending = std::move(*it);
    inflight.erase(it);
    if (ack.reason >= 0x80) {
      finish(pending.done, false, ack.reason, "the broker refused the publish");
    } else {
      finish(pending.done, true, ack.reason, {});
    }
    flush();
  }

  // Subscriptions

  void subscribe(SubscriptionId id, std::string filter, QoS qos, std::function<void(const Message&)> on_message, std::function<void(SubscribeOutcome)> done) {
    auto refuse = [&](uint8_t code, std::string why) {
      if (done) done(SubscribeOutcome{false, QoS::at_most_once, code, std::move(why)});
    };
    if (stopping) return refuse(reason::unspecified_error, "stopped");
    if (qos == QoS::exactly_once) return refuse(reason::qos_not_supported, "QoS 2 is not supported by this client");
    std::string matches = filter;
    if (mqtt::is_shared(filter)) {
      const auto shared = mqtt::parse_shared(filter);
      if (!shared) return refuse(reason::topic_filter_invalid, "not a shared subscription filter");
      matches = shared->filter;
    } else if (!mqtt::valid_topic_filter(filter)) {
      return refuse(reason::topic_filter_invalid, "not a topic filter");
    }
    subscriptions[id] = Subscription{std::move(filter), std::move(matches), qos, std::move(on_message), std::move(done)};
    if (connected) send_subscribe(id);
  }

  void send_subscribe(SubscriptionId id) {
    const auto& subscription = subscriptions.at(id);
    const uint16_t packet_id = take_packet_id();
    mqtt::Subscribe packet;
    packet.packet_id = packet_id;
    packet.subscriptions.push_back({subscription.filter, subscription.qos});
    subacks[packet_id] = id;
    send(packet);
  }

  void on_suback(const mqtt::Suback& suback) {
    const auto found = subacks.find(suback.packet_id);
    if (found == subacks.end()) return;
    const auto id = found->second;
    subacks.erase(found);
    auto it = subscriptions.find(id);
    if (it == subscriptions.end()) return;
    const uint8_t code = suback.reasons.empty() ? 0x80 : suback.reasons.front();
    auto done = std::move(it->second.done);
    it->second.done = nullptr;
    if (code >= 0x80) {
      // Refused: not asked again.
      subscriptions.erase(it);
      if (done) done(SubscribeOutcome{false, QoS::at_most_once, code, "the broker refused the subscription"});
      return;
    }
    if (done) done(SubscribeOutcome{true, static_cast<QoS>(std::min<uint8_t>(code, 2)), code, {}});
  }

  void unsubscribe(SubscriptionId id) {
    auto it = subscriptions.find(id);
    if (it == subscriptions.end()) return;
    auto subscription = std::move(it->second);
    subscriptions.erase(it);
    if (subscription.done) subscription.done(SubscribeOutcome{false, QoS::at_most_once, reason::unspecified_error, "unsubscribed before the broker answered"});
    const bool shared_filter =
        std::any_of(subscriptions.begin(), subscriptions.end(), [&](const auto& entry) { return entry.second.filter == subscription.filter; });
    if (!connected || shared_filter) return;
    const uint16_t packet_id = take_packet_id();
    unsubacks.insert(packet_id);
    send(mqtt::Unsubscribe{packet_id, {}, {subscription.filter}});
  }

  void on_publish(const mqtt::Publish& incoming) {
    if (incoming.qos == QoS::exactly_once) return violated(reason::qos_not_supported, "a QoS 2 PUBLISH, which no subscription asked for");
    // [MQTT-4.3.2-4]: acknowledged once taken.
    if (incoming.qos == QoS::at_least_once) send(mqtt::Ack{mqtt::PacketType::puback, incoming.packet_id, reason::success, {}});
    const Message message{incoming.topic, incoming.payload, incoming.qos, incoming.retain};
    // A handler may unsubscribe: the ids are taken first.
    std::vector<SubscriptionId> matching;
    for (const auto& [id, subscription] : subscriptions) {
      if (mqtt::matches(subscription.matches, incoming.topic)) matching.push_back(id);
    }
    for (const auto id : matching) {
      const auto it = subscriptions.find(id);
      if (it != subscriptions.end() && it->second.on_message) it->second.on_message(message);
    }
  }

  // Stopping

  void stop(std::function<void()> done) {
    if (!stopping) {
      stopping = true;
      retry_timer.cancel();
      const bool was = connected;
      // A normal DISCONNECT: the broker discards the will.
      if (connected) send(mqtt::Disconnect{});
      drop_stream();
      for (auto& pending : inflight) finish(pending.done, false, reason::unspecified_error, "stopped");
      for (auto& pending : queue) finish(pending.done, false, reason::unspecified_error, "stopped");
      inflight.clear();
      queue.clear();
      queued_bytes = 0;
      for (auto& [id, subscription] : subscriptions) {
        if (subscription.done) subscription.done(SubscribeOutcome{false, QoS::at_most_once, reason::unspecified_error, "stopped"});
        subscription.done = nullptr;
      }
      if (was) emit({ConnectionState::disconnected, false, reason::success, "stopped"});
    }
    connector->close_all(strand, [done = std::move(done)] {
      if (done) done();
    });
  }

  boost::asio::strand<boost::asio::io_context::executor_type> strand;
  const ClientConfig config;
  const dannan::contract::TimeSource& time;
  dannan::status::ScopedLogger logger;
  std::shared_ptr<dannan::listen::ClientTrust> trust;
  std::shared_ptr<dannan::listen::Certificates> certificate;
  std::unique_ptr<dannan::listen::StreamConnector> connector;
  boost::asio::steady_timer retry_timer;
  boost::asio::steady_timer connack_timer;
  boost::asio::steady_timer keep_alive_timer;
  const dannan::sched::RetryPolicy policy;
  dannan::sched::RetryState retry;
  std::minstd_rand jitter;
  std::atomic<SubscriptionId> next_subscription{1};

  // The strand's.
  bool started = false;
  bool stopping = false;
  bool connected = false;
  bool first_connect = true;
  // Moves on with every attempt and every connection lost, so what an old
  // one left running finds itself stale.
  uint64_t generation = 0;
  size_t next_address = 0;
  size_t round_size = 1;
  size_t round_failures = 0;
  TimePoint attempt_started{};
  std::shared_ptr<dannan::listen::Stream> stream;
  std::array<uint8_t, 16384> buffer{};
  mqtt::Bytes input;
  std::string client_id;
  uint8_t maximum_qos = 2;
  bool retain_available = true;
  uint32_t broker_maximum_packet_size = protocol_maximum_packet_size;
  uint16_t receive_maximum = 65535;
  std::chrono::milliseconds keep_alive{0};
  TimePoint last_sent{};
  bool ping_outstanding = false;
  TimePoint ping_sent{};
  std::deque<Pending> queue;
  size_t queued_bytes = 0;
  // In the order sent.
  std::vector<Pending> inflight;
  std::map<SubscriptionId, Subscription> subscriptions;
  std::map<uint16_t, SubscriptionId> subacks;
  std::set<uint16_t> unsubacks;
  uint16_t next_packet_id = 1;
  std::vector<std::function<void(const StateChange&)>> state_handlers;
};

Client::Client(boost::asio::any_io_executor executor, ClientConfig config, const dannan::contract::TimeSource& time, dannan::status::ScopedLogger logger)
    : _state(std::make_shared<State>(std::move(executor), std::move(config), time, std::move(logger))) {}

Client::~Client() {
  _state->post([](State& state) { state.stop({}); });
}

void Client::start() {
  _state->post([](State& state) {
    if (state.started || state.stopping) return;
    state.started = true;
    state.attempt();
  });
}

void Client::stop(std::function<void()> done) {
  _state->post([done = std::move(done)](State& state) mutable { state.stop(std::move(done)); });
}

void Client::publish(Message message, std::function<void(Outcome)> done) {
  _state->post([message = std::move(message), done = std::move(done)](State& state) mutable { state.publish(std::move(message), std::move(done)); });
}

SubscriptionId Client::subscribe(std::string filter, mqtt::QoS qos, std::function<void(const Message&)> on_message,
                                 std::function<void(SubscribeOutcome)> done) {
  const auto id = _state->next_subscription.fetch_add(1);
  _state->post([id, filter = std::move(filter), qos, on_message = std::move(on_message), done = std::move(done)](State& state) mutable {
    state.subscribe(id, std::move(filter), qos, std::move(on_message), std::move(done));
  });
  return id;
}

void Client::unsubscribe(SubscriptionId id) {
  _state->post([id](State& state) { state.unsubscribe(id); });
}

void Client::on_state(std::function<void(const StateChange&)> handler) {
  _state->post([handler = std::move(handler)](State& state) mutable { state.state_handlers.push_back(std::move(handler)); });
}

}  // namespace lir::client
