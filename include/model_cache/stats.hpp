#pragma once
// Model Cache — bounded statistics.
//
// Statistics are purely observational and never influence correctness unless the
// eviction/admission policy explicitly reads them as an input.

#include <atomic>
#include <cstdint>

namespace model_cache {

class Stats {
 public:
  std::atomic<std::uint64_t> lookups{0};
  std::atomic<std::uint64_t> hits{0};
  std::atomic<std::uint64_t> misses{0};
  std::atomic<std::uint64_t> compatible_hits{0};
  std::atomic<std::uint64_t> incompatible_candidates{0};
  std::atomic<std::uint64_t> bytes_reused{0};
  std::atomic<std::uint64_t> bytes_ingested{0};
  std::atomic<std::uint64_t> bytes_deduplicated{0};
  std::atomic<std::uint64_t> artifacts_admitted{0};
  std::atomic<std::uint64_t> artifacts_rejected{0};
  std::atomic<std::uint64_t> artifacts_evicted{0};
  std::atomic<std::uint64_t> corruption_detections{0};
  std::atomic<std::uint64_t> quarantine_events{0};
  std::atomic<std::uint64_t> validation_failures{0};
  std::atomic<std::uint64_t> restores{0};
  std::atomic<std::uint64_t> recoveries{0};
  std::atomic<std::uint64_t> ingest_latency_ns{0};
  std::atomic<std::uint64_t> ingest_count{0};
  std::atomic<std::uint64_t> lookup_latency_ns{0};
  std::atomic<std::uint64_t> lookup_count{0};

  void record_ingest_latency(std::uint64_t ns) {
    ingest_latency_ns.fetch_add(ns, std::memory_order_relaxed);
    ingest_count.fetch_add(1, std::memory_order_relaxed);
  }
  void record_lookup_latency(std::uint64_t ns) {
    lookup_latency_ns.fetch_add(ns, std::memory_order_relaxed);
    lookup_count.fetch_add(1, std::memory_order_relaxed);
  }
  std::uint64_t avg_ingest_latency_ns() const {
    const auto c = ingest_count.load(std::memory_order_relaxed);
    return c ? ingest_latency_ns.load(std::memory_order_relaxed) / c : 0;
  }
  std::uint64_t avg_lookup_latency_ns() const {
    const auto c = lookup_count.load(std::memory_order_relaxed);
    return c ? lookup_latency_ns.load(std::memory_order_relaxed) / c : 0;
  }
};

}  // namespace model_cache
