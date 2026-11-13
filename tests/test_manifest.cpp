#include "test_helpers.hpp"

TEST(manifest_add_get_list) {
  TempDir td;
  ModelCache cache(td.join("root"));
  ingest_bytes(cache, ArtifactId(ArtifactNamespace("n"), "w1"), spec(ArtifactType::MODEL_WEIGHTS, "weights", "1"), "w1");
  ingest_bytes(cache, ArtifactId(ArtifactNamespace("n"), "w2"), spec(ArtifactType::MODEL_SHARD, "shard", "1"), "w2");
  Manifest m;
  m.model = ModelIdentity("org/model");
  m.revision = ModelRevision("rev1");
  m.artifacts = {"n/w1", "n/w2"};
  m.roles = {{"weights", "n/w1"}, {"shard", "n/w2"}};
  cache.manifest_add(m);
  auto got = cache.manifest_get(ModelIdentity("org/model"), ModelRevision("rev1"));
  CHECK(got.has_value());
  CHECK_EQ(got->validation, ValidationState::VALID);
  CHECK_EQ(cache.manifest_list().size(), 1u);
  cache.manifest_invalidate(ModelIdentity("org/model"), ModelRevision("rev1"));
  got = cache.manifest_get(ModelIdentity("org/model"), ModelRevision("rev1"));
  CHECK_EQ(got->validation, ValidationState::INVALID);
}

TEST(manifest_deterministic_hash) {
  Manifest a, b;
  a.model = ModelIdentity("org/m"); a.revision = ModelRevision("r1");
  a.artifacts = {"n/w1", "n/w2"};
  a.roles = {{"weights", "n/w1"}};
  b.model = ModelIdentity("org/m"); b.revision = ModelRevision("r1");
  b.artifacts = {"n/w1", "n/w2"};
  b.roles = {{"weights", "n/w1"}};
  canon::Writer wa, wb;
  a.to_canonical(wa);
  b.to_canonical(wb);
  CHECK(wa.digest() == wb.digest());
  // Order independence for the roles map.
  Manifest c;
  c.model = ModelIdentity("org/m"); c.revision = ModelRevision("r1");
  c.artifacts = {"n/w1", "n/w2"};
  c.roles = {{"shard", "n/w2"}, {"weights", "n/w1"}};
  canon::Writer wc;
  c.to_canonical(wc);
  Manifest d;
  d.model = ModelIdentity("org/m"); d.revision = ModelRevision("r1");
  d.artifacts = {"n/w1", "n/w2"};
  d.roles = {{"weights", "n/w1"}, {"shard", "n/w2"}};
  canon::Writer wd;
  d.to_canonical(wd);
  CHECK(wc.digest() == wd.digest());
}

TEST(manifest_missing_member_invalid) {
  TempDir td;
  ModelCache cache(td.join("root"));
  Manifest m;
  m.model = ModelIdentity("org/m"); m.revision = ModelRevision("r1");
  m.artifacts = {"n/does-not-exist"};
  cache.manifest_add(m);
  auto got = cache.manifest_get(ModelIdentity("org/m"), ModelRevision("r1"));
  CHECK_EQ(got->validation, ValidationState::INVALID);
}

TEST(manifest_is_invalid_once_a_member_is_gone) {
  TempDir td;
  {
    ModelCache cache(td.join("root"));
    const ArtifactId w(ArtifactNamespace("n"), "w1");
    ingest_bytes(cache, w, spec(ArtifactType::MODEL_WEIGHTS, "weights", "1"), "w1");
    Manifest m;
    m.model = ModelIdentity("org/model"); m.revision = ModelRevision("rev1");
    m.artifacts = {"n/w1"};
    cache.manifest_add(m);
    CHECK_EQ(cache.manifest_get(ModelIdentity("org/model"), ModelRevision("rev1"))->validation, ValidationState::VALID);
    cache.purge(w);
    CHECK_EQ(cache.manifest_get(ModelIdentity("org/model"), ModelRevision("rev1"))->validation, ValidationState::INVALID);
  }
  {
    ModelCache cache(td.join("root"));
    auto got = cache.manifest_get(ModelIdentity("org/model"), ModelRevision("rev1"));
    CHECK(got.has_value());
    if (got) CHECK_EQ(got->validation, ValidationState::INVALID);
  }
}

TEST(manifest_survives_reopen) {
  TempDir td;
  {
    ModelCache cache(td.join("root"));
    ingest_bytes(cache, ArtifactId(ArtifactNamespace("n"), "w1"), spec(ArtifactType::MODEL_WEIGHTS, "weights", "1"), "w1");
    Manifest m;
    m.model = ModelIdentity("org/model"); m.revision = ModelRevision("rev1");
    m.artifacts = {"n/w1"};
    cache.manifest_add(m);
  }
  {
    ModelCache cache(td.join("root"));
    CHECK_EQ(cache.manifest_list().size(), 1u);
    auto got = cache.manifest_get(ModelIdentity("org/model"), ModelRevision("rev1"));
    CHECK(got.has_value());
    if (got) {
      CHECK_EQ(got->validation, ValidationState::VALID);
      CHECK_EQ(got->artifacts.size(), 1u);
    }
  }
}

TEST(manifest_bounds_are_enforced) {
  TempDir td;
  Config cfg;
  cfg.limits.max_manifest_count = 1;
  cfg.limits.max_metadata_bytes = 4096;
  {
    ModelCache cache(td.join("root"), cfg);
    Manifest m1;
    m1.model = ModelIdentity("org/m"); m1.revision = ModelRevision("r1");
    cache.manifest_add(m1);
    Manifest m2;
    m2.model = ModelIdentity("org/m"); m2.revision = ModelRevision("r2");
    CHECK_THROWS(cache.manifest_add(m2), ErrorCode::capacity_exceeded);
    CHECK_NO_THROW(cache.manifest_add(m1));   // replacing one that exists adds none
    CHECK_EQ(cache.manifest_list().size(), 1u);
    // A manifest that could not be read back under the metadata bound is not written.
    Manifest big = m1;
    for (int i = 0; i < 400; ++i) big.artifacts.push_back("n/artifact-number-" + std::to_string(i));
    CHECK_THROWS(cache.manifest_add(big), ErrorCode::capacity_exceeded);
    CHECK_EQ(cache.manifest_get(ModelIdentity("org/m"), ModelRevision("r1"))->artifacts.size(), 0u);
  }
  bool reopened = true;
  try { ModelCache again(td.join("root"), cfg); } catch (...) { reopened = false; }
  CHECK(reopened);
}
