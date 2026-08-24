#pragma once
// Model Cache — canonical enumerations. String round-trips are strict: unknown
// values are rejected, never coerced to a default. The string forms are the
// stable on-disk representation.

#include <string>
#include <string_view>

namespace model_cache {

enum class ArtifactType {
  MODEL_WEIGHTS,
  MODEL_SHARD,
  QUANTIZED_WEIGHTS,
  ADAPTER,
  TOKENIZER,
  VOCABULARY,
  MODEL_CONFIG,
  GENERATION_CONFIG,
  COMPILED_ARTIFACT,
  ENGINE_ARTIFACT,
  KERNEL_BUNDLE,
  GRAPH_ARTIFACT,
  PREPROCESSOR,
  POSTPROCESSOR,
  MANIFEST,
  AUXILIARY,
  APPLICATION_DEFINED,
};

constexpr const char* to_string(ArtifactType t) noexcept {
  switch (t) {
    case ArtifactType::MODEL_WEIGHTS: return "MODEL_WEIGHTS";
    case ArtifactType::MODEL_SHARD: return "MODEL_SHARD";
    case ArtifactType::QUANTIZED_WEIGHTS: return "QUANTIZED_WEIGHTS";
    case ArtifactType::ADAPTER: return "ADAPTER";
    case ArtifactType::TOKENIZER: return "TOKENIZER";
    case ArtifactType::VOCABULARY: return "VOCABULARY";
    case ArtifactType::MODEL_CONFIG: return "MODEL_CONFIG";
    case ArtifactType::GENERATION_CONFIG: return "GENERATION_CONFIG";
    case ArtifactType::COMPILED_ARTIFACT: return "COMPILED_ARTIFACT";
    case ArtifactType::ENGINE_ARTIFACT: return "ENGINE_ARTIFACT";
    case ArtifactType::KERNEL_BUNDLE: return "KERNEL_BUNDLE";
    case ArtifactType::GRAPH_ARTIFACT: return "GRAPH_ARTIFACT";
    case ArtifactType::PREPROCESSOR: return "PREPROCESSOR";
    case ArtifactType::POSTPROCESSOR: return "POSTPROCESSOR";
    case ArtifactType::MANIFEST: return "MANIFEST";
    case ArtifactType::AUXILIARY: return "AUXILIARY";
    case ArtifactType::APPLICATION_DEFINED: return "APPLICATION_DEFINED";
  }
  return "UNKNOWN";
}

enum class ValidationState {
  UNKNOWN,
  VALIDATING,
  VALID,
  INVALID,
  STALE,
  QUARANTINED,
  RETIRED,
};

constexpr const char* to_string(ValidationState s) noexcept {
  switch (s) {
    case ValidationState::UNKNOWN: return "UNKNOWN";
    case ValidationState::VALIDATING: return "VALIDATING";
    case ValidationState::VALID: return "VALID";
    case ValidationState::INVALID: return "INVALID";
    case ValidationState::STALE: return "STALE";
    case ValidationState::QUARANTINED: return "QUARANTINED";
    case ValidationState::RETIRED: return "RETIRED";
  }
  return "UNKNOWN";
}

enum class ReuseAction {
  REUSE_LOCAL,
  RESTORE_CACHED,
  REUSE_SHARED_BACKING,
  FETCH_REQUIRED,
  REBUILD_REQUIRED,
  RECOMPILE_REQUIRED,
  REVALIDATE_REQUIRED,
  REJECT_INCOMPATIBLE,
  REJECT_CORRUPT,
  REJECT_POLICY,
};

constexpr const char* to_string(ReuseAction a) noexcept {
  switch (a) {
    case ReuseAction::REUSE_LOCAL: return "REUSE_LOCAL";
    case ReuseAction::RESTORE_CACHED: return "RESTORE_CACHED";
    case ReuseAction::REUSE_SHARED_BACKING: return "REUSE_SHARED_BACKING";
    case ReuseAction::FETCH_REQUIRED: return "FETCH_REQUIRED";
    case ReuseAction::REBUILD_REQUIRED: return "REBUILD_REQUIRED";
    case ReuseAction::RECOMPILE_REQUIRED: return "RECOMPILE_REQUIRED";
    case ReuseAction::REVALIDATE_REQUIRED: return "REVALIDATE_REQUIRED";
    case ReuseAction::REJECT_INCOMPATIBLE: return "REJECT_INCOMPATIBLE";
    case ReuseAction::REJECT_CORRUPT: return "REJECT_CORRUPT";
    case ReuseAction::REJECT_POLICY: return "REJECT_POLICY";
  }
  return "UNKNOWN";
}

enum class EventType {
  ARTIFACT_INGEST_STARTED,
  ARTIFACT_ADMITTED,
  ARTIFACT_REJECTED,
  ARTIFACT_VALIDATED,
  ARTIFACT_INVALID,
  ARTIFACT_QUARANTINED,
  ARTIFACT_REUSED,
  ARTIFACT_EVICTED,
  ARTIFACT_RETIRED,
  ARTIFACT_SUPERSEDED,
  ALIAS_UPDATED,
  MANIFEST_ADDED,
  MANIFEST_INVALIDATED,
  DEPENDENCY_INVALIDATED,
  CORRUPTION_DETECTED,
  RECOVERY_COMPLETED,
  CAPACITY_HIGH,
  CACHE_HIT,
  CACHE_MISS,
};

constexpr const char* to_string(EventType e) noexcept {
  switch (e) {
    case EventType::ARTIFACT_INGEST_STARTED: return "ARTIFACT_INGEST_STARTED";
    case EventType::ARTIFACT_ADMITTED: return "ARTIFACT_ADMITTED";
    case EventType::ARTIFACT_REJECTED: return "ARTIFACT_REJECTED";
    case EventType::ARTIFACT_VALIDATED: return "ARTIFACT_VALIDATED";
    case EventType::ARTIFACT_INVALID: return "ARTIFACT_INVALID";
    case EventType::ARTIFACT_QUARANTINED: return "ARTIFACT_QUARANTINED";
    case EventType::ARTIFACT_REUSED: return "ARTIFACT_REUSED";
    case EventType::ARTIFACT_EVICTED: return "ARTIFACT_EVICTED";
    case EventType::ARTIFACT_RETIRED: return "ARTIFACT_RETIRED";
    case EventType::ARTIFACT_SUPERSEDED: return "ARTIFACT_SUPERSEDED";
    case EventType::ALIAS_UPDATED: return "ALIAS_UPDATED";
    case EventType::MANIFEST_ADDED: return "MANIFEST_ADDED";
    case EventType::MANIFEST_INVALIDATED: return "MANIFEST_INVALIDATED";
    case EventType::DEPENDENCY_INVALIDATED: return "DEPENDENCY_INVALIDATED";
    case EventType::CORRUPTION_DETECTED: return "CORRUPTION_DETECTED";
    case EventType::RECOVERY_COMPLETED: return "RECOVERY_COMPLETED";
    case EventType::CAPACITY_HIGH: return "CAPACITY_HIGH";
    case EventType::CACHE_HIT: return "CACHE_HIT";
    case EventType::CACHE_MISS: return "CACHE_MISS";
  }
  return "UNKNOWN";
}

// Strict parsers. These throw Error(ErrorCode::invalid_argument) on unknown
// values, so malformed persisted enumerations are rejected, not coerced.
namespace detail {
[[noreturn]] void unknown_enum(std::string_view what, std::string_view value);
}

ArtifactType parse_artifact_type(std::string_view s);
ValidationState parse_validation_state(std::string_view s);
ReuseAction parse_reuse_action(std::string_view s);
EventType parse_event_type(std::string_view s);

}  // namespace model_cache
