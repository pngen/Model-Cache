#pragma once
// Model Cache — primary runtime facade.
//
// Model Cache owns reusable model artifacts as cache objects and metadata:
// identity, manifests, content-addressable backing, validation, compatibility,
// admission, eviction, persistence, recovery, aliases, generations, dependency
// graphs, deduplication, integrity, quarantine, and reuse discovery. It does NOT
// execute model loading, inference, compilation, network fetching, or serving.
//
// The facade is genuinely thread-safe and never invokes external callbacks while
// holding an internal lock.

#include "model_cache/compatibility.hpp"
#include "model_cache/config.hpp"
#include "model_cache/events.hpp"
#include "model_cache/identity.hpp"
#include "model_cache/record.hpp"
#include "model_cache/stats.hpp"
#include "model_cache/types.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace model_cache {

using StreamReadFn = std::function<std::size_t(std::uint8_t* buf, std::size_t max)>;

enum class ValidationMode { Validate, Trust, External };

struct IngestSpec {
  ArtifactType type = ArtifactType::AUXILIARY;
  ArtifactRole role;
  ArtifactVersion version;
  std::optional<ArtifactVariant> variant;
  std::optional<ArtifactFormat> format;
  std::optional<ArtifactArchitecture> architecture;
  CompatibilityKey compatibility;
  std::vector<std::string> dependencies;
  std::map<std::string, std::string> provenance;
  std::optional<ModelIdentity> model;
  std::optional<ModelRevision> revision;
  std::optional<std::string> source_uri;
  std::optional<Digest> expected_digest;
  std::uint64_t expected_size = 0;
  ValidationMode validation_mode = ValidationMode::Validate;
  std::function<bool(const Digest&, std::uint64_t)> external_validator;
};

struct IngestResult {
  bool admitted = false;
  std::string artifact_key;
  Digest digest;
  std::uint64_t generation = 0;
  std::uint64_t size = 0;
  bool dedup = false;
  bool new_backing = false;
  ValidationState validation = ValidationState::UNKNOWN;
  std::string backing_location;
  std::string rejection_reason;
  std::uint64_t bytes_ingested = 0;
  std::uint64_t bytes_deduplicated = 0;
};

struct LookupRequest {
  std::optional<ArtifactId> id;
  std::optional<ArtifactType> type;
  std::optional<ArtifactRole> role;
  std::optional<ArtifactVersion> version;
  std::optional<ArtifactVariant> variant;
  std::optional<ArtifactFormat> format;
  std::optional<ArtifactArchitecture> architecture;
  CompatibilityKey compatibility;
  std::optional<ModelIdentity> model;
  std::optional<ModelRevision> revision;
  bool require_valid = true;
  bool resolve_content = true;
};

struct LookupResult {
  bool found = false;
  bool valid = false;
  std::string artifact_key;
  ValidationState validation = ValidationState::UNKNOWN;
  std::uint64_t generation = 0;
  std::uint64_t size = 0;
  std::optional<Digest> content_digest;
  std::optional<std::string> backing_location;
  std::vector<std::string> dependencies;
  std::optional<std::string> provenance;
  std::vector<std::string> variants;
  std::vector<std::pair<std::string, std::string>> rejected;
  std::string compatibility_reasoning;
  std::uint64_t hit_count = 0;
};

struct ReuseRequest {
  ArtifactType type;
  ArtifactRole role;
  std::optional<ArtifactVersion> version;
  std::optional<ArtifactFormat> format;
  std::optional<ArtifactArchitecture> architecture;
  CompatibilityKey compatibility;
  std::optional<ModelIdentity> model;
  std::optional<ModelRevision> revision;
  bool allow_valid_only = true;
  std::optional<std::string> logical_key;
};

struct ReusePlan {
  ReuseAction action = ReuseAction::FETCH_REQUIRED;
  std::optional<std::string> artifact_key;
  ValidationState validation = ValidationState::UNKNOWN;
  std::uint64_t generation = 0;
  std::uint64_t size = 0;
  std::optional<Digest> content_digest;
  std::optional<std::string> backing_location;
  std::vector<std::string> dependencies;
  std::vector<std::string> candidates;
  std::vector<std::pair<std::string, std::string>> rejected;
  std::string compatibility_reasoning;
  std::uint64_t bytes_reusable = 0;
};

struct ExplainResult {
  std::string requested_identity;
  std::string compatibility_key;
  std::optional<std::string> selected_artifact;
  ValidationState validation = ValidationState::UNKNOWN;
  std::uint64_t generation = 0;
  std::vector<std::string> candidates_evaluated;
  std::vector<std::pair<std::string, std::string>> candidates_rejected;
  std::string rejected_reason;
  std::uint64_t cache_budget_bytes = 0;
  std::uint64_t physical_bytes = 0;
  std::uint64_t expected_reuse_bytes = 0;
  bool dedup = false;
  std::vector<std::string> dependencies;
  ReuseAction selected_action = ReuseAction::FETCH_REQUIRED;
};

struct Snapshot {
  std::uint64_t cache_generation = 0;
  std::uint64_t logical_artifact_count = 0;
  std::uint64_t physical_backing_count = 0;
  std::uint64_t total_logical_bytes = 0;
  std::uint64_t total_physical_bytes = 0;
  std::uint64_t dedup_bytes_saved = 0;
  std::uint64_t valid_count = 0;
  std::uint64_t invalid_count = 0;
  std::uint64_t quarantined_count = 0;
  std::uint64_t manifest_count = 0;
  std::uint64_t alias_count = 0;
  std::uint64_t dependency_edge_count = 0;
  std::uint64_t capacity_bytes = 0;
  std::uint64_t pinned_count = 0;
  std::uint64_t lookups = 0;
  std::uint64_t hits = 0;
  std::uint64_t misses = 0;
  std::uint64_t evicted = 0;
};

struct EvictionRecord {
  std::string artifact_key;
  Digest digest;
  std::uint64_t generation = 0;
  std::uint64_t bytes = 0;
  bool freed_backing = false;
  std::string policy_reason;
};

class ModelCache {
 public:
  explicit ModelCache(std::string root, Config config = Config{});
  ~ModelCache();
  ModelCache(const ModelCache&) = delete;
  ModelCache& operator=(const ModelCache&) = delete;

  const Config& config() const noexcept;
  const std::string& root() const noexcept;

  IngestResult ingest(const ArtifactId& id, const IngestSpec& spec,
                      const std::uint8_t* data, std::size_t size);
  IngestResult ingest_file(const ArtifactId& id, const IngestSpec& spec,
                           const std::string& path);
  IngestResult ingest_stream(const ArtifactId& id, const IngestSpec& spec, StreamReadFn reader);

  bool exists(const ArtifactId& id) const;
  LookupResult lookup(const LookupRequest& req) const;
  ReusePlan reuse_plan(const ReuseRequest& req) const;
  std::optional<ArtifactRecord> record(const ArtifactId& id) const;

  std::vector<std::string> list(const std::optional<ArtifactType>& type = std::nullopt) const;
  std::vector<std::string> variants(const ArtifactId& id) const;

  void manifest_add(const Manifest& m);
  std::optional<Manifest> manifest_get(const ModelIdentity& model,
                                       const ModelRevision& revision) const;
  std::vector<std::string> manifest_list() const;
  void manifest_invalidate(const ModelIdentity& model, const ModelRevision& revision);

  void validate(const ArtifactId& id, bool force = true);
  void quarantine(const ArtifactId& id, const std::string& reason);
  void purge(const ArtifactId& id);
  void retire(const ArtifactId& id);
  void supersede(const ArtifactId& id, const std::string& new_name_suffix);
  std::uint64_t generations(const ArtifactId& id) const;

  void alias_set(const std::string& alias, const ArtifactId& target);
  std::optional<std::string> alias_get(const std::string& alias) const;
  std::vector<std::string> alias_list() const;

  void pin(const ArtifactId& id);
  void unpin(const ArtifactId& id);
  bool is_pinned(const ArtifactId& id) const;

  std::vector<std::string> dependencies(const ArtifactId& id) const;
  std::vector<std::string> dependents(const ArtifactId& id) const;
  std::vector<std::string> ancestors(const ArtifactId& id) const;

  ExplainResult explain(const LookupRequest& req) const;
  ExplainResult explain_reuse(const ReuseRequest& req) const;

  Snapshot snapshot() const;
  Stats& stats() noexcept;
  EventBus& events() noexcept;

  std::size_t gc();
  std::vector<EvictionRecord> evict(std::optional<std::uint64_t> max_bytes = std::nullopt);
  std::size_t verify() const;
  void save();
  void restore();
  void close();

 private:
  struct Impl;
  bool ensure_capacity(Impl& im, const Digest& d, std::uint64_t size, const std::string& write_lk);
  void drop_record(Impl& im, const std::string& lk);
  const ArtifactRecord* lookup_record(const Impl& im, const ArtifactId& id) const;
  void release_backing(Impl& im, const ArtifactRecord& rec);
  bool key_exists(const std::string& key) const;
  std::unique_ptr<Impl> impl_;
};

}  // namespace model_cache
