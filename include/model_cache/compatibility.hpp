#pragma once
// Model Cache — compatibility keys.
//
// A CompatibilityKey is a canonical map from dimension name to value. It captures
// the dimensions that determine whether one artifact can serve another: model
// architecture/revision, artifact format, quantization scheme, tokenizer and
// vocabulary identity, adapter base-model identity, backend/ABI/compiler version,
// target architecture, device capability class, precision, layout, build flags,
// and arbitrary caller-defined fields.
//
// Canonical spelling and canonical serialization guarantee that two keys that
// describe the same requirements are byte-identical. Matching is strict: a
// candidate satisfies a request only if every non-empty requested dimension
// exists in the candidate with an identical value. This prevents a false reuse
// hit between artifacts that merely "look similar".

#include "model_cache/identity.hpp"
#include "model_cache/serialization.hpp"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace model_cache {

/// Built-in dimension names. Callers may also introduce custom dimensions.
namespace dim {
inline constexpr const char* model_architecture = "model_architecture";
inline constexpr const char* model_revision = "model_revision";
inline constexpr const char* artifact_format = "artifact_format";
inline constexpr const char* quantization_scheme = "quantization_scheme";
inline constexpr const char* tokenizer_identity = "tokenizer_identity";
inline constexpr const char* vocabulary_identity = "vocabulary_identity";
inline constexpr const char* adapter_base_model = "adapter_base_model";
inline constexpr const char* adapter_format_version = "adapter_format_version";
inline constexpr const char* backend_identity = "backend_identity";
inline constexpr const char* runtime_abi = "runtime_abi";
inline constexpr const char* compiler_identity = "compiler_identity";
inline constexpr const char* compiler_version = "compiler_version";
inline constexpr const char* target_architecture = "target_architecture";
inline constexpr const char* device_capability = "device_capability";
inline constexpr const char* precision = "precision";
inline constexpr const char* layout = "layout";
inline constexpr const char* build_flags = "build_flags";
}  // namespace dim

class CompatibilityKey {
 public:
  CompatibilityKey() = default;

  explicit CompatibilityKey(std::map<std::string, std::string> dims)
      : dims_(std::move(dims)) {}

  CompatibilityKey& set(std::string dimension, std::string value) {
    if (dimension.empty()) fail(ErrorCode::invalid_argument, "dimension must be non-empty");
    dims_[std::move(dimension)] = std::move(value);
    return *this;
  }

  CompatibilityKey& set(std::string dimension, const ArtifactFormat& f) {
    return set(std::move(dimension), f.str());
  }
  CompatibilityKey& set(std::string dimension, const ArtifactArchitecture& a) {
    return set(std::move(dimension), a.str());
  }

  bool has(std::string_view dim) const { return dims_.find(std::string(dim)) != dims_.end(); }

  const std::optional<std::string> get(std::string_view dim) const {
    auto it = dims_.find(std::string(dim));
    if (it == dims_.end()) return std::nullopt;
    return it->second;
  }

  std::size_t size() const noexcept { return dims_.size(); }
  bool empty() const noexcept { return dims_.empty(); }

  const std::map<std::string, std::string>& dims() const noexcept { return dims_; }

  bool operator==(const CompatibilityKey& o) const noexcept { return dims_ == o.dims_; }
  bool operator!=(const CompatibilityKey& o) const noexcept { return dims_ != o.dims_; }

  /// Canonical text form: "dim1=value1;dim2=value2;..." sorted by dimension.
  std::string canonical() const {
    std::string s;
    for (const auto& [k, v] : dims_) {
      if (!s.empty()) s.push_back(';');
      s += k;
      s.push_back('=');
      s += v;
    }
    return s;
  }

  void to_canonical(canon::Writer& w) const {
    w.begin_map(dims_.size());
    for (const auto& [k, v] : dims_) {
      w.write_key(k);
      w.write_string(v);
    }
    w.end_map();
  }

  static CompatibilityKey from_canonical(canon::Reader& r) {
    CompatibilityKey k;
    const auto n = r.begin_map();
    for (std::size_t i = 0; i < n; ++i) {
      auto key = r.read_key();
      auto val = r.read_string();
      k.dims_[std::move(key)] = std::move(val);
    }
    r.end_map();
    return k;
  }

  /// Strict compatibility: candidate satisfies request iff every non-empty
  /// requested dimension is present in candidate with the identical value.
  static bool compatible(const CompatibilityKey& request, const CompatibilityKey& candidate) {
    for (const auto& [k, v] : request.dims_) {
      if (v.empty()) continue;  // empty request value = wildcard
      auto it = candidate.dims_.find(k);
      if (it == candidate.dims_.end()) return false;
      if (it->second != v) return false;
    }
    return true;
  }

  /// Missing dimensions in the candidate relative to the request (for explain).
  std::vector<std::string> unmet(const CompatibilityKey& request) const {
    std::vector<std::string> out;
    for (const auto& [k, v] : request.dims_) {
      if (v.empty()) continue;
      auto it = dims_.find(k);
      if (it == dims_.end() || it->second != v) out.push_back(k);
    }
    return out;
  }

 private:
  std::map<std::string, std::string> dims_;
};

}  // namespace model_cache
