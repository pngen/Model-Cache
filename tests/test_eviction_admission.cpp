#include "test_helpers.hpp"

TEST(capacity_bound_after_admission) {
  TempDir td;
  Config cfg;
  cfg.limits.max_total_bytes = 2000;
  ModelCache cache(td.join("root"), cfg);
  const ArtifactId a(ArtifactNamespace("n"), "a");
  const ArtifactId b(ArtifactNamespace("n"), "b");
  const ArtifactId c(ArtifactNamespace("n"), "c");
  ingest_bytes(cache, a, spec(ArtifactType::MODEL_SHARD, "shard", "1"), std::string(1000, 'a'));
  CHECK(cache.snapshot().total_physical_bytes <= 2000u);
  ingest_bytes(cache, b, spec(ArtifactType::MODEL_SHARD, "shard", "1"), std::string(1000, 'b'));
  CHECK(cache.snapshot().total_physical_bytes <= 2000u);
  ingest_bytes(cache, c, spec(ArtifactType::MODEL_SHARD, "shard", "1"), std::string(1000, 'c'));
  CHECK(cache.snapshot().total_physical_bytes <= 2000u);
}

TEST(admission_deterministic) {
  TempDir td;
  Config cfg;
  cfg.limits.max_total_bytes = 1500;
  ModelCache cache(td.join("root"), cfg);
  const ArtifactId a(ArtifactNamespace("n"), "a");
  ingest_bytes(cache, a, spec(ArtifactType::MODEL_SHARD, "shard", "1"), std::string(1500, 'x'));
  CHECK(cache.exists(a));
  CHECK(cache.exists(a));
}

TEST(no_backing_evicted_while_reference_remains) {
  TempDir td;
  ModelCache cache(td.join("root"));
  const ArtifactId a(ArtifactNamespace("n"), "a");
  const ArtifactId b(ArtifactNamespace("n"), "b");
  ingest_bytes(cache, a, spec(ArtifactType::MODEL_WEIGHTS, "weights", "1"), "same");
  ingest_bytes(cache, b, spec(ArtifactType::MODEL_WEIGHTS, "weights", "1"), "same");
  // Evict everything eligible (all unpinned). Because a and b share backing,
  // the backing must survive until the last reference is released.
  cache.evict(1);
  CHECK(cache.exists(a) || cache.exists(b));
  cache.evict(1000000);
  CHECK_EQ(cache.snapshot().physical_backing_count, 0u);
}
