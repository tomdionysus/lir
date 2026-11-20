//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "lir/mqtt/packet.h"

#include <algorithm>
#include <initializer_list>
#include <stdexcept>
#include <type_traits>

#include "lir/mqtt/topic.h"

namespace lir::mqtt {

namespace {

using dannan::net::Reader;
using dannan::net::Writer;

bool is5(Version version) noexcept { return version == Version::v5; }

// The reason codes each 5.0 packet may carry (sections 3.2.2.2 to 3.15.2.1).
constexpr std::initializer_list<uint8_t> connack_codes = {0x00, 0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
                                                          0x8A, 0x8C, 0x90, 0x95, 0x97, 0x99, 0x9A, 0x9B, 0x9C, 0x9D, 0x9F};
constexpr std::initializer_list<uint8_t> puback_codes = {0x00, 0x10, 0x80, 0x83, 0x87, 0x90, 0x91, 0x97, 0x99};
constexpr std::initializer_list<uint8_t> pubrel_codes = {0x00, 0x92};
constexpr std::initializer_list<uint8_t> suback_codes = {0x00, 0x01, 0x02, 0x80, 0x83, 0x87, 0x8F, 0x91, 0x97, 0x9E, 0xA1, 0xA2};
constexpr std::initializer_list<uint8_t> suback_codes_3 = {0x00, 0x01, 0x02, 0x80};
// MQTT 3.1's SUBACK grants a QoS and has no failure code.
constexpr std::initializer_list<uint8_t> suback_codes_31 = {0x00, 0x01, 0x02};

std::initializer_list<uint8_t> suback_codes_of(Version version) noexcept {
  if (version == Version::v5) return suback_codes;
  return version == Version::v3_1 ? suback_codes_31 : suback_codes_3;
}
constexpr std::initializer_list<uint8_t> unsuback_codes = {0x00, 0x11, 0x80, 0x83, 0x87, 0x8F, 0x91};
constexpr std::initializer_list<uint8_t> disconnect_codes = {0x00, 0x04, 0x80, 0x81, 0x82, 0x83, 0x87, 0x89, 0x8B, 0x8D, 0x8E, 0x8F, 0x90, 0x93, 0x94,
                                                             0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0x9F, 0xA0, 0xA1, 0xA2};
constexpr std::initializer_list<uint8_t> auth_codes = {0x00, 0x18, 0x19};

bool one_of(uint8_t code, std::initializer_list<uint8_t> codes) noexcept { return std::find(codes.begin(), codes.end(), code) != codes.end(); }

std::initializer_list<uint8_t> ack_codes(PacketType type) noexcept {
  return type == PacketType::puback || type == PacketType::pubrec ? puback_codes : pubrel_codes;
}

PropertyPlace ack_place(PacketType type) noexcept {
  switch (type) {
    case PacketType::puback:
      return PropertyPlace::puback;
    case PacketType::pubrec:
      return PropertyPlace::pubrec;
    case PacketType::pubrel:
      return PropertyPlace::pubrel;
    default:
      return PropertyPlace::pubcomp;
  }
}

// The flags each type's first byte carries [MQTT-2.1.3-1]: 0010 for PUBREL,
// SUBSCRIBE and UNSUBSCRIBE, PUBLISH's own, 0000 for the rest.
uint8_t fixed_flags(PacketType type) noexcept {
  return type == PacketType::pubrel || type == PacketType::subscribe || type == PacketType::unsubscribe ? 0x02 : 0x00;
}

uint8_t byte(Reader& r) {
  if (r.remaining() < 1) malformed("packet ends early");
  return r.u8();
}

uint16_t two(Reader& r) {
  if (r.remaining() < 2) malformed("packet ends early");
  return r.u16();
}

uint16_t packet_id(Reader& r) {
  const uint16_t id = two(r);
  if (id == 0) malformed("packet identifier 0");
  return id;
}

void done(const Reader& r) {
  if (r.remaining() != 0) malformed("bytes past the end of the packet");
}

Properties properties_of(Reader& r, Version version, PropertyPlace place) { return is5(version) ? read_properties(r, place) : Properties{}; }

Connect decode_connect(Reader& r) {
  Connect c;
  const std::string name = read_string(r);
  const uint8_t level = byte(r);
  if (name == "MQTT" && level == 4) {
    c.version = Version::v3_1_1;
  } else if (name == "MQTT" && level == 5) {
    c.version = Version::v5;
  } else if (name == "MQIsdp" && level == 3) {
    c.version = Version::v3_1;
  } else {
    throw UnsupportedVersion(level, "protocol " + name + " level " + std::to_string(level));
  }
  const uint8_t flags = byte(r);
  if (flags & 0x01) malformed("CONNECT reserved flag set");  // [MQTT-3.1.2-3]
  c.clean_start = (flags & 0x02) != 0;
  const bool will = (flags & 0x04) != 0;
  const auto will_qos = static_cast<uint8_t>((flags >> 3) & 0x03);
  const bool will_retain = (flags & 0x20) != 0;
  const bool has_password = (flags & 0x40) != 0;
  const bool has_username = (flags & 0x80) != 0;
  if (!will && (will_qos != 0 || will_retain)) malformed("will QoS or retain without a will");  // [MQTT-3.1.2-11], [MQTT-3.1.2-13]
  if (will_qos == 3) malformed("will QoS 3");
  if (!is5(c.version) && has_password && !has_username) malformed("password without a user name");  // 3.1.1 [MQTT-3.1.2-22]
  c.keep_alive = two(r);
  c.properties = properties_of(r, c.version, PropertyPlace::connect);
  if (c.properties.authentication_data && !c.properties.authentication_method) protocol_error("authentication data without a method");
  c.client_id = read_string(r);  // [MQTT-3.1.3-3], [MQTT-3.1.3-4]
  if (will) {
    Will w;
    w.properties = properties_of(r, c.version, PropertyPlace::will);
    w.topic = read_string(r);
    w.payload = read_binary(r);
    w.qos = static_cast<QoS>(will_qos);
    w.retain = will_retain;
    c.will = std::move(w);
  }
  if (has_username) c.username = read_string(r);
  if (has_password) c.password = read_binary(r);
  done(r);
  return c;
}

Connack decode_connack(Reader& r, Version version) {
  Connack c;
  const uint8_t flags = byte(r);
  // 3.1 has no Session Present flag: the whole byte is reserved.
  if (flags & (version == Version::v3_1 ? 0xFF : 0xFE)) malformed("CONNACK reserved flags set");  // [MQTT-3.2.2-1]
  c.session_present = (flags & 0x01) != 0;
  c.reason = byte(r);
  if (is5(version) ? !one_of(c.reason, connack_codes) : c.reason > 5) malformed("CONNACK code not allowed");
  if (c.session_present && c.reason != reason::success) protocol_error("session present on a refused CONNACK");  // [MQTT-3.2.2-6]
  c.properties = properties_of(r, version, PropertyPlace::connack);
  done(r);
  return c;
}

Publish decode_publish(Reader& r, uint8_t flags, Version version) {
  Publish p;
  p.dup = (flags & 0x08) != 0;
  const auto qos = static_cast<uint8_t>((flags >> 1) & 0x03);
  if (qos == 3) malformed("PUBLISH QoS 3");  // [MQTT-3.3.1-4]
  p.qos = static_cast<QoS>(qos);
  p.retain = (flags & 0x01) != 0;
  if (p.dup && p.qos == QoS::at_most_once) malformed("DUP set at QoS 0");  // [MQTT-3.3.1-2]
  p.topic = read_string(r);
  if (has_wildcard(p.topic)) throw MalformedPacket(reason::topic_name_invalid, "wildcard in a PUBLISH topic");  // [MQTT-3.3.2-2]
  if (p.qos != QoS::at_most_once) p.packet_id = packet_id(r);
  p.properties = properties_of(r, version, PropertyPlace::publish);
  if (p.topic.empty()) {
    if (!is5(version)) throw MalformedPacket(reason::topic_name_invalid, "empty PUBLISH topic");
    if (!p.properties.topic_alias) protocol_error("empty PUBLISH topic with no topic alias");
  }
  p.payload = r.raw(r.remaining());
  return p;
}

Ack decode_ack(Reader& r, PacketType type, Version version) {
  Ack a;
  a.type = type;
  a.packet_id = packet_id(r);
  if (is5(version) && r.remaining() > 0) {
    a.reason = byte(r);
    if (!one_of(a.reason, ack_codes(type))) malformed("acknowledgement code not allowed");
    if (r.remaining() > 0) a.properties = read_properties(r, ack_place(type));
  }
  done(r);
  return a;
}

Subscribe decode_subscribe(Reader& r, Version version) {
  Subscribe s;
  s.packet_id = packet_id(r);
  s.properties = properties_of(r, version, PropertyPlace::subscribe);
  while (r.remaining() > 0) {
    Subscription sub;
    sub.filter = read_string(r);  // [MQTT-3.8.3-1]
    const uint8_t options = byte(r);
    if (is5(version)) {
      if (options & 0xC0) malformed("subscription options reserved bits set");  // [MQTT-3.8.3-5]
    } else if (options & 0xFC) {
      malformed("requested QoS reserved bits set");  // 3.1.1 [MQTT-3.8.3-4]
    }
    if ((options & 0x03) == 3) malformed("requested QoS 3");
    sub.qos = static_cast<QoS>(options & 0x03);
    sub.no_local = (options & 0x04) != 0;
    sub.retain_as_published = (options & 0x08) != 0;
    sub.retain_handling = static_cast<uint8_t>((options >> 4) & 0x03);
    if (sub.retain_handling == 3) protocol_error("retain handling 3");
    if (sub.no_local && is_shared(sub.filter)) protocol_error("no local on a shared subscription");  // [MQTT-3.8.3-4]
    s.subscriptions.push_back(std::move(sub));
  }
  if (s.subscriptions.empty()) protocol_error("SUBSCRIBE with no topic filter");  // [MQTT-3.8.3-2]
  return s;
}

Suback decode_suback(Reader& r, Version version) {
  Suback s;
  s.packet_id = packet_id(r);
  s.properties = properties_of(r, version, PropertyPlace::suback);
  while (r.remaining() > 0) {
    const uint8_t code = byte(r);
    if (!one_of(code, suback_codes_of(version))) malformed("SUBACK code not allowed");  // [MQTT-3.9.3-2]
    s.reasons.push_back(code);
  }
  if (s.reasons.empty()) protocol_error("SUBACK with no reason codes");
  return s;
}

Unsubscribe decode_unsubscribe(Reader& r, Version version) {
  Unsubscribe u;
  u.packet_id = packet_id(r);
  u.properties = properties_of(r, version, PropertyPlace::unsubscribe);
  while (r.remaining() > 0) u.filters.push_back(read_string(r));              // [MQTT-3.10.3-1]
  if (u.filters.empty()) protocol_error("UNSUBSCRIBE with no topic filter");  // [MQTT-3.10.3-2]
  return u;
}

Unsuback decode_unsuback(Reader& r, Version version) {
  Unsuback u;
  u.packet_id = packet_id(r);
  if (is5(version)) {
    u.properties = read_properties(r, PropertyPlace::unsuback);
    while (r.remaining() > 0) {
      const uint8_t code = byte(r);
      if (!one_of(code, unsuback_codes)) malformed("UNSUBACK code not allowed");  // [MQTT-3.11.3-2]
      u.reasons.push_back(code);
    }
    if (u.reasons.empty()) protocol_error("UNSUBACK with no reason codes");
  }
  done(r);
  return u;
}

Disconnect decode_disconnect(Reader& r, Version version) {
  Disconnect d;
  if (is5(version) && r.remaining() > 0) {
    d.reason = byte(r);
    if (!one_of(d.reason, disconnect_codes)) malformed("DISCONNECT code not allowed");  // [MQTT-3.14.2-1]
    if (r.remaining() > 0) d.properties = read_properties(r, PropertyPlace::disconnect);
  }
  done(r);
  return d;
}

Auth decode_auth(Reader& r) {
  Auth a;
  if (r.remaining() > 0) {
    a.reason = byte(r);
    if (!one_of(a.reason, auth_codes)) malformed("AUTH code not allowed");  // [MQTT-3.15.2-1]
    a.properties = read_properties(r, PropertyPlace::auth);
  }
  done(r);
  return a;
}

void require(bool condition, const char* what) {
  if (!condition) throw std::invalid_argument(what);
}

void no_properties(const Properties& properties, Version version) { require(is5(version) || properties.empty(), "properties on MQTT 3.x"); }

uint8_t encode_connect(Writer& w, const Connect& c, Version version) {
  require(c.version == version, "CONNECT version differs from the one encoded");
  no_properties(c.properties, version);
  if (c.will) no_properties(c.will->properties, version);
  require(!c.will || c.will->qos <= QoS::exactly_once, "will QoS over 2");
  require(is5(version) || !c.password || c.username, "password without a user name");
  require(!c.properties.authentication_data || c.properties.authentication_method, "authentication data without a method");
  write_string(w, version == Version::v3_1 ? "MQIsdp" : "MQTT");
  w.u8(static_cast<uint8_t>(version));
  uint8_t flags = 0;
  if (c.clean_start) flags |= 0x02;
  if (c.will) {
    flags |= 0x04;
    flags |= static_cast<uint8_t>(static_cast<uint8_t>(c.will->qos) << 3);
    if (c.will->retain) flags |= 0x20;
  }
  if (c.password) flags |= 0x40;
  if (c.username) flags |= 0x80;
  w.u8(flags);
  w.u16(c.keep_alive);
  if (is5(version)) write_properties(w, c.properties, PropertyPlace::connect);
  write_string(w, c.client_id);
  if (c.will) {
    if (is5(version)) write_properties(w, c.will->properties, PropertyPlace::will);
    write_string(w, c.will->topic);
    write_binary(w, c.will->payload);
  }
  if (c.username) write_string(w, *c.username);
  if (c.password) write_binary(w, *c.password);
  return 0;
}

uint8_t encode_connack(Writer& w, const Connack& c, Version version) {
  no_properties(c.properties, version);
  require(is5(version) ? one_of(c.reason, connack_codes) : c.reason <= 5, "CONNACK code not allowed");
  require(!c.session_present || c.reason == reason::success, "session present on a refused CONNACK");  // [MQTT-3.2.2-6]
  require(!c.session_present || version != Version::v3_1, "MQTT 3.1 has no session present flag");
  w.u8(c.session_present ? 0x01 : 0x00);
  w.u8(c.reason);
  if (is5(version)) write_properties(w, c.properties, PropertyPlace::connack);
  return 0;
}

uint8_t encode_publish(Writer& w, const Publish& p, Version version) {
  no_properties(p.properties, version);
  require(p.qos <= QoS::exactly_once, "PUBLISH QoS over 2");
  require(p.qos != QoS::at_most_once || (!p.dup && p.packet_id == 0), "DUP or packet identifier at QoS 0");  // [MQTT-2.2.1-2], [MQTT-3.3.1-2]
  require(p.qos == QoS::at_most_once || p.packet_id != 0, "packet identifier 0");
  require(!has_wildcard(p.topic), "wildcard in a PUBLISH topic");
  require(!p.topic.empty() || (is5(version) && p.properties.topic_alias), "empty PUBLISH topic with no topic alias");
  write_string(w, p.topic);
  if (p.qos != QoS::at_most_once) w.u16(p.packet_id);
  if (is5(version)) write_properties(w, p.properties, PropertyPlace::publish);
  w.raw(p.payload);
  uint8_t flags = static_cast<uint8_t>(static_cast<uint8_t>(p.qos) << 1);
  if (p.dup) flags |= 0x08;
  if (p.retain) flags |= 0x01;
  return flags;
}

uint8_t encode_ack(Writer& w, const Ack& a, Version version) {
  require(a.type >= PacketType::puback && a.type <= PacketType::pubcomp, "not an acknowledgement type");
  require(a.packet_id != 0, "packet identifier 0");
  no_properties(a.properties, version);
  require(is5(version) ? one_of(a.reason, ack_codes(a.type)) : a.reason == reason::success, "acknowledgement code not allowed");
  w.u16(a.packet_id);
  if (is5(version) && (a.reason != reason::success || !a.properties.empty())) {
    w.u8(a.reason);
    if (!a.properties.empty()) write_properties(w, a.properties, ack_place(a.type));
  }
  return fixed_flags(a.type);
}

uint8_t encode_subscribe(Writer& w, const Subscribe& s, Version version) {
  require(s.packet_id != 0, "packet identifier 0");
  require(!s.subscriptions.empty(), "SUBSCRIBE with no topic filter");
  no_properties(s.properties, version);
  w.u16(s.packet_id);
  if (is5(version)) write_properties(w, s.properties, PropertyPlace::subscribe);
  for (const auto& sub : s.subscriptions) {
    require(sub.qos <= QoS::exactly_once, "requested QoS over 2");
    require(sub.retain_handling <= 2, "retain handling over 2");
    require(is5(version) || (!sub.no_local && !sub.retain_as_published && sub.retain_handling == 0), "5.0 subscription options on MQTT 3.x");
    require(!sub.no_local || !is_shared(sub.filter), "no local on a shared subscription");
    write_string(w, sub.filter);
    uint8_t options = static_cast<uint8_t>(sub.qos);
    if (sub.no_local) options |= 0x04;
    if (sub.retain_as_published) options |= 0x08;
    options |= static_cast<uint8_t>(sub.retain_handling << 4);
    w.u8(options);
  }
  return fixed_flags(PacketType::subscribe);
}

uint8_t encode_suback(Writer& w, const Suback& s, Version version) {
  require(s.packet_id != 0, "packet identifier 0");
  require(!s.reasons.empty(), "SUBACK with no reason codes");
  no_properties(s.properties, version);
  w.u16(s.packet_id);
  if (is5(version)) write_properties(w, s.properties, PropertyPlace::suback);
  for (const uint8_t code : s.reasons) {
    require(one_of(code, suback_codes_of(version)), "SUBACK code not allowed");
    w.u8(code);
  }
  return 0;
}

uint8_t encode_unsubscribe(Writer& w, const Unsubscribe& u, Version version) {
  require(u.packet_id != 0, "packet identifier 0");
  require(!u.filters.empty(), "UNSUBSCRIBE with no topic filter");
  no_properties(u.properties, version);
  w.u16(u.packet_id);
  if (is5(version)) write_properties(w, u.properties, PropertyPlace::unsubscribe);
  for (const auto& filter : u.filters) write_string(w, filter);
  return fixed_flags(PacketType::unsubscribe);
}

uint8_t encode_unsuback(Writer& w, const Unsuback& u, Version version) {
  require(u.packet_id != 0, "packet identifier 0");
  no_properties(u.properties, version);
  require(is5(version) ? !u.reasons.empty() : u.reasons.empty(), "UNSUBACK reason codes are 5.0's, one or more");
  w.u16(u.packet_id);
  if (is5(version)) {
    write_properties(w, u.properties, PropertyPlace::unsuback);
    for (const uint8_t code : u.reasons) {
      require(one_of(code, unsuback_codes), "UNSUBACK code not allowed");
      w.u8(code);
    }
  }
  return 0;
}

uint8_t encode_disconnect(Writer& w, const Disconnect& d, Version version) {
  no_properties(d.properties, version);
  require(is5(version) ? one_of(d.reason, disconnect_codes) : d.reason == reason::normal_disconnection, "DISCONNECT code not allowed");
  if (is5(version) && (d.reason != reason::normal_disconnection || !d.properties.empty())) {
    w.u8(d.reason);
    if (!d.properties.empty()) write_properties(w, d.properties, PropertyPlace::disconnect);
  }
  return 0;
}

uint8_t encode_auth(Writer& w, const Auth& a, Version version) {
  require(is5(version), "AUTH on MQTT 3.x");
  require(one_of(a.reason, auth_codes), "AUTH code not allowed");
  if (a.reason != reason::success || !a.properties.empty()) {
    w.u8(a.reason);
    write_properties(w, a.properties, PropertyPlace::auth);
  }
  return 0;
}

}  // namespace

PacketType type_of(const Packet& packet) noexcept {
  struct Visit {
    PacketType operator()(const Connect&) const { return PacketType::connect; }
    PacketType operator()(const Connack&) const { return PacketType::connack; }
    PacketType operator()(const Publish&) const { return PacketType::publish; }
    PacketType operator()(const Ack& a) const { return a.type; }
    PacketType operator()(const Subscribe&) const { return PacketType::subscribe; }
    PacketType operator()(const Suback&) const { return PacketType::suback; }
    PacketType operator()(const Unsubscribe&) const { return PacketType::unsubscribe; }
    PacketType operator()(const Unsuback&) const { return PacketType::unsuback; }
    PacketType operator()(const Pingreq&) const { return PacketType::pingreq; }
    PacketType operator()(const Pingresp&) const { return PacketType::pingresp; }
    PacketType operator()(const Disconnect&) const { return PacketType::disconnect; }
    PacketType operator()(const Auth&) const { return PacketType::auth; }
  };
  return std::visit(Visit{}, packet);
}

std::optional<Frame> read_frame(std::span<const uint8_t> stream, uint32_t maximum_size) {
  if (stream.empty()) return std::nullopt;
  uint32_t length = 0;
  for (size_t index = 1; index <= 4; ++index) {
    if (index >= stream.size()) return std::nullopt;
    const uint8_t next = stream[index];
    length |= static_cast<uint32_t>(next & 0x7F) << (7 * (index - 1));
    if ((next & 0x80) == 0) {
      if (index > 1 && next == 0) malformed("remaining length not in its shortest form");
      Frame frame{stream[0], index + 1, length};
      if (frame.size() > maximum_size) throw MalformedPacket(reason::packet_too_large, "packet over the maximum packet size");
      return frame;
    }
  }
  malformed("remaining length longer than four bytes");
}

Packet decode(std::span<const uint8_t> packet, Version version) {
  const auto frame = read_frame(packet, varint_max + 5);
  if (!frame || frame->size() != packet.size()) malformed("remaining length differs from the packet");
  const auto type = static_cast<PacketType>(frame->first_byte >> 4);
  const auto flags = static_cast<uint8_t>(frame->first_byte & 0x0F);
  if (type < PacketType::connect || type > PacketType::auth) malformed("reserved packet type");
  if (type == PacketType::auth && !is5(version)) malformed("AUTH on MQTT 3.x");
  if (type != PacketType::publish && flags != fixed_flags(type)) malformed("fixed header flags not allowed for the type");  // [MQTT-2.1.3-1]
  Reader r(packet.subspan(frame->header_size));
  switch (type) {
    case PacketType::connect:
      return decode_connect(r);
    case PacketType::connack:
      return decode_connack(r, version);
    case PacketType::publish:
      return decode_publish(r, flags, version);
    case PacketType::puback:
    case PacketType::pubrec:
    case PacketType::pubrel:
    case PacketType::pubcomp:
      return decode_ack(r, type, version);
    case PacketType::subscribe:
      return decode_subscribe(r, version);
    case PacketType::suback:
      return decode_suback(r, version);
    case PacketType::unsubscribe:
      return decode_unsubscribe(r, version);
    case PacketType::unsuback:
      return decode_unsuback(r, version);
    case PacketType::pingreq:
      done(r);
      return Pingreq{};
    case PacketType::pingresp:
      done(r);
      return Pingresp{};
    case PacketType::disconnect:
      return decode_disconnect(r, version);
    default:
      return decode_auth(r);  // the last type the range check above leaves
  }
}

Bytes encode(const Packet& packet, Version version) {
  Writer body;
  const PacketType type = type_of(packet);
  uint8_t flags = 0;
  std::visit(
      [&](const auto& p) {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, Connect>) {
          flags = encode_connect(body, p, version);
        } else if constexpr (std::is_same_v<T, Connack>) {
          flags = encode_connack(body, p, version);
        } else if constexpr (std::is_same_v<T, Publish>) {
          flags = encode_publish(body, p, version);
        } else if constexpr (std::is_same_v<T, Ack>) {
          flags = encode_ack(body, p, version);
        } else if constexpr (std::is_same_v<T, Subscribe>) {
          flags = encode_subscribe(body, p, version);
        } else if constexpr (std::is_same_v<T, Suback>) {
          flags = encode_suback(body, p, version);
        } else if constexpr (std::is_same_v<T, Unsubscribe>) {
          flags = encode_unsubscribe(body, p, version);
        } else if constexpr (std::is_same_v<T, Unsuback>) {
          flags = encode_unsuback(body, p, version);
        } else if constexpr (std::is_same_v<T, Disconnect>) {
          flags = encode_disconnect(body, p, version);
        } else if constexpr (std::is_same_v<T, Auth>) {
          flags = encode_auth(body, p, version);
        }
      },
      packet);
  require(body.data().size() <= varint_max, "packet over the largest remaining length");
  Writer out;
  out.u8(static_cast<uint8_t>((static_cast<uint8_t>(type) << 4) | flags));
  write_varint(out, static_cast<uint32_t>(body.data().size()));
  out.raw(body.data());
  return out.take();
}

}  // namespace lir::mqtt
