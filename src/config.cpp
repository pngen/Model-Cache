#include "model_cache/config.hpp"
namespace model_cache {
void validate_config(const Config& c) {
  validate_limit(c.limits.max_total_bytes, "max_total_bytes");
  validate_limit(c.limits.max_object_count, "max_object_count");
  validate_limit(c.limits.max_manifest_count, "max_manifest_count");
  validate_limit(c.limits.max_dependency_edges, "max_dependency_edges");
  validate_limit(c.limits.max_artifact_size, "max_artifact_size");
  validate_limit(c.limits.max_staging_bytes, "max_staging_bytes");
  // Oversized artifacts are handled at admission (eviction then rejection), not
  // by forbidding the configuration, so no cross-field ordering is enforced.
}
}  // namespace model_cache
