#include "test_helpers.hpp"

TEST(alias_set_get_list) {
  TempDir td;
  ModelCache cache(td.join("root"));
  const ArtifactId id(ArtifactNamespace("n"), "w");
  ingest_bytes(cache, id, spec(ArtifactType::MODEL_WEIGHTS, "weights", "1"), "weights");
  cache.alias_set("latest", id);
  cache.alias_set("checkpoint", ArtifactId(ArtifactNamespace("n"), "w"));
  auto v = cache.alias_get("latest");
  CHECK(v.has_value());
  CHECK_EQ(*v, id.canonical());
  auto all = cache.alias_list();
  CHECK_EQ(all.size(), 2u);
  // atomic update of an alias to a different target
  const ArtifactId id2(ArtifactNamespace("n"), "w2");
  ingest_bytes(cache, id2, spec(ArtifactType::MODEL_WEIGHTS, "weights", "1"), "weights2");
  cache.alias_set("latest", id2);
  v = cache.alias_get("latest");
  CHECK_EQ(*v, id2.canonical());
}

TEST(supersede_bumps_generation) {
  TempDir td;
  ModelCache cache(td.join("root"));
  const ArtifactId id(ArtifactNamespace("n"), "w");
  ingest_bytes(cache, id, spec(ArtifactType::MODEL_WEIGHTS, "weights", "1"), "v1");
  CHECK_EQ(cache.generations(id), 1u);
  cache.supersede(id, "");   // generation bump, same content
  CHECK_EQ(cache.generations(id), 2u);
  auto rec = cache.record(id);
  CHECK(rec.has_value());
  CHECK_EQ(rec->current_generation, 2u);
}

TEST(supersede_creates_new_logical) {
  TempDir td;
  ModelCache cache(td.join("root"));
  const ArtifactId id(ArtifactNamespace("n"), "base");
  ingest_bytes(cache, id, spec(ArtifactType::MODEL_WEIGHTS, "weights", "1"), "weights");
  cache.supersede(id, "-v2");
  const ArtifactId s(ArtifactNamespace("n"), "base-v2");
  CHECK(cache.exists(s));
  auto s2 = cache.snapshot();
  CHECK(s2.logical_artifact_count >= 2u);
}

TEST(alias_invalid_rejected) {
  TempDir td;
  ModelCache cache(td.join("root"));
  const ArtifactId id(ArtifactNamespace("n"), "w");
  ingest_bytes(cache, id, spec(ArtifactType::MODEL_WEIGHTS, "weights", "1"), "w");
  bool threw = false;
  try { cache.alias_set("bad\nname", id); } catch (const model_cache::Error&) { threw = true; }
  CHECK(threw);
}

TEST(supersede_refuses_a_name_that_could_not_be_reloaded) {
  TempDir td;
  const std::string long_name(100, 'x');
  {
    ModelCache cache(td.join("root"));
    const ArtifactId id(ArtifactNamespace("n"), long_name);
    CHECK(ingest_bytes(cache, id, spec(ArtifactType::MODEL_WEIGHTS, "weights", "1"), "weights").admitted);
    // 100 + 160 characters: longer than any artifact name may be.
    CHECK_THROWS(cache.supersede(id, "-" + std::string(159, 's')), ErrorCode::invalid_identity);
    CHECK(cache.exists(id));
    CHECK_EQ(cache.snapshot().logical_artifact_count, 1u);
  }
  bool reopened = true;
  try { ModelCache again(td.join("root")); } catch (...) { reopened = false; }
  CHECK(reopened);
}

TEST(ingest_that_cannot_be_stored_leaves_nothing) {
  TempDir td;
  ModelCache cache(td.join("root"));
  // A valid name whose record file name is longer than a file name can be.
  const ArtifactId id(ArtifactNamespace("n"), std::string(254, 'x'));
  bool threw = false;
  try { ingest_bytes(cache, id, spec(ArtifactType::MODEL_WEIGHTS, "weights", "1"), "weights"); }
  catch (const model_cache::Error&) { threw = true; }
  CHECK(threw);
  CHECK(!cache.exists(id));
  CHECK_EQ(cache.snapshot().logical_artifact_count, 0u);
  CHECK_EQ(cache.gc(), 0u);
}

TEST(alias_that_cannot_be_stored_is_not_set) {
  TempDir td;
  ModelCache cache(td.join("root"));
  const ArtifactId id(ArtifactNamespace("n"), "w");
  ingest_bytes(cache, id, spec(ArtifactType::MODEL_WEIGHTS, "weights", "1"), "w");
  const std::string too_long(400, 'a');   // no file name can be this long
  bool threw = false;
  try { cache.alias_set(too_long, id); } catch (const model_cache::Error&) { threw = true; }
  CHECK(threw);
  CHECK(!cache.alias_get(too_long).has_value());
  CHECK_EQ(cache.alias_list().size(), 0u);
}
