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
