//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "lir/mqtt/topic.h"

// Topic filters held by level, '+' and '#' as levels of their own, so a
// topic name is matched by walking its levels once rather than testing every
// filter. Each filter holds entries by key (a session, an edge), the latest
// value per key. Not thread-safe: the owner holds it under its own lock or
// strand. Filters must be valid (mqtt::valid_topic_filter); the trie does not
// check.
namespace lir::subs {

template <typename Key, typename Value>
class Trie {
 public:
  // Adds or replaces `key`'s entry under `filter`. True if it was new.
  bool insert(std::string_view filter, const Key& key, Value value) {
    Node* node = &_root;
    for (const auto level : mqtt::levels(filter)) {
      auto& child = node->children[std::string(level)];
      if (!child) child = std::make_unique<Node>();
      node = child.get();
    }
    const bool added = node->entries.insert_or_assign(key, std::move(value)).second;
    if (added) ++_size;
    return added;
  }

  // Removes `key`'s entry under `filter`, and any level left empty. True if
  // there was one.
  bool erase(std::string_view filter, const Key& key) {
    const auto parts = mqtt::levels(filter);
    const bool erased = erase_at(_root, parts, 0, key);
    if (erased) --_size;
    return erased;
  }

  // `visit(const Key&, const Value&)` once per entry whose filter matches
  // `name`. An entry under several matching filters is visited once per
  // filter; the caller merges (MQTT 5.0 section 3.3.4).
  template <typename Visit>
  void match(std::string_view name, Visit&& visit) const {
    const auto parts = mqtt::levels(name);
    const bool system = !name.empty() && name.front() == '$';
    match_at(_root, parts, 0, system, visit);
  }

  // `visit(std::string filter, const Key&, const Value&)` for every entry.
  template <typename Visit>
  void each(Visit&& visit) const {
    std::string path;
    each_at(_root, path, true, visit);
  }

  size_t size() const noexcept { return _size; }
  bool empty() const noexcept { return _size == 0; }

 private:
  struct Node {
    std::map<std::string, std::unique_ptr<Node>, std::less<>> children;
    std::map<Key, Value> entries;
  };

  static bool erase_at(Node& node, const std::vector<std::string_view>& parts, size_t index, const Key& key) {
    if (index == parts.size()) return node.entries.erase(key) > 0;
    const auto it = node.children.find(parts[index]);
    if (it == node.children.end()) return false;
    const bool erased = erase_at(*it->second, parts, index + 1, key);
    if (erased && it->second->entries.empty() && it->second->children.empty()) node.children.erase(it);
    return erased;
  }

  template <typename Visit>
  static void visit_all(const Node& node, Visit& visit) {
    for (const auto& [key, value] : node.entries) visit(key, value);
  }

  template <typename Visit>
  static void match_at(const Node& node, const std::vector<std::string_view>& parts, size_t index, bool system, Visit& visit) {
    // A '$' name is not matched by a wildcard in the first level.
    const bool wildcards = !(system && index == 0);
    if (wildcards) {
      // '#' matches this level, everything under it, and the parent itself.
      if (const auto hash = node.children.find("#"); hash != node.children.end()) visit_all(*hash->second, visit);
    }
    if (index == parts.size()) {
      visit_all(node, visit);
      return;
    }
    if (const auto exact = node.children.find(parts[index]); exact != node.children.end()) match_at(*exact->second, parts, index + 1, system, visit);
    if (wildcards) {
      if (const auto plus = node.children.find("+"); plus != node.children.end()) match_at(*plus->second, parts, index + 1, system, visit);
    }
  }

  template <typename Visit>
  static void each_at(const Node& node, std::string& path, bool root, Visit& visit) {
    for (const auto& [key, value] : node.entries) visit(path, key, value);
    for (const auto& [level, child] : node.children) {
      const size_t mark = path.size();
      if (!root) path += '/';
      path += level;
      each_at(*child, path, false, visit);
      path.resize(mark);
    }
  }

  Node _root;
  size_t _size = 0;
};

}  // namespace lir::subs
