//
// Lir - The Distributed Event System
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "lir/broker/broker.h"

#include <algorithm>

#include "dannan/identity/crypto.h"
#include "lir/mqtt/topic.h"

namespace lir::broker {

namespace {

using mqtt::PacketType;
using mqtt::QoS;
namespace reason = mqtt::reason;

constexpr uint32_t never = 0xFFFFFFFF;

QoS lesser(QoS a, QoS b) { return std::min(a, b); }

// The 3.x CONNACK return code nearest a 5.0 refusal; nothing where 3.x has
// none, and the connection is then closed without a CONNACK.
std::optional<uint8_t> v3_return_code(uint8_t code) {
  switch (code) {
    case reason::unsupported_protocol_version:
      return 0x01;
    case reason::client_identifier_not_valid:
      return 0x02;
    case reason::server_unavailable:
    case reason::server_busy:
      return 0x03;
    case reason::bad_user_name_or_password:
      return 0x04;
    case reason::not_authorized:
    case reason::bad_authentication_method:
    case reason::banned:
      return 0x05;
    default:
      return std::nullopt;
  }
}

std::string_view as_text(const Bytes& bytes) { return {reinterpret_cast<const char*>(bytes.data()), bytes.size()}; }

}  // namespace

// ---------------------------------------------------------------- Link

Link::Link(Broker& broker, std::shared_ptr<contract::Connection> connection)
    : _broker(broker), _connection(std::move(connection)), _opened(broker._time.now()), _last_received(_opened) {}

void Link::send(const mqtt::Packet& packet) { _connection->send(mqtt::encode(packet, _version)); }

void Link::receive(std::span<const uint8_t> bytes) {
  if (_state == State::closed) return;
  _buffer.insert(_buffer.end(), bytes.begin(), bytes.end());
  if (_state != State::taking) process();
}

void Link::process() {
  const auto self = shared_from_this();
  size_t offset = 0;
  try {
    while (_state != State::closed && _state != State::taking) {
      const auto rest = std::span<const uint8_t>(_buffer).subspan(offset);
      const auto frame = mqtt::read_frame(rest, _broker._config.maximum_packet_size);
      if (!frame || rest.size() < frame->size()) break;
      // The first packet must be a CONNECT; anything else is closed on.
      if (_state == State::awaiting_connect && (frame->first_byte >> 4) != static_cast<uint8_t>(PacketType::connect)) {
        ++_broker._stats.protocol_errors;
        shutdown(std::nullopt, false);
        break;
      }
      auto packet = mqtt::decode(rest.first(frame->size()), _version);
      offset += frame->size();
      _last_received = _broker._time.now();
      handle(std::move(packet));
    }
  } catch (const mqtt::UnsupportedVersion&) {
    // A 3.1.1-form CONNACK with return code 1: what every client can read.
    ++_broker._stats.connections_refused;
    _connection->send(Bytes{0x20, 0x02, 0x00, 0x01});
    _state = State::closed;
    _connection->close();
    _broker.detach(*this, false);
  } catch (const mqtt::MalformedPacket& e) {
    ++_broker._stats.protocol_errors;
    _broker._logger.debug("malformed packet from " + _connection->peer() + ": " + e.what());
    shutdown(e.reason(), true);
  } catch (const std::exception& e) {
    _broker._logger.warn("closing " + _connection->peer() + ": " + e.what());
    shutdown(reason::implementation_specific_error, true);
  }
  if (_state != State::closed) _buffer.erase(_buffer.begin(), _buffer.begin() + static_cast<std::ptrdiff_t>(offset));
}

void Link::closed() { shutdown(std::nullopt, true); }

void Link::shutdown(std::optional<uint8_t> code, bool publish_will) {
  if (_state == State::closed) return;
  const bool connected = _state == State::connected;
  if (code && connected && v5()) send(mqtt::Disconnect{*code, {}});
  _state = State::closed;
  _connection->close();
  _broker.detach(*this, publish_will && connected);
}

void Link::handle(mqtt::Packet&& packet) {
  if (_state == State::awaiting_connect) return on_connect(std::get<mqtt::Connect>(std::move(packet)));
  if (_state == State::authenticating) {
    if (const auto* auth = std::get_if<mqtt::Auth>(&packet)) return on_auth(*auth);
    if (std::holds_alternative<mqtt::Disconnect>(packet)) return shutdown(std::nullopt, false);
    return refuse_connect(reason::protocol_error);
  }
  std::visit(
      [this](auto&& p) {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, mqtt::Publish>) {
          on_publish(std::move(p));
        } else if constexpr (std::is_same_v<T, mqtt::Ack>) {
          on_ack(p);
        } else if constexpr (std::is_same_v<T, mqtt::Subscribe>) {
          on_subscribe(p);
        } else if constexpr (std::is_same_v<T, mqtt::Unsubscribe>) {
          on_unsubscribe(p);
        } else if constexpr (std::is_same_v<T, mqtt::Pingreq>) {
          send(mqtt::Pingresp{});
        } else if constexpr (std::is_same_v<T, mqtt::Disconnect>) {
          on_disconnect(p);
        } else if constexpr (std::is_same_v<T, mqtt::Auth>) {
          on_auth(p);
        } else {
          // A second CONNECT, or a packet only a server sends.
          ++_broker._stats.protocol_errors;
          shutdown(reason::protocol_error, true);
        }
      },
      std::move(packet));
}

void Link::refuse_connect(uint8_t code) {
  ++_broker._stats.connections_refused;
  if (v5()) {
    send(mqtt::Connack{false, code, {}});
  } else if (const auto v3 = v3_return_code(code)) {
    send(mqtt::Connack{false, *v3, {}});
  }
  _state = State::closed;
  _connection->close();
  _broker.detach(*this, false);
}

void Link::on_connect(mqtt::Connect&& connect) {
  const auto& config = _broker._config;
  _version = connect.version;
  if (connect.will) {
    const auto& will = *connect.will;
    if (!mqtt::valid_topic_name(will.topic)) return refuse_connect(reason::topic_name_invalid);
    if (will.qos > config.maximum_qos) return refuse_connect(reason::qos_not_supported);
    if (will.retain && !config.retain_available) return refuse_connect(reason::retain_not_supported);
    if (_broker.reserved(will.topic) || !_broker._authenticator.may_publish(connect.client_id, will.topic)) return refuse_connect(reason::not_authorized);
    if (will.properties.response_topic && !mqtt::valid_topic_name(*will.properties.response_topic)) return refuse_connect(reason::protocol_error);
  }
  // 3.1 requires an identifier; 3.1.1 allows none only with a clean session.
  if (connect.client_id.empty() && (_version == mqtt::Version::v3_1 || (_version == mqtt::Version::v3_1_1 && !connect.clean_start))) {
    return refuse_connect(reason::client_identifier_not_valid);
  }
  if (v5()) _auth_method = connect.properties.authentication_method;
  _connect = std::move(connect);
  const auto decision = _broker._authenticator.connect(*_connect);
  switch (decision.kind) {
    case contract::AuthDecision::Kind::accept:
      return begin_connect(decision.data);
    case contract::AuthDecision::Kind::refuse:
      return refuse_connect(decision.reason);
    case contract::AuthDecision::Kind::challenge: {
      if (!_auth_method) return refuse_connect(reason::bad_authentication_method);
      _state = State::authenticating;
      mqtt::Auth auth{reason::continue_authentication, {}};
      auth.properties.authentication_method = _auth_method;
      auth.properties.authentication_data = decision.data;
      return send(auth);
    }
  }
}

void Link::begin_connect(std::optional<Bytes> auth_data) {
  _assigned = _connect->client_id.empty();
  _client_id = _assigned ? _broker.assign_client_id() : _connect->client_id;
  // An assigned identifier is new everywhere; any other may be held by
  // another node, which lets go of it first.
  if (!_broker.clustered() || _assigned) return finish_connect(std::move(auth_data), std::nullopt);
  _state = State::taking;
  std::weak_ptr<Link> weak = shared_from_this();
  _broker._hooks.take(_client_id, !_connect->clean_start, [weak, auth = std::move(auth_data)](std::optional<SessionImage> image) mutable {
    const auto self = weak.lock();
    if (!self || self->_state != State::taking) return;
    self->finish_connect(std::move(auth), std::move(image));
    if (self->_state == State::connected && !self->_buffer.empty()) self->process();
  });
}

void Link::finish_connect(std::optional<Bytes> auth_data, std::optional<SessionImage> image) {
  auto& broker = _broker;
  const auto& config = broker._config;
  const auto& connect = *_connect;
  const std::string id = _client_id;
  const bool assigned = _assigned;

  // Session takeover: the existing connection is told and closed; its will
  // follows the usual rules (a resumed session cancels a delayed one).
  if (const auto it = broker._sessions.find(id); it != broker._sessions.end() && it->second->link) {
    it->second->link->shutdown(reason::session_taken_over, true);
  }
  bool present = false;
  if (const auto it = broker._sessions.find(id); it != broker._sessions.end()) {
    if (connect.clean_start) {
      broker.end_session(id);
    } else {
      present = true;
      broker._wills.erase(id);
    }
  }
  // A session another node gave up comes here whole.
  if (!present && image && !connect.clean_start) {
    image->client_id = id;
    broker.import(std::move(*image));
    present = true;
    ++broker._stats.sessions_taken;
  }
  if (!present) {
    auto session = std::make_unique<Broker::Session>();
    session->client_id = id;
    broker._sessions.emplace(id, std::move(session));
  }
  auto& session = *broker._sessions.at(id);
  session.link = this;
  session.expires_at.reset();
  session.version = _version;

  uint32_t requested = 0;
  if (v5()) {
    requested = connect.properties.session_expiry_interval.value_or(0);
  } else {
    requested = connect.clean_start ? 0 : never;
  }
  session.expiry_interval = std::min(requested, config.maximum_session_expiry);
  session.client_receive_maximum = connect.properties.receive_maximum.value_or(65535);
  session.client_maximum_packet_size = connect.properties.maximum_packet_size.value_or(never);
  _will = connect.will;
  _keep_alive = v5() && config.server_keep_alive ? *config.server_keep_alive : connect.keep_alive;
  _state = State::connected;

  mqtt::Connack ack{present, reason::success, {}};
  if (v5()) {
    auto& p = ack.properties;
    if (config.receive_maximum != 65535) p.receive_maximum = config.receive_maximum;
    if (config.maximum_qos != QoS::exactly_once) p.maximum_qos = static_cast<uint8_t>(config.maximum_qos);
    if (!config.retain_available) p.retain_available = 0;
    p.maximum_packet_size = config.maximum_packet_size;
    if (assigned) p.assigned_client_identifier = id;
    if (config.topic_alias_maximum > 0) p.topic_alias_maximum = config.topic_alias_maximum;
    if (!config.wildcard_subscription_available) p.wildcard_subscription_available = 0;
    if (!config.subscription_identifier_available) p.subscription_identifier_available = 0;
    if (!config.shared_subscription_available) p.shared_subscription_available = 0;
    if (config.server_keep_alive) p.server_keep_alive = *config.server_keep_alive;
    if (session.expiry_interval != requested) p.session_expiry_interval = session.expiry_interval;
    if (_auth_method) {
      p.authentication_method = _auth_method;
      p.authentication_data = std::move(auth_data);
    }
  }
  send(ack);
  ++broker._stats.connections_accepted;
  broker.resend(session);
  broker.pump(session);
}

void Link::on_publish(mqtt::Publish&& publish) {
  auto& broker = _broker;
  const auto& config = broker._config;
  if (v5() && publish.properties.topic_alias) {
    const uint16_t alias = *publish.properties.topic_alias;
    if (alias > config.topic_alias_maximum) return shutdown(reason::topic_alias_invalid, true);
    if (publish.topic.empty()) {
      const auto it = _inbound_aliases.find(alias);
      if (it == _inbound_aliases.end()) return shutdown(reason::protocol_error, true);
      publish.topic = it->second;
    } else {
      _inbound_aliases[alias] = publish.topic;
    }
  }
  if (!mqtt::valid_topic_name(publish.topic)) return shutdown(reason::topic_name_invalid, true);
  if (publish.qos > config.maximum_qos) return shutdown(reason::qos_not_supported, true);
  if (publish.retain && !config.retain_available) return shutdown(reason::retain_not_supported, true);
  if (publish.properties.response_topic && !mqtt::valid_topic_name(*publish.properties.response_topic)) return shutdown(reason::protocol_error, true);

  auto& session = *broker._sessions.at(_client_id);
  const uint16_t id = publish.packet_id;
  if (publish.qos == QoS::exactly_once) {
    // A resent QoS 2 PUBLISH whose PUBREL has not come: answered, not
    // delivered again.
    if (session.inbound_qos2.contains(id)) return send(mqtt::Ack{PacketType::pubrec, id, reason::success, {}});
    if (session.inbound_qos2.size() >= config.receive_maximum) return shutdown(reason::receive_maximum_exceeded, true);
  }

  uint8_t code = reason::success;
  if (broker.reserved(publish.topic) || !broker._authenticator.may_publish(_client_id, publish.topic)) {
    code = reason::not_authorized;
  } else if (publish.properties.payload_format_indicator == 1 && !mqtt::valid_utf8(as_text(publish.payload))) {
    code = reason::payload_format_invalid;
  } else {
    Message message;
    message.topic = std::move(publish.topic);
    message.payload = std::move(publish.payload);
    message.qos = publish.qos;
    message.retain = publish.retain;
    auto& p = publish.properties;
    message.payload_format_indicator = p.payload_format_indicator;
    if (p.message_expiry_interval) message.expires_at = broker._time.now() + std::chrono::seconds(*p.message_expiry_interval);
    message.content_type = std::move(p.content_type);
    message.response_topic = std::move(p.response_topic);
    message.correlation_data = std::move(p.correlation_data);
    message.user_properties = std::move(p.user_properties);
    message.origin = _client_id;
    ++broker._stats.messages_received;
    if (broker.route(message) == 0) code = reason::no_matching_subscribers;
  }
  if (!v5()) code = reason::success;
  switch (publish.qos) {
    case QoS::at_most_once:
      return;
    case QoS::at_least_once:
      return send(mqtt::Ack{PacketType::puback, id, code, {}});
    case QoS::exactly_once:
      if (code < 0x80) session.inbound_qos2.insert(id);
      return send(mqtt::Ack{PacketType::pubrec, id, code, {}});
  }
}

void Link::on_ack(const mqtt::Ack& ack) {
  auto& broker = _broker;
  auto& session = *broker._sessions.at(_client_id);
  using State = Inflight::State;
  const auto it = session.outbound.find(ack.packet_id);
  const auto not_found = v5() ? reason::packet_identifier_not_found : reason::success;
  switch (ack.type) {
    case PacketType::puback:
      if (it != session.outbound.end() && it->second.state == State::awaiting_puback) {
        session.outbound.erase(it);
        broker.pump(session);
      }
      return;
    case PacketType::pubrec:
      if (it == session.outbound.end()) return send(mqtt::Ack{PacketType::pubrel, ack.packet_id, not_found, {}});
      if (it->second.state == State::awaiting_puback) return;
      if (ack.reason >= 0x80) {
        session.outbound.erase(it);
        broker.pump(session);
        return;
      }
      it->second.state = State::awaiting_pubcomp;
      return send(mqtt::Ack{PacketType::pubrel, ack.packet_id, reason::success, {}});
    case PacketType::pubrel:
      if (session.inbound_qos2.erase(ack.packet_id) > 0) return send(mqtt::Ack{PacketType::pubcomp, ack.packet_id, reason::success, {}});
      return send(mqtt::Ack{PacketType::pubcomp, ack.packet_id, not_found, {}});
    case PacketType::pubcomp:
      if (it != session.outbound.end() && it->second.state == State::awaiting_pubcomp) {
        session.outbound.erase(it);
        broker.pump(session);
      }
      return;
    default:
      return;
  }
}

void Link::on_subscribe(const mqtt::Subscribe& subscribe) {
  auto& broker = _broker;
  const auto& config = broker._config;
  auto& session = *broker._sessions.at(_client_id);
  std::optional<uint32_t> identifier;
  if (!subscribe.properties.subscription_identifiers.empty()) {
    if (!config.subscription_identifier_available) return shutdown(reason::subscription_identifiers_not_supported, true);
    identifier = subscribe.properties.subscription_identifiers.front();
  }
  mqtt::Suback ack{subscribe.packet_id, {}, {}};
  std::vector<std::pair<std::string, Options>> retained_for;
  for (const auto& request : subscribe.subscriptions) {
    const bool shared = mqtt::is_shared(request.filter);
    const auto parsed = shared ? mqtt::parse_shared(request.filter) : std::nullopt;
    const std::string_view inner = parsed ? std::string_view(parsed->filter) : std::string_view(request.filter);
    uint8_t code = 0;
    if (shared && request.no_local) return shutdown(reason::protocol_error, true);
    if (shared ? !parsed : !mqtt::valid_topic_filter(request.filter)) {
      code = reason::topic_filter_invalid;
    } else if (shared && !config.shared_subscription_available) {
      code = reason::shared_subscriptions_not_supported;
    } else if (mqtt::has_wildcard(inner) && !config.wildcard_subscription_available) {
      code = reason::wildcard_subscriptions_not_supported;
    } else if (!broker._authenticator.may_subscribe(_client_id, request.filter)) {
      code = reason::not_authorized;
    } else {
      Options options{lesser(request.qos, config.maximum_qos), request.no_local, request.retain_as_published, request.retain_handling, identifier};
      const bool existed = session.subscriptions.contains(request.filter);
      broker.subscribe(session, request.filter, options);
      code = static_cast<uint8_t>(options.qos);
      // Shared subscriptions are never sent retained messages.
      if (!shared && (options.retain_handling == 0 || (options.retain_handling == 1 && !existed))) retained_for.emplace_back(request.filter, options);
    }
    if (!v5() && code >= 0x80) code = 0x80;
    ack.reasons.push_back(code);
  }
  send(ack);
  for (const auto& [filter, options] : retained_for) broker.send_retained(session, filter, options);
}

void Link::on_unsubscribe(const mqtt::Unsubscribe& unsubscribe) {
  auto& session = *_broker._sessions.at(_client_id);
  mqtt::Unsuback ack{unsubscribe.packet_id, {}, {}};
  for (const auto& filter : unsubscribe.filters) {
    const bool had = _broker.unsubscribe(session, filter);
    if (v5()) ack.reasons.push_back(had ? reason::success : reason::no_subscription_existed);
  }
  send(ack);
}

void Link::on_disconnect(const mqtt::Disconnect& disconnect) {
  if (v5() && disconnect.properties.session_expiry_interval) {
    // A session that was to end at the disconnect may not be kept now.
    const uint32_t at_connect = _connect->properties.session_expiry_interval.value_or(0);
    if (at_connect == 0 && *disconnect.properties.session_expiry_interval != 0) return shutdown(reason::protocol_error, true);
    auto& session = *_broker._sessions.at(_client_id);
    session.expiry_interval = std::min(*disconnect.properties.session_expiry_interval, _broker._config.maximum_session_expiry);
  }
  shutdown(std::nullopt, disconnect.reason == reason::disconnect_with_will);
}

void Link::on_auth(const mqtt::Auth& auth) {
  if (!_auth_method || auth.properties.authentication_method != _auth_method) {
    if (_state == State::authenticating) return refuse_connect(reason::protocol_error);
    return shutdown(reason::protocol_error, true);
  }
  const Bytes data = auth.properties.authentication_data.value_or(Bytes{});
  const std::string& client = _state == State::authenticating ? _connect->client_id : _client_id;
  if (_state == State::authenticating) {
    if (auth.reason != reason::continue_authentication) return refuse_connect(reason::protocol_error);
  } else if (auth.reason != reason::reauthenticate && auth.reason != reason::continue_authentication) {
    return shutdown(reason::protocol_error, true);
  }
  const auto decision = _broker._authenticator.auth(client, *_auth_method, data);
  switch (decision.kind) {
    case contract::AuthDecision::Kind::accept:
      if (_state == State::authenticating) return begin_connect(decision.data);
      {
        mqtt::Auth done{reason::success, {}};
        done.properties.authentication_method = _auth_method;
        done.properties.authentication_data = decision.data;
        return send(done);
      }
    case contract::AuthDecision::Kind::challenge: {
      mqtt::Auth more{reason::continue_authentication, {}};
      more.properties.authentication_method = _auth_method;
      more.properties.authentication_data = decision.data;
      return send(more);
    }
    case contract::AuthDecision::Kind::refuse:
      if (_state == State::authenticating) return refuse_connect(decision.reason);
      return shutdown(decision.reason, true);
  }
}

// ---------------------------------------------------------------- Broker

Broker::Broker(Config config, const dannan::contract::TimeSource& time, contract::Authenticator& authenticator, dannan::status::ScopedLogger logger)
    : _config(std::move(config)), _time(time), _authenticator(authenticator), _logger(std::move(logger)) {}

std::shared_ptr<Link> Broker::accept(std::shared_ptr<contract::Connection> connection) {
  auto link = std::make_shared<Link>(*this, std::move(connection));
  _links.emplace(link.get(), link);
  return link;
}

size_t Broker::publish(Message message) { return route(std::move(message)); }

bool Broker::reserved(std::string_view topic) const {
  return std::any_of(_config.reserved_prefixes.begin(), _config.reserved_prefixes.end(),
                     [topic](const std::string& prefix) { return topic.starts_with(prefix); });
}

size_t Broker::route(Message message) {
  if (message.retain) {
    message.version = RetainedVersion{++_lamport, _hooks.node};
    apply_retained(message);
  }
  size_t count = deliver_here(message, !clustered());
  if (clustered()) count += _hooks.published(message);
  return count;
}

size_t Broker::inject(const Message& message) {
  ++_stats.messages_injected;
  return deliver_here(message, false);
}

bool Broker::apply_retained(const Message& message) {
  if (!message.version) return false;
  const auto& version = *message.version;
  _lamport = std::max(_lamport, version.lamport);
  if (const auto held = _retained.find(message.topic); held != _retained.end() && held->second.version && *held->second.version >= version) return false;
  if (const auto cleared = _cleared.find(message.topic); cleared != _cleared.end() && cleared->second >= version) return false;
  if (message.payload.empty()) {
    _retained.erase(message.topic);
    _cleared[message.topic] = version;
  } else {
    _retained[message.topic] = message;
    _cleared.erase(message.topic);
  }
  return true;
}

std::vector<Message> Broker::retained_entries() const {
  std::vector<Message> out;
  for (const auto& [topic, message] : _retained) out.push_back(message);
  for (const auto& [topic, version] : _cleared) {
    Message clearing;
    clearing.topic = topic;
    clearing.retain = true;
    clearing.version = version;
    out.push_back(std::move(clearing));
  }
  return out;
}

size_t Broker::deliver_here(const Message& message, bool shared) {
  // One delivery per session, at the greatest QoS of its matching
  // subscriptions, carrying every matching subscription's identifier.
  struct Target {
    QoS qos = QoS::at_most_once;
    bool retain_as_published = false;
    std::vector<uint32_t> identifiers;
  };
  std::map<std::string, Target> targets;
  _subscriptions.match(message.topic, [&](const std::string& client, const Options& options) {
    if (options.no_local && client == message.origin) return;
    auto [it, added] = targets.try_emplace(client);
    auto& target = it->second;
    target.qos = added ? options.qos : std::max(target.qos, options.qos);
    target.retain_as_published = target.retain_as_published || options.retain_as_published;
    if (options.identifier) target.identifiers.push_back(*options.identifier);
  });
  size_t count = 0;
  for (auto& [client, target] : targets) {
    const auto it = _sessions.find(client);
    if (it == _sessions.end()) continue;
    std::sort(target.identifiers.begin(), target.identifiers.end());
    deliver(*it->second, Delivery{message, lesser(message.qos, target.qos), target.retain_as_published && message.retain, std::move(target.identifiers)});
    ++count;
  }
  if (!shared) return count;
  std::vector<std::pair<std::string, std::string>> groups;
  _shared_filters.match(message.topic, [&groups](const std::string& group, const std::string& filter) { groups.emplace_back(group, filter); });
  for (const auto& key : groups) {
    if (deliver_to_group(message, _shared.at(key))) ++count;
  }
  return count;
}

bool Broker::deliver_shared(const Message& message, const std::string& group, const std::string& filter) {
  const auto it = _shared.find({group, filter});
  if (it == _shared.end()) return false;
  return deliver_to_group(message, it->second);
}

// One member, connected ones first, in turn.
bool Broker::deliver_to_group(const Message& message, SharedGroup& group) {
  if (group.members.empty()) return false;
  std::vector<std::pair<const std::string, Options>*> members;
  for (auto& member : group.members) members.push_back(&member);
  const size_t start = group.next % members.size();
  size_t pick = start;
  for (size_t i = 0; i < members.size(); ++i) {
    const size_t index = (start + i) % members.size();
    const auto it = _sessions.find(members[index]->first);
    if (it != _sessions.end() && it->second->link) {
      pick = index;
      break;
    }
  }
  group.next = pick + 1;
  const auto* chosen = members[pick];
  const auto it = _sessions.find(chosen->first);
  if (it == _sessions.end()) return false;
  const auto& options = chosen->second;
  std::vector<uint32_t> ids;
  if (options.identifier) ids.push_back(*options.identifier);
  deliver(*it->second, Delivery{message, lesser(message.qos, options.qos), options.retain_as_published && message.retain, std::move(ids)});
  return true;
}

void Broker::deliver(Session& session, Delivery delivery) {
  if (session.link && delivery.qos == QoS::at_most_once) {
    transmit(session, std::move(delivery));
    return;
  }
  if (!session.link && delivery.qos == QoS::at_most_once) {
    ++_stats.dropped_offline_qos0;
    return;
  }
  if (session.queued.size() >= _config.maximum_queued) {
    session.queued.pop_front();
    ++_stats.dropped_queue_full;
  }
  session.queued.push_back(std::move(delivery));
  pump(session);
}

void Broker::pump(Session& session) {
  while (session.link && !session.queued.empty() && session.outbound.size() < session.client_receive_maximum) {
    auto delivery = std::move(session.queued.front());
    session.queued.pop_front();
    transmit(session, std::move(delivery));
  }
}

mqtt::Publish Broker::build(const Session& session, const Delivery& delivery, uint16_t packet_id, bool dup) const {
  mqtt::Publish publish;
  const auto& message = delivery.message;
  publish.topic = message.topic;
  publish.payload = message.payload;
  publish.qos = delivery.qos;
  publish.retain = delivery.retain;
  publish.packet_id = packet_id;
  publish.dup = dup;
  if (session.version == mqtt::Version::v5) {
    auto& p = publish.properties;
    p.payload_format_indicator = message.payload_format_indicator;
    if (message.expires_at) {
      const auto left = std::chrono::ceil<std::chrono::seconds>(*message.expires_at - _time.now()).count();
      p.message_expiry_interval = static_cast<uint32_t>(std::max<int64_t>(left, 0));
    }
    p.content_type = message.content_type;
    p.response_topic = message.response_topic;
    p.correlation_data = message.correlation_data;
    p.user_properties = message.user_properties;
    p.subscription_identifiers = delivery.identifiers;
  }
  return publish;
}

bool Broker::transmit(Session& session, Delivery delivery) {
  if (delivery.message.expires_at && *delivery.message.expires_at <= _time.now()) {
    ++_stats.dropped_expired;
    return false;
  }
  uint16_t id = 0;
  if (delivery.qos != QoS::at_most_once) {
    while (session.next_packet_id == 0 || session.outbound.contains(session.next_packet_id)) ++session.next_packet_id;
    id = session.next_packet_id++;
  }
  Bytes bytes = mqtt::encode(build(session, delivery, id, false), session.version);
  // A packet over the client's maximum is discarded as if delivered.
  if (bytes.size() > session.client_maximum_packet_size) {
    ++_stats.dropped_too_large;
    return false;
  }
  if (delivery.qos != QoS::at_most_once) {
    const auto state = delivery.qos == QoS::at_least_once ? Inflight::State::awaiting_puback : Inflight::State::awaiting_pubrec;
    session.outbound.emplace(id, Inflight{std::move(delivery), state});
  }
  session.link->_connection->send(std::move(bytes));
  ++_stats.messages_delivered;
  return true;
}

void Broker::resend(Session& session) {
  for (auto it = session.outbound.begin(); it != session.outbound.end();) {
    const uint16_t id = it->first;
    if (it->second.state == Inflight::State::awaiting_pubcomp) {
      session.link->send(mqtt::Ack{PacketType::pubrel, id, mqtt::reason::success, {}});
      ++it;
      continue;
    }
    Bytes bytes = mqtt::encode(build(session, it->second.delivery, id, true), session.version);
    if (bytes.size() > session.client_maximum_packet_size) {
      ++_stats.dropped_too_large;
      it = session.outbound.erase(it);
      continue;
    }
    session.link->_connection->send(std::move(bytes));
    ++it;
  }
}

void Broker::subscribe(Session& session, const std::string& filter, const Options& options) {
  session.subscriptions[filter] = options;
  if (const auto shared = mqtt::parse_shared(filter)) {
    _shared_filters.insert(shared->filter, shared->group, shared->filter);
    _shared[{shared->group, shared->filter}].members[session.client_id] = options;
  } else {
    _subscriptions.insert(filter, session.client_id, options);
  }
  interest_changed();
}

bool Broker::unsubscribe(Session& session, const std::string& filter) {
  if (session.subscriptions.erase(filter) == 0) return false;
  if (const auto shared = mqtt::parse_shared(filter)) {
    const std::pair key{shared->group, shared->filter};
    auto& group = _shared.at(key);
    group.members.erase(session.client_id);
    if (group.members.empty()) {
      _shared.erase(key);
      _shared_filters.erase(shared->filter, shared->group);
    }
  } else {
    _subscriptions.erase(filter, session.client_id);
  }
  interest_changed();
  return true;
}

void Broker::send_retained(Session& session, const std::string& filter, const Options& options) {
  const auto now = _time.now();
  for (const auto& [topic, message] : _retained) {
    if (!mqtt::matches(filter, topic)) continue;
    if (message.expires_at && *message.expires_at <= now) continue;
    std::vector<uint32_t> ids;
    if (options.identifier) ids.push_back(*options.identifier);
    deliver(session, Delivery{message, lesser(message.qos, options.qos), true, std::move(ids)});
  }
}

void Broker::end_session(const std::string& client_id) {
  fire_will(client_id);
  drop_session(client_id);
}

void Broker::drop_session(const std::string& client_id) {
  const auto it = _sessions.find(client_id);
  if (it == _sessions.end()) return;
  auto& session = *it->second;
  std::vector<std::string> filters;
  for (const auto& [filter, options] : session.subscriptions) filters.push_back(filter);
  for (const auto& filter : filters) unsubscribe(session, filter);
  _sessions.erase(it);
}

SessionImage Broker::image_of(const Session& session) const {
  SessionImage image;
  image.client_id = session.client_id;
  image.version = session.version;
  image.expiry_interval = session.expiry_interval;
  for (const auto& [filter, options] : session.subscriptions) image.subscriptions.emplace_back(filter, options);
  for (const auto& [id, inflight] : session.outbound) image.outbound.emplace_back(id, inflight);
  image.queued.assign(session.queued.begin(), session.queued.end());
  image.inbound_qos2.assign(session.inbound_qos2.begin(), session.inbound_qos2.end());
  image.next_packet_id = session.next_packet_id;
  return image;
}

Broker::Session& Broker::import(SessionImage image) {
  auto session = std::make_unique<Session>();
  session->client_id = image.client_id;
  session->version = image.version;
  session->expiry_interval = image.expiry_interval;
  for (auto& [id, inflight] : image.outbound) session->outbound.emplace(id, std::move(inflight));
  session->queued.assign(std::make_move_iterator(image.queued.begin()), std::make_move_iterator(image.queued.end()));
  session->inbound_qos2.insert(image.inbound_qos2.begin(), image.inbound_qos2.end());
  session->next_packet_id = image.next_packet_id;
  auto& held = *_sessions.emplace(image.client_id, std::move(session)).first->second;
  for (const auto& [filter, options] : image.subscriptions) subscribe(held, filter, options);
  return held;
}

std::optional<SessionImage> Broker::surrender(const std::string& client_id, bool want_state) {
  if (const auto it = _sessions.find(client_id); it != _sessions.end() && it->second->link) {
    it->second->link->shutdown(mqtt::reason::session_taken_over, true);
  }
  const auto it = _sessions.find(client_id);
  if (it == _sessions.end()) return std::nullopt;
  ++_stats.sessions_surrendered;
  if (!want_state) {
    end_session(client_id);
    return std::nullopt;
  }
  // Resumed elsewhere: a delayed will is not sent (section 3.1.3.2).
  auto image = image_of(*it->second);
  _wills.erase(client_id);
  drop_session(client_id);
  return image;
}

std::set<std::string> Broker::local_filters() const {
  std::set<std::string> out;
  _subscriptions.each([&out](const std::string& filter, const std::string&, const Options&) { out.insert(filter); });
  return out;
}

std::set<std::pair<std::string, std::string>> Broker::local_shared() const {
  std::set<std::pair<std::string, std::string>> out;
  for (const auto& [key, group] : _shared) {
    if (!group.members.empty()) out.insert(key);
  }
  return out;
}

void Broker::attach(ClusterHooks hooks) {
  _hooks = std::move(hooks);
  interest_changed();
}

void Broker::interest_changed() {
  if (_hooks.interest_changed) _hooks.interest_changed();
}

void Broker::schedule_will(const std::string& client_id, const mqtt::Will& will, uint32_t session_expiry) {
  PendingWill pending;
  auto& message = pending.message;
  message.topic = will.topic;
  message.payload = will.payload;
  message.qos = will.qos;
  message.retain = will.retain;
  message.payload_format_indicator = will.properties.payload_format_indicator;
  message.content_type = will.properties.content_type;
  message.response_topic = will.properties.response_topic;
  message.correlation_data = will.properties.correlation_data;
  message.user_properties = will.properties.user_properties;
  message.origin = client_id;
  pending.expiry_seconds = will.properties.message_expiry_interval;
  // Sent when the will delay passes or the session ends, whichever is first.
  const uint32_t delay = std::min(will.properties.will_delay_interval.value_or(0), session_expiry);
  pending.due = _time.now() + std::chrono::seconds(delay);
  _wills[client_id] = std::move(pending);
  if (delay == 0) fire_will(client_id);
}

void Broker::fire_will(const std::string& client_id) {
  const auto it = _wills.find(client_id);
  if (it == _wills.end()) return;
  auto pending = std::move(it->second);
  _wills.erase(it);
  if (pending.expiry_seconds) pending.message.expires_at = _time.now() + std::chrono::seconds(*pending.expiry_seconds);
  ++_stats.wills_published;
  route(pending.message);
}

void Broker::detach(Link& link, bool publish_will) {
  const auto self = _links.find(&link);
  // Held until this returns: the link may be the last owner of itself.
  const auto keep = self != _links.end() ? self->second : nullptr;
  if (self != _links.end()) _links.erase(self);
  const auto it = _sessions.find(link._client_id);
  if (it == _sessions.end() || it->second->link != &link) return;
  auto& session = *it->second;
  session.link = nullptr;
  const std::string client_id = session.client_id;
  const uint32_t expiry = session.expiry_interval;
  if (publish_will && link._will) schedule_will(client_id, *link._will, expiry);
  if (expiry == 0) {
    end_session(client_id);
  } else if (expiry != never) {
    if (const auto again = _sessions.find(client_id); again != _sessions.end()) again->second->expires_at = _time.now() + std::chrono::seconds(expiry);
  }
}

void Broker::poll() {
  const auto now = _time.now();
  std::vector<std::shared_ptr<Link>> links;
  for (const auto& [raw, link] : _links) links.push_back(link);
  for (const auto& link : links) {
    if (link->_state == Link::State::awaiting_connect || link->_state == Link::State::authenticating) {
      if (now - link->_opened > _config.connect_timeout) link->shutdown(std::nullopt, false);
    } else if (link->_state == Link::State::connected && link->_keep_alive > 0) {
      // One and a half keep alive periods with nothing heard (section 3.1.2.10).
      if (now - link->_last_received > std::chrono::milliseconds(uint64_t{link->_keep_alive} * 1500)) link->shutdown(mqtt::reason::keep_alive_timeout, true);
    }
  }
  std::vector<std::string> due;
  for (const auto& [client, will] : _wills) {
    if (will.due <= now) due.push_back(client);
  }
  for (const auto& client : due) fire_will(client);
  std::vector<std::string> expired;
  for (const auto& [client, session] : _sessions) {
    if (!session->link && session->expires_at && *session->expires_at <= now) expired.push_back(client);
  }
  for (const auto& client : expired) {
    ++_stats.sessions_expired;
    end_session(client);
  }
  std::erase_if(_retained, [now](const auto& entry) { return entry.second.expires_at && *entry.second.expires_at <= now; });
}

std::string Broker::assign_client_id() {
  while (true) {
    std::string id = _config.assigned_id_prefix + dannan::identity::hex(dannan::identity::random_bytes(12));
    if (!_sessions.contains(id)) return id;
  }
}

}  // namespace lir::broker
