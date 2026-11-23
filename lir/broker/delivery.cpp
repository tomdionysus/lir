//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "lir/broker/delivery.h"

#include <stdexcept>
#include <utility>
#include <variant>

#include "lir/mqtt/topic.h"

namespace lir::broker {

std::string interest_prefix(std::string_view filter) {
  std::string out;
  for (const auto level : mqtt::levels(filter)) {
    if (level == "+") return out;
    if (level == "#") {
      if (!out.empty()) out.pop_back();
      return out;
    }
    out.append(level);
    out.push_back('/');
  }
  out.pop_back();
  return out;
}

// Ordered: at most once, and in order per origin (Dannan 0.11.1), so every
// topic is an ordered topic [MQTT-4.6.0-6]; a late message is dropped, as
// QoS 0 allows. Nothing is replayed.
Delivery::Delivery(dannan::Channels& channels) : _channels(channels) { _channels.open(std::string(channel_name), dannan::net::Delivery::ordered, 0); }

uint64_t Delivery::subscribe(std::string filter, boost::asio::any_io_executor executor, Handler handler, std::chrono::milliseconds deadline,
                             boost::asio::any_io_executor done_executor, std::function<void(dannan::membership::Pushed)> held) {
  if (mqtt::is_shared(filter) || !mqtt::valid_topic_filter(filter)) throw std::invalid_argument("lir::broker::Delivery: not a filter a node holds: " + filter);
  auto prefix = interest_prefix(filter);
  auto deliver = [filter = std::move(filter), handler = std::move(handler)](dannan::net::ChannelMessage message) {
    if (!mqtt::matches(filter, message.topic)) return;
    // Only Lir's nodes publish on the channel, and each checked its packet
    // before sending it: one that fails here came from a node built with
    // another codec, and is not this subscription's to guess at.
    std::string publisher;
    mqtt::Packet packet;
    try {
      dannan::net::Reader reader(message.payload);
      publisher = mqtt::read_string(reader);
      packet = mqtt::decode(std::span<const uint8_t>(message.payload).subspan(message.payload.size() - reader.remaining()), mqtt::Version::v5);
    } catch (const dannan::net::DecodeError&) {
      return;
    }
    const auto* publish = std::get_if<mqtt::Publish>(&packet);
    if (publish == nullptr || publish->topic != message.topic) return;
    handler(*publish, publisher);
  };
  return _channels.subscribe(std::string(channel_name), std::move(prefix), std::move(executor), std::move(deliver), deadline, std::move(done_executor),
                             std::move(held));
}

void Delivery::unsubscribe(uint64_t id) { _channels.unsubscribe(id); }

dannan::net::Published Delivery::publish(const mqtt::Publish& publish, std::string_view publisher) {
  if (!mqtt::valid_topic_name(publish.topic)) throw std::invalid_argument("lir::broker::Delivery: not a topic name: " + publish.topic);
  if (publish.properties.topic_alias) throw std::invalid_argument("lir::broker::Delivery: a topic alias is resolved before delivery");
  if (publish.qos != mqtt::QoS::at_most_once) throw std::invalid_argument("lir::broker::Delivery: QoS 0 only until Dagda's log");
  dannan::net::Writer writer;
  mqtt::write_string(writer, publisher);
  writer.raw(mqtt::encode(publish, mqtt::Version::v5));
  return _channels.publish(std::string(channel_name), publish.topic, writer.take());
}

}  // namespace lir::broker
