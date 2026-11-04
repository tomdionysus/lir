//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lir/broker/broker.h"

// The rules engine (the maintainer, 2026-11-04; the sections file, "Further
// decisions"). A rule is a retained JSON message on $lir/rules/<name>, so it
// replicates like any retained message and an empty publish deletes it.
// Every node holds every rule and applies them, in name order, to each
// message published on it, before fan-out:
//
//   {"from": "sensors/+/temp",                    the topic filter
//    "where": {"payload.value": {">": 30},        conditions, all of them
//              "any": [{"qos": {"=": 1}}, {"user.site": {"in": ["a","b"]}}],
//              "not": {"client": {"=": "test"}}},
//    "do": [{"republish": {"topic": "alerts/{1}/hot", "qos": 1, "retain": false,
//                          "transform": {"pick": ["value"], "rename": {"value": "t"},
//                                        "add": {"room": "{1}"}}}},
//           {"set": {"user": {"seen": "rules"}, "content_type": "application/json",
//                    "expiry": 60}},
//           {"drop": true}]}
//
// Paths: topic, client (the publisher's id), qos, retain, content_type,
// user.<name>, payload (the whole JSON payload) and payload.<a>.<b> (an
// array element by number). Operators: = != < <= > >= exists in. An absent
// value (a payload that is not JSON, a field or property not there) is equal
// to nothing: "!=" holds, every other comparison fails. Templates in a topic or a
// string: {1}..{9} (the levels the filter's wildcards matched; '#' gives the
// rest), {topic}, {client}, {payload.<path>}. A republish without "payload"
// or "transform" carries the original payload; "payload" gives a JSON value
// (a string is sent as its text, templated). Messages a rule produces are
// published here and pass through the rules again, at most 8 hops deep.
namespace lir::rules {

inline constexpr std::string_view kPrefix = "$lir/rules/";
inline constexpr uint8_t kMaximumHops = 8;

struct Outcome {
  // The message is delivered to no one, and not retained.
  bool drop = false;
  std::vector<broker::Message> republished;
  // Republishes skipped: a templated topic that is not a valid name, or a
  // transform of a payload that is not a JSON object.
  size_t skipped = 0;
};

class RuleSet {
 public:
  RuleSet();
  ~RuleSet();
  RuleSet(RuleSet&&) noexcept;
  RuleSet& operator=(RuleSet&&) noexcept;

  // Compiles and holds `json` as rule `name`; an invalid rule is not held
  // (a rule of that name already held is dropped) and the reason returned.
  std::optional<std::string> set(const std::string& name, std::string_view json);
  void erase(const std::string& name);
  bool empty() const noexcept;
  size_t size() const noexcept;

  // Applies every rule whose filter matches, in name order: a "set" changes
  // `message` for the rules after it and for delivery; a "drop" marks it; a
  // "republish" adds a message one hop deeper.
  Outcome apply(broker::Message& message, dannan::contract::TimePoint now) const;

 private:
  struct Rule;
  std::map<std::string, std::shared_ptr<const Rule>> _rules;
};

}  // namespace lir::rules
