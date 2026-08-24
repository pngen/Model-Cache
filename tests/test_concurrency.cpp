#include "test_helpers.hpp"
#include <thread>
#include <atomic>

TEST(concurrent_ingest_lookup) {
  TempDir td;
  ModelCache cache(td.join("root"));
  const int nthreads = 8;
  const int per = 150;
  std::atomic<int> errors{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < nthreads; ++t) {
    threads.emplace_back([&cache, &errors, t]() {
      try {
        for (int i = 0; i < per; ++i) {
          const std::string ns = "n" + std::to_string(t);
          const std::string name = "a" + std::to_string(i);
          const ArtifactId id(ArtifactNamespace(ns), name);
          const std::string content = ns + "/" + name + "/payload";
          ingest_bytes(cache, id, spec(ArtifactType::MODEL_SHARD, "shard", "1"), content);
          LookupRequest req; req.id = id;
          auto lr = cache.lookup(req);
          if (!lr.found) ++errors;
          if (i % 7 == 0) cache.pin(id);
          if (i % 11 == 0) cache.alias_set("alias-" + name, id);
        }
      } catch (...) { ++errors; }
    });
  }
  for (auto& th : threads) th.join();
  CHECK_EQ(errors.load(), 0);
  // Total artifacts = nthreads * per (unless evicted; use a big bound).
  auto s = cache.snapshot();
  CHECK_EQ(s.logical_artifact_count, (std::uint64_t)(nthreads * per));
  CHECK_GE(s.physical_backing_count, 1u);
}

TEST(concurrent_mixed_ops) {
  TempDir td;
  Config cfg;
  cfg.limits.max_total_bytes = 1ull << 40;   // effectively unbounded
  ModelCache cache(td.join("root"), cfg);
  std::atomic<int> errors{0};
  std::mutex diagmu;
  std::vector<std::string> diags;
  const int nthreads = 6;
  std::vector<std::thread> ths;
  for (int t = 0; t < nthreads; ++t) {
    ths.emplace_back([&, t]() {
      try {
        const ArtifactId id(ArtifactNamespace("comb"), "w" + std::to_string(t));
        const std::string content = "shared-" + std::to_string(t);   // idempotent re-ingest
        for (int i = 0; i < 100; ++i) {
          ingest_bytes(cache, id, spec(ArtifactType::MODEL_WEIGHTS, "weights", "1"), content);
          cache.pin(id);
          cache.unpin(id);
          Manifest m;
          m.model = ModelIdentity("org/m"); m.revision = ModelRevision("r" + std::to_string(t));
          m.artifacts = {id.canonical()};
          cache.manifest_add(m);
          LookupRequest req; req.id = id;
          auto lr = cache.lookup(req);
          if (!lr.found) ++errors;
        }
        cache.retire(id);
        cache.purge(id);
      } catch (const std::exception& e) { ++errors; { std::lock_guard<std::mutex> g(diagmu); diags.push_back(e.what()); } }
        catch (...) { ++errors; }
    });
  }
  for (auto& th : ths) th.join();
  for (auto& d : diags) std::printf("DIAG: %s\n", d.c_str());
  CHECK_EQ(errors.load(), 0);
}
