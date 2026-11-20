//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "lir/mqtt/properties.h"

#include <stdexcept>

#include "lir/mqtt/topic.h"

namespace lir::mqtt {

namespace {

using P = PropertyPlace;

constexpr uint32_t bit(P place) { return uint32_t{1} << static_cast<uint8_t>(place); }

constexpr uint32_t message_places = bit(P::publish) | bit(P::will);
constexpr uint32_t ack_places = bit(P::puback) | bit(P::pubrec) | bit(P::pubrel) | bit(P::pubcomp);
constexpr uint32_t reason_string_places = bit(P::connack) | ack_places | bit(P::suback) | bit(P::unsuback) | bit(P::disconnect) | bit(P::auth);
constexpr uint32_t every_place = 0x3FFF;

// Where each property may stand: MQTT 5.0 section 2.2.2.2's table. Zero for
// an identifier the specification does not define.
uint32_t places_of(uint8_t id) {
  switch (id) {
    case property::payload_format_indicator:
    case property::message_expiry_interval:
    case property::content_type:
    case property::response_topic:
    case property::correlation_data:
      return message_places;
    case property::subscription_identifier:
      return bit(P::publish) | bit(P::subscribe);
    case property::session_expiry_interval:
      return bit(P::connect) | bit(P::connack) | bit(P::disconnect);
    case property::assigned_client_identifier:
    case property::server_keep_alive:
    case property::response_information:
    case property::maximum_qos:
    case property::retain_available:
    case property::wildcard_subscription_available:
    case property::subscription_identifier_available:
    case property::shared_subscription_available:
      return bit(P::connack);
    case property::authentication_method:
    case property::authentication_data:
      return bit(P::connect) | bit(P::connack) | bit(P::auth);
    case property::request_problem_information:
    case property::request_response_information:
      return bit(P::connect);
    case property::will_delay_interval:
      return bit(P::will);
    case property::server_reference:
      return bit(P::connack) | bit(P::disconnect);
    case property::reason_string:
      return reason_string_places;
    case property::receive_maximum:
    case property::topic_alias_maximum:
    case property::maximum_packet_size:
      return bit(P::connect) | bit(P::connack);
    case property::topic_alias:
      return bit(P::publish);
    case property::user_property:
      return every_place;
    default:
      return 0;
  }
}

template <class T>
void once(std::optional<T>& slot, T value) {
  if (slot) protocol_error("property included more than once");
  slot = std::move(value);
}

uint8_t flag(dannan::net::Reader& reader) {
  if (reader.remaining() < 1) malformed("packet ends inside a property");
  const uint8_t value = reader.u8();
  if (value > 1) protocol_error("property value other than 0 or 1");
  return value;
}

uint16_t two(dannan::net::Reader& reader) {
  if (reader.remaining() < 2) malformed("packet ends inside a property");
  return reader.u16();
}

uint32_t four(dannan::net::Reader& reader) {
  if (reader.remaining() < 4) malformed("packet ends inside a property");
  return reader.u32();
}

void read_one(dannan::net::Reader& reader, Properties& p, P place) {
  const uint8_t id = reader.u8();
  const uint32_t allowed = places_of(id);
  if (allowed == 0) malformed("unknown property identifier");
  if ((allowed & bit(place)) == 0) protocol_error("property not allowed in this packet");
  switch (id) {
    case property::payload_format_indicator:
      return once(p.payload_format_indicator, flag(reader));
    case property::message_expiry_interval:
      return once(p.message_expiry_interval, four(reader));
    case property::content_type:
      return once(p.content_type, read_string(reader));
    case property::response_topic: {
      std::string topic = read_string(reader);
      if (!valid_topic_name(topic)) protocol_error("response topic is not a topic name");  // [MQTT-3.3.2-14]
      return once(p.response_topic, std::move(topic));
    }
    case property::correlation_data:
      return once(p.correlation_data, read_binary(reader));
    case property::subscription_identifier: {
      const uint32_t value = read_varint(reader);
      if (value == 0) protocol_error("subscription identifier 0");
      if (place == P::subscribe && !p.subscription_identifiers.empty()) protocol_error("subscription identifier included more than once");
      p.subscription_identifiers.push_back(value);
      return;
    }
    case property::session_expiry_interval:
      return once(p.session_expiry_interval, four(reader));
    case property::assigned_client_identifier:
      return once(p.assigned_client_identifier, read_string(reader));
    case property::server_keep_alive:
      return once(p.server_keep_alive, two(reader));
    case property::authentication_method:
      return once(p.authentication_method, read_string(reader));
    case property::authentication_data:
      return once(p.authentication_data, read_binary(reader));
    case property::request_problem_information:
      return once(p.request_problem_information, flag(reader));
    case property::will_delay_interval:
      return once(p.will_delay_interval, four(reader));
    case property::request_response_information:
      return once(p.request_response_information, flag(reader));
    case property::response_information:
      return once(p.response_information, read_string(reader));
    case property::server_reference:
      return once(p.server_reference, read_string(reader));
    case property::reason_string:
      return once(p.reason_string, read_string(reader));
    case property::receive_maximum: {
      const uint16_t value = two(reader);
      if (value == 0) protocol_error("receive maximum 0");
      return once(p.receive_maximum, value);
    }
    case property::topic_alias_maximum:
      return once(p.topic_alias_maximum, two(reader));
    case property::topic_alias: {
      const uint16_t value = two(reader);
      if (value == 0) throw MalformedPacket(reason::topic_alias_invalid, "topic alias 0");  // [MQTT-3.3.2-8]
      return once(p.topic_alias, value);
    }
    case property::maximum_qos:
      return once(p.maximum_qos, flag(reader));
    case property::retain_available:
      return once(p.retain_available, flag(reader));
    case property::user_property: {
      std::string name = read_string(reader);
      std::string value = read_string(reader);
      p.user_properties.emplace_back(std::move(name), std::move(value));
      return;
    }
    case property::maximum_packet_size: {
      const uint32_t value = four(reader);
      if (value == 0) protocol_error("maximum packet size 0");
      return once(p.maximum_packet_size, value);
    }
    case property::wildcard_subscription_available:
      return once(p.wildcard_subscription_available, flag(reader));
    case property::subscription_identifier_available:
      return once(p.subscription_identifier_available, flag(reader));
    case property::shared_subscription_available:
      return once(p.shared_subscription_available, flag(reader));
  }
  // Every identifier places_of() knows has a case above; it refused the rest.
}

void require(bool condition, const char* what) {
  if (!condition) throw std::invalid_argument(what);
}

// Calls `emit(id, write)` for each property set, in identifier order; the
// same walk gives the size and the bytes.
template <class Emit>
void each(const Properties& p, Emit&& emit) {
  using W = dannan::net::Writer;
  if (p.payload_format_indicator) emit(property::payload_format_indicator, [&](W& w) { w.u8(*p.payload_format_indicator); });
  if (p.message_expiry_interval) emit(property::message_expiry_interval, [&](W& w) { w.u32(*p.message_expiry_interval); });
  if (p.content_type) emit(property::content_type, [&](W& w) { write_string(w, *p.content_type); });
  if (p.response_topic) emit(property::response_topic, [&](W& w) { write_string(w, *p.response_topic); });
  if (p.correlation_data) emit(property::correlation_data, [&](W& w) { write_binary(w, *p.correlation_data); });
  for (const uint32_t id : p.subscription_identifiers) emit(property::subscription_identifier, [id](W& w) { write_varint(w, id); });
  if (p.session_expiry_interval) emit(property::session_expiry_interval, [&](W& w) { w.u32(*p.session_expiry_interval); });
  if (p.assigned_client_identifier) emit(property::assigned_client_identifier, [&](W& w) { write_string(w, *p.assigned_client_identifier); });
  if (p.server_keep_alive) emit(property::server_keep_alive, [&](W& w) { w.u16(*p.server_keep_alive); });
  if (p.authentication_method) emit(property::authentication_method, [&](W& w) { write_string(w, *p.authentication_method); });
  if (p.authentication_data) emit(property::authentication_data, [&](W& w) { write_binary(w, *p.authentication_data); });
  if (p.request_problem_information) emit(property::request_problem_information, [&](W& w) { w.u8(*p.request_problem_information); });
  if (p.will_delay_interval) emit(property::will_delay_interval, [&](W& w) { w.u32(*p.will_delay_interval); });
  if (p.request_response_information) emit(property::request_response_information, [&](W& w) { w.u8(*p.request_response_information); });
  if (p.response_information) emit(property::response_information, [&](W& w) { write_string(w, *p.response_information); });
  if (p.server_reference) emit(property::server_reference, [&](W& w) { write_string(w, *p.server_reference); });
  if (p.reason_string) emit(property::reason_string, [&](W& w) { write_string(w, *p.reason_string); });
  if (p.receive_maximum) emit(property::receive_maximum, [&](W& w) { w.u16(*p.receive_maximum); });
  if (p.topic_alias_maximum) emit(property::topic_alias_maximum, [&](W& w) { w.u16(*p.topic_alias_maximum); });
  if (p.topic_alias) emit(property::topic_alias, [&](W& w) { w.u16(*p.topic_alias); });
  if (p.maximum_qos) emit(property::maximum_qos, [&](W& w) { w.u8(*p.maximum_qos); });
  if (p.retain_available) emit(property::retain_available, [&](W& w) { w.u8(*p.retain_available); });
  for (const auto& [name, value] : p.user_properties) {
    emit(property::user_property, [&](W& w) {
      write_string(w, name);
      write_string(w, value);
    });
  }
  if (p.maximum_packet_size) emit(property::maximum_packet_size, [&](W& w) { w.u32(*p.maximum_packet_size); });
  if (p.wildcard_subscription_available) emit(property::wildcard_subscription_available, [&](W& w) { w.u8(*p.wildcard_subscription_available); });
  if (p.subscription_identifier_available) emit(property::subscription_identifier_available, [&](W& w) { w.u8(*p.subscription_identifier_available); });
  if (p.shared_subscription_available) emit(property::shared_subscription_available, [&](W& w) { w.u8(*p.shared_subscription_available); });
}

// The values the specification bounds; where each property may stand is
// checked as the block is written.
void check(const Properties& p, P place) {
  for (const auto* value : {&p.payload_format_indicator, &p.request_problem_information, &p.request_response_information, &p.maximum_qos, &p.retain_available,
                            &p.wildcard_subscription_available, &p.subscription_identifier_available, &p.shared_subscription_available}) {
    require(!*value || **value <= 1, "property value other than 0 or 1");
  }
  require(!p.receive_maximum || *p.receive_maximum != 0, "receive maximum 0");
  require(!p.maximum_packet_size || *p.maximum_packet_size != 0, "maximum packet size 0");
  require(!p.topic_alias || *p.topic_alias != 0, "topic alias 0");
  require(!p.response_topic || valid_topic_name(*p.response_topic), "response topic is not a topic name");
  require(place != P::subscribe || p.subscription_identifiers.size() <= 1, "more than one subscription identifier on a SUBSCRIBE");
  for (const uint32_t id : p.subscription_identifiers) require(id >= 1 && id <= subscription_identifier_max, "subscription identifier out of range");
}

// The block's bytes, one walk for writing and sizing alike; with a place,
// each property is checked against it on the way.
Bytes block_of(const Properties& properties, std::optional<P> place) {
  dannan::net::Writer block;
  each(properties, [&block, place](uint8_t id, auto&& write) {
    require(!place || (places_of(id) & bit(*place)) != 0, "property not allowed in this packet");
    block.u8(id);
    write(block);
  });
  return block.take();
}

}  // namespace

bool Properties::empty() const noexcept { return *this == Properties{}; }

Properties read_properties(dannan::net::Reader& reader, PropertyPlace place) {
  const uint32_t length = read_varint(reader);
  if (reader.remaining() < length) malformed("property length past the packet");
  dannan::net::Reader block(reader.view(length));
  Properties out;
  while (block.remaining() > 0) read_one(block, out, place);
  return out;
}

void write_properties(dannan::net::Writer& writer, const Properties& properties, PropertyPlace place) {
  check(properties, place);
  const Bytes block = block_of(properties, place);
  write_varint(writer, static_cast<uint32_t>(block.size()));
  writer.raw(block);
}

size_t properties_size(const Properties& properties) {
  const Bytes block = block_of(properties, std::nullopt);
  return varint_size(static_cast<uint32_t>(block.size())) + block.size();
}

}  // namespace lir::mqtt
