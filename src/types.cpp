#include "model_cache/types.hpp"
#include "model_cache/error.hpp"

#include <string_view>

namespace model_cache {
namespace detail {
[[noreturn]] void unknown_enum(std::string_view what, std::string_view value) {
  fail(ErrorCode::invalid_argument,
       std::string("unknown ") + std::string(what) + ": " + std::string(value));
}
}  // namespace detail

namespace {
template <typename E>
E parse(std::string_view s, std::string_view what, const std::pair<std::string_view, E>* table,
        std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) {
    if (table[i].first == s) return table[i].second;
  }
  detail::unknown_enum(what, s);
}
}  // namespace

ArtifactType parse_artifact_type(std::string_view s) {
  static const std::pair<std::string_view, ArtifactType> table[] = {
      {"MODEL_WEIGHTS", ArtifactType::MODEL_WEIGHTS},
      {"MODEL_SHARD", ArtifactType::MODEL_SHARD},
      {"QUANTIZED_WEIGHTS", ArtifactType::QUANTIZED_WEIGHTS},
      {"ADAPTER", ArtifactType::ADAPTER},
      {"TOKENIZER", ArtifactType::TOKENIZER},
      {"VOCABULARY", ArtifactType::VOCABULARY},
      {"MODEL_CONFIG", ArtifactType::MODEL_CONFIG},
      {"GENERATION_CONFIG", ArtifactType::GENERATION_CONFIG},
      {"COMPILED_ARTIFACT", ArtifactType::COMPILED_ARTIFACT},
      {"ENGINE_ARTIFACT", ArtifactType::ENGINE_ARTIFACT},
      {"KERNEL_BUNDLE", ArtifactType::KERNEL_BUNDLE},
      {"GRAPH_ARTIFACT", ArtifactType::GRAPH_ARTIFACT},
      {"PREPROCESSOR", ArtifactType::PREPROCESSOR},
      {"POSTPROCESSOR", ArtifactType::POSTPROCESSOR},
      {"MANIFEST", ArtifactType::MANIFEST},
      {"AUXILIARY", ArtifactType::AUXILIARY},
      {"APPLICATION_DEFINED", ArtifactType::APPLICATION_DEFINED},
  };
  return parse(s, "artifact type", table, std::size(table));
}

ValidationState parse_validation_state(std::string_view s) {
  static const std::pair<std::string_view, ValidationState> table[] = {
      {"UNKNOWN", ValidationState::UNKNOWN},
      {"VALIDATING", ValidationState::VALIDATING},
      {"VALID", ValidationState::VALID},
      {"INVALID", ValidationState::INVALID},
      {"STALE", ValidationState::STALE},
      {"QUARANTINED", ValidationState::QUARANTINED},
      {"RETIRED", ValidationState::RETIRED},
  };
  return parse(s, "validation state", table, std::size(table));
}

ReuseAction parse_reuse_action(std::string_view s) {
  static const std::pair<std::string_view, ReuseAction> table[] = {
      {"REUSE_LOCAL", ReuseAction::REUSE_LOCAL},
      {"RESTORE_CACHED", ReuseAction::RESTORE_CACHED},
      {"REUSE_SHARED_BACKING", ReuseAction::REUSE_SHARED_BACKING},
      {"FETCH_REQUIRED", ReuseAction::FETCH_REQUIRED},
      {"REBUILD_REQUIRED", ReuseAction::REBUILD_REQUIRED},
      {"RECOMPILE_REQUIRED", ReuseAction::RECOMPILE_REQUIRED},
      {"REVALIDATE_REQUIRED", ReuseAction::REVALIDATE_REQUIRED},
      {"REJECT_INCOMPATIBLE", ReuseAction::REJECT_INCOMPATIBLE},
      {"REJECT_CORRUPT", ReuseAction::REJECT_CORRUPT},
      {"REJECT_POLICY", ReuseAction::REJECT_POLICY},
  };
  return parse(s, "reuse action", table, std::size(table));
}

EventType parse_event_type(std::string_view s) {
  static const std::pair<std::string_view, EventType> table[] = {
      {"ARTIFACT_INGEST_STARTED", EventType::ARTIFACT_INGEST_STARTED},
      {"ARTIFACT_ADMITTED", EventType::ARTIFACT_ADMITTED},
      {"ARTIFACT_REJECTED", EventType::ARTIFACT_REJECTED},
      {"ARTIFACT_VALIDATED", EventType::ARTIFACT_VALIDATED},
      {"ARTIFACT_INVALID", EventType::ARTIFACT_INVALID},
      {"ARTIFACT_QUARANTINED", EventType::ARTIFACT_QUARANTINED},
      {"ARTIFACT_REUSED", EventType::ARTIFACT_REUSED},
      {"ARTIFACT_EVICTED", EventType::ARTIFACT_EVICTED},
      {"ARTIFACT_RETIRED", EventType::ARTIFACT_RETIRED},
      {"ARTIFACT_SUPERSEDED", EventType::ARTIFACT_SUPERSEDED},
      {"ALIAS_UPDATED", EventType::ALIAS_UPDATED},
      {"MANIFEST_ADDED", EventType::MANIFEST_ADDED},
      {"MANIFEST_INVALIDATED", EventType::MANIFEST_INVALIDATED},
      {"DEPENDENCY_INVALIDATED", EventType::DEPENDENCY_INVALIDATED},
      {"CORRUPTION_DETECTED", EventType::CORRUPTION_DETECTED},
      {"RECOVERY_COMPLETED", EventType::RECOVERY_COMPLETED},
      {"CAPACITY_HIGH", EventType::CAPACITY_HIGH},
      {"CACHE_HIT", EventType::CACHE_HIT},
      {"CACHE_MISS", EventType::CACHE_MISS},
  };
  return parse(s, "event type", table, std::size(table));
}

}  // namespace model_cache
