#pragma once
// Model Cache — bounded capacity configuration.
//
// Model Cache must never silently grow without bound. Every limit below has an
// explicit default and an explicit action when reached. Persisted limits and
// parameter validation feed into one Config struct that is validated at open.

#include "model_cache/error.hpp"

#include <cstdint>
#include <string>

namespace model_cache {

struct Limits {
  std::uint64_t max_total_bytes = 64ull * 1024 * 1024 * 1024;   // 64 GiB cache
  std::uint64_t max_object_count = 1'000'000;
  std::uint64_t max_manifest_count = 1'000'000;
  std::uint64_t max_dependency_edges = 16'000'000;
  std::uint64_t max_metadata_bytes = 512ull * 1024 * 1024;      // 512 MiB
  std::uint64_t max_artifact_size = 16ull * 1024 * 1024 * 1024; // single artifact cap
  std::uint64_t max_staging_bytes = 16ull * 1024 * 1024 * 1024;
  std::uint32_t max_concurrent_ingests = 32;
  std::uint32_t max_subscriptions = 4096;
  std::uint32_t max_event_backlog = 1'000'000;
  std::uint32_t max_serialized_message_bytes = 128ull * 1024 * 1024;
  std::uint32_t max_path_length = 1024;
  std::uint32_t max_metadata_field_bytes = 16 * 1024 * 1024;
};

struct Config {
  Limits limits;
  bool verify_on_lookup = false;      // re-hash content on lookup when policy requests
  bool deduplicate = true;            // share backing for identical content
  bool enforce_compatibility = true;  // reject incompatible reuse
};

/// Validate a Config, throwing on invalid bounds (negative/zero for required
/// positive limits, sanity relationships).
void validate_config(const Config& c);

inline void validate_limit(uint64_t v, const char* name) {
  if (v == 0) fail(ErrorCode::invalid_argument, std::string("limit must be positive: ") + name);
}

}  // namespace model_cache
