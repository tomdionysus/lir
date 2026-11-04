//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "lir/mqtt/properties.h"

#include <stdexcept>

namespace lir::mqtt {

namespace {

using dannan::net::Reader;
using dannan::net::Writer;

constexpr uint32_t bit(PropertyPlace place) { return uint32_t{1} << static_cast<uint8_t>(place); }

constexpr uint32_t message_places = bit(PropertyPlace::publish) | bit(PropertyPlace::will);
constexpr uint32_t acks = bit(PropertyPlace::puback) | bit(PropertyPlace::pubrec) | bit(PropertyPlace::pubrel) | bit(PropertyPlace::pubcomp);
constexpr uint32_t every_place = 0x3FFF;

// Where each property may appear (MQTT 5.0 table 2-4).
uint32_t allowed(uint8_t id) {
  using P = PropertyPlace;
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
      return bit(P::connack) | acks | bit(P::suback) | bit(P::unsuback) | bit(P::disconnect) | bit(P::auth);
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

template <typename T>
void once(std::optional<T>& slot, T value) {
  if (slot) protocol_error("property included more than once");
  slot = std::move(value);
}

uint8_t flag(Reader& reader) {
  const uint8_t value = reader.u8();
  if (value > 1) protocol_error("property must be 0 or 1");
  return value;
}

uint16_t nonzero_u16(Reader& reader) {
  const uint16_t value = reader.u16();
  if (value == 0) protocol_error("property must not be 0");
  return value;
}

void need(const Reader& reader, size_t count) {
  if (reader.remaining() < count) malformed("property runs past its block");
}

void read_one(Reader& reader, Properties& p, uint8_t id, PropertyPlace place) {
  switch (id) {
    case property::payload_format_indicator:
      need(reader, 1);
      return once(p.payload_format_indicator, flag(reader));
    case property::message_expiry_interval:
      need(reader, 4);
      return once(p.message_expiry_interval, reader.u32());
    case property::content_type:
      return once(p.content_type, read_string(reader));
    case property::response_topic:
      return once(p.response_topic, read_string(reader));
    case property::correlation_data:
      return once(p.correlation_data, read_binary(reader));
    case property::subscription_identifier: {
      const uint32_t value = read_varint(reader);
      if (value == 0) protocol_error("subscription identifier 0");
      if (place == PropertyPlace::subscribe && !p.subscription_identifiers.empty()) protocol_error("subscription identifier included more than once");
      p.subscription_identifiers.push_back(value);
      return;
    }
    case property::session_expiry_interval:
      need(reader, 4);
      return once(p.session_expiry_interval, reader.u32());
    case property::assigned_client_identifier:
      return once(p.assigned_client_identifier, read_string(reader));
    case property::server_keep_alive:
      need(reader, 2);
      return once(p.server_keep_alive, reader.u16());
    case property::authentication_method:
      return once(p.authentication_method, read_string(reader));
    case property::authentication_data:
      return once(p.authentication_data, read_binary(reader));
    case property::request_problem_information:
      need(reader, 1);
      return once(p.request_problem_information, flag(reader));
    case property::will_delay_interval:
      need(reader, 4);
      return once(p.will_delay_interval, reader.u32());
    case property::request_response_information:
      need(reader, 1);
      return once(p.request_response_information, flag(reader));
    case property::response_information:
      return once(p.response_information, read_string(reader));
    case property::server_reference:
      return once(p.server_reference, read_string(reader));
    case property::reason_string:
      return once(p.reason_string, read_string(reader));
    case property::receive_maximum:
      need(reader, 2);
      return once(p.receive_maximum, nonzero_u16(reader));
    case property::topic_alias_maximum:
      need(reader, 2);
      return once(p.topic_alias_maximum, reader.u16());
    case property::topic_alias:
      need(reader, 2);
      return once(p.topic_alias, nonzero_u16(reader));
    case property::maximum_qos:
      need(reader, 1);
      return once(p.maximum_qos, flag(reader));
    case property::retain_available:
      need(reader, 1);
      return once(p.retain_available, flag(reader));
    case property::user_property: {
      auto key = read_string(reader);
      auto value = read_string(reader);
      p.user_properties.emplace_back(std::move(key), std::move(value));
      return;
    }
    case property::maximum_packet_size: {
      need(reader, 4);
      const uint32_t value = reader.u32();
      if (value == 0) protocol_error("maximum packet size 0");
      return once(p.maximum_packet_size, value);
    }
    case property::wildcard_subscription_available:
      need(reader, 1);
      return once(p.wildcard_subscription_available, flag(reader));
    case property::subscription_identifier_available:
      need(reader, 1);
      return once(p.subscription_identifier_available, flag(reader));
    case property::shared_subscription_available:
      need(reader, 1);
      return once(p.shared_subscription_available, flag(reader));
    default:
      protocol_error("unknown property");
  }
}

// Each present property as (id, writer); the one walk both size and write use.
template <typename Visit>
void each(const Properties& p, Visit&& visit) {
  auto u8 = [](uint8_t v) { return [v](Writer& w) { w.u8(v); }; };
  auto u16 = [](uint16_t v) { return [v](Writer& w) { w.u16(v); }; };
  auto u32 = [](uint32_t v) { return [v](Writer& w) { w.u32(v); }; };
  auto str = [](const std::string& v) { return [&v](Writer& w) { write_string(w, v); }; };
  auto bin = [](const Bytes& v) { return [&v](Writer& w) { write_binary(w, v); }; };
  if (p.payload_format_indicator) visit(property::payload_format_indicator, u8(*p.payload_format_indicator));
  if (p.message_expiry_interval) visit(property::message_expiry_interval, u32(*p.message_expiry_interval));
  if (p.content_type) visit(property::content_type, str(*p.content_type));
  if (p.response_topic) visit(property::response_topic, str(*p.response_topic));
  if (p.correlation_data) visit(property::correlation_data, bin(*p.correlation_data));
  for (const uint32_t id : p.subscription_identifiers) visit(property::subscription_identifier, [id](Writer& w) { write_varint(w, id); });
  if (p.session_expiry_interval) visit(property::session_expiry_interval, u32(*p.session_expiry_interval));
  if (p.assigned_client_identifier) visit(property::assigned_client_identifier, str(*p.assigned_client_identifier));
  if (p.server_keep_alive) visit(property::server_keep_alive, u16(*p.server_keep_alive));
  if (p.authentication_method) visit(property::authentication_method, str(*p.authentication_method));
  if (p.authentication_data) visit(property::authentication_data, bin(*p.authentication_data));
  if (p.request_problem_information) visit(property::request_problem_information, u8(*p.request_problem_information));
  if (p.will_delay_interval) visit(property::will_delay_interval, u32(*p.will_delay_interval));
  if (p.request_response_information) visit(property::request_response_information, u8(*p.request_response_information));
  if (p.response_information) visit(property::response_information, str(*p.response_information));
  if (p.server_reference) visit(property::server_reference, str(*p.server_reference));
  if (p.reason_string) visit(property::reason_string, str(*p.reason_string));
  if (p.receive_maximum) visit(property::receive_maximum, u16(*p.receive_maximum));
  if (p.topic_alias_maximum) visit(property::topic_alias_maximum, u16(*p.topic_alias_maximum));
  if (p.topic_alias) visit(property::topic_alias, u16(*p.topic_alias));
  if (p.maximum_qos) visit(property::maximum_qos, u8(*p.maximum_qos));
  if (p.retain_available) visit(property::retain_available, u8(*p.retain_available));
  for (const auto& [key, value] : p.user_properties) {
    visit(property::user_property, [&key, &value](Writer& w) {
      write_string(w, key);
      write_string(w, value);
    });
  }
  if (p.maximum_packet_size) visit(property::maximum_packet_size, u32(*p.maximum_packet_size));
  if (p.wildcard_subscription_available) visit(property::wildcard_subscription_available, u8(*p.wildcard_subscription_available));
  if (p.subscription_identifier_available) visit(property::subscription_identifier_available, u8(*p.subscription_identifier_available));
  if (p.shared_subscription_available) visit(property::shared_subscription_available, u8(*p.shared_subscription_available));
}

Bytes body(const Properties& p) {
  Writer writer;
  each(p, [&writer](uint8_t id, auto&& write) {
    writer.u8(id);
    write(writer);
  });
  return writer.take();
}

}  // namespace

bool Properties::empty() const noexcept { return *this == Properties{}; }

Properties read_properties(Reader& reader, PropertyPlace place) {
  const uint32_t length = read_varint(reader);
  if (reader.remaining() < length) malformed("property block runs past the packet");
  Reader block(reader.view(length));
  Properties properties;
  while (block.remaining() > 0) {
    const uint32_t id = read_varint(block);
    if (id > 0xFF || (allowed(static_cast<uint8_t>(id)) & bit(place)) == 0) protocol_error("property not allowed here");
    read_one(block, properties, static_cast<uint8_t>(id), place);
  }
  return properties;
}

void write_properties(Writer& writer, const Properties& properties, PropertyPlace place) {
  each(properties, [place](uint8_t id, auto&&) {
    if ((allowed(id) & bit(place)) == 0) throw std::invalid_argument("property not allowed in this packet");
  });
  if (place == PropertyPlace::subscribe && properties.subscription_identifiers.size() > 1)
    throw std::invalid_argument("one subscription identifier per SUBSCRIBE");
  for (const uint32_t id : properties.subscription_identifiers) {
    if (id == 0 || id > subscription_identifier_max) throw std::invalid_argument("subscription identifier out of range");
  }
  const Bytes encoded = body(properties);
  write_varint(writer, static_cast<uint32_t>(encoded.size()));
  writer.raw(encoded);
}

size_t properties_size(const Properties& properties) {
  const size_t length = body(properties).size();
  return varint_size(static_cast<uint32_t>(length)) + length;
}

}  // namespace lir::mqtt
