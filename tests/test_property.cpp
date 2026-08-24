#include "test_helpers.hpp"
#include <random>
#include <set>

namespace {
struct Prop {
  std::exponential_distribution<double> size_d{0.002};
  // content pool seeded for dedup
  std::mt19937_64 rng;
  std::vector<std::string> pool;
  std::vector<std::string> names;
  std::vector<std::string> nss;
  Prop() : rng(0xC0FFEEu) {
    for (int i = 0; i < 24; ++i) pool.push_back("payload-" + std::to_string(i) + "-xxxxxxxxxxxxxxxx");
    for (int i = 0; i < 30; ++i) names.push_back("a" + std::to_string(i));
    for (int i = 0; i < 3; ++i) nss.push_back("ns" + std::to_string(i));
  }
  std::string content() { return pool[static_cast<std::size_t>(rng() % pool.size())]; }
  ArtifactId id() {
    return ArtifactId(ArtifactNamespace(nss[rng() % nss.size()]), names[rng() % names.size()]);
  }
};
}  // namespace

TEST(property_randomized_ops) {
  TempDir td;
  Config cfg;
  cfg.limits.max_total_bytes = 1ull << 40;
  ModelCache cache(td.join("root"), cfg);
  Prop p;
  std::set<std::string> pinned;
  std::set<std::string> alive;   // ids we believe still exist (not purged)

  auto check_invariants = [&](const char* when) {
    auto s = cache.snapshot();
    // Valid invariants: non-negative accounting and bounded physical capacity.
    CHECK_GE(s.physical_backing_count, 0u);
    CHECK_GE(s.total_physical_bytes, 0u);
    CHECK_LE(s.total_physical_bytes, cfg.limits.max_total_bytes);
    CHECK_LE(s.physical_backing_count, 1u + s.logical_artifact_count * 32u);
    for (auto& k : pinned) { if (alive.count(k)) CHECK(cache.exists(id_from_key(k))); }
    (void)when;
  };

  for (int op = 0; op < 3000; ++op) {
    const auto id = p.id();
    const int kind = static_cast<int>(p.rng() % 10);
    switch (kind) {
      case 0: case 1: case 2: case 3: {   // ingest (duplicates possible -> dedup)
        auto r = ingest_bytes(cache, id, spec(ArtifactType::MODEL_WEIGHTS, "weights", "1"), p.content());
        if (r.admitted) { alive.insert(id.canonical()); }
        LookupRequest req; req.id = id;
        auto lr = cache.lookup(req);
        if (lr.found && !lr.artifact_key.empty()) CHECK_EQ(lr.artifact_key, std::string(id.ns.str() + "/" + id.name));
        break;
      }
      case 4: {
        if (cache.exists(id)) { cache.pin(id); pinned.insert(id.canonical()); alive.insert(id.canonical()); }
        break;
      }
      case 5: {
        if (cache.exists(id)) {
          try { cache.unpin(id); } catch (const model_cache::Error&) {}
          std::set<std::string> np;
          for (auto& k : pinned) if (k != id.canonical()) np.insert(k);
          pinned = np;
        }
        break;
      }
      case 6: {
        cache.alias_set("alias-" + std::to_string(op), id);
        break;
      }
      case 7: {
        if (cache.exists(id)) cache.supersede(id, "");
        break;
      }
      case 8: {
        if (cache.exists(id)) { cache.purge(id); alive.erase(id.canonical()); pinned.erase(id.canonical()); }
        break;
      }
      case 9: {
        if (op % 3 == 0) { cache.evict(static_cast<std::uint64_t>(p.rng() % 200000)); }
        break;
      }
    }
    if (op % 500 == 0) check_invariants("mid");
  }
  check_invariants("end");

  // Persistence round-trip preserves authoritative state.
  auto before = cache.snapshot();
  cache.save();
  cache.restore();
  auto after = cache.snapshot();
  CHECK_EQ(after.logical_artifact_count, before.logical_artifact_count);
  CHECK_EQ(after.physical_backing_count, before.physical_backing_count);
  CHECK_EQ(after.total_logical_bytes, before.total_logical_bytes);
  CHECK_EQ(after.total_physical_bytes, before.total_physical_bytes);
}
