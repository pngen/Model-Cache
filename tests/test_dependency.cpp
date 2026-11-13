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

TEST(dependency_cycle_is_refused_at_ingest) {
  TempDir td;
  ModelCache cache(td.join("root"));
  IngestSpec sa = spec(ArtifactType::ADAPTER, "adapter", "1");
  sa.dependencies = {"n/b"};
  CHECK(ingest_bytes(cache, ArtifactId(ArtifactNamespace("n"), "a"), sa, "a-bytes").admitted);
  IngestSpec sb = spec(ArtifactType::ADAPTER, "adapter", "1");
  sb.dependencies = {"n/a"};   // would close a -> b -> a
  auto r = ingest_bytes(cache, ArtifactId(ArtifactNamespace("n"), "b"), sb, "b-bytes");
  CHECK(!r.admitted);
  CHECK(!cache.exists(ArtifactId(ArtifactNamespace("n"), "b")));
  IngestSpec self = spec(ArtifactType::ADAPTER, "adapter", "1");
  self.dependencies = {"n/c"};
  CHECK(!ingest_bytes(cache, ArtifactId(ArtifactNamespace("n"), "c"), self, "c-bytes").admitted);
  CHECK_EQ(cache.gc(), 0u);   // nothing left behind by the refused ingests
}

TEST(dependency_edge_bound_is_enforced) {
  TempDir td;
  Config cfg;
  cfg.limits.max_dependency_edges = 1;
  ModelCache cache(td.join("root"), cfg);
  IngestSpec one = spec(ArtifactType::ADAPTER, "adapter", "1");
  one.dependencies = {"n/x"};
  CHECK(ingest_bytes(cache, ArtifactId(ArtifactNamespace("n"), "a"), one, "a-bytes").admitted);
  IngestSpec two = spec(ArtifactType::ADAPTER, "adapter", "1");
  two.dependencies = {"n/y", "n/z"};
  CHECK(!ingest_bytes(cache, ArtifactId(ArtifactNamespace("n"), "b"), two, "b-bytes").admitted);
  CHECK_EQ(cache.snapshot().dependency_edge_count, 1u);
}

TEST(dependency_named_with_a_content_pin_is_protected) {
  TempDir td;
  Config cfg;
  cfg.limits.max_total_bytes = 300;
  ModelCache cache(td.join("root"), cfg);
  const ArtifactId base(ArtifactNamespace("n"), "base");
  const ArtifactId ad(ArtifactNamespace("n"), "adapter");
  const std::string base_bytes(100, 'b');
  auto rb = ingest_bytes(cache, base, spec(ArtifactType::MODEL_WEIGHTS, "base", "1"), base_bytes);
  IngestSpec s = spec(ArtifactType::ADAPTER, "adapter", "1");
  s.dependencies = {"n/base#" + rb.digest.hex()};   // exactly these bytes of n/base
  CHECK(ingest_bytes(cache, ad, s, std::string(100, 'a')).admitted);
  cache.pin(ad);
  ingest_bytes(cache, ArtifactId(ArtifactNamespace("n"), "filler1"), spec(ArtifactType::MODEL_SHARD, "shard", "1"), std::string(100, '1'));
  ingest_bytes(cache, ArtifactId(ArtifactNamespace("n"), "filler2"), spec(ArtifactType::MODEL_SHARD, "shard", "1"), std::string(100, '2'));
  CHECK(cache.exists(base));   // the retained adapter depends on it
  CHECK(cache.exists(ad));
  auto deps = cache.dependents(base);
  CHECK_EQ(deps.size(), 1u);
}
