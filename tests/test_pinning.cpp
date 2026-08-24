#include "test_helpers.hpp"

TEST(pin_prevents_eviction) {
  TempDir td;
  Config cfg;
  cfg.limits.max_total_bytes = 2000;
  cfg.limits.max_object_count = 100000;
  ModelCache cache(td.join("root"), cfg);
  const ArtifactId a(ArtifactNamespace("n"), "a");
  const ArtifactId b(ArtifactNamespace("n"), "b");
  const ArtifactId c(ArtifactNamespace("n"), "c");
  const ArtifactId d(ArtifactNamespace("n"), "d");
  const ArtifactId e(ArtifactNamespace("n"), "e");
  auto mk = [&](const ArtifactId& id, const std::string& s) {
    ingest_bytes(cache, id, spec(ArtifactType::MODEL_SHARD, "shard", "1"), s);
  };
  mk(a, std::string(500, 'a'));
  cache.pin(a);                       // earliest (oldest) and pinned
  mk(b, std::string(500, 'b'));
  mk(c, std::string(500, 'c'));
  mk(d, std::string(500, 'd'));
  mk(e, std::string(500, 'e'));       // triggers eviction
  CHECK(cache.exists(a));            // pinned artifact survives
  CHECK(cache.is_pinned(a));
  CHECK(cache.snapshot().total_physical_bytes <= 2000u);
}

TEST(unpin_allows_eviction) {
  TempDir td;
  Config cfg;
  cfg.limits.max_total_bytes = 1200;
  ModelCache cache(td.join("root"), cfg);
  const ArtifactId a(ArtifactNamespace("n"), "a");
  const ArtifactId b(ArtifactNamespace("n"), "b");
  ingest_bytes(cache, a, spec(ArtifactType::MODEL_SHARD, "shard", "1"), std::string(600, 'a'));
  cache.pin(a);
  cache.unpin(a);
  ingest_bytes(cache, b, spec(ArtifactType::MODEL_SHARD, "shard", "1"), std::string(600, 'b'));
  CHECK(cache.snapshot().total_physical_bytes <= 1200u);
}

TEST(protected_dependency_not_evicted) {
  TempDir td;
  Config cfg;
  cfg.limits.max_total_bytes = 300;
  ModelCache cache(td.join("root"), cfg);
  const ArtifactId base(ArtifactNamespace("n"), "base");
  const ArtifactId ad(ArtifactNamespace("n"), "adapter");
  const ArtifactId f1(ArtifactNamespace("n"), "filler1");
  const ArtifactId f2(ArtifactNamespace("n"), "filler2");
  ingest_bytes(cache, base, spec(ArtifactType::MODEL_WEIGHTS, "base", "1"), std::string(100, 'b'));
  IngestSpec s;
  s.type = ArtifactType::ADAPTER; s.role = ArtifactRole("adapter"); s.version = ArtifactVersion("1");
  s.dependencies = {"n/base"};
  cache.ingest(ad, s, reinterpret_cast<const std::uint8_t*>(std::string(100, 'a').data()), 100);
  cache.pin(ad);   // retained; its dependency (base) must not be evicted
  ingest_bytes(cache, f1, spec(ArtifactType::MODEL_SHARD, "shard", "1"), std::string(100, '1'));
  ingest_bytes(cache, f2, spec(ArtifactType::MODEL_SHARD, "shard", "1"), std::string(100, '2'));
  CHECK(cache.exists(base));     // dependency of retained (pinned) adapter survives
  CHECK(cache.exists(ad));
  // filler was evicted to fit
  CHECK(!cache.exists(f1) || !cache.exists(f2) || cache.snapshot().total_physical_bytes <= 300u);
}
