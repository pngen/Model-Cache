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

TEST(quarantine_reaches_every_holder_of_the_bytes) {
  TempDir td;
  ModelCache cache(td.join("root"));
  const ArtifactId a(ArtifactNamespace("n"), "a");
  const ArtifactId b(ArtifactNamespace("n"), "b");
  ingest_bytes(cache, a, spec(ArtifactType::MODEL_WEIGHTS, "weights", "1"), "shared-bytes");
  ingest_bytes(cache, b, spec(ArtifactType::MODEL_WEIGHTS, "weights", "1"), "shared-bytes");
  cache.quarantine(a, "suspicious");
  // The bytes left the object store: b cannot go on calling them valid.
  LookupRequest req; req.id = b;
  auto lr = cache.lookup(req);
  CHECK(!lr.valid);
  CHECK(lr.validation == ValidationState::QUARANTINED);
}

TEST(corruption_found_by_validate_reaches_every_holder) {
  TempDir td;
  ModelCache cache(td.join("root"));
  const ArtifactId a(ArtifactNamespace("n"), "a");
  const ArtifactId b(ArtifactNamespace("n"), "b");
  ingest_bytes(cache, a, spec(ArtifactType::MODEL_WEIGHTS, "weights", "1"), "shared-bytes");
  ingest_bytes(cache, b, spec(ArtifactType::MODEL_WEIGHTS, "weights", "1"), "shared-bytes");
  LookupRequest ra; ra.id = a;
  const std::string path = *cache.lookup(ra).backing_location;
  const std::string junk(32, '!');
  model_cache::platform::write_file_atomic(path, junk.data(), junk.size());
  cache.validate(a);
  LookupRequest rb; rb.id = b;
  CHECK(!cache.lookup(ra).valid);
  CHECK(!cache.lookup(rb).valid);
}

TEST(validate_does_not_revive_a_retired_generation) {
  TempDir td;
  ModelCache cache(td.join("root"));
  const ArtifactId id(ArtifactNamespace("n"), "old");
  ingest_bytes(cache, id, spec(ArtifactType::MODEL_WEIGHTS, "weights", "1"), "retired-bytes");
  cache.retire(id);
  cache.validate(id);   // the bytes are intact, the generation is still retired
  LookupRequest req; req.id = id;
  auto lr = cache.lookup(req);
  CHECK(!lr.valid);
  CHECK(lr.validation == ValidationState::RETIRED);
}

TEST(ingesting_the_bytes_again_heals_a_quarantined_artifact) {
  TempDir td;
  ModelCache cache(td.join("root"));
  const ArtifactId id(ArtifactNamespace("n"), "w");
  const std::string content = "fetched-again";
  ingest_bytes(cache, id, spec(ArtifactType::MODEL_WEIGHTS, "weights", "1"), content);
  cache.quarantine(id, "bad download");
  auto r = ingest_bytes(cache, id, spec(ArtifactType::MODEL_WEIGHTS, "weights", "1"), content);
  CHECK(r.admitted);
  CHECK(r.validation == ValidationState::VALID);
  LookupRequest req; req.id = id;
  auto lr = cache.lookup(req);
  CHECK(lr.valid);
  CHECK(lr.backing_location && model_cache::platform::path_exists(*lr.backing_location));
  CHECK_EQ(cache.verify(), 0u);
}

TEST(external_rejection_leaves_a_shared_backing_alone) {
  TempDir td;
  ModelCache cache(td.join("root"));
  const ArtifactId a(ArtifactNamespace("n"), "trusted");
  const ArtifactId b(ArtifactNamespace("n"), "checked");
  const std::string content = "bytes-two-callers-have";
  ingest_bytes(cache, a, spec(ArtifactType::MODEL_WEIGHTS, "weights", "1"), content);
  IngestSpec s = spec(ArtifactType::MODEL_WEIGHTS, "weights", "1");
  s.validation_mode = ValidationMode::External;
  s.external_validator = [](const Digest&, std::uint64_t) { return false; };
  auto r = ingest_bytes(cache, b, s, content);
  CHECK(!r.admitted);
  CHECK(!cache.exists(b));
  // The bytes are the valid backing of another artifact and stay where they are.
  LookupRequest req; req.id = a;
  auto lr = cache.lookup(req);
  CHECK(lr.valid);
  CHECK(lr.backing_location && model_cache::platform::path_exists(*lr.backing_location));
  CHECK_EQ(cache.verify(), 0u);
}

TEST(recovery_keeps_stored_files_whose_names_contain_tmp) {
  TempDir td;
  const ArtifactId id(ArtifactNamespace("n"), "weights.tmp");
  const std::string content = "not-a-temp-file";
  const std::string leftover = td.join("root") + "/meta/artifacts/n/leftover.mc.tmp1700000000000.42.7";
  {
    ModelCache cache(td.join("root"));
    CHECK(ingest_bytes(cache, id, spec(ArtifactType::MODEL_WEIGHTS, "weights", "1"), content).admitted);
    cache.alias_set("stable.tmp", id);
    // What an interrupted atomic write leaves behind.
    model_cache::platform::write_file_atomic(leftover, content.data(), content.size());
    CHECK(model_cache::platform::path_exists(leftover));
  }
  {
    ModelCache cache(td.join("root"));   // recovery runs here
    CHECK(!model_cache::platform::path_exists(leftover));
    CHECK(cache.exists(id));
    LookupRequest req; req.id = id;
    auto lr = cache.lookup(req);
    CHECK(lr.found);
    CHECK(lr.valid);
    CHECK(lr.backing_location && model_cache::platform::path_exists(*lr.backing_location));
    CHECK(cache.alias_get("stable.tmp").has_value());
  }
}
