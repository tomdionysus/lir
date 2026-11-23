//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "lir/system/views.h"

#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <map>
#include <span>
#include <utility>

#include "dannan/json/json.h"
#include "lir/mqtt/topic.h"

namespace lir::system {

namespace json = dannan::json;

namespace {

mqtt::Publish message(const std::string& topic, const mqtt::Bytes& payload, bool retain) {
  mqtt::Publish out;
  out.topic = topic;
  out.payload = payload;
  out.retain = retain;
  return out;
}

mqtt::Bytes bytes_of(const json::object& object) { return json::to_bytes(object); }

std::string id_of(const dannan::identity::NodeId& id) { return dannan::identity::to_string(id); }

// A figure's number, or zero for anything that is not one it can be: an
// advert is another node's, and is read, never trusted.
template <class T>
T number(const json::object& figure, std::string_view key) {
  const auto* value = figure.if_contains(key);
  if (!value || !value->is_number()) return T{};
  boost::system::error_code error;
  const auto out = value->to_number<T>(error);
  return error ? T{} : out;
}

}  // namespace

// Everything below on `strand`; what the watches and subscribers hold is a
// weak reference, so nothing reaches it once the views have gone.
struct Views::Inner : std::enable_shared_from_this<Views::Inner> {
  struct Subscriber {
    std::string filter;
    boost::asio::any_io_executor executor;
    Handler handler;
  };

  Inner(dannan::Context& node_context, std::string node_id)
      : context(node_context), strand(boost::asio::make_strand(node_context.time().executor())), self(std::move(node_id)) {}

  // A view's new state (empty: gone), or an event (`state` false), to each
  // subscriber whose filter matches.
  void emit(const std::string& topic, const mqtt::Bytes& payload, bool state) {
    if (state) {
      const auto found = views.find(topic);
      if (found != views.end() && found->second == payload) return;
      if (payload.empty()) {
        if (found == views.end()) return;
        views.erase(found);
      } else {
        views[topic] = payload;
      }
    }
    const auto out = message(topic, payload, false);
    for (const auto& [id, subscriber] : subscribers)
      if (mqtt::matches(subscriber.filter, topic)) boost::asio::post(subscriber.executor, [handler = subscriber.handler, out] { handler(out); });
  }

  void on_figures(const dannan::AdvertChange& change) {
    const auto node = id_of(change.node);
    const auto topic = "$lir/nodes/" + node;
    std::optional<json::value> parsed;
    if (change.value) parsed = json::parse(std::span<const uint8_t>(change.value->data(), change.value->size()));
    if (!parsed || !parsed->is_object()) {
      // Withdrawn, gone, or not figures this node can read.
      figures.erase(node);
      emit(topic, {}, true);
    } else {
      figures[node] = parsed->as_object();
      json::object out;
      out["from"] = self;
      out["node"] = node;
      for (const auto& [key, value] : parsed->as_object()) out[key] = value;
      emit(topic, bytes_of(out), true);
    }
    render_cluster();
  }

  // The same sum on every node that has the same adverts.
  void render_cluster() {
    uint64_t connections = 0;
    double rate = 0;
    bool agree = true;
    const json::value* first_limits = nullptr;
    for (const auto& [node, figure] : figures) {
      connections += number<uint64_t>(figure, "connections");
      rate += number<double>(figure, "published_per_second");
      const auto* limits = figure.if_contains("limits");
      if (!first_limits) {
        first_limits = limits;
      } else if (!limits || *limits != *first_limits) {
        agree = false;
      }
    }
    json::object out;
    out["from"] = self;
    out["nodes"] = static_cast<uint64_t>(figures.size());
    out["connections"] = connections;
    out["published_per_second"] = rate;
    out["limits_agree"] = agree;
    emit("$lir/cluster", bytes_of(out), true);
  }

  void on_roster(const dannan::RosterChange& change) {
    const auto& record = change.node;
    const auto node = id_of(record.id);
    json::object out;
    out["from"] = self;
    out["node"] = node;
    out["host"] = record.host;
    out["port"] = static_cast<uint64_t>(record.port);
    out["domain"] = record.failure_domain;
    out["path_class"] = dannan::membership::path_class_name(record.path_class);
    if (change.kind == dannan::RosterChange::Kind::left) {
      emit("$dannan/nodes/" + node, {}, true);
    } else {
      auto view = out;
      if (const auto latency = context.nodes().latency(record.id)) view["latency_ms"] = static_cast<uint64_t>(latency->count());
      emit("$dannan/nodes/" + node, bytes_of(view), true);
    }
    emit(std::string("$dannan/roster/") + dannan::roster_change_name(change.kind), bytes_of(out), false);
  }

  dannan::Context& context;
  boost::asio::strand<boost::asio::any_io_executor> strand;
  const std::string self;
  uint64_t roster_watch = 0;
  uint64_t figures_watch = 0;
  std::map<std::string, mqtt::Bytes> views;
  std::map<std::string, json::object> figures;
  std::map<uint64_t, Subscriber> subscribers;
  std::atomic<uint64_t> next{1};
};

Views::Views(dannan::Context& context, std::chrono::milliseconds advert_every)
    : _self(id_of(context.nodes().self().id)), _inner(std::make_shared<Inner>(context, _self)) {
  std::weak_ptr<Inner> weak = _inner;
  _inner->roster_watch = context.nodes().watch(_inner->strand, [weak](dannan::RosterChange change) {
    if (auto inner = weak.lock()) inner->on_roster(change);
  });
  _inner->figures_watch = context.nodes().watch_advert(std::string(figures_advert), advert_every, _inner->strand, [weak](dannan::AdvertChange change) {
    if (auto inner = weak.lock()) inner->on_figures(change);
  });
}

Views::~Views() {
  _inner->context.nodes().unwatch(_inner->roster_watch);
  _inner->context.nodes().unwatch_advert(_inner->figures_watch);
}

void Views::own_figures(std::optional<mqtt::Bytes> figures) {
  dannan::AdvertChange change;
  change.node = _inner->context.nodes().self().id;
  if (figures) change.value = dannan::net::Bytes(figures->begin(), figures->end());
  boost::asio::post(_inner->strand, [weak = std::weak_ptr<Inner>(_inner), change = std::move(change)] {
    if (auto inner = weak.lock()) inner->on_figures(change);
  });
}

uint64_t Views::subscribe(std::string filter, boost::asio::any_io_executor executor, Handler handler) {
  const uint64_t id = _inner->next.fetch_add(1);
  boost::asio::post(_inner->strand, [weak = std::weak_ptr<Inner>(_inner), id, filter = std::move(filter), executor = std::move(executor),
                                     handler = std::move(handler)]() mutable {
    if (auto inner = weak.lock()) inner->subscribers.emplace(id, Inner::Subscriber{std::move(filter), std::move(executor), std::move(handler)});
  });
  return id;
}

void Views::unsubscribe(uint64_t id) {
  boost::asio::post(_inner->strand, [weak = std::weak_ptr<Inner>(_inner), id] {
    if (auto inner = weak.lock()) inner->subscribers.erase(id);
  });
}

void Views::current(std::string filter, boost::asio::any_io_executor executor, Handler handler) {
  boost::asio::post(_inner->strand,
                    [weak = std::weak_ptr<Inner>(_inner), filter = std::move(filter), executor = std::move(executor), handler = std::move(handler)] {
                      auto inner = weak.lock();
                      if (!inner) return;
                      for (const auto& [topic, payload] : inner->views)
                        if (mqtt::matches(filter, topic)) boost::asio::post(executor, [handler, out = message(topic, payload, true)] { handler(out); });
                    });
}

}  // namespace lir::system
