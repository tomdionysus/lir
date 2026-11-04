//
// Lir - The Distributed Event System
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "lir/cluster/codec.h"

#include <chrono>

namespace lir::cluster {

namespace {

using dannan::net::DecodeError;
using dannan::net::Reader;
using dannan::net::Writer;

// Counts are bounded before anything is allocated for them.
constexpr uint32_t maximum_count = 1 << 20;

void header(Writer& w) { w.u8(format); }

void header(Reader& r) {
  if (r.u8() != format) throw DecodeError("unknown Lir cluster format");
}

uint32_t count(Reader& r) {
  const uint32_t n = r.u32();
  if (n > maximum_count) throw DecodeError("count over the bound");
  return n;
}

template <typename T, typename Write>
void optional(Writer& w, const std::optional<T>& value, Write write) {
  w.u8(value ? 1 : 0);
  if (value) write(*value);
}

bool present(Reader& r) {
  const uint8_t flag = r.u8();
  if (flag > 1) throw DecodeError("presence flag not 0 or 1");
  return flag == 1;
}

mqtt::QoS qos(Reader& r) {
  const uint8_t value = r.u8();
  if (value > 2) throw DecodeError("QoS over 2");
  return static_cast<mqtt::QoS>(value);
}

void write_message(Writer& w, const broker::Message& m, TimePoint now) {
  w.string(m.topic);
  w.bytes(m.payload);
  w.u8(static_cast<uint8_t>(m.qos));
  w.u8(m.retain ? 1 : 0);
  optional(w, m.payload_format_indicator, [&w](uint8_t v) { w.u8(v); });
  optional(w, m.expires_at, [&w, now](TimePoint at) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(at - now).count();
    w.u64(static_cast<uint64_t>(std::max<int64_t>(left, 0)));
  });
  optional(w, m.content_type, [&w](const std::string& v) { w.string(v); });
  optional(w, m.response_topic, [&w](const std::string& v) { w.string(v); });
  optional(w, m.correlation_data, [&w](const Bytes& v) { w.bytes(v); });
  w.u32(static_cast<uint32_t>(m.user_properties.size()));
  for (const auto& [key, value] : m.user_properties) {
    w.string(key);
    w.string(value);
  }
  w.string(m.origin);
  optional(w, m.version, [&w](const broker::RetainedVersion& v) {
    w.u64(v.lamport);
    w.string(v.node);
  });
}

broker::Message read_message(Reader& r, TimePoint now) {
  broker::Message m;
  m.topic = r.string();
  m.payload = r.bytes();
  m.qos = qos(r);
  m.retain = present(r);
  if (present(r)) m.payload_format_indicator = r.u8();
  if (present(r)) m.expires_at = now + std::chrono::milliseconds(r.u64());
  if (present(r)) m.content_type = r.string();
  if (present(r)) m.response_topic = r.string();
  if (present(r)) m.correlation_data = r.bytes();
  const uint32_t properties = count(r);
  for (uint32_t i = 0; i < properties; ++i) {
    auto key = r.string();
    auto value = r.string();
    m.user_properties.emplace_back(std::move(key), std::move(value));
  }
  m.origin = r.string();
  if (present(r)) {
    broker::RetainedVersion v;
    v.lamport = r.u64();
    v.node = r.string();
    m.version = std::move(v);
  }
  return m;
}

void write_options(Writer& w, const broker::Options& o) {
  w.u8(static_cast<uint8_t>(o.qos));
  w.u8(o.no_local ? 1 : 0);
  w.u8(o.retain_as_published ? 1 : 0);
  w.u8(o.retain_handling);
  optional(w, o.identifier, [&w](uint32_t v) { w.u32(v); });
}

broker::Options read_options(Reader& r) {
  broker::Options o;
  o.qos = qos(r);
  o.no_local = present(r);
  o.retain_as_published = present(r);
  o.retain_handling = r.u8();
  if (o.retain_handling > 2) throw DecodeError("retain handling over 2");
  if (present(r)) o.identifier = r.u32();
  return o;
}

void write_delivery(Writer& w, const broker::Delivery& d, TimePoint now) {
  write_message(w, d.message, now);
  w.u8(static_cast<uint8_t>(d.qos));
  w.u8(d.retain ? 1 : 0);
  w.u32(static_cast<uint32_t>(d.identifiers.size()));
  for (const uint32_t id : d.identifiers) w.u32(id);
}

broker::Delivery read_delivery(Reader& r, TimePoint now) {
  broker::Delivery d;
  d.message = read_message(r, now);
  d.qos = qos(r);
  d.retain = present(r);
  const uint32_t n = count(r);
  for (uint32_t i = 0; i < n; ++i) d.identifiers.push_back(r.u32());
  return d;
}

void write_image(Writer& w, const broker::SessionImage& image, TimePoint now) {
  w.string(image.client_id);
  w.u8(static_cast<uint8_t>(image.version));
  w.u32(image.expiry_interval);
  w.u32(static_cast<uint32_t>(image.subscriptions.size()));
  for (const auto& [filter, options] : image.subscriptions) {
    w.string(filter);
    write_options(w, options);
  }
  w.u32(static_cast<uint32_t>(image.outbound.size()));
  for (const auto& [id, inflight] : image.outbound) {
    w.u16(id);
    w.u8(static_cast<uint8_t>(inflight.state));
    write_delivery(w, inflight.delivery, now);
  }
  w.u32(static_cast<uint32_t>(image.queued.size()));
  for (const auto& delivery : image.queued) write_delivery(w, delivery, now);
  w.u32(static_cast<uint32_t>(image.inbound_qos2.size()));
  for (const uint16_t id : image.inbound_qos2) w.u16(id);
  w.u16(image.next_packet_id);
}

broker::SessionImage read_image(Reader& r, TimePoint now) {
  broker::SessionImage image;
  image.client_id = r.string();
  const uint8_t version = r.u8();
  if (version < 3 || version > 5) throw DecodeError("unknown MQTT version");
  image.version = static_cast<mqtt::Version>(version);
  image.expiry_interval = r.u32();
  const uint32_t subscriptions = count(r);
  for (uint32_t i = 0; i < subscriptions; ++i) {
    auto filter = r.string();
    image.subscriptions.emplace_back(std::move(filter), read_options(r));
  }
  const uint32_t outbound = count(r);
  for (uint32_t i = 0; i < outbound; ++i) {
    const uint16_t id = r.u16();
    const uint8_t state = r.u8();
    if (state > 2) throw DecodeError("unknown inflight state");
    broker::Inflight inflight;
    inflight.state = static_cast<broker::Inflight::State>(state);
    inflight.delivery = read_delivery(r, now);
    image.outbound.emplace_back(id, std::move(inflight));
  }
  const uint32_t queued = count(r);
  for (uint32_t i = 0; i < queued; ++i) image.queued.push_back(read_delivery(r, now));
  const uint32_t inbound = count(r);
  for (uint32_t i = 0; i < inbound; ++i) image.inbound_qos2.push_back(r.u16());
  image.next_packet_id = r.u16();
  return image;
}

}  // namespace

Bytes encode_message(const broker::Message& message, TimePoint now) {
  Writer w;
  header(w);
  write_message(w, message, now);
  return w.take();
}

broker::Message decode_message(std::span<const uint8_t> bytes, TimePoint now) {
  Reader r(bytes);
  header(r);
  auto m = read_message(r, now);
  r.finish();
  return m;
}

Bytes encode_messages(const std::vector<broker::Message>& messages, TimePoint now) {
  Writer w;
  header(w);
  w.u32(static_cast<uint32_t>(messages.size()));
  for (const auto& m : messages) write_message(w, m, now);
  return w.take();
}

std::vector<broker::Message> decode_messages(std::span<const uint8_t> bytes, TimePoint now) {
  Reader r(bytes);
  header(r);
  std::vector<broker::Message> out;
  const uint32_t n = count(r);
  for (uint32_t i = 0; i < n; ++i) out.push_back(read_message(r, now));
  r.finish();
  return out;
}

Bytes encode_image(const broker::SessionImage& image, TimePoint now) {
  Writer w;
  header(w);
  write_image(w, image, now);
  return w.take();
}

broker::SessionImage decode_image(std::span<const uint8_t> bytes, TimePoint now) {
  Reader r(bytes);
  header(r);
  auto image = read_image(r, now);
  r.finish();
  return image;
}

Bytes encode_groups(const Groups& groups) {
  Writer w;
  header(w);
  w.u32(static_cast<uint32_t>(groups.size()));
  for (const auto& [group, filter] : groups) {
    w.string(group);
    w.string(filter);
  }
  return w.take();
}

Groups decode_groups(std::span<const uint8_t> bytes) {
  Reader r(bytes);
  header(r);
  Groups out;
  const uint32_t n = count(r);
  for (uint32_t i = 0; i < n; ++i) {
    auto group = r.string();
    auto filter = r.string();
    out.emplace(std::move(group), std::move(filter));
  }
  r.finish();
  return out;
}

Bytes encode_take(const Take& take) {
  Writer w;
  header(w);
  w.string(take.client_id);
  w.u8(take.want_state ? 1 : 0);
  return w.take();
}

Take decode_take(std::span<const uint8_t> bytes) {
  Reader r(bytes);
  header(r);
  Take take;
  take.client_id = r.string();
  take.want_state = present(r);
  r.finish();
  return take;
}

Bytes encode_taken(const std::optional<broker::SessionImage>& image, TimePoint now) {
  Writer w;
  header(w);
  w.u8(image ? 1 : 0);
  if (image) write_image(w, *image, now);
  return w.take();
}

std::optional<broker::SessionImage> decode_taken(std::span<const uint8_t> bytes, TimePoint now) {
  Reader r(bytes);
  header(r);
  std::optional<broker::SessionImage> out;
  if (present(r)) out = read_image(r, now);
  r.finish();
  return out;
}

Bytes encode_shared(const SharedDelivery& delivery, TimePoint now) {
  Writer w;
  header(w);
  w.string(delivery.group);
  w.string(delivery.filter);
  write_message(w, delivery.message, now);
  return w.take();
}

SharedDelivery decode_shared(std::span<const uint8_t> bytes, TimePoint now) {
  Reader r(bytes);
  header(r);
  SharedDelivery delivery;
  delivery.group = r.string();
  delivery.filter = r.string();
  delivery.message = read_message(r, now);
  r.finish();
  return delivery;
}

}  // namespace lir::cluster
