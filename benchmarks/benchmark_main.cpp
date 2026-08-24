// model-cache benchmark — measures completed work (not submission).
#include "model_cache/cache.hpp"
#include "model_cache/digest.hpp"
#include "model_cache/record.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace model_cache;

static double now_s() {
  using namespace std::chrono;
  return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}
template <typename F>
double time_us(F&& f, int reps) {
  const double t0 = now_s();
  for (int i = 0; i < reps; ++i) f();
  return (now_s() - t0) * 1e6 / reps;
}

static IngestSpec spec(ArtifactType t, const std::string& role) {
  IngestSpec s; s.type = t; s.role = ArtifactRole(role); s.version = ArtifactVersion("1"); return s;
}

int main() {
  const fs::path root = fs::temp_directory_path() / "mc_bench";

  // SHA-256 throughput
  {
    std::string buf(1 << 20, 'x');
    const std::uint64_t reps = 200;
    const double t0 = now_s();
    Digest d;
    for (std::uint64_t i = 0; i < reps; ++i) d = sha256(buf.data(), buf.size());
    const double dt = now_s() - t0;
    std::printf("sha256 throughput: %.2f MiB/s (%.2f us/op)\n", (reps * buf.size()) / (dt * 1e6), dt * 1e6 / reps);
    (void)d;
  }

  std::printf("\nscale,ingest_us,ingest_ops_s,lookup_us,dup_ingest_us,manifest_us\n");
  for (int n : {1, 10, 100, 1000}) {
    ModelCache cache(root.string());
    std::vector<std::string> keys;
    // populate
    for (int i = 0; i < n; ++i) {
      const ArtifactId id(ArtifactNamespace("bench"), "a" + std::to_string(i));
      const std::string payload = "payload-" + std::to_string(i) + std::string(4096, 'x');
      cache.ingest(id, spec(ArtifactType::MODEL_WEIGHTS, "weights"), reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size());
      keys.push_back(id.canonical());
    }
    // ingest throughput
    const double ingest_us = time_us([&]() {
      const ArtifactId id(ArtifactNamespace("bench"), "a" + std::to_string(n));
      std::string p(4096, 'z');
      cache.ingest(id, spec(ArtifactType::MODEL_WEIGHTS, "weights"), reinterpret_cast<const std::uint8_t*>(p.data()), p.size());
    }, 50);
    // exact lookup throughput
    const double lookup_us = time_us([&]() {
      LookupRequest r; r.id = ArtifactId(ArtifactNamespace("bench"), "a0");
      cache.lookup(r);
    }, n > 1000 ? 1000 : 5000);
    // duplicate ingest (dedup hit)
    const double dup_us = time_us([&]() {
      const std::string p = "payload-3" + std::string(4096, 'x');
      const ArtifactId id(ArtifactNamespace("bench"), "a3");
      cache.ingest(id, spec(ArtifactType::MODEL_WEIGHTS, "weights"), reinterpret_cast<const std::uint8_t*>(p.data()), p.size());
    }, 100);
    // manifest serialization
    const double man_us = time_us([&]() {
      Manifest m; m.model = ModelIdentity("bench/m"); m.revision = ModelRevision("r");
      m.artifacts = keys;
      canon::Writer w; m.to_canonical(w); (void)w.digest();
    }, 200);
    const double ingest_ops_s = 1.0 / (ingest_us / 1e6);
    std::printf("%d,%.2f,%.1f,%.2f,%.2f,%.2f\n", n, ingest_us, ingest_ops_s, lookup_us, dup_us, man_us);
  }

  // snapshot + eviction selection at a fixed scale
  {
    ModelCache cache(root.string());
    for (int i = 0; i < 5000; ++i) {
      const ArtifactId id(ArtifactNamespace("snap"), "a" + std::to_string(i));
      const std::string p = "snap-" + std::to_string(i);
      cache.ingest(id, spec(ArtifactType::MODEL_WEIGHTS, "weights"), reinterpret_cast<const std::uint8_t*>(p.data()), p.size());
    }
    const double snap_us = time_us([&]() { (void)cache.snapshot(); }, 200);
    const double evict_us = time_us([&]() {
      cache.evict(1000000);
    }, 1);
    std::printf("\nsnapshot_us=%.2f  evict_us=%.2f (5000 artifacts)\n", snap_us, evict_us);
  }

  // persistence save / recovery load
  {
    const auto s = fs::temp_directory_path() / "mc_bench_persist";
    fs::remove_all(s);
    double save_us = 0, recover_us = 0;
    {
      ModelCache cache(s.string());
      for (int i = 0; i < 2000; ++i) {
        const ArtifactId id(ArtifactNamespace("p"), "a" + std::to_string(i));
        const std::string c = "p" + std::to_string(i);
        cache.ingest(id, spec(ArtifactType::MODEL_SHARD, "shard"), reinterpret_cast<const std::uint8_t*>(c.data()), c.size());
      }
      const double t0 = now_s();
      cache.save();
      save_us = (now_s() - t0) * 1e6;
    }
    {
      const double t0 = now_s();
      ModelCache cache(s.string());   // recovery + reload
      recover_us = (now_s() - t0) * 1e6;
      std::printf("save_us=%.2f  recover_us=%.2f (2000 artifacts)\n", save_us, recover_us);
    }
  }

  std::printf("benchmark complete\n");
  return 0;
}
