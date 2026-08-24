#pragma once
// Model Cache — durable storage engine.
//
// Storage owns the cache root layout and provides two responsibilities:
//   * content-addressable byte storage (dedup by SHA-256), and
//   * persisted metadata files (artifact records, manifest blobs, aliases).
//
// All file writes are atomic (temp + fsync + rename). Recovery removes stale
// temp files, detects and re-quarantines corrupted backing objects, and
// reconstructs the derived object index by scanning authoritative records.

#include "model_cache/config.hpp"
#include "model_cache/record.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace model_cache {

class Storage {
 public:
  Storage(std::string root, Config config);
  ~Storage();

  Storage(const Storage&) = delete;
  Storage& operator=(const Storage&) = delete;

  const std::string& root() const noexcept { return root_; }
  const Config& config() const noexcept { return config_; }

  // --- content-addressable byte store -------------------------------------
  bool object_exists(const Digest& d) const noexcept;
  std::string object_path(const Digest& d) const;
  std::string object_meta_path(const Digest& d) const;
  /// Write a backing object at its digest path, deduping (verify-before-reuse).
  /// Returns true if new, false if it already existed and matched.
  bool write_object(const Digest& d, const void* data, std::size_t bytes);
  std::uint64_t object_size(const Digest& d) const;
  /// Streaming re-hash of an existing object; true iff it matches the digest.
  bool verify_object(const Digest& d) const;
  void remove_object(const Digest& d) noexcept;
  /// Move a backing object into quarantine (keeps bytes for diagnosis).
  void quarantine_object(const Digest& d, const std::string& reason);
  void list_object_digests(std::vector<Digest>& out) const;

  // --- metadata files ------------------------------------------------------
  std::string record_file(const std::string& ns, const std::string& name) const;
  std::optional<ArtifactRecord> load_record(const std::string& ns,
                                            const std::string& name) const;
  void save_record(const ArtifactRecord& rec) const;
  void remove_record(const std::string& ns, const std::string& name) const;
  std::vector<std::string> list_record_keys() const;

  std::string manifest_file(const std::string& model, const std::string& revision) const;
  std::optional<Manifest> load_manifest(const std::string& model,
                                        const std::string& revision) const;
  void save_manifest(const Manifest& m) const;
  void remove_manifest(const std::string& model, const std::string& revision) const;
  std::vector<std::string> list_manifest_keys() const;

  // aliases: name -> target canonical id (text file).
  std::string alias_file(const std::string& alias) const;
  std::optional<std::string> load_alias(const std::string& alias) const;
  void save_alias(const std::string& alias, const std::string& target) const;
  void remove_alias(const std::string& alias) const;
  std::vector<std::string> list_aliases() const;

  // --- staging / temp -------------------------------------------------------
  std::string new_temp_path(const std::string& hint) const;

  // --- recovery -------------------------------------------------------------
  void recover();

 private:
  void ensure_layout();
  std::string join(std::initializer_list<std::string> parts) const;

  std::string root_;
  Config config_;
};

}  // namespace model_cache
