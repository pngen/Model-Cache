#pragma once
#include "framework.hpp"
#include "test_util.hpp"

#include "model_cache/cache.hpp"
#include "model_cache/detail/platform.hpp"
#include "model_cache/digest.hpp"
#include "model_cache/error.hpp"
#include "model_cache/record.hpp"

#include <string>

using namespace model_cache;
using testfw::TempDir;

inline IngestResult ingest_bytes(ModelCache& c, const ArtifactId& id, const IngestSpec& s,
                                 const std::string& content) {
  return c.ingest(id, s, reinterpret_cast<const std::uint8_t*>(content.data()), content.size());
}

inline IngestSpec spec(ArtifactType t, const std::string& role, const std::string& ver) {
  IngestSpec s;
  s.type = t;
  s.role = ArtifactRole(role);
  s.version = ArtifactVersion(ver);
  return s;
}

inline void expect_invalid_identity(const std::string& name) {
  bool threw = false;
  try {
    ArtifactId bad(ArtifactNamespace("a"), name);
    (void)bad;
  } catch (const model_cache::Error& e) {
    threw = (e.code() == ErrorCode::invalid_identity);
  }
  CHECK(threw);
}

inline ArtifactId id_from_key(const std::string& key) {
  auto slash = key.find('/');
  return ArtifactId(ArtifactNamespace(key.substr(0, slash)), key.substr(slash + 1));
}

inline std::string abs_hex(Digest d) { return d.hex(); }
