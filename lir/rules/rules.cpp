//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "lir/rules/rules.h"

#include <boost/json.hpp>
#include <charconv>
#include <stdexcept>

#include "lir/mqtt/topic.h"

namespace lir::rules {

namespace json = boost::json;
using broker::Message;
using mqtt::QoS;

namespace {

struct Invalid : std::runtime_error {
  using std::runtime_error::runtime_error;
};

enum class Op { eq, ne, lt, le, gt, ge, exists, in };

std::optional<Op> op_of(std::string_view name) {
  if (name == "=") return Op::eq;
  if (name == "!=") return Op::ne;
  if (name == "<") return Op::lt;
  if (name == "<=") return Op::le;
  if (name == ">") return Op::gt;
  if (name == ">=") return Op::ge;
  if (name == "exists") return Op::exists;
  if (name == "in") return Op::in;
  return std::nullopt;
}

struct Condition {
  enum class Kind { all, any, negate, compare };
  Kind kind = Kind::all;
  std::vector<Condition> children;
  std::string path;
  Op op = Op::eq;
  json::value operand;
};

struct Transform {
  std::vector<std::string> pick;
  std::vector<std::pair<std::string, std::string>> rename;
  json::object add;
};

struct Action {
  enum class Kind { republish, drop, set };
  Kind kind = Kind::drop;
  // republish
  std::string topic;
  std::optional<QoS> qos;
  bool retain = false;
  std::optional<json::value> payload;
  std::optional<Transform> transform;
  // set
  std::vector<std::pair<std::string, std::string>> user;
  std::optional<std::string> content_type;
  std::optional<uint32_t> expiry;
};

bool valid_path(std::string_view path) {
  return path == "topic" || path == "client" || path == "qos" || path == "retain" || path == "content_type" || path == "payload" ||
         (path.starts_with("user.") && path.size() > 5) || (path.starts_with("payload.") && path.size() > 8);
}

// A template's placeholders, checked when the rule is compiled.
void check_template(std::string_view text) {
  for (size_t at = text.find('{'); at != std::string_view::npos; at = text.find('{', at + 1)) {
    const size_t end = text.find('}', at);
    if (end == std::string_view::npos) throw Invalid("an unclosed '{' in \"" + std::string(text) + "\"");
    const auto name = text.substr(at + 1, end - at - 1);
    const bool level = name.size() == 1 && name[0] >= '1' && name[0] <= '9';
    if (!level && name != "topic" && name != "client" && !(name.starts_with("payload.") && name.size() > 8)) {
      throw Invalid("an unknown placeholder {" + std::string(name) + "}");
    }
  }
}

Condition compile_where(const json::value& where);

Condition compile_object(const json::object& object) {
  Condition all;
  all.kind = Condition::Kind::all;
  for (const auto& [key, value] : object) {
    if (key == "all" || key == "any") {
      if (!value.is_array()) throw Invalid("\"" + std::string(key) + "\" takes an array");
      Condition group;
      group.kind = key == "all" ? Condition::Kind::all : Condition::Kind::any;
      for (const auto& child : value.as_array()) group.children.push_back(compile_where(child));
      all.children.push_back(std::move(group));
    } else if (key == "not") {
      Condition negate;
      negate.kind = Condition::Kind::negate;
      negate.children.push_back(compile_where(value));
      all.children.push_back(std::move(negate));
    } else {
      if (!valid_path(key)) throw Invalid("an unknown path \"" + std::string(key) + "\"");
      if (!value.is_object() || value.as_object().empty()) throw Invalid("\"" + std::string(key) + "\" takes {operator: operand}");
      for (const auto& [name, operand] : value.as_object()) {
        const auto op = op_of(name);
        if (!op) throw Invalid("an unknown operator \"" + std::string(name) + "\"");
        if (*op == Op::in && !operand.is_array()) throw Invalid("\"in\" takes an array");
        if (*op == Op::exists && !operand.is_bool()) throw Invalid("\"exists\" takes true or false");
        Condition compare;
        compare.kind = Condition::Kind::compare;
        compare.path = std::string(key);
        compare.op = *op;
        compare.operand = operand;
        all.children.push_back(std::move(compare));
      }
    }
  }
  return all;
}

Condition compile_where(const json::value& where) {
  if (!where.is_object()) throw Invalid("a condition is an object");
  return compile_object(where.as_object());
}

std::optional<QoS> qos_of(const json::value& v) {
  if (!v.is_int64() && !v.is_uint64()) return std::nullopt;
  const auto n = v.to_number<int64_t>();
  if (n < 0 || n > 2) return std::nullopt;
  return static_cast<QoS>(n);
}

void check_add(const json::value& v) {
  if (v.is_string()) {
    check_template(v.as_string());
  } else if (v.is_object()) {
    for (const auto& [k, child] : v.as_object()) check_add(child);
  } else if (v.is_array()) {
    for (const auto& child : v.as_array()) check_add(child);
  }
}

Action compile_action(const json::value& v) {
  if (!v.is_object() || v.as_object().size() != 1) throw Invalid("an action is one of {\"republish\": ...}, {\"set\": ...}, {\"drop\": true}");
  const auto& [name, body] = *v.as_object().begin();
  Action action;
  if (name == "drop") {
    if (!body.is_bool() || !body.as_bool()) throw Invalid("\"drop\" takes true");
    action.kind = Action::Kind::drop;
  } else if (name == "republish") {
    action.kind = Action::Kind::republish;
    if (!body.is_object()) throw Invalid("\"republish\" takes an object");
    const auto& r = body.as_object();
    const auto* topic = r.if_contains("topic");
    if (!topic || !topic->is_string() || topic->as_string().empty()) throw Invalid("\"republish\" needs a \"topic\"");
    action.topic = std::string(topic->as_string());
    check_template(action.topic);
    if (const auto* q = r.if_contains("qos")) {
      action.qos = qos_of(*q);
      if (!action.qos) throw Invalid("\"qos\" is 0, 1 or 2");
    }
    if (const auto* retain = r.if_contains("retain")) {
      if (!retain->is_bool()) throw Invalid("\"retain\" is true or false");
      action.retain = retain->as_bool();
    }
    if (const auto* payload = r.if_contains("payload")) {
      check_add(*payload);
      action.payload = *payload;
    }
    if (const auto* t = r.if_contains("transform")) {
      if (action.payload) throw Invalid("\"payload\" and \"transform\" do not go together");
      if (!t->is_object()) throw Invalid("\"transform\" takes an object");
      Transform transform;
      for (const auto& [key, value] : t->as_object()) {
        if (key == "pick") {
          if (!value.is_array()) throw Invalid("\"pick\" takes an array of field names");
          for (const auto& field : value.as_array()) {
            if (!field.is_string()) throw Invalid("\"pick\" takes an array of field names");
            transform.pick.emplace_back(field.as_string());
          }
        } else if (key == "rename") {
          if (!value.is_object()) throw Invalid("\"rename\" takes {from: to}");
          for (const auto& [from, to] : value.as_object()) {
            if (!to.is_string()) throw Invalid("\"rename\" takes {from: to}");
            transform.rename.emplace_back(std::string(from), std::string(to.as_string()));
          }
        } else if (key == "add") {
          if (!value.is_object()) throw Invalid("\"add\" takes an object");
          check_add(value);
          transform.add = value.as_object();
        } else {
          throw Invalid("an unknown transform \"" + std::string(key) + "\"");
        }
      }
      action.transform = std::move(transform);
    }
    for (const auto& [key, value] : r) {
      if (key != "topic" && key != "qos" && key != "retain" && key != "payload" && key != "transform") {
        throw Invalid("an unknown republish setting \"" + std::string(key) + "\"");
      }
    }
  } else if (name == "set") {
    action.kind = Action::Kind::set;
    if (!body.is_object()) throw Invalid("\"set\" takes an object");
    for (const auto& [key, value] : body.as_object()) {
      if (key == "user") {
        if (!value.is_object()) throw Invalid("\"user\" takes {name: value}");
        for (const auto& [k, uv] : value.as_object()) {
          if (!uv.is_string()) throw Invalid("\"user\" takes {name: value}");
          check_template(uv.as_string());
          action.user.emplace_back(std::string(k), std::string(uv.as_string()));
        }
      } else if (key == "content_type") {
        if (!value.is_string()) throw Invalid("\"content_type\" takes a string");
        action.content_type = std::string(value.as_string());
      } else if (key == "expiry") {
        if (!value.is_int64() && !value.is_uint64()) throw Invalid("\"expiry\" takes seconds");
        const auto seconds = value.to_number<int64_t>();
        if (seconds < 1 || seconds > 0xFFFFFFFFll) throw Invalid("\"expiry\" takes seconds");
        action.expiry = static_cast<uint32_t>(seconds);
      } else {
        throw Invalid("an unknown setting \"" + std::string(key) + "\"");
      }
    }
  } else {
    throw Invalid("an unknown action \"" + std::string(name) + "\"");
  }
  return action;
}

// ---- evaluation

// What one message offers the conditions and templates.
struct Facts {
  const Message& message;
  const std::vector<std::string>& wildcards;
  // Parsed once, when first asked for; null when the payload is not JSON.
  mutable std::optional<std::optional<json::value>> parsed;

  const std::optional<json::value>& payload() const {
    if (!parsed) {
      boost::system::error_code ec;
      auto v = json::parse(std::string_view(reinterpret_cast<const char*>(message.payload.data()), message.payload.size()), ec);
      parsed = ec ? std::nullopt : std::optional<json::value>(std::move(v));
    }
    return *parsed;
  }

  // The value at `path`, or nothing when absent.
  std::optional<json::value> at(std::string_view path) const {
    if (path == "topic") return json::value(message.topic);
    if (path == "client") return json::value(message.origin);
    if (path == "qos") return json::value(static_cast<int64_t>(message.qos));
    if (path == "retain") return json::value(message.retain);
    if (path == "content_type") return message.content_type ? std::optional<json::value>(json::value(*message.content_type)) : std::nullopt;
    if (path.starts_with("user.")) {
      const auto name = path.substr(5);
      for (const auto& [k, v] : message.user_properties)
        if (k == name) return json::value(v);
      return std::nullopt;
    }
    const auto& root = payload();
    if (!root) return std::nullopt;
    if (path == "payload") return *root;
    const json::value* at = &*root;
    std::string_view rest = path.substr(8);
    while (!rest.empty()) {
      const size_t dot = rest.find('.');
      const auto part = rest.substr(0, dot);
      if (at->is_object()) {
        at = at->as_object().if_contains(part);
      } else if (at->is_array()) {
        size_t index = 0;
        const auto [end, ec] = std::from_chars(part.data(), part.data() + part.size(), index);
        if (ec != std::errc() || end != part.data() + part.size() || index >= at->as_array().size()) return std::nullopt;
        at = &at->as_array()[index];
      } else {
        return std::nullopt;
      }
      if (!at) return std::nullopt;
      if (dot == std::string_view::npos) break;
      rest.remove_prefix(dot + 1);
    }
    return *at;
  }

  std::string text_of(const json::value& v) const {
    if (v.is_string()) return std::string(v.as_string());
    return json::serialize(v);
  }

  std::string fill(std::string_view text) const {
    std::string out;
    size_t from = 0;
    for (size_t open = text.find('{'); open != std::string_view::npos; open = text.find('{', from)) {
      const size_t end = text.find('}', open);
      out.append(text.substr(from, open - from));
      const auto name = text.substr(open + 1, end - open - 1);
      if (name == "topic") {
        out += message.topic;
      } else if (name == "client") {
        out += message.origin;
      } else if (name.size() == 1) {
        const size_t index = static_cast<size_t>(name[0] - '1');
        if (index < wildcards.size()) out += wildcards[index];
      } else if (const auto v = at(name)) {
        out += text_of(*v);
      }
      from = end + 1;
    }
    out.append(text.substr(from));
    return out;
  }

  json::value fill_value(const json::value& v) const {
    if (v.is_string()) return json::value(fill(v.as_string()));
    if (v.is_object()) {
      json::object out;
      for (const auto& [k, child] : v.as_object()) out[k] = fill_value(child);
      return out;
    }
    if (v.is_array()) {
      json::array out;
      for (const auto& child : v.as_array()) out.push_back(fill_value(child));
      return out;
    }
    return v;
  }
};

bool numeric(const json::value& v) { return v.is_int64() || v.is_uint64() || v.is_double(); }

bool equal(const json::value& a, const json::value& b) {
  if (numeric(a) && numeric(b)) return a.to_number<double>() == b.to_number<double>();
  return a == b;
}

std::optional<int> order(const json::value& a, const json::value& b) {
  if (numeric(a) && numeric(b)) {
    const double x = a.to_number<double>();
    const double y = b.to_number<double>();
    return x < y ? -1 : x > y ? 1 : 0;
  }
  if (a.is_string() && b.is_string()) {
    const int c = a.as_string().compare(b.as_string());
    return c < 0 ? -1 : c > 0 ? 1 : 0;
  }
  return std::nullopt;
}

bool holds(const Condition& c, const Facts& facts) {
  switch (c.kind) {
    case Condition::Kind::all:
      for (const auto& child : c.children)
        if (!holds(child, facts)) return false;
      return true;
    case Condition::Kind::any:
      for (const auto& child : c.children)
        if (holds(child, facts)) return true;
      return false;
    case Condition::Kind::negate:
      return !holds(c.children.front(), facts);
    case Condition::Kind::compare:
      break;
  }
  const auto value = facts.at(c.path);
  if (c.op == Op::exists) return value.has_value() == c.operand.as_bool();
  // Absent: equal to nothing, so "!=" holds and every other test fails.
  if (!value) return c.op == Op::ne;
  switch (c.op) {
    case Op::eq:
      return equal(*value, c.operand);
    case Op::ne:
      return !equal(*value, c.operand);
    case Op::in:
      for (const auto& candidate : c.operand.as_array())
        if (equal(*value, candidate)) return true;
      return false;
    default: {
      const auto o = order(*value, c.operand);
      if (!o) return false;
      if (c.op == Op::lt) return *o < 0;
      if (c.op == Op::le) return *o <= 0;
      if (c.op == Op::gt) return *o > 0;
      return *o >= 0;
    }
  }
}

// The levels the filter's wildcards matched, in order; '#' takes the rest.
std::vector<std::string> wildcards_of(std::string_view filter, std::string_view topic) {
  std::vector<std::string> out;
  const auto f = mqtt::levels(filter);
  const auto t = mqtt::levels(topic);
  for (size_t i = 0; i < f.size(); ++i) {
    if (f[i] == "+" && i < t.size()) out.emplace_back(t[i]);
    if (f[i] == "#") {
      std::string rest;
      for (size_t j = i; j < t.size(); ++j) rest += (j > i ? "/" : "") + std::string(t[j]);
      out.push_back(std::move(rest));
    }
  }
  return out;
}

}  // namespace

struct RuleSet::Rule {
  std::string from;
  std::optional<Condition> where;
  std::vector<Action> actions;
};

RuleSet::RuleSet() = default;
RuleSet::~RuleSet() = default;
RuleSet::RuleSet(RuleSet&&) noexcept = default;
RuleSet& RuleSet::operator=(RuleSet&&) noexcept = default;

std::optional<std::string> RuleSet::set(const std::string& name, std::string_view text) {
  _rules.erase(name);
  try {
    boost::system::error_code ec;
    const auto root = json::parse(text, ec);
    if (ec) throw Invalid("not JSON: " + ec.message());
    if (!root.is_object()) throw Invalid("a rule is a JSON object");
    const auto& o = root.as_object();
    auto rule = std::make_shared<Rule>();
    const auto* from = o.if_contains("from");
    if (!from || !from->is_string() || !mqtt::valid_topic_filter(std::string_view(from->as_string())) || mqtt::is_shared(std::string_view(from->as_string()))) {
      throw Invalid("\"from\" is a topic filter");
    }
    rule->from = std::string(from->as_string());
    if (const auto* where = o.if_contains("where")) rule->where = compile_where(*where);
    const auto* actions = o.if_contains("do");
    if (!actions) throw Invalid("a rule needs \"do\"");
    if (actions->is_array()) {
      for (const auto& a : actions->as_array()) rule->actions.push_back(compile_action(a));
    } else {
      rule->actions.push_back(compile_action(*actions));
    }
    if (rule->actions.empty()) throw Invalid("\"do\" is empty");
    for (const auto& [key, value] : o) {
      if (key != "from" && key != "where" && key != "do") throw Invalid("an unknown key \"" + std::string(key) + "\"");
    }
    _rules[name] = std::move(rule);
    return std::nullopt;
  } catch (const Invalid& e) {
    return std::string(e.what());
  } catch (const std::exception& e) {
    return std::string("invalid: ") + e.what();
  }
}

void RuleSet::erase(const std::string& name) { _rules.erase(name); }

bool RuleSet::empty() const noexcept { return _rules.empty(); }

size_t RuleSet::size() const noexcept { return _rules.size(); }

Outcome RuleSet::apply(Message& message, dannan::contract::TimePoint now) const {
  Outcome out;
  for (const auto& [name, rule] : _rules) {
    if (!mqtt::matches(rule->from, message.topic)) continue;
    const auto wildcards = wildcards_of(rule->from, message.topic);
    const Facts facts{message, wildcards, std::nullopt};
    if (rule->where && !holds(*rule->where, facts)) continue;
    for (const auto& action : rule->actions) {
      switch (action.kind) {
        case Action::Kind::drop:
          out.drop = true;
          break;
        case Action::Kind::set: {
          const Facts now_facts{message, wildcards, std::nullopt};
          std::vector<std::pair<std::string, std::string>> added;
          for (const auto& [k, v] : action.user) added.emplace_back(k, now_facts.fill(v));
          for (auto& entry : added) message.user_properties.push_back(std::move(entry));
          if (action.content_type) message.content_type = action.content_type;
          if (action.expiry) message.expires_at = now + std::chrono::seconds(*action.expiry);
          break;
        }
        case Action::Kind::republish: {
          const Facts current{message, wildcards, std::nullopt};
          Message produced;
          produced.topic = current.fill(action.topic);
          if (!mqtt::valid_topic_name(produced.topic)) {
            ++out.skipped;
            break;
          }
          if (action.payload) {
            const auto filled = current.fill_value(*action.payload);
            const auto text = current.text_of(filled);
            produced.payload = broker::Bytes(text.begin(), text.end());
          } else if (action.transform) {
            const auto& root = current.payload();
            if (!root || !root->is_object()) {
              ++out.skipped;
              break;
            }
            json::object shaped;
            if (action.transform->pick.empty()) {
              shaped = root->as_object();
            } else {
              for (const auto& field : action.transform->pick)
                if (const auto* v = root->as_object().if_contains(field)) shaped[field] = *v;
            }
            for (const auto& [from, to] : action.transform->rename) {
              if (const auto it = shaped.find(from); it != shaped.end()) {
                auto v = it->value();
                shaped.erase(it);
                shaped[to] = std::move(v);
              }
            }
            for (const auto& [k, v] : action.transform->add) shaped[k] = current.fill_value(v);
            const auto text = json::serialize(shaped);
            produced.payload = broker::Bytes(text.begin(), text.end());
            produced.content_type = "application/json";
            produced.payload_format_indicator = 1;
          } else {
            produced.payload = message.payload;
            produced.payload_format_indicator = message.payload_format_indicator;
            produced.content_type = message.content_type;
          }
          produced.qos = action.qos.value_or(message.qos);
          produced.retain = action.retain;
          produced.expires_at = message.expires_at;
          produced.user_properties = message.user_properties;
          produced.correlation_data = message.correlation_data;
          produced.response_topic = message.response_topic;
          produced.hops = static_cast<uint8_t>(message.hops + 1);
          out.republished.push_back(std::move(produced));
          break;
        }
      }
    }
  }
  return out;
}

}  // namespace lir::rules
