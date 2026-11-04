//
// Lir - The Distributed Event System
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "lir/cluster/cluster.h"

#include <algorithm>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <map>
#include <tuple>

#include "lir/cluster/codec.h"
#include "lir/mqtt/topic.h"

namespace lir::cluster {

namespace {

namespace asio = boost::asio;
using dannan::identity::NodeId;
using dannan::membership::NodeRecord;
using dannan::net::Bytes;

const std::string notify_channel = "lir.notify";
const std::string seq_channel = "lir.seq";
const std::string retained_channel = "lir.retained";
const std::string groups_channel = "lir.groups";
const std::string advert = "lir";

// The part of a filter before its first wildcard: every topic it matches
// starts with it.
std::string literal_prefix(const std::string& filter) { return filter.substr(0, filter.find_first_of("+#")); }

// The fewest prefixes covering the same topics: none is a prefix of
// another, so a topic matches at most one and arrives once.
std::set<std::string> minimal(const std::set<std::string>& prefixes) {
  std::set<std::string> out;
  const std::string* kept = nullptr;
  for (const auto& p : prefixes) {
    if (kept && p.starts_with(*kept)) continue;
    kept = &*out.insert(p).first;
  }
  return out;
}

bool ok(const dannan::net::CallOutcome& outcome) {
  return outcome.reply && outcome.reply->type == dannan::net::message::ok && !dannan::net::refusal_of(*outcome.reply);
}

}  // namespace

struct Cluster::State : std::enable_shared_from_this<Cluster::State> {
  State(dannan::Context& c, edge::Edge& e, ClusterConfig cfg)
      : context(c), edge(e), config(cfg), logger(c.logger("lir.cluster")), self(c.identity().id()), timer(e.strand()) {}

  dannan::Context& context;
  edge::Edge& edge;
  ClusterConfig config;
  dannan::status::ScopedLogger logger;
  NodeId self;
  asio::steady_timer timer;

  // On the strand from here on.
  bool running = false;
  uint64_t watch_id = 0;
  std::vector<uint64_t> fixed_subscriptions;
  // Prefix -> (lir.notify id, lir.seq id).
  std::map<std::string, std::pair<uint64_t, uint64_t>> prefixes;
  std::map<NodeId, NodeRecord> peers;
  std::map<NodeId, Groups> remote_groups;
  Groups local_groups;
  bool groups_published = false;
  std::map<std::pair<std::string, std::string>, size_t> turn;
  std::map<std::tuple<NodeId, NodeId, std::string>, uint64_t> last_sequence;
  bool interest_pending = false;

  broker::Broker& broker() { return edge.broker(); }
  dannan::contract::TimePoint now() { return context.time().now(); }

  void subscribe_fixed() {
    auto& channels = context.channels();
    std::weak_ptr<State> weak = shared_from_this();
    fixed_subscriptions.push_back(channels.subscribe(retained_channel, "", edge.strand(), [weak](dannan::net::ChannelMessage m) {
      if (const auto s = weak.lock()) s->on_retained(std::move(m));
    }));
    fixed_subscriptions.push_back(channels.subscribe(groups_channel, "", edge.strand(), [weak](dannan::net::ChannelMessage m) {
      if (const auto s = weak.lock()) s->on_groups(std::move(m));
    }));
  }

  // A stream message seen before, or our own: dropped.
  bool fresh(const dannan::net::ChannelMessage& m) {
    if (m.origin == self) return false;
    if (m.lost > 0) logger.warn(std::to_string(m.lost) + " messages lost on " + m.channel + " from " + dannan::identity::to_string(m.origin));
    if (m.sequence == 0) return true;
    auto& last = last_sequence[{m.origin, m.incarnation, m.channel}];
    if (m.sequence <= last) return false;
    last = m.sequence;
    return true;
  }

  void on_message(dannan::net::ChannelMessage m) {
    if (!running || !fresh(m)) return;
    try {
      broker().inject(decode_message(m.payload, now()));
    } catch (const std::exception& e) {
      logger.warn("dropping a message from " + dannan::identity::to_string(m.origin) + ": " + e.what());
    }
  }

  void on_retained(dannan::net::ChannelMessage m) {
    if (!running || !fresh(m)) return;
    try {
      broker().apply_retained(decode_message(m.payload, now()));
    } catch (const std::exception& e) {
      logger.warn("dropping a retained update from " + dannan::identity::to_string(m.origin) + ": " + e.what());
    }
  }

  void on_groups(dannan::net::ChannelMessage m) {
    if (!running || !fresh(m) || !peers.contains(m.origin)) return;
    try {
      remote_groups[m.origin] = decode_groups(m.payload);
    } catch (const std::exception& e) {
      logger.warn("dropping groups from " + dannan::identity::to_string(m.origin) + ": " + e.what());
    }
  }

  // ---- the broker's hooks

  size_t published(const broker::Message& message) {
    auto& channels = context.channels();
    const Bytes payload = encode_message(message, now());
    const auto sent = channels.publish(message.qos == mqtt::QoS::at_most_once ? notify_channel : seq_channel, message.topic, payload);
    size_t others = static_cast<size_t>(std::count_if(sent.to.begin(), sent.to.end(), [this](const NodeId& id) { return id != self; }));
    if (message.retain && message.version) channels.publish(retained_channel, message.topic, payload);
    others += share(message);
    return others;
  }

  // Each matching shared group anywhere: one member node, in turn.
  size_t share(const broker::Message& message) {
    std::map<std::pair<std::string, std::string>, std::vector<NodeId>> candidates;
    for (const auto& key : broker().local_shared()) candidates[key].push_back(self);
    for (const auto& [node, groups] : remote_groups) {
      if (!peers.contains(node)) continue;
      for (const auto& key : groups) candidates[key].push_back(node);
    }
    size_t count = 0;
    for (auto& [key, nodes] : candidates) {
      if (!mqtt::matches(key.second, message.topic)) continue;
      std::sort(nodes.begin(), nodes.end());
      const size_t first = turn[key]++ % nodes.size();
      std::vector<NodeId> order;
      for (size_t i = 0; i < nodes.size(); ++i) order.push_back(nodes[(first + i) % nodes.size()]);
      offer(SharedDelivery{key.first, key.second, message}, std::move(order), 0);
      ++count;
    }
    return count;
  }

  // Gives a shared message to order[index], or the next node if that one
  // cannot take it.
  void offer(SharedDelivery delivery, std::vector<NodeId> order, size_t index) {
    if (index >= order.size()) {
      logger.info("no member of shared group " + delivery.group + " could take a message on " + delivery.message.topic);
      return;
    }
    const NodeId node = order[index];
    std::weak_ptr<State> weak = shared_from_this();
    if (node == self) {
      // Never back into the broker from inside its hook: after it returns.
      asio::post(edge.strand(), [weak, delivery = std::move(delivery), order = std::move(order), index]() mutable {
        const auto s = weak.lock();
        if (!s || !s->running) return;
        if (!s->broker().deliver_shared(delivery.message, delivery.group, delivery.filter)) s->offer(std::move(delivery), std::move(order), index + 1);
      });
      return;
    }
    const auto peer = peers.find(node);
    if (peer == peers.end()) return offer(std::move(delivery), std::move(order), index + 1);
    auto payload = encode_shared(delivery, now());
    context.routes().call(peer->second, shared_type, std::move(payload), dannan::sched::FrameType::control, config.call_deadline, edge.strand(),
                          [weak, delivery = std::move(delivery), order = std::move(order), index](dannan::net::CallOutcome outcome) mutable {
                            const auto s = weak.lock();
                            if (!s || !s->running) return;
                            if (!ok(outcome) || outcome.reply->payload != Bytes{1}) s->offer(std::move(delivery), std::move(order), index + 1);
                          });
  }

  void interest_changed() {
    if (interest_pending) return;
    interest_pending = true;
    asio::post(edge.strand(), [weak = weak_from_this()] {
      if (const auto s = weak.lock()) s->refresh_interest();
    });
  }

  void refresh_interest() {
    interest_pending = false;
    if (!running) return;
    std::set<std::string> wanted;
    for (const auto& filter : broker().local_filters()) wanted.insert(literal_prefix(filter));
    wanted = minimal(wanted);
    auto& channels = context.channels();
    for (auto it = prefixes.begin(); it != prefixes.end();) {
      if (wanted.contains(it->first)) {
        ++it;
        continue;
      }
      channels.unsubscribe(it->second.first);
      channels.unsubscribe(it->second.second);
      it = prefixes.erase(it);
    }
    std::weak_ptr<State> weak = shared_from_this();
    const auto handler = [weak](dannan::net::ChannelMessage m) {
      if (const auto s = weak.lock()) s->on_message(std::move(m));
    };
    for (const auto& prefix : wanted) {
      if (prefixes.contains(prefix)) continue;
      prefixes[prefix] = {channels.subscribe(notify_channel, prefix, edge.strand(), handler), channels.subscribe(seq_channel, prefix, edge.strand(), handler)};
    }
    publish_groups(false);
  }

  void publish_groups(bool always) {
    auto groups = broker().local_shared();
    if (!always && groups_published && groups == local_groups) return;
    local_groups = std::move(groups);
    groups_published = true;
    context.channels().publish(groups_channel, "groups", encode_groups(local_groups));
  }

  void take(const std::string& client_id, bool want_state, std::function<void(std::optional<broker::SessionImage>)> done) {
    struct Gathering {
      size_t waiting = 0;
      std::optional<broker::SessionImage> image;
      std::function<void(std::optional<broker::SessionImage>)> done;
    };
    auto gathering = std::make_shared<Gathering>();
    gathering->done = std::move(done);
    gathering->waiting = peers.size();
    if (peers.empty()) {
      asio::post(edge.strand(), [gathering] { gathering->done(std::nullopt); });
      return;
    }
    const auto request = encode_take(Take{client_id, want_state});
    std::weak_ptr<State> weak = shared_from_this();
    for (const auto& [id, peer] : peers) {
      context.routes().call(peer, take_type, request, dannan::sched::FrameType::control, config.call_deadline, edge.strand(),
                            [weak, gathering](dannan::net::CallOutcome outcome) {
                              if (const auto s = weak.lock(); s && ok(outcome)) {
                                try {
                                  auto image = decode_taken(outcome.reply->payload, s->now());
                                  if (image && !gathering->image) gathering->image = std::move(image);
                                } catch (const std::exception& e) {
                                  s->logger.warn(std::string("a session handed over could not be read: ") + e.what());
                                }
                              }
                              if (--gathering->waiting == 0) gathering->done(std::move(gathering->image));
                            });
    }
  }

  // ---- the roster

  void on_roster(const dannan::RosterChange& change) {
    if (!running) return;
    const auto& node = change.node;
    if (node.id == self) return;
    if (change.kind == dannan::RosterChange::Kind::left || !node.advert(advert)) {
      peers.erase(node.id);
      remote_groups.erase(node.id);
      return;
    }
    const auto known = peers.find(node.id);
    const bool fresh_peer = known == peers.end() || known->second.incarnation != node.incarnation;
    peers[node.id] = node;
    if (!fresh_peer) return;
    remote_groups.erase(node.id);
    catch_up(node);
    publish_groups(true);
  }

  // A node joined (or restarted): its retained values and its groups.
  void catch_up(const NodeRecord& peer) {
    std::weak_ptr<State> weak = shared_from_this();
    context.routes().call(peer, snapshot_type, {}, dannan::sched::FrameType::control, config.call_deadline, edge.strand(),
                          [weak, id = peer.id](dannan::net::CallOutcome outcome) {
                            const auto s = weak.lock();
                            if (!s || !s->running) return;
                            if (!ok(outcome)) {
                              s->logger.info("no snapshot from " + dannan::identity::to_string(id) + ": " + outcome.failure);
                              return;
                            }
                            try {
                              dannan::net::Reader r(outcome.reply->payload);
                              const auto retained = r.view_bytes();
                              const auto groups = r.view_bytes();
                              r.finish();
                              for (const auto& m : decode_messages(retained, s->now())) s->broker().apply_retained(m);
                              if (s->peers.contains(id)) s->remote_groups[id] = decode_groups(groups);
                            } catch (const std::exception& e) {
                              s->logger.warn("a snapshot from " + dannan::identity::to_string(id) + " could not be read: " + e.what());
                            }
                          });
  }

  void resync() {
    timer.expires_after(config.resync_every);
    timer.async_wait([weak = weak_from_this()](const boost::system::error_code& ec) {
      const auto s = weak.lock();
      if (ec || !s || !s->running) return;
      s->publish_groups(true);
      s->resync();
    });
  }
};

Cluster::Cluster(dannan::Context& context, edge::Edge& edge, ClusterConfig config) : _state(std::make_shared<State>(context, edge, config)) {
  std::weak_ptr<State> weak = _state;
  auto& routes = context.routes();
  const auto bad = [](const std::exception& e) { return dannan::net::Handled::failed(400, "bad_request", e.what()); };
  routes.add(snapshot_type, {"lir.snapshot", dannan::net::Lanes::control, dannan::net::Frames::any, false},
             [weak](const NodeRecord&, dannan::sched::FrameType, const dannan::net::Message&) {
               const auto s = weak.lock();
               if (!s) return dannan::net::Handled::failed(503, "stopping", "Lir is stopping");
               auto payload = s->edge.call<Bytes>([&s](broker::Broker& b) {
                 dannan::net::Writer w;
                 w.bytes(encode_messages(b.retained_entries(), s->now()));
                 w.bytes(encode_groups(b.local_shared()));
                 return w.take();
               });
               return dannan::net::Handled::processed({dannan::net::message::ok, std::move(payload), {}});
             });
  routes.add(take_type, {"lir.session_take", dannan::net::Lanes::control, dannan::net::Frames::any, false},
             [weak, bad](const NodeRecord&, dannan::sched::FrameType, const dannan::net::Message& request) {
               const auto s = weak.lock();
               if (!s) return dannan::net::Handled::failed(503, "stopping", "Lir is stopping");
               try {
                 const auto take = decode_take(request.payload);
                 auto payload = s->edge.call<Bytes>([&](broker::Broker& b) { return encode_taken(b.surrender(take.client_id, take.want_state), s->now()); });
                 return dannan::net::Handled::processed({dannan::net::message::ok, std::move(payload), {}});
               } catch (const dannan::net::DecodeError& e) {
                 return bad(e);
               }
             });
  routes.add(shared_type, {"lir.shared_deliver", dannan::net::Lanes::control, dannan::net::Frames::any, false},
             [weak, bad](const NodeRecord&, dannan::sched::FrameType, const dannan::net::Message& request) {
               const auto s = weak.lock();
               if (!s) return dannan::net::Handled::failed(503, "stopping", "Lir is stopping");
               try {
                 auto delivery = decode_shared(request.payload, s->now());
                 const bool taken = s->edge.call<bool>([&](broker::Broker& b) { return b.deliver_shared(delivery.message, delivery.group, delivery.filter); });
                 return dannan::net::Handled::processed({dannan::net::message::ok, Bytes{static_cast<uint8_t>(taken ? 1 : 0)}, {}});
               } catch (const dannan::net::DecodeError& e) {
                 return bad(e);
               }
             });
}

Cluster::~Cluster() {
  try {
    stop();
  } catch (const std::exception& e) {
    _state->logger.error(std::string("stopping the cluster: ") + e.what());
  }
}

void Cluster::start() {
  auto& channels = _state->context.channels();
  channels.open(notify_channel, dannan::net::Delivery::notify, 0);
  channels.open(seq_channel, dannan::net::Delivery::stream, _state->config.replay_bytes);
  channels.open(retained_channel, dannan::net::Delivery::stream, _state->config.replay_bytes);
  channels.open(groups_channel, dannan::net::Delivery::stream, 64u << 10);
  _state->context.nodes().advertise(advert, {});
  _state->edge.call<bool>([s = _state](broker::Broker& b) {
    s->running = true;
    s->subscribe_fixed();
    std::weak_ptr<State> weak = s;
    broker::ClusterHooks hooks;
    hooks.node = dannan::identity::to_string(s->self);
    hooks.published = [weak](const broker::Message& m) -> size_t {
      const auto st = weak.lock();
      return st && st->running ? st->published(m) : 0;
    };
    hooks.interest_changed = [weak] {
      if (const auto st = weak.lock()) st->interest_changed();
    };
    hooks.take = [weak](const std::string& id, bool want, std::function<void(std::optional<broker::SessionImage>)> done) {
      if (const auto st = weak.lock(); st && st->running) return st->take(id, want, std::move(done));
      done(std::nullopt);
    };
    b.attach(std::move(hooks));
    s->watch_id = s->context.nodes().watch(s->edge.strand(), [weak](dannan::RosterChange change) {
      if (const auto st = weak.lock()) st->on_roster(change);
    });
    s->resync();
    return true;
  });
}

void Cluster::stop() {
  auto& state = *_state;
  const bool was_running = state.edge.call<bool>([s = _state](broker::Broker& b) {
    if (!s->running) return false;
    s->running = false;
    b.attach({});
    s->context.nodes().unwatch(s->watch_id);
    auto& channels = s->context.channels();
    for (const auto id : s->fixed_subscriptions) channels.unsubscribe(id);
    for (const auto& [prefix, ids] : s->prefixes) {
      channels.unsubscribe(ids.first);
      channels.unsubscribe(ids.second);
    }
    s->fixed_subscriptions.clear();
    s->prefixes.clear();
    s->timer.cancel();
    return true;
  });
  if (!was_running) return;
  state.context.nodes().withdraw(advert);
  auto& routes = state.context.routes();
  routes.unbind(snapshot_type);
  routes.unbind(take_type);
  routes.unbind(shared_type);
}

}  // namespace lir::cluster
