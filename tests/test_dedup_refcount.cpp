#include "test_helpers.hpp"

TEST(refcount_shared_backing_survives_purge) {
  TempDir td;
  ModelCache cache(td.join("root"));
  const std::string content = "shared-payload";
  const ArtifactId a(ArtifactNamespace("n"), "a");
  const ArtifactId b(ArtifactNamespace("n"), "b");
  ingest_bytes(cache, a, spec(ArtifactType::MODEL_SHARD, "shard", "1"), content);
  ingest_bytes(cache, b, spec(ArtifactType::MODEL_SHARD, "shard", "1"), content);
  CHECK_EQ(cache.snapshot().physical_backing_count, 1u);
  cache.purge(a);
  CHECK(cache.snapshot().physical_backing_count == 1u);   // b still references it
  CHECK(cache.exists(b));
  cache.purge(b);
  auto s = cache.snapshot();
  CHECK_EQ(s.physical_backing_count, 0u);
  CHECK_EQ(s.total_physical_bytes, 0u);
}

TEST(reference_count_accounting) {
  TempDir td;
  ModelCache cache(td.join("root"));
  const std::string content = "abc";
  const ArtifactId a(ArtifactNamespace("n"), "a");
  const ArtifactId b(ArtifactNamespace("n"), "b");
  const ArtifactId c(ArtifactNamespace("n"), "c");
  ingest_bytes(cache, a, spec(ArtifactType::MODEL_SHARD, "shard", "1"), content);
  ingest_bytes(cache, b, spec(ArtifactType::MODEL_SHARD, "shard", "1"), content);
  ingest_bytes(cache, c, spec(ArtifactType::MODEL_WEIGHTS, "weights", "1"), content);
  auto s = cache.snapshot();
  CHECK_EQ(s.dedup_bytes_saved, 2u * content.size());
  CHECK_EQ(s.total_physical_bytes, content.size());
  CHECK_EQ(s.total_logical_bytes, 3u * content.size());
}

TEST(dedup_preserves_logical_identity) {
  TempDir td;
  ModelCache cache(td.join("root"));
  const std::string content = "same-bytes";
  const ArtifactId a(ArtifactNamespace("n"), "a");
  const ArtifactId b(ArtifactNamespace("n"), "b");
  ingest_bytes(cache, a, spec(ArtifactType::MODEL_WEIGHTS, "weights", "1"), content);
  ingest_bytes(cache, b, spec(ArtifactType::MODEL_WEIGHTS, "weights", "1"), content);
  LookupRequest la; la.id = a;
  LookupRequest lb; lb.id = b;
  auto ra = cache.lookup(la), rb = cache.lookup(lb);
  CHECK_EQ(ra.artifact_key, std::string("n/a"));
  CHECK_EQ(rb.artifact_key, std::string("n/b"));
  CHECK(ra.content_digest == rb.content_digest);
  CHECK_EQ(ra.artifact_key != rb.artifact_key, true);
}
