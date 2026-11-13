#include "test_helpers.hpp"
#include "model_cache/compatibility.hpp"

TEST(compat_canonical_deterministic) {
  CompatibilityKey a;
  a.set("b", "2").set("a", "1");
  a.set(dim::model_architecture, "llama");
  const std::string c1 = a.canonical();
  CompatibilityKey b;
  b.set(dim::model_architecture, "llama").set("a", "1").set("b", "2");
  CHECK_EQ(c1, b.canonical());
  canon::Writer w;
  a.to_canonical(w);
  canon::Reader r(w.bytes().data(), w.size());
  auto back = CompatibilityKey::from_canonical(r);
  r.require_consumed();
  CHECK_EQ(back.canonical(), c1);
  CHECK(a == back);
}

TEST(compat_strict_matching) {
  CompatibilityKey request;
  request.set(dim::model_architecture, "llama-2").set(dim::precision, "fp16");
  CompatibilityKey candidate;
  candidate.set(dim::model_architecture, "llama-2").set(dim::precision, "fp16").set(dim::backend_identity, "cuda");
  CHECK(CompatibilityKey::compatible(request, candidate));
  CompatibilityKey wrong;
  wrong.set(dim::model_architecture, "llama-2").set(dim::precision, "fp32");
  CHECK(!CompatibilityKey::compatible(request, wrong));
  CompatibilityKey missing;
  missing.set(dim::model_architecture, "llama-2");
  CHECK(!CompatibilityKey::compatible(request, missing));
  CompatibilityKey wild;
  wild.set(dim::precision, "fp16");
  CHECK(!CompatibilityKey::compatible(request, wild));
  CompatibilityKey req2;
  req2.set(dim::model_architecture, "llama-2");
  req2.set(dim::precision, "");
  CHECK(CompatibilityKey::compatible(req2, candidate));
}

TEST(compat_unmet_dimensions) {
  CompatibilityKey req;
  req.set(dim::backend_identity, "vllm").set(dim::precision, "fp16");
  CompatibilityKey cand;
  cand.set(dim::backend_identity, "sglang").set(dim::precision, "fp16");
  auto unmet = cand.unmet(req);
  CHECK_EQ(unmet.size(), 1u);
  CHECK_EQ(unmet[0], std::string("backend_identity"));
}

TEST(new_content_carries_its_own_description) {
  TempDir td;
  ModelCache cache(td.join("root"));
  const ArtifactId id(ArtifactNamespace("n"), "engine");
  IngestSpec v1 = spec(ArtifactType::COMPILED_ARTIFACT, "engine", "1");
  v1.compatibility.set(dim::runtime_abi, "abi-1");
  v1.dependencies = {"n/base"};
  ingest_bytes(cache, id, v1, "built-for-abi-1");
  IngestSpec v2 = spec(ArtifactType::COMPILED_ARTIFACT, "engine", "2");
  v2.compatibility.set(dim::runtime_abi, "abi-2");
  CHECK(ingest_bytes(cache, id, v2, "built-for-abi-2").admitted);
  auto rec = cache.record(id);
  CHECK(rec.has_value());
  if (rec) {
    CHECK_EQ(*rec->compatibility.get(dim::runtime_abi), std::string("abi-2"));
    CHECK_EQ(rec->version.str(), std::string("2"));
    CHECK(rec->dependencies.empty());
  }
  // The bytes that are current were built for abi-2: a request for abi-1 misses.
  LookupRequest old;
  old.type = ArtifactType::COMPILED_ARTIFACT;
  old.compatibility.set(dim::runtime_abi, "abi-1");
  CHECK(!cache.lookup(old).found);
  LookupRequest cur;
  cur.type = ArtifactType::COMPILED_ARTIFACT;
  cur.compatibility.set(dim::runtime_abi, "abi-2");
  CHECK(cache.lookup(cur).found);
}

TEST(lookup_checks_every_requested_attribute) {
  TempDir td;
  ModelCache cache(td.join("root"));
  IngestSpec x86 = spec(ArtifactType::ENGINE_ARTIFACT, "engine", "1");
  x86.architecture = ArtifactArchitecture("x86_64");
  x86.format = ArtifactFormat("plan");
  ingest_bytes(cache, ArtifactId(ArtifactNamespace("n"), "engine-x86"), x86, "x86-bytes");
  IngestSpec arm = spec(ArtifactType::ENGINE_ARTIFACT, "engine", "2");
  arm.architecture = ArtifactArchitecture("arm64");
  arm.format = ArtifactFormat("plan");
  ingest_bytes(cache, ArtifactId(ArtifactNamespace("n"), "engine-arm"), arm, "arm-bytes");
  LookupRequest req;
  req.type = ArtifactType::ENGINE_ARTIFACT;
  req.architecture = ArtifactArchitecture("arm64");
  auto lr = cache.lookup(req);
  CHECK(lr.found);
  CHECK_EQ(lr.artifact_key, std::string("n/engine-arm"));
  req.architecture.reset();
  req.version = ArtifactVersion("2");
  CHECK_EQ(cache.lookup(req).artifact_key, std::string("n/engine-arm"));
  req.version.reset();
  req.format = ArtifactFormat("onnx");
  CHECK(!cache.lookup(req).found);
}

TEST(lookup_never_calls_a_quarantined_artifact_valid) {
  TempDir td;
  ModelCache cache(td.join("root"));
  const ArtifactId id(ArtifactNamespace("n"), "tok");
  ingest_bytes(cache, id, spec(ArtifactType::TOKENIZER, "tokenizer", "1"), "tokens");
  cache.quarantine(id, "suspicious");
  LookupRequest req;
  req.type = ArtifactType::TOKENIZER;
  req.require_valid = false;   // "tell me about it even if it is not usable"
  auto lr = cache.lookup(req);
  CHECK(lr.found);
  CHECK(!lr.valid);
  CHECK(lr.validation == ValidationState::QUARANTINED);
}

TEST(reuse_plan_prefers_a_valid_candidate) {
  TempDir td;
  ModelCache cache(td.join("root"));
  const ArtifactId bad(ArtifactNamespace("n"), "a-corrupt");
  const ArtifactId good(ArtifactNamespace("n"), "b-good");
  ingest_bytes(cache, bad, spec(ArtifactType::TOKENIZER, "tokenizer", "1"), "one");
  ingest_bytes(cache, good, spec(ArtifactType::TOKENIZER, "tokenizer", "1"), "two");
  cache.pin(bad);                    // would otherwise be preferred
  cache.quarantine(bad, "corrupt");
  ReuseRequest req;
  req.type = ArtifactType::TOKENIZER;
  req.role = ArtifactRole("tokenizer");
  auto plan = cache.reuse_plan(req);
  CHECK(plan.action == ReuseAction::REUSE_LOCAL);
  CHECK(plan.artifact_key.has_value());
  if (plan.artifact_key) CHECK_EQ(*plan.artifact_key, std::string("n/b-good"));
  // Asking for one artifact by key gets that artifact, whatever its state.
  req.logical_key = "n/a-corrupt";
  auto only = cache.reuse_plan(req);
  CHECK(only.action == ReuseAction::REJECT_CORRUPT);
  CHECK_EQ(only.candidates.size(), 1u);
}
