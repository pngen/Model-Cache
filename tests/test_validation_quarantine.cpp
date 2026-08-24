#include "test_helpers.hpp"

TEST(corruption_detected_by_verify) {
  TempDir td;
  ModelCache cache(td.join("root"));
  const ArtifactId id(ArtifactNamespace("n"), "w");
  const std::string content = "corruptible";
  ingest_bytes(cache, id, spec(ArtifactType::MODEL_WEIGHTS, "weights", "1"), content);
  LookupRequest req; req.id = id;
  auto path = cache.lookup(req).backing_location;
  CHECK(path.has_value());
  // Corrupt the backing bytes.
  const std::string junk(64, 'X');
  model_cache::platform::write_file_atomic(*path, junk.data(), junk.size());
  CHECK(cache.verify() >= 1u);
}

TEST(recovery_quarantines_corrupt_backing) {
  TempDir td;
  const ArtifactId id(ArtifactNamespace("n"), "w");
  const std::string content = "recovery-proof";
  std::string backing_path;
  {
    ModelCache cache(td.join("root"));
    ingest_bytes(cache, id, spec(ArtifactType::MODEL_WEIGHTS, "weights", "1"), content);
    LookupRequest req; req.id = id;
    backing_path = *cache.lookup(req).backing_location;
    // Corrupt the backing.
    const std::string junk(128, '0');
    model_cache::platform::write_file_atomic(backing_path, junk.data(), junk.size());
    cache.save();
  }
  {
    // Reopen: recovery must quarantine the corrupt object and flag the record.
    ModelCache cache(td.join("root"));
    LookupRequest req; req.id = id; req.require_valid = true;
    auto lr = cache.lookup(req);
    CHECK(!lr.valid);
    CHECK(lr.validation == ValidationState::QUARANTINED);
  }
}

TEST(quarantine_path) {
  TempDir td;
  ModelCache cache(td.join("root"));
  const ArtifactId id(ArtifactNamespace("n"), "q");
  ingest_bytes(cache, id, spec(ArtifactType::MODEL_WEIGHTS, "weights", "1"), "payload");
  cache.quarantine(id, "suspicious");
  LookupRequest req; req.id = id; req.require_valid = true;
  auto lr = cache.lookup(req);
  CHECK(!lr.valid);
  CHECK(lr.validation == ValidationState::QUARANTINED);
}

TEST(purge_after_quarantine_frees_backing) {
  TempDir td;
  ModelCache cache(td.join("root"));
  const ArtifactId id(ArtifactNamespace("n"), "q2");
  ingest_bytes(cache, id, spec(ArtifactType::MODEL_WEIGHTS, "weights", "1"), "payload");
  cache.quarantine(id, "bad");
  cache.purge(id);
  CHECK_EQ(cache.snapshot().physical_backing_count, 0u);
}
