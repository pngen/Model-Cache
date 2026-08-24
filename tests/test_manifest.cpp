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
