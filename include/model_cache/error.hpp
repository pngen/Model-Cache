#pragma once
// Model Cache — error model.
// Exceptions carry a stable category code and a human-readable message. The
// runtime never "fabricates success": any failure surfaces as an exception that
// propagates out of the public API before a transaction is committed.

#include <stdexcept>
#include <string>

namespace model_cache {

enum class ErrorCode {
  none = 0,
  invalid_argument,
  invalid_identity,
  invalid_digest,
  malformed_metadata,
  unsupported_version,
  not_found,
  already_exists,
  integrity_failure,
  capacity_exceeded,
  validation_failed,
  incompatible,
  dependency_cycle,
  dependency_missing,
  stale_generation,
  io_failure,
  quarantine_required,
  policy_rejected,
  concurrent_modification,
  internal_error,
};

inline const char* to_string(ErrorCode c) noexcept {
  switch (c) {
    case ErrorCode::none: return "none";
    case ErrorCode::invalid_argument: return "invalid_argument";
    case ErrorCode::invalid_identity: return "invalid_identity";
    case ErrorCode::invalid_digest: return "invalid_digest";
    case ErrorCode::malformed_metadata: return "malformed_metadata";
    case ErrorCode::unsupported_version: return "unsupported_version";
    case ErrorCode::not_found: return "not_found";
    case ErrorCode::already_exists: return "already_exists";
    case ErrorCode::integrity_failure: return "integrity_failure";
    case ErrorCode::capacity_exceeded: return "capacity_exceeded";
    case ErrorCode::validation_failed: return "validation_failed";
    case ErrorCode::incompatible: return "incompatible";
    case ErrorCode::dependency_cycle: return "dependency_cycle";
    case ErrorCode::dependency_missing: return "dependency_missing";
    case ErrorCode::stale_generation: return "stale_generation";
    case ErrorCode::io_failure: return "io_failure";
    case ErrorCode::quarantine_required: return "quarantine_required";
    case ErrorCode::policy_rejected: return "policy_rejected";
    case ErrorCode::concurrent_modification: return "concurrent_modification";
    case ErrorCode::internal_error: return "internal_error";
  }
  return "unknown";
}

class Error : public std::runtime_error {
 public:
  Error(ErrorCode code, std::string message)
      : std::runtime_error(std::move(message)), code_(code) {}
  ErrorCode code() const noexcept { return code_; }

 private:
  ErrorCode code_;
};

/// Convenience helpers.
[[noreturn]] inline void fail(ErrorCode code, std::string message) {
  throw Error(code, std::move(message));
}

}  // namespace model_cache
