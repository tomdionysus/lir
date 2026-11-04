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

namespace lir::mqtt {

namespace {

using dannan::net::Reader;
using dannan::net::Writer;

bool v5(Version version) { return version == Version::v5; }

bool one_of(uint8_t value, std::initializer_list<uint8_t> allowed) { return std::find(allowed.begin(), allowed.end(), value) != allowed.end(); }

// The reason codes each packet may carry on 5.0 (section 2.4 and each
// packet's own table).
const std::initializer_list<uint8_t> connack_reasons = {0x00, 0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
                                                        0x8A, 0x8C, 0x90, 0x95, 0x97, 0x99, 0x9A, 0x9B, 0x9C, 0x9D, 0x9F};
const std::initializer_list<uint8_t> puback_reasons = {0x00, 0x10, 0x80, 0x83, 0x87, 0x90, 0x91, 0x97, 0x99};
const std::initializer_list<uint8_t> pubrel_reasons = {0x00, 0x92};
const std::initializer_list<uint8_t> suback_reasons = {0x00, 0x01, 0x02, 0x80, 0x83, 0x87, 0x8F, 0x91, 0x97, 0x9E, 0xA1, 0xA2};
const std::initializer_list<uint8_t> unsuback_reasons = {0x00, 0x11, 0x80, 0x83, 0x87, 0x8F, 0x91};
const std::initializer_list<uint8_t> disconnect_reasons = {0x00, 0x04, 0x80, 0x81, 0x82, 0x83, 0x87, 0x89, 0x8B, 0x8D, 0x8E, 0x8F, 0x90, 0x93, 0x94,
                                                           0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0x9F, 0xA0, 0xA1, 0xA2};
const std::initializer_list<uint8_t> auth_reasons = {0x00, 0x18, 0x19};
const std::initializer_list<uint8_t> v3_suback_reasons = {0x00, 0x01, 0x02, 0x80};

const std::initializer_list<uint8_t>& ack_reasons(PacketType type) {
  return type == PacketType::puback || type == PacketType::pubrec ? puback_reasons : pubrel_reasons;
}

PropertyPlace ack_place(PacketType type) {
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

uint8_t fixed_flags(PacketType type) { return type == PacketType::pubrel || type == PacketType::subscribe || type == PacketType::unsubscribe ? 0x02 : 0x00; }

// ---- decode

uint16_t packet_id(Reader& reader) {
  if (reader.remaining() < 2) malformed("packet identifier runs past the packet");
  const uint16_t id = reader.u16();
  if (id == 0) protocol_error("packet identifier 0");
  return id;
}

uint8_t byte(Reader& reader) {
  if (reader.remaining() < 1) malformed("packet ends early");
  return reader.u8();
}

void done(const Reader& reader) {
  if (reader.remaining() != 0) malformed("bytes left over at the end of the packet");
}

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
    throw UnsupportedVersion(level, "unsupported protocol name or level");
  }
  const uint8_t flags = byte(r);
  if (flags & 0x01) malformed("CONNECT reserved flag set");
  c.clean_start = (flags & 0x02) != 0;
  const bool will = (flags & 0x04) != 0;
  const uint8_t will_qos = (flags >> 3) & 0x03;
  const bool will_retain = (flags & 0x20) != 0;
  const bool has_password = (flags & 0x40) != 0;
  const bool has_username = (flags & 0x80) != 0;
  if (!will && (will_qos != 0 || will_retain)) malformed("will QoS or retain set without a will");
  if (will_qos == 3) malformed("will QoS 3");
  if (!v5(c.version) && has_password && !has_username) malformed("password without a user name");
  if (r.remaining() < 2) malformed("CONNECT ends early");
  c.keep_alive = r.u16();
  if (v5(c.version)) c.properties = read_properties(r, PropertyPlace::connect);
  c.client_id = read_string(r);
  if (will) {
    Will w;
    if (v5(c.version)) w.properties = read_properties(r, PropertyPlace::will);
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
  if (flags & 0xFE) malformed("CONNACK reserved flags set");
  c.session_present = (flags & 0x01) != 0;
  c.reason = byte(r);
  if (v5(version)) {
    if (!one_of(c.reason, connack_reasons)) protocol_error("CONNACK reason code not allowed");
    c.properties = read_properties(r, PropertyPlace::connack);
  } else if (c.reason > 5) {
    protocol_error("CONNACK return code over 5");
  }
  if (c.session_present && c.reason != reason::success) protocol_error("session present on a refused CONNACK");
  done(r);
  return c;
}

Publish decode_publish(Reader& r, uint8_t flags, Version version) {
  Publish p;
  p.dup = (flags & 0x08) != 0;
  const uint8_t qos = (flags >> 1) & 0x03;
  if (qos == 3) malformed("PUBLISH QoS 3");
  if (qos == 0 && p.dup) malformed("DUP set at QoS 0");
  p.qos = static_cast<QoS>(qos);
  p.retain = (flags & 0x01) != 0;
  p.topic = read_string(r);
  if (qos > 0) p.packet_id = packet_id(r);
  if (v5(version)) p.properties = read_properties(r, PropertyPlace::publish);
  if (p.topic.empty() && !p.properties.topic_alias) protocol_error("PUBLISH with no topic and no topic alias");
  p.payload = r.raw(r.remaining());
  return p;
}

Ack decode_ack(Reader& r, PacketType type, Version version) {
  Ack a;
  a.type = type;
  a.packet_id = packet_id(r);
  if (v5(version) && r.remaining() > 0) {
    a.reason = byte(r);
    if (!one_of(a.reason, ack_reasons(type))) protocol_error("acknowledgement reason code not allowed");
    if (r.remaining() > 0) a.properties = read_properties(r, ack_place(type));
  }
  done(r);
  return a;
}

Subscribe decode_subscribe(Reader& r, Version version) {
  Subscribe s;
  s.packet_id = packet_id(r);
  if (v5(version)) s.properties = read_properties(r, PropertyPlace::subscribe);
  while (r.remaining() > 0) {
    Subscription sub;
    sub.filter = read_string(r);
    const uint8_t options = byte(r);
    const uint8_t qos = options & 0x03;
    if (qos == 3) malformed("subscription QoS 3");
    sub.qos = static_cast<QoS>(qos);
    if (v5(version)) {
      if (options & 0xC0) malformed("subscription options reserved bits set");
      sub.no_local = (options & 0x04) != 0;
      sub.retain_as_published = (options & 0x08) != 0;
      sub.retain_handling = (options >> 4) & 0x03;
      if (sub.retain_handling == 3) malformed("retain handling 3");
    } else if (options & 0xFC) {
      malformed("subscription reserved bits set");
    }
    s.subscriptions.push_back(std::move(sub));
  }
  if (s.subscriptions.empty()) protocol_error("SUBSCRIBE with no subscriptions");
  return s;
}

Suback decode_suback(Reader& r, Version version) {
  Suback s;
  s.packet_id = packet_id(r);
  if (v5(version)) s.properties = read_properties(r, PropertyPlace::suback);
  while (r.remaining() > 0) {
    const uint8_t code = r.u8();
    if (!one_of(code, v5(version) ? suback_reasons : v3_suback_reasons)) protocol_error("SUBACK reason code not allowed");
    s.reasons.push_back(code);
  }
  if (s.reasons.empty()) protocol_error("SUBACK with no reason codes");
  return s;
}

Unsubscribe decode_unsubscribe(Reader& r, Version version) {
  Unsubscribe u;
  u.packet_id = packet_id(r);
  if (v5(version)) u.properties = read_properties(r, PropertyPlace::unsubscribe);
  while (r.remaining() > 0) u.filters.push_back(read_string(r));
  if (u.filters.empty()) protocol_error("UNSUBSCRIBE with no topic filters");
  return u;
}

Unsuback decode_unsuback(Reader& r, Version version) {
  Unsuback u;
  u.packet_id = packet_id(r);
  if (v5(version)) {
    u.properties = read_properties(r, PropertyPlace::unsuback);
    while (r.remaining() > 0) {
      const uint8_t code = r.u8();
      if (!one_of(code, unsuback_reasons)) protocol_error("UNSUBACK reason code not allowed");
      u.reasons.push_back(code);
    }
    if (u.reasons.empty()) protocol_error("UNSUBACK with no reason codes");
  }
  done(r);
  return u;
}

Disconnect decode_disconnect(Reader& r, Version version) {
  Disconnect d;
  if (v5(version) && r.remaining() > 0) {
    d.reason = byte(r);
    if (!one_of(d.reason, disconnect_reasons)) protocol_error("DISCONNECT reason code not allowed");
    if (r.remaining() > 0) d.properties = read_properties(r, PropertyPlace::disconnect);
  }
  done(r);
  return d;
}

Auth decode_auth(Reader& r) {
  Auth a;
  if (r.remaining() > 0) {
    a.reason = byte(r);
    if (!one_of(a.reason, auth_reasons)) protocol_error("AUTH reason code not allowed");
    a.properties = read_properties(r, PropertyPlace::auth);
  }
  done(r);
  return a;
}

// ---- encode

void require(bool condition, const char* what) {
  if (!condition) throw std::invalid_argument(what);
}

void no_properties(const Properties& properties, Version version) { require(v5(version) || properties.empty(), "properties on an MQTT 3.x packet"); }

void encode_connect(Writer& w, const Connect& c, Version version) {
  require(c.version == version, "CONNECT version differs from the session's");
  no_properties(c.properties, version);
  if (c.will) no_properties(c.will->properties, version);
  require(!c.will || c.will->qos <= QoS::exactly_once, "will QoS over 2");
  require(v5(version) || !c.password || c.username, "password without a user name");
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
  if (v5(version)) write_properties(w, c.properties, PropertyPlace::connect);
  write_string(w, c.client_id);
  if (c.will) {
    if (v5(version)) write_properties(w, c.will->properties, PropertyPlace::will);
    write_string(w, c.will->topic);
    write_binary(w, c.will->payload);
  }
  if (c.username) write_string(w, *c.username);
  if (c.password) write_binary(w, *c.password);
}

void encode_connack(Writer& w, const Connack& c, Version version) {
  no_properties(c.properties, version);
  require(v5(version) ? one_of(c.reason, connack_reasons) : c.reason <= 5, "CONNACK reason code not allowed");
  require(!c.session_present || c.reason == reason::success, "session present on a refused CONNACK");
  w.u8(c.session_present ? 0x01 : 0x00);
  w.u8(c.reason);
  if (v5(version)) write_properties(w, c.properties, PropertyPlace::connack);
}

uint8_t encode_publish(Writer& w, const Publish& p, Version version) {
  no_properties(p.properties, version);
  require(p.qos <= QoS::exactly_once, "PUBLISH QoS over 2");
  require(p.qos != QoS::at_most_once || (!p.dup && p.packet_id == 0), "DUP or packet identifier at QoS 0");
  require(p.qos == QoS::at_most_once || p.packet_id != 0, "packet identifier 0");
  require(!p.topic.empty() || p.properties.topic_alias, "PUBLISH with no topic and no topic alias");
  write_string(w, p.topic);
  if (p.qos != QoS::at_most_once) w.u16(p.packet_id);
  if (v5(version)) write_properties(w, p.properties, PropertyPlace::publish);
  w.raw(p.payload);
  uint8_t flags = static_cast<uint8_t>(static_cast<uint8_t>(p.qos) << 1);
  if (p.dup) flags |= 0x08;
  if (p.retain) flags |= 0x01;
  return flags;
}

void encode_ack(Writer& w, const Ack& a, Version version) {
  require(a.type >= PacketType::puback && a.type <= PacketType::pubcomp, "not an acknowledgement type");
  require(a.packet_id != 0, "packet identifier 0");
  no_properties(a.properties, version);
  require(v5(version) ? one_of(a.reason, ack_reasons(a.type)) : a.reason == reason::success, "acknowledgement reason code not allowed");
  w.u16(a.packet_id);
  if (!v5(version) || (a.reason == reason::success && a.properties.empty())) return;
  w.u8(a.reason);
  if (!a.properties.empty()) write_properties(w, a.properties, ack_place(a.type));
}

void encode_subscribe(Writer& w, const Subscribe& s, Version version) {
  require(s.packet_id != 0, "packet identifier 0");
  require(!s.subscriptions.empty(), "SUBSCRIBE with no subscriptions");
  no_properties(s.properties, version);
  w.u16(s.packet_id);
  if (v5(version)) write_properties(w, s.properties, PropertyPlace::subscribe);
  for (const auto& sub : s.subscriptions) {
    require(sub.qos <= QoS::exactly_once, "subscription QoS over 2");
    require(sub.retain_handling <= 2, "retain handling over 2");
    require(v5(version) || (!sub.no_local && !sub.retain_as_published && sub.retain_handling == 0), "5.0 subscription options on MQTT 3.x");
    write_string(w, sub.filter);
    uint8_t options = static_cast<uint8_t>(sub.qos);
    if (sub.no_local) options |= 0x04;
    if (sub.retain_as_published) options |= 0x08;
    options |= static_cast<uint8_t>(sub.retain_handling << 4);
    w.u8(options);
  }
}

void encode_suback(Writer& w, const Suback& s, Version version) {
  require(s.packet_id != 0, "packet identifier 0");
  require(!s.reasons.empty(), "SUBACK with no reason codes");
  no_properties(s.properties, version);
  w.u16(s.packet_id);
  if (v5(version)) write_properties(w, s.properties, PropertyPlace::suback);
  for (const uint8_t code : s.reasons) {
    require(one_of(code, v5(version) ? suback_reasons : v3_suback_reasons), "SUBACK reason code not allowed");
    w.u8(code);
  }
}

void encode_unsubscribe(Writer& w, const Unsubscribe& u, Version version) {
  require(u.packet_id != 0, "packet identifier 0");
  require(!u.filters.empty(), "UNSUBSCRIBE with no topic filters");
  no_properties(u.properties, version);
  w.u16(u.packet_id);
  if (v5(version)) write_properties(w, u.properties, PropertyPlace::unsubscribe);
  for (const auto& filter : u.filters) write_string(w, filter);
}

void encode_unsuback(Writer& w, const Unsuback& u, Version version) {
  require(u.packet_id != 0, "packet identifier 0");
  no_properties(u.properties, version);
  require(v5(version) ? !u.reasons.empty() : u.reasons.empty(), "UNSUBACK reason codes are 5.0's, and 5.0 needs one");
  w.u16(u.packet_id);
  if (!v5(version)) return;
  write_properties(w, u.properties, PropertyPlace::unsuback);
  for (const uint8_t code : u.reasons) {
    require(one_of(code, unsuback_reasons), "UNSUBACK reason code not allowed");
    w.u8(code);
  }
}

void encode_disconnect(Writer& w, const Disconnect& d, Version version) {
  no_properties(d.properties, version);
  require(v5(version) ? one_of(d.reason, disconnect_reasons) : d.reason == reason::normal_disconnection, "DISCONNECT reason code not allowed");
  if (!v5(version) || (d.reason == reason::normal_disconnection && d.properties.empty())) return;
  w.u8(d.reason);
  if (!d.properties.empty()) write_properties(w, d.properties, PropertyPlace::disconnect);
}

void encode_auth(Writer& w, const Auth& a, Version version) {
  require(v5(version), "AUTH on MQTT 3.x");
  require(one_of(a.reason, auth_reasons), "AUTH reason code not allowed");
  if (a.reason == reason::success && a.properties.empty()) return;
  w.u8(a.reason);
  write_properties(w, a.properties, PropertyPlace::auth);
}

}  // namespace

PacketType type_of(const Packet& packet) noexcept {
  return std::visit(
      [](const auto& p) -> PacketType {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, Connect>) return PacketType::connect;
        if constexpr (std::is_same_v<T, Connack>) return PacketType::connack;
        if constexpr (std::is_same_v<T, Publish>) return PacketType::publish;
        if constexpr (std::is_same_v<T, Ack>) return p.type;
        if constexpr (std::is_same_v<T, Subscribe>) return PacketType::subscribe;
        if constexpr (std::is_same_v<T, Suback>) return PacketType::suback;
        if constexpr (std::is_same_v<T, Unsubscribe>) return PacketType::unsubscribe;
        if constexpr (std::is_same_v<T, Unsuback>) return PacketType::unsuback;
        if constexpr (std::is_same_v<T, Pingreq>) return PacketType::pingreq;
        if constexpr (std::is_same_v<T, Pingresp>) return PacketType::pingresp;
        if constexpr (std::is_same_v<T, Disconnect>) return PacketType::disconnect;
        return PacketType::auth;
      },
      packet);
}

std::optional<Frame> read_frame(std::span<const uint8_t> stream, uint32_t maximum_size) {
  if (stream.empty()) return std::nullopt;
  uint32_t length = 0;
  uint32_t multiplier = 1;
  for (size_t i = 1; i <= 4; ++i) {
    if (i >= stream.size()) return std::nullopt;
    const uint8_t b = stream[i];
    length += static_cast<uint32_t>(b & 0x7F) * multiplier;
    if ((b & 0x80) == 0) {
      if (i > 1 && b == 0) malformed("remaining length not in its shortest form");
      Frame frame{stream[0], i + 1, length};
      if (frame.size() > maximum_size) throw MalformedPacket(reason::packet_too_large, "packet over the maximum packet size");
      return frame;
    }
    multiplier *= 128;
  }
  malformed("remaining length longer than four bytes");
}

Packet decode(std::span<const uint8_t> packet, Version version) {
  const auto frame = read_frame(packet, UINT32_MAX);
  if (!frame) malformed("fixed header incomplete");
  if (frame->size() != packet.size()) malformed("remaining length does not match the packet");
  const auto type = static_cast<PacketType>(frame->first_byte >> 4);
  const uint8_t flags = frame->first_byte & 0x0F;
  const auto raw_type = static_cast<uint8_t>(type);
  if (raw_type == 0 || (type == PacketType::auth && !v5(version))) malformed("reserved packet type");
  if (type != PacketType::publish && flags != fixed_flags(type)) malformed("fixed header flags not as specified");
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
    case PacketType::auth:
      return decode_auth(r);
  }
  malformed("reserved packet type");
}

Bytes encode(const Packet& packet, Version version) {
  const PacketType type = type_of(packet);
  uint8_t flags = fixed_flags(type);
  Writer body;
  std::visit(
      [&](const auto& p) {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, Connect>) encode_connect(body, p, version);
        if constexpr (std::is_same_v<T, Connack>) encode_connack(body, p, version);
        if constexpr (std::is_same_v<T, Publish>) flags = encode_publish(body, p, version);
        if constexpr (std::is_same_v<T, Ack>) encode_ack(body, p, version);
        if constexpr (std::is_same_v<T, Subscribe>) encode_subscribe(body, p, version);
        if constexpr (std::is_same_v<T, Suback>) encode_suback(body, p, version);
        if constexpr (std::is_same_v<T, Unsubscribe>) encode_unsubscribe(body, p, version);
        if constexpr (std::is_same_v<T, Unsuback>) encode_unsuback(body, p, version);
        if constexpr (std::is_same_v<T, Disconnect>) encode_disconnect(body, p, version);
        if constexpr (std::is_same_v<T, Auth>) encode_auth(body, p, version);
      },
      packet);
  const Bytes& content = body.data();
  if (content.size() > varint_max) throw std::length_error("packet over the largest remaining length");
  Writer out;
  out.u8(static_cast<uint8_t>((static_cast<uint8_t>(type) << 4) | flags));
  write_varint(out, static_cast<uint32_t>(content.size()));
  out.raw(content);
  return out.take();
}

}  // namespace lir::mqtt
