//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "lir/broker/link.h"

#include <boost/asio/post.hpp>
#include <stdexcept>
#include <utility>
#include <variant>

#include "dannan/identity/random_id.h"
#include "dannan/json/json.h"
#include "lir/mqtt/topic.h"
#include "lir/system/topics.h"

namespace lir::broker {

namespace {

using mqtt::QoS;
using mqtt::Version;
namespace reason = mqtt::reason;

// The fixed header's most and the variable byte integer's largest value.
constexpr uint32_t protocol_maximum_packet_size = 5 + mqtt::varint_max;

struct AssignedClientIdTag {};

bool is5(Version version) noexcept { return version == Version::v5; }

}  // namespace

uint8_t connack_code_3x(uint8_t code) noexcept {
  switch (code) {
    case reason::success:
      return 0;
    case reason::unsupported_protocol_version:
      return 1;
    case reason::client_identifier_not_valid:
      return 2;
    case reason::bad_user_name_or_password:
      return 4;
    case reason::not_authorized:
    case reason::banned:
    case reason::bad_authentication_method:
      return 5;
    default:
      return 3;
  }
}

std::shared_ptr<Link> Link::create(boost::asio::any_io_executor strand, Delivery& delivery, contract::Authenticator& authenticator, dannan::Time& time,
                                   std::shared_ptr<contract::Connection> connection, dannan::status::ScopedLogger logger, Limits limits, system::Views* views) {
  return std::shared_ptr<Link>(new Link(std::move(strand), delivery, authenticator, time, std::move(connection), std::move(logger), limits, views));
}

Link::Link(boost::asio::any_io_executor strand, Delivery& delivery, contract::Authenticator& authenticator, dannan::Time& time,
           std::shared_ptr<contract::Connection> connection, dannan::status::ScopedLogger logger, Limits limits, system::Views* views)
    : _strand(std::move(strand)),
      _delivery(delivery),
      _authenticator(authenticator),
      _time(time),
      _connection(std::move(connection)),
      _logger(std::move(logger)),
      _limits(limits),
      _views(views) {
  _peer.address = _connection->peer();
  if (auto certificate = _connection->certificate()) {
    _peer.certificate_subject = certificate->subject;
    _peer.certificate_thumbprint = certificate->thumbprint;
  }
}

void Link::receive(std::span<const uint8_t> bytes) {
  if (_state == State::closed) return;
  _input.insert(_input.end(), bytes.begin(), bytes.end());
  size_t used = 0;
  while (_state != State::closed) {
    const std::span<const uint8_t> rest(_input.data() + used, _input.size() - used);
    std::optional<mqtt::Frame> frame;
    try {
      frame = mqtt::read_frame(rest, _limits.maximum_packet_size);
    } catch (const mqtt::MalformedPacket& error) {
      fail(error.reason(), error.what());
      break;
    }
    if (!frame || rest.size() < frame->size()) break;
    const auto whole = rest.first(frame->size());
    used += frame->size();
    _last_heard = _time.now();
    // [MQTT-3.1.0-1]: the first packet is CONNECT, or the connection closes,
    // with nothing sent: no version has been named to answer in.
    if (_state == State::awaiting_connect && (frame->first_byte >> 4) != static_cast<uint8_t>(mqtt::PacketType::connect)) {
      _logger.debug(_peer.address + ": first packet is not CONNECT");
      close();
      break;
    }
    mqtt::Packet packet;
    try {
      packet = mqtt::decode(whole, _version);
    } catch (const mqtt::UnsupportedVersion& error) {
      // 3.1.1 [MQTT-3.1.2-2]: return code 1, in the form a 3.x client reads;
      // 5.0 [MQTT-3.1.2-2]: reason 0x84 for a level of 5 or more.
      const bool five = error.level() >= 5;
      _connection->send(mqtt::encode(mqtt::Connack{false, five ? reason::unsupported_protocol_version : uint8_t{1}, {}}, five ? Version::v5 : Version::v3_1_1));
      close();
      break;
    } catch (const mqtt::MalformedPacket& error) {
      fail(error.reason(), error.what());
      break;
    }
    handle(packet);
  }
  if (_state == State::closed) {
    _input.clear();
  } else {
    _input.erase(_input.begin(), _input.begin() + static_cast<std::ptrdiff_t>(used));
  }
}

void Link::closed() {
  if (_state == State::closed) return;
  if (_end_why.empty()) _end_why = "the connection ended";
  close();
}

void Link::handle(const mqtt::Packet& packet) {
  if (const auto* connect = std::get_if<mqtt::Connect>(&packet)) {
    // [MQTT-3.1.0-2]: a second CONNECT is a protocol error.
    if (_state != State::awaiting_connect) return fail(reason::protocol_error, "a second CONNECT");
    return on_connect(*connect);
  }
  if (const auto* auth = std::get_if<mqtt::Auth>(&packet)) return on_auth(*auth);
  if (const auto* disconnect = std::get_if<mqtt::Disconnect>(&packet)) return on_disconnect(*disconnect);
  // [MQTT-3.1.2-30]: while authenticating, only AUTH and DISCONNECT.
  if (_state == State::authenticating) return fail(reason::protocol_error, "a packet other than AUTH during authentication");
  if (const auto* subscribe = std::get_if<mqtt::Subscribe>(&packet)) return on_subscribe(*subscribe);
  if (const auto* unsubscribe = std::get_if<mqtt::Unsubscribe>(&packet)) return on_unsubscribe(*unsubscribe);
  if (const auto* publish = std::get_if<mqtt::Publish>(&packet)) return on_publish(*publish);
  if (std::holds_alternative<mqtt::Pingreq>(packet)) return send(mqtt::Pingresp{});  // [MQTT-3.12.4-1]
  // A server's packet, or an acknowledgement of a QoS 1 or 2 exchange this
  // server never began (Maximum QoS 0).
  fail(reason::protocol_error, "a packet a client does not send here");
}

void Link::on_connect(const mqtt::Connect& connect) {
  _version = connect.version;
  _connect = connect;
  _client_maximum_packet_size = connect.properties.maximum_packet_size;
  if (connect.client_id.empty()) {
    // 3.1 has no empty identifier; 3.1.1 only with a clean session
    // (3.1.1 [MQTT-3.1.3-8]); 5.0 is told the one assigned [MQTT-3.2.2-16].
    if (connect.version == Version::v3_1 || (connect.version == Version::v3_1_1 && !connect.clean_start)) {
      return refuse_connect(reason::client_identifier_not_valid);
    }
    _client_id = dannan::identity::to_string(dannan::identity::RandomId<AssignedClientIdTag>::random());
  } else if (connect.version == Version::v3_1 && connect.client_id.size() > 23) {
    // MQTT 3.1: an identifier of 1 to 23 characters, else return code 2.
    return refuse_connect(reason::client_identifier_not_valid);
  } else {
    _client_id = connect.client_id;
  }
  decided(_authenticator.connect(connect, _peer));
}

void Link::on_auth(const mqtt::Auth& auth) {
  const auto& method = _connect->properties.authentication_method;
  // [MQTT-4.12.0-1] and after: AUTH belongs to the method CONNECT named, and
  // continues an exchange or starts a re-authentication.
  if (!method || auth.properties.authentication_method != method) return fail(reason::protocol_error, "AUTH without the CONNECT's method");
  const bool continuing = auth.reason == reason::continue_authentication;
  if (_state == State::authenticating ? !continuing : !(continuing || auth.reason == reason::reauthenticate)) {
    return fail(reason::protocol_error, "AUTH with a reason out of place");
  }
  decided(_authenticator.auth(_client_id, *method, auth.properties.authentication_data.value_or(mqtt::Bytes{}), _peer));
}

void Link::decided(const contract::AuthDecision& decision) {
  const auto& method = _connect->properties.authentication_method;
  switch (decision.kind) {
    case contract::AuthDecision::Kind::accept:
      if (_state != State::connected) return accept(decision.grant, decision.data);
      {
        // A re-authentication: the new grant replaces the old.
        _grant = decision.grant ? decision.grant : std::make_shared<contract::Nothing>();
        ++_grant_generation;
        mqtt::Auth done;
        done.properties.authentication_method = method;
        done.properties.authentication_data = decision.data;
        send(done);
        arm_expiry();
      }
      return;
    case contract::AuthDecision::Kind::refuse:
      if (_state != State::connected) return refuse_connect(decision.reason);
      return fail(decision.reason, "re-authentication refused");
    case contract::AuthDecision::Kind::challenge: {
      // Only an exchange the client began by naming a method.
      if (!method) {
        _logger.error("the authenticator challenged a CONNECT that named no method");
        return _state == State::connected ? fail(reason::unspecified_error, "challenge without a method") : refuse_connect(reason::unspecified_error);
      }
      mqtt::Auth challenge;
      challenge.reason = reason::continue_authentication;
      challenge.properties.authentication_method = method;
      challenge.properties.authentication_data = decision.data;
      if (_state == State::awaiting_connect) _state = State::authenticating;
      return send(challenge);
    }
  }
}

void Link::accept(std::shared_ptr<const contract::Grant> grant, std::optional<mqtt::Bytes> data) {
  if (!grant) grant = std::make_shared<contract::Nothing>();
  const auto& connect = *_connect;
  if (connect.will) {
    // Every will is refused until Dagda's leases (D5) let one outlive its
    // node (the maintainer, 2026-11-22): accepting it would acknowledge a
    // guarantee Lir does not have [MQTT-3.1.2-7]. Above QoS 0 and retained
    // wills are told why [MQTT-3.2.2-12], [MQTT-3.2.2-13]; a 3.x client hears
    // return code 3.
    if (connect.will->qos != QoS::at_most_once) return refuse_connect(reason::qos_not_supported);
    if (connect.will->retain) return refuse_connect(reason::retain_not_supported);
    return refuse_connect(reason::implementation_specific_error);
  }
  _grant = std::move(grant);
  ++_grant_generation;
  _state = State::connected;
  mqtt::Connack connack;
  if (is5(_version)) {
    auto& p = connack.properties;
    p.maximum_qos = 0;       // [MQTT-3.2.2-9]
    p.retain_available = 0;  // [MQTT-3.2.2-13]
    p.shared_subscription_available = 0;
    p.subscription_identifier_available = 0;
    if (_limits.maximum_packet_size < protocol_maximum_packet_size) p.maximum_packet_size = _limits.maximum_packet_size;
    if (_limits.topic_alias_maximum > 0) p.topic_alias_maximum = _limits.topic_alias_maximum;  // [MQTT-3.2.2-17]
    if (connect.client_id.empty()) p.assigned_client_identifier = _client_id;                  // [MQTT-3.2.2-16]
    // The session ends with the connection until Dagda's replicated spaces.
    if (connect.properties.session_expiry_interval.value_or(0) != 0) p.session_expiry_interval = 0;
    p.authentication_method = connect.properties.authentication_method;
    p.authentication_data = std::move(data);
  }
  // Session Present 0: nothing survives a connection yet [MQTT-3.2.2-2].
  send(connack);
  _logger.debug(_peer.address + ": connected as " + _client_id);
  _announced = true;
  announce("connected", std::nullopt, {});
  _keep_alive = std::chrono::seconds(connect.keep_alive);
  arm_keep_alive();
  arm_expiry();
}

void Link::refuse_connect(uint8_t code) {
  mqtt::Connack connack;
  connack.reason = is5(_version) ? code : connack_code_3x(code);
  try {
    _connection->send(mqtt::encode(connack, _version));
  } catch (const std::invalid_argument&) {
    // A reason a CONNACK cannot carry (the authenticator's choice).
    connack.reason = reason::unspecified_error;
    _connection->send(mqtt::encode(connack, _version));
  }
  _logger.debug(_peer.address + ": CONNECT refused");
  close();  // [MQTT-3.2.2-7]
}

void Link::on_subscribe(const mqtt::Subscribe& subscribe) {
  const uint64_t suback = _next_suback++;
  auto& pending = _subacks[suback];
  pending.packet_id = subscribe.packet_id;
  pending.reasons.assign(subscribe.subscriptions.size(), reason::granted_qos_0);
  // Counted before any answer can arrive, so none completes the SUBACK early.
  pending.outstanding = subscribe.subscriptions.size() + 1;
  std::weak_ptr<Link> weak = weak_from_this();
  for (size_t i = 0; i < subscribe.subscriptions.size(); ++i) {
    const auto& subscription = subscribe.subscriptions[i];
    const auto& filter = subscription.filter;
    if (!subscribe.properties.subscription_identifiers.empty()) {
      answer(suback, i, reason::subscription_identifiers_not_supported);
    } else if (mqtt::is_shared(filter)) {
      answer(suback, i, reason::shared_subscriptions_not_supported);
    } else if (!mqtt::valid_topic_filter(filter)) {
      answer(suback, i, reason::topic_filter_invalid);
    } else if (!_grant->may_subscribe(filter)) {
      answer(suback, i, reason::not_authorized);
    } else if (auto it = _filters.find(filter); it != _filters.end()) {
      // [MQTT-3.8.4-3]: the same filter replaces the subscription's options;
      // the cluster already holds, or is being asked to hold, its interest.
      it->second.options = subscription;
      // Retain Handling 0 sends the views again; 1 and 2 do not, the
      // subscription existing [MQTT-3.3.1-9], [MQTT-3.3.1-10].
      if (it->second.view && subscription.retain_handling == 0) send_views(it->second.token, filter);
      if (it->second.held) {
        answer(suback, i, reason::granted_qos_0);
      } else {
        it->second.waiting.emplace_back(suback, i);
      }
    } else {
      const uint64_t token = _next_token++;
      Held held;
      held.token = token;
      held.options = subscription;
      held.waiting.emplace_back(suback, i);
      try {
        held.delivery = _delivery.subscribe(
            filter, _strand,
            [weak, token, filter](const mqtt::Publish& publish, const std::string& publisher) {
              if (auto self = weak.lock()) self->deliver(token, filter, publish, publisher);
            },
            _limits.subscribe_deadline, _strand,
            [weak, token, filter](dannan::membership::Pushed pushed) {
              if (auto self = weak.lock()) self->on_held(token, filter, !pushed.missed.empty());
            });
      } catch (const std::invalid_argument&) {
        answer(suback, i, reason::topic_filter_invalid);
        continue;
      }
      // A system filter is also answered from this node's views: their
      // state now, as Retain Handling allows (0 and 1 for a new
      // subscription, never 2 [MQTT-3.3.1-11]), then each change.
      if (_views && system::reserved(filter)) {
        held.view = _views->subscribe(filter, _strand, [weak, token, filter](const mqtt::Publish& publish) {
          if (auto self = weak.lock()) self->deliver_view(token, filter, publish);
        });
      }
      const bool views = held.view != 0;
      _filters.emplace(filter, std::move(held));
      if (views && subscription.retain_handling != 2) send_views(token, filter);
    }
  }
  answer(suback, subscribe.subscriptions.size(), reason::granted_qos_0);
}

void Link::on_held(uint64_t token, const std::string& filter, bool missed) {
  auto it = _filters.find(filter);
  if (it == _filters.end() || it->second.token != token) return;
  if (missed) {
    // Not every node holds it by the deadline: a publish made now might not
    // reach it, so the SUBACK cannot say granted.
    _logger.debug(_peer.address + ": subscription to " + filter + " not held in time");
    return drop(it, reason::unspecified_error);
  }
  it->second.held = true;
  auto waiting = std::move(it->second.waiting);
  it->second.waiting.clear();
  for (const auto& [suback, index] : waiting) answer(suback, index, reason::granted_qos_0);
}

void Link::answer(uint64_t suback, size_t index, uint8_t code) {
  auto it = _subacks.find(suback);
  if (it == _subacks.end()) return;
  auto& pending = it->second;
  if (index < pending.reasons.size()) pending.reasons[index] = code;
  if (--pending.outstanding > 0) return;
  mqtt::Suback out;
  out.packet_id = pending.packet_id;
  out.reasons = std::move(pending.reasons);
  _subacks.erase(it);
  if (!is5(_version)) {
    for (auto& r : out.reasons) {
      if (r < 0x80) continue;
      // 3.1 has no failure code: the connection is all it can be told by.
      if (_version == Version::v3_1) return fail(reason::unspecified_error, "a subscription 3.1 cannot refuse");
      r = 0x80;
    }
  }
  send(out);  // [MQTT-3.8.4-1], [MQTT-3.8.4-2]
}

void Link::drop(std::map<std::string, Held>::iterator it, uint8_t code) {
  _delivery.unsubscribe(it->second.delivery);
  if (it->second.view) _views->unsubscribe(it->second.view);
  auto waiting = std::move(it->second.waiting);
  _filters.erase(it);
  for (const auto& [suback, index] : waiting) answer(suback, index, code);
}

void Link::on_unsubscribe(const mqtt::Unsubscribe& unsubscribe) {
  mqtt::Unsuback out;
  out.packet_id = unsubscribe.packet_id;  // [MQTT-3.10.4-4]
  for (const auto& filter : unsubscribe.filters) {
    auto it = _filters.find(filter);
    if (it == _filters.end()) {
      out.reasons.push_back(reason::no_subscription_existed);
      continue;
    }
    // [MQTT-3.10.4-2]: nothing more for it from here on; a SUBACK that
    // waited on it is answered, the subscription having been granted.
    drop(it, reason::granted_qos_0);
    out.reasons.push_back(reason::success);
  }
  if (!is5(_version)) out.reasons.clear();
  send(out);  // [MQTT-3.10.4-4], [MQTT-3.10.4-5]
}

void Link::on_publish(const mqtt::Publish& publish) {
  // Maximum QoS 0 until Dagda's log [MQTT-3.2.2-11]; 3.x cannot be told, so
  // it is closed.
  if (publish.qos != QoS::at_most_once) return fail(reason::qos_not_supported, "PUBLISH above QoS 0");
  // Retain Available 0 until Dagda's register map [MQTT-3.2.2-14]; a 3.x
  // retained publish is closed for the same reason as its QoS.
  if (publish.retain) return fail(reason::retain_not_supported, "retained PUBLISH");
  // [MQTT-3.3.4-6]: a client does not send Subscription Identifiers.
  if (!publish.properties.subscription_identifiers.empty()) return fail(reason::protocol_error, "subscription identifier from a client");
  // A topic alias stands for a topic on this connection only [MQTT-3.3.2-7]:
  // 1 to the maximum CONNACK gave is taken [MQTT-3.3.2-12]; 0, or one past
  // it, is invalid [MQTT-3.3.2-8], [MQTT-3.3.2-9], [MQTT-3.2.2-17],
  // [MQTT-3.2.2-18]. With a topic it is set; without, it is used, and one
  // never set is a protocol error.
  std::string topic = publish.topic;
  if (const auto alias = publish.properties.topic_alias) {
    if (*alias == 0 || *alias > _limits.topic_alias_maximum) return fail(reason::topic_alias_invalid, "topic alias out of range");
    if (topic.empty()) {
      const auto found = _aliases.find(*alias);
      if (found == _aliases.end()) return fail(reason::protocol_error, "a topic alias never set");
      topic = found->second;
    } else if (mqtt::valid_topic_name(topic)) {
      _aliases[*alias] = topic;
    }
  }
  if (!mqtt::valid_topic_name(topic)) return fail(reason::topic_name_invalid, "invalid topic name");
  if (!_grant->may_publish(topic, publish.retain)) {
    // At QoS 0 there is nothing to answer: it goes nowhere.
    _logger.debug(_peer.address + ": not authorized to publish to " + topic);
    return;
  }
  // Only Lir writes the system topics, whatever a grant says (MQTT 4.7.2
  // leaves `$` topics to the server); as a refusal, it goes nowhere.
  if (system::reserved(topic)) {
    _logger.debug(_peer.address + ": a client's publish to the system topic " + topic + " dropped");
    return;
  }
  auto out = publish;
  out.topic = std::move(topic);
  out.properties.topic_alias.reset();
  out.dup = false;
  out.packet_id = 0;
  (void)_delivery.publish(out, _client_id);
  if (_views) _views->published();
}

void Link::on_disconnect(const mqtt::Disconnect& disconnect) {
  // No will to discard or publish: none was accepted.
  _end_reason = disconnect.reason;
  _end_why = "the client disconnected";
  close();  // [MQTT-3.14.4-1]
}

void Link::deliver(uint64_t token, const std::string& filter, const mqtt::Publish& publish, const std::string& publisher) {
  if (_state != State::connected) return;
  auto it = _filters.find(filter);
  if (it == _filters.end() || it->second.token != token) return;
  // [MQTT-3.8.3-3]: not back to the client id that published it.
  if (it->second.options.no_local && publisher == _client_id) return;
  auto out = publish;
  out.dup = false;
  out.qos = QoS::at_most_once;
  out.packet_id = 0;
  // [MQTT-3.3.1-12], [MQTT-3.3.1-13]
  out.retain = it->second.options.retain_as_published && publish.retain;
  if (is5(_version)) {
    out.properties.topic_alias.reset();
    out.properties.subscription_identifiers.clear();
  } else {
    out.properties = {};
  }
  send(out);
}

void Link::deliver_view(uint64_t token, const std::string& filter, const mqtt::Publish& publish) {
  if (_state != State::connected) return;
  auto it = _filters.find(filter);
  if (it == _filters.end() || it->second.token != token) return;
  // A view's state for a new subscription keeps RETAIN 1 [MQTT-3.3.1-9]; a
  // change or an event carries 0.
  send(publish);
}

void Link::send_views(uint64_t token, const std::string& filter) {
  std::weak_ptr<Link> weak = weak_from_this();
  _views->current(filter, _strand, [weak, token, filter](const mqtt::Publish& publish) {
    if (auto self = weak.lock()) self->deliver_view(token, filter, publish);
  });
}

void Link::announce(const std::string& what, std::optional<uint8_t> code, const std::string& why) {
  if (!_views) return;
  dannan::json::object event;
  event["from"] = _views->self();
  event["client_id"] = _client_id;
  event["version"] = _version == Version::v5 ? "5.0" : _version == Version::v3_1_1 ? "3.1.1" : "3.1";
  event["transport"] = _connection->transport();
  if (code) event["reason"] = static_cast<uint64_t>(*code);
  if (!why.empty()) event["why"] = why;
  mqtt::Publish publish;
  publish.topic = "$lir/clients/" + what;
  publish.payload = dannan::json::to_bytes(event);
  try {
    (void)_delivery.publish(publish, {});
  } catch (const std::invalid_argument& error) {
    _logger.error("the " + what + " event was not published: " + error.what());
  }
}

void Link::send(const mqtt::Packet& packet) {
  mqtt::Bytes bytes;
  try {
    bytes = mqtt::encode(packet, _version);
  } catch (const std::invalid_argument& error) {
    _logger.error(_peer.address + ": not sent: " + error.what());
    return;
  }
  // [MQTT-3.1.2-24], [MQTT-3.1.2-25]: too large for the client is dropped.
  if (_client_maximum_packet_size && bytes.size() > *_client_maximum_packet_size) {
    _logger.debug(_peer.address + ": a packet over the client's maximum size dropped");
    return;
  }
  _connection->send(std::move(bytes));
}

void Link::fail(uint8_t code, const std::string& why) {
  if (_state == State::closed) return;
  _logger.debug(_peer.address + ": " + why);
  _end_reason = code;
  _end_why = why;
  if (_state == State::awaiting_connect) return close();
  // [MQTT-3.14.0-1]: no DISCONNECT before a successful CONNACK; the refusal
  // is the CONNACK.
  if (_state == State::authenticating) return refuse_connect(code);
  if (is5(_version)) send(mqtt::Disconnect{code, {}});
  close();
}

void Link::close() {
  if (_state == State::closed) return;
  _state = State::closed;
  for (auto& [filter, held] : _filters) {
    _delivery.unsubscribe(held.delivery);
    if (held.view) _views->unsubscribe(held.view);
  }
  _filters.clear();
  _subacks.clear();
  _connection->close();
  if (_announced) announce("disconnected", _end_reason, _end_why.empty() ? "the server closed it" : _end_why);
}

void Link::arm_keep_alive() {
  if (_keep_alive.count() == 0) return;
  after(_keep_alive * 3 / 2, [](Link& link) { link.check_keep_alive(); });
}

void Link::check_keep_alive() {
  // [MQTT-3.1.2-22]: one and a half keep-alives with nothing heard.
  const auto limit = _keep_alive * 3 / 2;
  const auto quiet = std::chrono::duration_cast<std::chrono::milliseconds>(_time.now() - _last_heard);
  if (quiet >= limit) return fail(reason::keep_alive_timeout, "keep alive expired");
  after(limit - quiet, [](Link& link) { link.check_keep_alive(); });
}

void Link::arm_expiry() {
  const auto expires = _grant->expires();
  if (!expires) return;
  const auto generation = _grant_generation;
  const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(*expires - _time.now());
  after(std::max(left, std::chrono::milliseconds(0)), [generation](Link& link) {
    if (link._grant_generation == generation) link.fail(reason::not_authorized, "credential expired");
  });
}

void Link::after(std::chrono::milliseconds delay, std::function<void(Link&)> fn) {
  std::weak_ptr<Link> weak = weak_from_this();
  _time.after(delay, [weak, strand = _strand, fn = std::move(fn)] {
    boost::asio::post(strand, [weak, fn] {
      auto self = weak.lock();
      if (self && self->_state != State::closed) fn(*self);
    });
  });
}

}  // namespace lir::broker
