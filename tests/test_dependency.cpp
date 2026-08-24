#include "test_helpers.hpp"
#include "model_cache/dependency.hpp"

TEST(dependency_adapter_on_base) {
  TempDir td;
  ModelCache cache(td.join("root"));
  ingest_bytes(cache, ArtifactId(ArtifactNamespace("n"), "base"), spec(ArtifactType::MODEL_WEIGHTS, "base", "1"), "base");
  IngestSpec s;
  s.type = ArtifactType::ADAPTER;
  s.role = ArtifactRole("adapter");
  s.version = ArtifactVersion("1");
  s.dependencies = {"n/base"};
  auto r = cache.ingest(ArtifactId(ArtifactNamespace("n"), "adapter"), s,
                        reinterpret_cast<const std::uint8_t*>("adapter"), 7);
  CHECK(r.admitted);
  auto deps = cache.dependencies(ArtifactId(ArtifactNamespace("n"), "adapter"));
  CHECK_EQ(deps.size(), 1u);
  CHECK_EQ(deps[0], std::string("n/base"));
  auto dep = cache.dependents(ArtifactId(ArtifactNamespace("n"), "base"));
  CHECK_EQ(dep.size(), 1u);
  CHECK_EQ(dep[0], std::string("n/adapter"));
}

TEST(dependency_transitive) {
  DependencyGraph g;
  CHECK(g.add_edge("c", "b"));
  CHECK(g.add_edge("b", "a"));
  CHECK(g.add_edge("c", "a"));
  auto anc = g.ancestors("c");
  CHECK_EQ(anc.count("a"), 1u);
  CHECK_EQ(anc.count("b"), 1u);
  auto rr = g.reverse_reachable("a");
  CHECK_EQ(rr.count("c"), 1u);
  CHECK_EQ(rr.count("b"), 1u);
}

TEST(dependency_cycle_rejected) {
  DependencyGraph g;
  CHECK(g.add_edge("a", "b"));
  CHECK(g.add_edge("b", "c"));
  CHECK(g.add_edge("c", "a") == false);   // would close a->b->c->a
  CHECK(g.has_edge("c", "a") == false);
  CHECK(g.add_edge("a", "a") == false);   // self-loop
  CHECK(g.has_edge("b", "a") == false);
}

TEST(dependency_remove_node_cleans_edges) {
  DependencyGraph g;
  g.add_edge("x", "y");
  g.add_edge("y", "z");
  g.remove_node("y");
  CHECK(g.has_edge("x", "y") == false);
  CHECK(g.has_edge("y", "z") == false);
  CHECK(g.has_node("y") == false);
}
