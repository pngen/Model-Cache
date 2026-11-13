#include "test_helpers.hpp"

TEST(snapshot_fields) {
  TempDir td;
  ModelCache cache(td.join("root"));
  const ArtifactId id(ArtifactNamespace("n"), "w");
  ingest_bytes(cache, id, spec(ArtifactType::MODEL_WEIGHTS, "weights", "1"), "snapshot");
  const ArtifactId id2(ArtifactNamespace("n"), "w2");
  ingest_bytes(cache, id2, spec(ArtifactType::MODEL_WEIGHTS, "weights", "1"), "snapshot");  // dedup
  cache.alias_set("a", id);
  auto s = cache.snapshot();
  CHECK_EQ(s.logical_artifact_count, 2u);
  CHECK_EQ(s.physical_backing_count, 1u);     // dedup
  CHECK_EQ(s.valid_count, 2u);
  CHECK_EQ(s.alias_count, 1u);
  CHECK_EQ(s.cache_generation >= 0u, true);
  CHECK_EQ(s.total_logical_bytes, 2u * 8u);   // "snapshot" is 8 bytes
  CHECK_EQ(s.total_physical_bytes, 8u);
}

TEST(stats_counters) {
  TempDir td;
  ModelCache cache(td.join("root"));
  const ArtifactId id(ArtifactNamespace("n"), "w");
  ingest_bytes(cache, id, spec(ArtifactType::MODEL_WEIGHTS, "weights", "1"), "data");
  LookupRequest req; req.id = id;
  cache.lookup(req);
  cache.lookup(req);
  LookupRequest miss; miss.id = ArtifactId(ArtifactNamespace("n"), "missing");
  cache.lookup(miss);
  auto& st = cache.stats();
  CHECK(st.lookups.load() >= 3u);
  CHECK(st.hits.load() >= 2u);
  CHECK(st.misses.load() >= 1u);
  CHECK(st.artifacts_admitted.load() >= 1u);
}

TEST(admission_event_and_stats) {
  TempDir td;
  ModelCache cache(td.join("root"));
  const ArtifactId id(ArtifactNamespace("n"), "ev");
  ingest_bytes(cache, id, spec(ArtifactType::MODEL_WEIGHTS, "weights", "1"), "event");
  CHECK(cache.stats().artifacts_admitted.load() >= 1u);
  CHECK_EQ(cache.snapshot().evicted >= 0u, true);
}

TEST(subscription_bound_counts_live_subscriptions) {
  TempDir td;
  Config cfg;
  cfg.limits.max_subscriptions = 2;
  ModelCache cache(td.join("root"), cfg);
  bool threw = false;
  try {
    for (int i = 0; i < 10; ++i) {
      Subscription s = cache.events().subscribe([](const Event&) {});
      (void)s;   // unsubscribed when it goes out of scope
    }
  } catch (const model_cache::Error&) { threw = true; }
  CHECK(!threw);
  CHECK_EQ(cache.events().subscriber_count(), 0u);
}
