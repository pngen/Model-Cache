#include "model_cache/dependency.hpp"

#include <algorithm>
#include <queue>

namespace model_cache {

bool DependencyGraph::add_edge(const std::string& from, const std::string& to) {
  if (from == to) return false;
  if (ancestors(to).count(from) != 0) return false;
  if (deps_[from].insert(to).second) {
    rdeps_[to].insert(from);
    return true;
  }
  return false;
}

bool DependencyGraph::remove_edge(const std::string& from, const std::string& to) {
  auto it = deps_.find(from);
  if (it == deps_.end()) return false;
  if (it->second.erase(to) == 0) return false;
  auto rit = rdeps_.find(to);
  if (rit != rdeps_.end()) rit->second.erase(from);
  if (it->second.empty()) deps_.erase(it);
  return true;
}

bool DependencyGraph::has_edge(const std::string& from, const std::string& to) const {
  auto it = deps_.find(from);
  return it != deps_.end() && it->second.count(to) != 0;
}

bool DependencyGraph::has_node(const std::string& node) const {
  return deps_.count(node) != 0 || rdeps_.count(node) != 0;
}

std::vector<std::string> DependencyGraph::dependencies(const std::string& node) const {
  std::vector<std::string> out;
  auto it = deps_.find(node);
  if (it != deps_.end()) out.assign(it->second.begin(), it->second.end());
  return out;
}

std::vector<std::string> DependencyGraph::dependents(const std::string& node) const {
  std::vector<std::string> out;
  auto it = rdeps_.find(node);
  if (it != rdeps_.end()) out.assign(it->second.begin(), it->second.end());
  return out;
}

std::set<std::string> DependencyGraph::ancestors(const std::string& node) const {
  std::set<std::string> out;
  std::queue<std::string> q;
  for (const auto& d : dependencies(node)) { if (out.insert(d).second) q.push(d); }
  while (!q.empty()) {
    const auto cur = q.front(); q.pop();
    for (const auto& nxt : dependencies(cur)) if (out.insert(nxt).second) q.push(nxt);
  }
  return out;
}

std::set<std::string> DependencyGraph::reverse_reachable(const std::string& node) const {
  std::set<std::string> out;
  std::queue<std::string> q;
  for (const auto& d : dependents(node)) { if (out.insert(d).second) q.push(d); }
  while (!q.empty()) {
    const auto cur = q.front(); q.pop();
    for (const auto& nxt : dependents(cur)) if (out.insert(nxt).second) q.push(nxt);
  }
  return out;
}

std::set<std::string> DependencyGraph::descendants(const std::string& node) const {
  std::set<std::string> out;
  std::queue<std::string> q;
  for (const auto& d : dependents(node)) { if (out.insert(d).second) q.push(d); }
  while (!q.empty()) {
    const auto cur = q.front(); q.pop();
    for (const auto& nxt : dependents(cur)) if (out.insert(nxt).second) q.push(nxt);
  }
  return out;
}

void DependencyGraph::set_dependencies(const std::string& node, const std::vector<std::string>& deps) {
  for (const auto& d : dependencies(node)) remove_edge(node, d);
  for (const auto& d : deps) add_edge(node, d);
}

void DependencyGraph::remove_node(const std::string& node) {
  for (const auto& d : dependencies(node)) remove_edge(node, d);
  for (const auto& k : dependents(node)) remove_edge(k, node);
  deps_.erase(node);
  rdeps_.erase(node);
}

std::vector<std::string> DependencyGraph::nodes() const {
  std::vector<std::string> out;
  out.reserve(deps_.size());
  for (const auto& kv : deps_) out.push_back(kv.first);
  for (const auto& kv : rdeps_) if (deps_.count(kv.first) == 0) out.push_back(kv.first);
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace model_cache
