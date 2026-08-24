#pragma once
// Model Cache — dependency graph.
#include <map>
#include <set>
#include <string>
#include <vector>

namespace model_cache {

class DependencyGraph {
 public:
  bool add_edge(const std::string& from, const std::string& to);
  bool remove_edge(const std::string& from, const std::string& to);
  bool has_edge(const std::string& from, const std::string& to) const;
  bool has_node(const std::string& node) const;

  std::vector<std::string> dependencies(const std::string& node) const;
  std::vector<std::string> dependents(const std::string& node) const;
  std::set<std::string> ancestors(const std::string& node) const;
  std::set<std::string> descendants(const std::string& node) const;
  std::set<std::string> reverse_reachable(const std::string& node) const;

  void set_dependencies(const std::string& node, const std::vector<std::string>& deps);
  void remove_node(const std::string& node);
  void clear() noexcept { deps_.clear(); rdeps_.clear(); }
  std::vector<std::string> nodes() const;
  std::size_t node_count() const noexcept { return deps_.size(); }

 private:
  std::map<std::string, std::set<std::string>> deps_;
  std::map<std::string, std::set<std::string>> rdeps_;
};

}  // namespace model_cache
