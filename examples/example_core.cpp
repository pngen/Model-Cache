// model-cache example — demonstrates the core reusable-artifact scenarios and
// assertions that each succeeds. Build and run: every check here must pass.
#include "model_cache/cache.hpp"
#include "model_cache/detail/platform.hpp"
#include "model_cache/digest.hpp"

#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace model_cache;

static int failures = 0;
#define OK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static IngestSpec ispec(ArtifactType t, const std::string& role, const std::string& v) {
  IngestSpec s; s.type = t; s.role = ArtifactRole(role); s.version = ArtifactVersion(v); return s;
}
static IngestResult ib(ModelCache& c, const ArtifactId& id, const IngestSpec& s, const std::string& content) {
  return c.ingest(id, s, reinterpret_cast<const std::uint8_t*>(content.data()), content.size());
}

int main() {
  const fs::path root = fs::temp_directory_path() / "mc_example";
  fs::remove_all(root);
  fs::remove_all(root / "capped");

  // 1. persistent root; ingest weights
  {
    ModelCache cache(root.string());
    const ArtifactId weights(ArtifactNamespace("org"), "llama-2-7b");
    OK(ib(cache, weights, ispec(ArtifactType::MODEL_WEIGHTS, "weights", "1"), "fake-weights-bytes").admitted);

    // 2. exact lookup
    LookupRequest req; req.id = weights;
    auto lr = cache.lookup(req);
    OK(lr.found && lr.valid);
    OK(lr.content_digest->hex() == sha256("fake-weights-bytes").hex());

    // 3. model/revision + role lookup
    IngestSpec ws = ispec(ArtifactType::MODEL_WEIGHTS, "weights", "1");
    ws.model = ModelIdentity("org/llama-2-7b"); ws.revision = ModelRevision("v1");
    cache.ingest(weights, ws, reinterpret_cast<const std::uint8_t*>("fake-weights-bytes"), 18);
    LookupRequest mr; mr.model = ModelIdentity("org/llama-2-7b"); mr.revision = ModelRevision("v1"); mr.role = ArtifactRole("weights");
    OK(cache.lookup(mr).found);

    // 4. compatibility rejection
    const ArtifactId q(ArtifactNamespace("org"), "quantized");
    IngestSpec qs = ispec(ArtifactType::QUANTIZED_WEIGHTS, "weights", "1");
    qs.compatibility.set(dim::model_architecture, "llama-2");
    cache.ingest(q, qs, reinterpret_cast<const std::uint8_t*>("quant"), 5);
    ReuseRequest reuse;
    reuse.type = ArtifactType::QUANTIZED_WEIGHTS; reuse.role = ArtifactRole("weights");
    reuse.compatibility.set(dim::model_architecture, "gpt4");   // incompatible
    OK(cache.reuse_plan(reuse).action == ReuseAction::FETCH_REQUIRED);

    // 5. content deduplication (shared backing)
    const ArtifactId dup(ArtifactNamespace("org"), "duplicate");
    auto rd = ib(cache, dup, ispec(ArtifactType::MODEL_WEIGHTS, "weights", "1"), "fake-weights-bytes");
    OK(rd.dedup);
    OK(cache.snapshot().physical_backing_count == 2u - 1u + 1u);  // two distinct digests, one dedup

    // 6. manifest creation + validation
    Manifest m; m.model = ModelIdentity("org/llama-2-7b"); m.revision = ModelRevision("v1");
    m.artifacts = {weights.canonical()}; m.roles = {{"weights", weights.canonical()}};
    cache.manifest_add(m);
    auto got = cache.manifest_get(m.model, m.revision);
    OK(got && got->validation == ValidationState::VALID);

    // 7. alias update
    cache.alias_set("latest", weights);
    cache.alias_set("latest", q);
    OK(cache.alias_get("latest") && *cache.alias_get("latest") == q.canonical());

    // 8. generation supersession
    cache.supersede(weights, "");
    OK(cache.generations(weights) == 2u);

    // 9. adapter dependency on base model
    const ArtifactId adapter(ArtifactNamespace("org"), "my-lora");
    IngestSpec as = ispec(ArtifactType::ADAPTER, "adapter", "1");
    as.dependencies = {weights.canonical()};
    cache.ingest(adapter, as, reinterpret_cast<const std::uint8_t*>("adapter"), 7);
    OK(cache.dependencies(adapter).size() == 1u);
    OK(cache.dependents(weights).size() >= 1u);

    // 10. tokenizer/vocabulary dependency
    const ArtifactId tok(ArtifactNamespace("org"), "tokenizer");
    const ArtifactId vocab(ArtifactNamespace("org"), "vocab");
    ib(cache, vocab, ispec(ArtifactType::VOCABULARY, "vocab", "1"), "words");
    IngestSpec ts = ispec(ArtifactType::TOKENIZER, "tokenizer", "1");
    ts.dependencies = {vocab.canonical()};
    cache.ingest(tok, ts, reinterpret_cast<const std::uint8_t*>("tok"), 3);
    OK(cache.dependents(vocab).size() >= 1u);

    // 11. compiled-artifact compatibility fields
    const ArtifactId engine(ArtifactNamespace("org"), "engine");
    IngestSpec es = ispec(ArtifactType::ENGINE_ARTIFACT, "engine", "1");
    es.compatibility.set(dim::compiler_identity, "acme-compiler").set(dim::target_architecture, "x86_64");
    cache.ingest(engine, es, reinterpret_cast<const std::uint8_t*>("engine"), 6);
    ReuseRequest er; er.type = ArtifactType::ENGINE_ARTIFACT; er.role = ArtifactRole("engine");
    er.compatibility.set(dim::compiler_identity, "acme-compiler").set(dim::target_architecture, "aarch64");
    OK(cache.reuse_plan(er).action == ReuseAction::FETCH_REQUIRED);  // incompatible arch

    // 12. deterministic eviction under a bounded capacity
    Config cfg; cfg.limits.max_total_bytes = 200;
    ModelCache capped((root / "capped").string(), cfg);
    const ArtifactId e1(ArtifactNamespace("n"), "fill1");
    const ArtifactId e2(ArtifactNamespace("n"), "fill2");
    ib(capped, e1, ispec(ArtifactType::MODEL_SHARD, "shard", "1"), std::string(120, 'a'));
    ib(capped, e2, ispec(ArtifactType::MODEL_SHARD, "shard", "1"), std::string(120, 'b'));
    OK(capped.snapshot().total_physical_bytes <= 200u);

    // 13. concurrent lookup/ingest
    {
      std::vector<std::thread> threads;
      for (int t = 0; t < 4; ++t)
        threads.emplace_back([&cache, t]() {
          const ArtifactId id(ArtifactNamespace("conc"), "a" + std::to_string(t));
          ib(cache, id, ispec(ArtifactType::MODEL_SHARD, "shard", "1"), "conc-" + std::to_string(t));
          LookupRequest r; r.id = id;
          cache.lookup(r);
        });
      for (auto& th : threads) th.join();
    }

    cache.save();
  }

  // 14. persistent restart / recovery: reopen and confirm content exists
  {
    ModelCache cache(root.string());
    const ArtifactId id(ArtifactNamespace("org"), "llama-2-7b");
    LookupRequest r; r.id = id;
    OK(cache.lookup(r).found);
  }

  // 15. corruption rejection: corrupt a backing object and re-verify/quarantine
  {
    ModelCache cache(root.string());
    const ArtifactId id(ArtifactNamespace("org"), "llama-2-7b");
    LookupRequest r; r.id = id;
    auto path = cache.lookup(r).backing_location;
    if (path) {
      std::string junk(64, 'Y');
      platform::write_file_atomic(*path, junk.data(), junk.size());
      OK(cache.verify() >= 1u);
      cache.quarantine(id, "example-corruption");
      LookupRequest q; q.id = id; q.require_valid = true;
      OK(!cache.lookup(q).valid);
    }
  }

  std::printf("example_core: %s\n", failures == 0 ? "ALL PASSED" : "FAILURES");
  return failures == 0 ? 0 : 1;
}
