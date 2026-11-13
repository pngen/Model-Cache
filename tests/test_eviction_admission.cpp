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

TEST(inadmissible_ingest_evicts_nothing) {
  TempDir td;
  Config cfg;
  cfg.limits.max_total_bytes = 2000;
  ModelCache cache(td.join("root"), cfg);
  const ArtifactId a(ArtifactNamespace("n"), "a");
  const ArtifactId b(ArtifactNamespace("n"), "b");
  const ArtifactId big(ArtifactNamespace("n"), "big");
  ingest_bytes(cache, a, spec(ArtifactType::MODEL_SHARD, "shard", "1"), std::string(800, 'a'));
  ingest_bytes(cache, b, spec(ArtifactType::MODEL_SHARD, "shard", "1"), std::string(800, 'b'));
  // More than the whole cache can hold: no eviction can make it fit, so nothing
  // is given up for it.
  auto r = ingest_bytes(cache, big, spec(ArtifactType::MODEL_SHARD, "shard", "1"), std::string(3000, 'c'));
  CHECK(!r.admitted);
  CHECK(cache.exists(a));
  CHECK(cache.exists(b));
  CHECK(!cache.exists(big));
  // What was written for the refused ingest is not left behind.
  CHECK_EQ(cache.gc(), 0u);
}

TEST(replacing_content_needs_no_new_slot) {
  TempDir td;
  Config cfg;
  cfg.limits.max_object_count = 2;
  ModelCache cache(td.join("root"), cfg);
  const ArtifactId a(ArtifactNamespace("n"), "a");
  const ArtifactId b(ArtifactNamespace("n"), "b");
  ingest_bytes(cache, a, spec(ArtifactType::MODEL_SHARD, "shard", "1"), "first");
  ingest_bytes(cache, b, spec(ArtifactType::MODEL_SHARD, "shard", "1"), "other");
  // New content for an artifact that already exists adds no object.
  auto r = ingest_bytes(cache, a, spec(ArtifactType::MODEL_SHARD, "shard", "2"), "second");
  CHECK(r.admitted);
  CHECK_EQ(r.generation, 2u);
  CHECK(cache.exists(a));
  CHECK(cache.exists(b));
}

TEST(content_survives_eviction_of_its_last_other_holder) {
  TempDir td;
  Config cfg;
  cfg.limits.max_object_count = 1;
  ModelCache cache(td.join("root"), cfg);
  const std::string content = "the-same-bytes-for-both";
  const ArtifactId a(ArtifactNamespace("n"), "a");
  const ArtifactId b(ArtifactNamespace("n"), "b");
  CHECK(ingest_bytes(cache, a, spec(ArtifactType::MODEL_SHARD, "shard", "1"), content).admitted);
  // Room for b is made by evicting a, the only other holder of these bytes.
  CHECK(ingest_bytes(cache, b, spec(ArtifactType::MODEL_SHARD, "shard", "1"), content).admitted);
  CHECK(!cache.exists(a));
  LookupRequest req; req.id = b;
  auto lr = cache.lookup(req);
  CHECK(lr.found);
  CHECK(lr.valid);
  CHECK(lr.backing_location.has_value());
  if (lr.backing_location) {
    CHECK(model_cache::platform::path_exists(*lr.backing_location));
    if (model_cache::platform::path_exists(*lr.backing_location)) {
      auto stored = model_cache::platform::read_file(*lr.backing_location, 1 << 20);
      CHECK_EQ(std::string(reinterpret_cast<const char*>(stored.data()), stored.size()), content);
    }
  }
  CHECK_EQ(cache.snapshot().total_physical_bytes, content.size());
}
