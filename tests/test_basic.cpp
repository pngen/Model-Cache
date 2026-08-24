#include "framework.hpp"
#include "test_util.hpp"

#include "model_cache/cache.hpp"
#include "model_cache/detail/platform.hpp"
#include "model_cache/digest.hpp"
#include "model_cache/error.hpp"
#include "model_cache/record.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace model_cache;
using testfw::TempDir;

namespace {
IngestResult ingest_bytes(ModelCache& c, const ArtifactId& id, const IngestSpec& s,
                          const std::string& content) {
  return c.ingest(id, s, reinterpret_cast<const std::uint8_t*>(content.data()), content.size());
}
Digest digest_hex(const std::string& h) { return Digest::from_hex(h); }
void expect_invalid_identity(const std::string& name) {
  bool threw = false;
  try {
    ArtifactId bad(ArtifactNamespace("a"), name);
    (void)bad;
  } catch (const model_cache::Error& e) {
    threw = (e.code() == ErrorCode::invalid_identity);
  }
  CHECK(threw);
}
IngestSpec spec(ArtifactType t, const std::string& role, const std::string& ver) {
  IngestSpec s;
  s.type = t;
  s.role = ArtifactRole(role);
  s.version = ArtifactVersion(ver);
  return s;
}
const std::string kEmptySha = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
const std::string kAbcSha = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
}  // namespace

// ---------------------------------------------------------------------------
TEST(sha256_known_vectors) {
  CHECK_EQ(sha256("").hex(), kEmptySha);
  CHECK_EQ(sha256("abc").hex(), kAbcSha);
  const std::string body = "Model Cache is reusable model artifact infrastructure.";
  Digest a = sha256(body);
  CHECK_EQ(a.hex(), sha256(body.data(), body.size()).hex());
  CHECK_EQ(Digest::from_hex(a.hex()).hex(), a.hex());
}

TEST(artifact_id_roundtrip) {
  ArtifactId id(ArtifactNamespace("org"), "llama-2-7b");
  CHECK_EQ(id.canonical(), "org/llama-2-7b");
  Digest d = sha256("content");
  ArtifactId pin(ArtifactNamespace("org"), "llama-2-7b", d);
  CHECK(pin.canonical().find('#') != std::string::npos);
  // canonical serialization round-trip
  canon::Writer w;
  id.to_canonical(w);
  canon::Reader r(w.bytes().data(), w.size());
  auto back = ArtifactId::from_canonical(r);
  r.require_consumed();
  CHECK_EQ(back.canonical(), id.canonical());
  // invalid identity rejected
  expect_invalid_identity("..");
  expect_invalid_identity("x/y");
}

// ---------------------------------------------------------------------------
TEST(ingest_and_exact_lookup) {
  TempDir td;
  ModelCache cache(td.join("root"));
  const ArtifactId id(ArtifactNamespace("ns"), "weights");
  const std::string content = "model-weights-bytes";
  auto r = ingest_bytes(cache, id, spec(ArtifactType::MODEL_WEIGHTS, "weights", "1.0"), content);
  CHECK(r.admitted);
  CHECK(r.generation == 1);
  CHECK_EQ(r.validation, ValidationState::VALID);
  CHECK(cache.exists(id));
  LookupRequest req;
  req.id = id;
  auto lr = cache.lookup(req);
  CHECK(lr.found);
  CHECK(lr.valid);
  CHECK_GE(lr.size, content.size());
  CHECK_EQ(lr.content_digest->hex(), sha256(content).hex());
}

TEST(dedup_shared_backing) {
  TempDir td;
  ModelCache cache(td.join("root"));
  const std::string content = "identical-payload-...";
  const ArtifactId a(ArtifactNamespace("n"), "a");
  const ArtifactId b(ArtifactNamespace("n"), "b");
  auto ra = ingest_bytes(cache, a, spec(ArtifactType::MODEL_SHARD, "shard", "1"), content);
  auto rb = ingest_bytes(cache, b, spec(ArtifactType::MODEL_SHARD, "shard", "1"), content);
  CHECK(ra.admitted); CHECK(rb.admitted);
  CHECK(rb.dedup);            // second ingest reused shared backing
  CHECK(ra.digest == rb.digest);
  auto snap = cache.snapshot();
  CHECK_EQ(snap.physical_backing_count, 1u);   // one distinct object
  CHECK_EQ(snap.logical_artifact_count, 2u);
  CHECK_EQ(snap.dedup_bytes_saved, content.size());   // one copy saved
  CHECK_EQ(snap.total_physical_bytes, content.size());
  CHECK_EQ(snap.total_logical_bytes, 2 * content.size());
}

TEST(persistence_roundtrip) {
  TempDir td;
  const std::string content = "persist-me";
  {
    ModelCache cache(td.join("root"));
    const ArtifactId id(ArtifactNamespace("ns"), "w");
    auto r = ingest_bytes(cache, id, spec(ArtifactType::MODEL_WEIGHTS, "weights", "1"), content);
    CHECK(r.admitted);
    cache.alias_set("latest", id);
    cache.save();
  }  // cache destroyed
  {
    ModelCache cache(td.join("root"));  // reopen (recovery + reload)
    const ArtifactId id(ArtifactNamespace("ns"), "w");
    LookupRequest req; req.id = id;
    auto lr = cache.lookup(req);
    CHECK(lr.found);
    CHECK(lr.valid);
    CHECK_EQ(lr.content_digest->hex(), sha256(content).hex());
    // read bytes back from backing and compare
    auto path = lr.backing_location;
    CHECK(!path->empty());
    auto stored = model_cache::platform::read_file(*path, 1 << 20);
    CHECK_EQ(std::string(reinterpret_cast<const char*>(stored.data()), stored.size()), content);
    auto alias = cache.alias_get("latest");
    CHECK(alias.has_value());
    CHECK_EQ(*alias, id.canonical());
  }
}