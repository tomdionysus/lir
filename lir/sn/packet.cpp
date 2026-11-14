//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "lir/sn/packet.h"

namespace lir::sn {

namespace {

uint8_t flags_byte(const Flags& f) {
  uint8_t b = 0;
  if (f.dup) b |= 0x80;
  b |= static_cast<uint8_t>((f.qos < 0 ? 3 : f.qos) << 5);
  if (f.retain) b |= 0x10;
  if (f.will) b |= 0x08;
  if (f.clean_session) b |= 0x04;
  b |= static_cast<uint8_t>(f.topic_id_type);
  return b;
}

Flags flags_of(uint8_t b) {
  Flags f;
  f.dup = (b & 0x80) != 0;
  const int q = (b >> 5) & 0x03;
  f.qos = q == 3 ? int8_t{-1} : static_cast<int8_t>(q);
  f.retain = (b & 0x10) != 0;
  f.will = (b & 0x08) != 0;
  f.clean_session = (b & 0x04) != 0;
  const int t = b & 0x03;
  if (t == 3) throw Malformed("reserved topic id type");
  f.topic_id_type = static_cast<TopicIdType>(t);
  return f;
}

class Reader {
 public:
  explicit Reader(std::span<const uint8_t> bytes) : _bytes(bytes) {}
  uint8_t u8() {
    need(1);
    return _bytes[_at++];
  }
  uint16_t u16() {
    need(2);
    const uint16_t v = static_cast<uint16_t>((_bytes[_at] << 8) | _bytes[_at + 1]);
    _at += 2;
    return v;
  }
  Bytes rest() {
    Bytes out(_bytes.begin() + static_cast<ptrdiff_t>(_at), _bytes.end());
    _at = _bytes.size();
    return out;
  }
  std::string text() {
    const auto b = rest();
    return {b.begin(), b.end()};
  }
  bool done() const { return _at == _bytes.size(); }
  void end() const {
    if (!done()) throw Malformed("trailing bytes");
  }

 private:
  void need(size_t n) const {
    if (_at + n > _bytes.size()) throw Malformed("truncated");
  }
  std::span<const uint8_t> _bytes;
  size_t _at = 0;
};

class Writer {
 public:
  void u8(uint8_t v) { _out.push_back(v); }
  void u16(uint16_t v) {
    _out.push_back(static_cast<uint8_t>(v >> 8));
    _out.push_back(static_cast<uint8_t>(v));
  }
  void bytes(const Bytes& b) { _out.insert(_out.end(), b.begin(), b.end()); }
  void text(const std::string& s) { _out.insert(_out.end(), s.begin(), s.end()); }
  Bytes framed(Type type) const {
    const size_t body = _out.size() + 1;
    Bytes out;
    out.reserve(body + 3);
    if (body + 1 <= 255) {
      out.push_back(static_cast<uint8_t>(body + 1));
    } else {
      if (body + 3 > 65535) throw std::invalid_argument("an MQTT-SN packet over 65535 bytes");
      out.push_back(0x01);
      out.push_back(static_cast<uint8_t>((body + 3) >> 8));
      out.push_back(static_cast<uint8_t>(body + 3));
    }
    out.push_back(static_cast<uint8_t>(type));
    out.insert(out.end(), _out.begin(), _out.end());
    return out;
  }

 private:
  Bytes _out;
};

// A topic carried as a name, a short name or a predefined id, by the flags.
void topic_in(Reader& r, const Flags& flags, std::string& name, uint16_t& id) {
  if (flags.topic_id_type == TopicIdType::normal) {
    name = r.text();
    if (name.empty()) throw Malformed("an empty topic name");
  } else {
    id = r.u16();
    // A short name is carried as its two characters, the id left 0.
    if (flags.topic_id_type == TopicIdType::short_name) {
      name = short_name(id);
      id = 0;
    }
    r.end();
  }
}

void topic_out(Writer& w, const Flags& flags, const std::string& name, uint16_t id) {
  if (flags.topic_id_type == TopicIdType::normal) {
    w.text(name);
  } else if (flags.topic_id_type == TopicIdType::short_name && !name.empty()) {
    w.u16(short_topic(name));
  } else {
    w.u16(id);
  }
}

}  // namespace

uint16_t short_topic(std::string_view name) {
  if (name.size() != 2) throw std::invalid_argument("a short topic name has two characters");
  return static_cast<uint16_t>((static_cast<uint8_t>(name[0]) << 8) | static_cast<uint8_t>(name[1]));
}

std::string short_name(uint16_t id) { return {static_cast<char>(id >> 8), static_cast<char>(id & 0xFF)}; }

Packet decode(std::span<const uint8_t> datagram) {
  if (datagram.size() < 2) throw Malformed("shorter than a header");
  size_t length = datagram[0];
  size_t header = 1;
  if (length == 0x01) {
    if (datagram.size() < 4) throw Malformed("truncated length");
    length = static_cast<size_t>((datagram[1] << 8) | datagram[2]);
    header = 3;
  }
  if (length < header + 1 || length > datagram.size()) throw Malformed("the length does not fit the datagram");
  const auto type = static_cast<Type>(datagram[header]);
  // An encapsulation's length runs to the end of the wireless node id; the
  // relayed packet follows it (section 5.5). Every other length is the
  // datagram's.
  if (type == Type::encapsulated) {
    if (length < header + 2) throw Malformed("an encapsulation without its control byte");
    Encapsulated p;
    p.radius = datagram[header + 1] & 0x03;
    p.wireless_node_id.assign(datagram.begin() + static_cast<ptrdiff_t>(header + 2), datagram.begin() + static_cast<ptrdiff_t>(length));
    p.packet.assign(datagram.begin() + static_cast<ptrdiff_t>(length), datagram.end());
    if (p.packet.size() < 2) throw Malformed("no encapsulated packet");
    return p;
  }
  if (length != datagram.size()) throw Malformed("the length does not match the datagram");
  Reader r(datagram.subspan(header + 1));
  switch (type) {
    case Type::advertise: {
      Advertise p{r.u8(), r.u16()};
      r.end();
      return p;
    }
    case Type::searchgw: {
      SearchGw p{r.u8()};
      r.end();
      return p;
    }
    case Type::gwinfo: {
      GwInfo p;
      p.gateway_id = r.u8();
      p.gateway_address = r.rest();
      return p;
    }
    case Type::connect: {
      Connect p;
      p.flags = flags_of(r.u8());
      p.protocol_id = r.u8();
      p.duration = r.u16();
      p.client_id = r.text();
      return p;
    }
    case Type::connack: {
      Connack p{r.u8()};
      r.end();
      return p;
    }
    case Type::willtopicreq:
      r.end();
      return WillTopicReq{};
    case Type::willtopic:
    case Type::willtopicupd: {
      // An empty WILLTOPIC, with no flags either, deletes the will.
      Flags flags;
      std::string topic;
      if (!r.done()) {
        flags = flags_of(r.u8());
        topic = r.text();
      }
      if (type == Type::willtopic) return WillTopic{flags, topic};
      return WillTopicUpd{flags, topic};
    }
    case Type::willmsgreq:
      r.end();
      return WillMsgReq{};
    case Type::willmsg:
      return WillMsg{r.rest()};
    case Type::willmsgupd:
      return WillMsgUpd{r.rest()};
    case Type::register_: {
      Register p;
      p.topic_id = r.u16();
      p.msg_id = r.u16();
      p.topic = r.text();
      if (p.topic.empty()) throw Malformed("REGISTER without a topic");
      return p;
    }
    case Type::regack: {
      Regack p;
      p.topic_id = r.u16();
      p.msg_id = r.u16();
      p.code = r.u8();
      r.end();
      return p;
    }
    case Type::publish: {
      Publish p;
      p.flags = flags_of(r.u8());
      p.topic_id = r.u16();
      p.msg_id = r.u16();
      p.data = r.rest();
      return p;
    }
    case Type::puback: {
      Puback p;
      p.topic_id = r.u16();
      p.msg_id = r.u16();
      p.code = r.u8();
      r.end();
      return p;
    }
    case Type::pubrec:
    case Type::pubrel:
    case Type::pubcomp: {
      PubFlow p{type, r.u16()};
      r.end();
      return p;
    }
    case Type::subscribe:
    case Type::unsubscribe: {
      const auto flags = flags_of(r.u8());
      const auto msg_id = r.u16();
      std::string topic;
      uint16_t id = 0;
      topic_in(r, flags, topic, id);
      if (type == Type::subscribe) return Subscribe{flags, msg_id, topic, id};
      return Unsubscribe{flags, msg_id, topic, id};
    }
    case Type::suback: {
      Suback p;
      p.flags = flags_of(r.u8());
      p.topic_id = r.u16();
      p.msg_id = r.u16();
      p.code = r.u8();
      r.end();
      return p;
    }
    case Type::unsuback: {
      Unsuback p{r.u16()};
      r.end();
      return p;
    }
    case Type::pingreq:
      return Pingreq{r.text()};
    case Type::pingresp:
      r.end();
      return Pingresp{};
    case Type::disconnect: {
      Disconnect p;
      if (!r.done()) p.duration = r.u16();
      r.end();
      return p;
    }
    case Type::willtopicresp:
    case Type::willmsgresp: {
      WillResp p{type, r.u8()};
      r.end();
      return p;
    }
    case Type::encapsulated:
      break;
  }
  throw Malformed("an unknown type");
}

Bytes encode(const Packet& packet) {
  Writer w;
  return std::visit(
      [&w](const auto& p) -> Bytes {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, Advertise>) {
          w.u8(p.gateway_id);
          w.u16(p.duration);
          return w.framed(Type::advertise);
        } else if constexpr (std::is_same_v<T, SearchGw>) {
          w.u8(p.radius);
          return w.framed(Type::searchgw);
        } else if constexpr (std::is_same_v<T, GwInfo>) {
          w.u8(p.gateway_id);
          w.bytes(p.gateway_address);
          return w.framed(Type::gwinfo);
        } else if constexpr (std::is_same_v<T, Connect>) {
          w.u8(flags_byte(p.flags));
          w.u8(p.protocol_id);
          w.u16(p.duration);
          w.text(p.client_id);
          return w.framed(Type::connect);
        } else if constexpr (std::is_same_v<T, Connack>) {
          w.u8(p.code);
          return w.framed(Type::connack);
        } else if constexpr (std::is_same_v<T, WillTopicReq>) {
          return w.framed(Type::willtopicreq);
        } else if constexpr (std::is_same_v<T, WillTopic>) {
          if (!p.topic.empty()) {
            w.u8(flags_byte(p.flags));
            w.text(p.topic);
          }
          return w.framed(Type::willtopic);
        } else if constexpr (std::is_same_v<T, WillMsgReq>) {
          return w.framed(Type::willmsgreq);
        } else if constexpr (std::is_same_v<T, WillMsg>) {
          w.bytes(p.message);
          return w.framed(Type::willmsg);
        } else if constexpr (std::is_same_v<T, Register>) {
          w.u16(p.topic_id);
          w.u16(p.msg_id);
          w.text(p.topic);
          return w.framed(Type::register_);
        } else if constexpr (std::is_same_v<T, Regack>) {
          w.u16(p.topic_id);
          w.u16(p.msg_id);
          w.u8(p.code);
          return w.framed(Type::regack);
        } else if constexpr (std::is_same_v<T, Publish>) {
          w.u8(flags_byte(p.flags));
          w.u16(p.topic_id);
          w.u16(p.msg_id);
          w.bytes(p.data);
          return w.framed(Type::publish);
        } else if constexpr (std::is_same_v<T, Puback>) {
          w.u16(p.topic_id);
          w.u16(p.msg_id);
          w.u8(p.code);
          return w.framed(Type::puback);
        } else if constexpr (std::is_same_v<T, PubFlow>) {
          w.u16(p.msg_id);
          return w.framed(p.type);
        } else if constexpr (std::is_same_v<T, Subscribe> || std::is_same_v<T, Unsubscribe>) {
          w.u8(flags_byte(p.flags));
          w.u16(p.msg_id);
          topic_out(w, p.flags, p.topic, p.topic_id);
          return w.framed(std::is_same_v<T, Subscribe> ? Type::subscribe : Type::unsubscribe);
        } else if constexpr (std::is_same_v<T, Suback>) {
          w.u8(flags_byte(p.flags));
          w.u16(p.topic_id);
          w.u16(p.msg_id);
          w.u8(p.code);
          return w.framed(Type::suback);
        } else if constexpr (std::is_same_v<T, Unsuback>) {
          w.u16(p.msg_id);
          return w.framed(Type::unsuback);
        } else if constexpr (std::is_same_v<T, Pingreq>) {
          w.text(p.client_id);
          return w.framed(Type::pingreq);
        } else if constexpr (std::is_same_v<T, Pingresp>) {
          return w.framed(Type::pingresp);
        } else if constexpr (std::is_same_v<T, Disconnect>) {
          if (p.duration) w.u16(*p.duration);
          return w.framed(Type::disconnect);
        } else if constexpr (std::is_same_v<T, WillTopicUpd>) {
          if (!p.topic.empty()) {
            w.u8(flags_byte(p.flags));
            w.text(p.topic);
          }
          return w.framed(Type::willtopicupd);
        } else if constexpr (std::is_same_v<T, WillMsgUpd>) {
          w.bytes(p.message);
          return w.framed(Type::willmsgupd);
        } else if constexpr (std::is_same_v<T, WillResp>) {
          w.u8(p.code);
          return w.framed(p.type);
        } else {
          static_assert(std::is_same_v<T, Encapsulated>);
          w.u8(p.radius & 0x03);
          w.bytes(p.wireless_node_id);
          auto out = w.framed(Type::encapsulated);
          out.insert(out.end(), p.packet.begin(), p.packet.end());
          return out;
        }
      },
      packet);
}

}  // namespace lir::sn
