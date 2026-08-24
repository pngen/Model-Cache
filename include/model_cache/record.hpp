#pragma once
// Model Cache — persisted data model.
//
// Artifacts are organized as logical records keyed by (namespace, name). A
// logical record holds a sequence of immutable content generations plus mutable
// metadata (validation, pin, dependencies, provenance). Content itself lives in
// a content-addressable store keyed by SHA-256, so identical byte content may be
// shared by many logical records while preserving independent logical identity.
//
// Every persisted structure carries an explicit version and is serialized
// through the strict canonical reader/writer, so malformed persisted metadata
// is rejected rather than silently coerced.

#include "model_cache/compatibility.hpp"
#include "model_cache/identity.hpp"
#include "model_cache/serialization.hpp"
#include "model_cache/types.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace model_cache {

inline constexpr std::uint32_t kStateVersion = 1;

struct GenerationRecord {
  std::uint64_t generation = 0;
  Digest content_digest;             // authoritative SHA-256
  std::uint64_t size = 0;            // logical bytes
  ValidationState validation = ValidationState::UNKNOWN;
  std::optional<std::string> backing_key;  // CAS object key when content present
  std::uint64_t created_at = 0;      // epoch milliseconds
  bool current = false;

  void to_canonical(canon::Writer& w) const {
    w.begin_map(7);
    w.write_key("gen"); w.write_uint64(generation);
    w.write_key("digest"); w.write_digest(content_digest);
    w.write_key("size"); w.write_uint64(size);
    w.write_key("validation"); w.write_string(to_string(validation));
    w.write_key("backing");
    if (backing_key) w.write_string(*backing_key); else w.write_null();
    w.write_key("ts"); w.write_uint64(created_at);
    w.write_key("current"); w.write_bool(current);
    w.end_map();
  }

  static GenerationRecord from_canonical(canon::Reader& r) {
    GenerationRecord g;
    const auto n = r.begin_map();
    bool has_gen = false, has_digest = false;
    for (std::size_t i = 0; i < n; ++i) {
      auto k = r.read_key();
      if (k == "gen") { g.generation = r.read_uint64(); has_gen = true; }
      else if (k == "digest") { g.content_digest = r.read_digest(); has_digest = true; }
      else if (k == "size") { g.size = r.read_uint64(); }
      else if (k == "validation") { g.validation = parse_validation_state(r.read_string()); }
      else if (k == "backing") { if (r.peek_tag() == canon::Tag::null) r.read_null(); else g.backing_key = r.read_string(); }
      else if (k == "ts") { g.created_at = r.read_uint64(); }
      else if (k == "current") { g.current = r.read_bool(); }
      else fail(ErrorCode::malformed_metadata, "unexpected GenerationRecord field: " + k);
    }
    r.end_map();
    if (!has_gen || !has_digest) fail(ErrorCode::malformed_metadata, "GenerationRecord missing fields");
    return g;
  }
};

struct ArtifactRecord {
  // Logical identity.
  std::string ns;
  std::string name;
  ArtifactType type = ArtifactType::AUXILIARY;
  ArtifactRole role;
  ArtifactVersion version;
  std::optional<ArtifactVariant> variant;
  std::optional<ArtifactFormat> format;
  std::optional<ArtifactArchitecture> architecture;
  std::uint64_t current_generation = 0;
  std::vector<GenerationRecord> generations;
  bool pinned = false;
  std::optional<std::string> source_uri;
  std::optional<Digest> manifest_digest;
  std::uint64_t created_at = 0;
  std::uint64_t updated_at = 0;
  CompatibilityKey compatibility;
  std::vector<std::string> dependencies;  // dependency node keys (canonical ids)
  std::map<std::string, std::string> provenance;  // caller-defined

  std::string logical_key() const { return ns + "/" + name; }

  // The authoritative generation (the one flagged current).
  GenerationRecord* current() {
    for (auto& g : generations) if (g.current) return &g;
    return nullptr;
  }
  const GenerationRecord* current() const {
    for (const auto& g : generations) if (g.current) return &g;
    return nullptr;
  }
  GenerationRecord* find_generation(std::uint64_t gen) {
    for (auto& g : generations) if (g.generation == gen) return &g;
    return nullptr;
  }
  const GenerationRecord* find_generation(std::uint64_t gen) const {
    for (const auto& g : generations) if (g.generation == gen) return &g;
    return nullptr;
  }
  GenerationRecord* find_generation(const Digest& d) {
    for (auto& g : generations) if (g.content_digest == d) return &g;
    return nullptr;
  }
  const GenerationRecord* find_generation(const Digest& d) const {
    for (const auto& g : generations) if (g.content_digest == d) return &g;
    return nullptr;
  }

  void to_canonical(canon::Writer& w) const {
    w.begin_map(19);
    w.write_key("ver"); w.write_uint64(kStateVersion);
    w.write_key("ns"); w.write_string(ns);
    w.write_key("name"); w.write_string(name);
    w.write_key("type"); w.write_string(to_string(type));
    w.write_key("role"); w.write_string(role.str());
    w.write_key("version"); w.write_string(version.str());
    w.write_key("variant"); if (variant) w.write_string(variant->str()); else w.write_null();
    w.write_key("format"); if (format) w.write_string(format->str()); else w.write_null();
    w.write_key("arch"); if (architecture) w.write_string(architecture->str()); else w.write_null();
    w.write_key("cur_gen"); w.write_uint64(current_generation);
    w.write_key("gens");
    w.begin_seq(generations.size());
    for (const auto& g : generations) g.to_canonical(w);
    w.end_seq();
    w.write_key("pinned"); w.write_bool(pinned);
    w.write_key("source_uri"); if (source_uri) w.write_string(*source_uri); else w.write_null();
    w.write_key("manifest_digest"); if (manifest_digest) w.write_digest(*manifest_digest); else w.write_null();
    w.write_key("created_at"); w.write_uint64(created_at);
    w.write_key("updated_at"); w.write_uint64(updated_at);
    w.write_key("compat"); compatibility.to_canonical(w);
    w.write_key("deps");
    w.begin_seq(dependencies.size());
    for (const auto& d : dependencies) w.write_string(d);
    w.end_seq();
    w.write_key("provenance");
    w.begin_map(provenance.size());
    for (const auto& [k, v] : provenance) { w.write_key(k); w.write_string(v); }
    w.end_map();
    w.end_map();
  }

  static ArtifactRecord from_canonical(canon::Reader& r) {
    ArtifactRecord a;
    const auto n = r.begin_map();
    bool has_ns = false, has_name = false, has_type = false;
    for (std::size_t i = 0; i < n; ++i) {
      auto k = r.read_key();
      if (k == "ver") { auto v = r.read_uint64(); if (v != kStateVersion) fail(ErrorCode::unsupported_version, "unsupported state version"); }
      else if (k == "ns") { a.ns = r.read_string(); has_ns = true; }
      else if (k == "name") { a.name = r.read_string(); has_name = true; }
      else if (k == "type") { a.type = parse_artifact_type(r.read_string()); has_type = true; }
      else if (k == "role") { a.role = ArtifactRole(r.read_string()); }
      else if (k == "version") { a.version = ArtifactVersion(r.read_string()); }
      else if (k == "variant") { if (r.peek_tag() == canon::Tag::null) r.read_null(); else a.variant = ArtifactVariant(r.read_string()); }
      else if (k == "format") { if (r.peek_tag() == canon::Tag::null) r.read_null(); else a.format = ArtifactFormat(r.read_string()); }
      else if (k == "arch") { if (r.peek_tag() == canon::Tag::null) r.read_null(); else a.architecture = ArtifactArchitecture(r.read_string()); }
      else if (k == "cur_gen") { a.current_generation = r.read_uint64(); }
      else if (k == "gens") {
        auto m = r.begin_seq();
        for (std::size_t j = 0; j < m; ++j) a.generations.push_back(GenerationRecord::from_canonical(r));
        r.end_seq();
      }
      else if (k == "pinned") { a.pinned = r.read_bool(); }
      else if (k == "source_uri") { if (r.peek_tag() == canon::Tag::null) r.read_null(); else a.source_uri = r.read_string(); }
      else if (k == "manifest_digest") { if (r.peek_tag() == canon::Tag::null) r.read_null(); else a.manifest_digest = r.read_digest(); }
      else if (k == "created_at") { a.created_at = r.read_uint64(); }
      else if (k == "updated_at") { a.updated_at = r.read_uint64(); }
      else if (k == "compat") { a.compatibility = CompatibilityKey::from_canonical(r); }
      else if (k == "deps") {
        auto m = r.begin_seq();
        for (std::size_t j = 0; j < m; ++j) a.dependencies.push_back(r.read_string());
        r.end_seq();
      }
      else if (k == "provenance") {
        auto m = r.begin_map();
        for (std::size_t j = 0; j < m; ++j) a.provenance[r.read_key()] = r.read_string();
        r.end_map();
      }
      else fail(ErrorCode::malformed_metadata, "unexpected ArtifactRecord field: " + k);
    }
    r.end_map();
    if (!has_ns || !has_name || !has_type) fail(ErrorCode::malformed_metadata, "ArtifactRecord missing required fields");
    detail::check_identity_string("ArtifactRecord.ns", a.ns);
    detail::check_identity_string("ArtifactRecord.name", a.name);
    return a;
  }
};

/// A manifest describes a reusable artifact collection (typically a model).
struct Manifest {
  ModelIdentity model;
  ModelRevision revision;
  std::uint64_t generation = 0;
  std::uint64_t created_at = 0;
  ValidationState validation = ValidationState::UNKNOWN;
  std::vector<std::string> artifacts;       // artifact logical or pinned keys
  std::map<std::string, std::string> roles; // role -> artifact key
  CompatibilityKey compatibility;
  std::vector<std::string> dependencies;    // manifest dependency keys
  std::map<std::string, std::string> provenance;
  std::optional<std::string> source_uri;

  std::string canonical_key() const { return "manifest:" + model.str() + "/" + revision.str(); }

  void to_canonical(canon::Writer& w) const {
    // Deterministic: sequences are written in their stored order; maps are
    // canonical because the writer orders nothing — so callers must keep maps
    // ordered. We enforce key ordering by sorting here.
    w.begin_map(12);
    w.write_key("ver"); w.write_uint64(kStateVersion);
    w.write_key("model"); w.write_string(model.str());
    w.write_key("revision"); w.write_string(revision.str());
    w.write_key("gen"); w.write_uint64(generation);
    w.write_key("ts"); w.write_uint64(created_at);
    w.write_key("validation"); w.write_string(to_string(validation));
    w.write_key("artifacts");
    w.begin_seq(artifacts.size());
    for (const auto& a : artifacts) w.write_string(a);
    w.end_seq();
    w.write_key("roles");
    std::vector<std::pair<std::string,std::string>> rolepairs(roles.begin(), roles.end());
    w.begin_map(rolepairs.size());
    for (const auto& [k, v] : rolepairs) { w.write_key(k); w.write_string(v); }
    w.end_map();
    w.write_key("compat"); compatibility.to_canonical(w);
    w.write_key("deps");
    w.begin_seq(dependencies.size());
    for (const auto& d : dependencies) w.write_string(d);
    w.end_seq();
    w.write_key("provenance");
    std::vector<std::pair<std::string,std::string>> prov(provenance.begin(), provenance.end());
    w.begin_map(prov.size());
    for (const auto& [k, v] : prov) { w.write_key(k); w.write_string(v); }
    w.end_map();
    w.write_key("source_uri"); if (source_uri) w.write_string(*source_uri); else w.write_null();
    w.end_map();
  }

  static Manifest from_canonical(canon::Reader& r) {
    Manifest m;
    const auto n = r.begin_map();
    bool has_model = false, has_rev = false;
    for (std::size_t i = 0; i < n; ++i) {
      auto k = r.read_key();
      if (k == "ver") { auto v = r.read_uint64(); if (v != kStateVersion) fail(ErrorCode::unsupported_version, "unsupported manifest version"); }
      else if (k == "model") { m.model = ModelIdentity(r.read_string()); has_model = true; }
      else if (k == "revision") { m.revision = ModelRevision(r.read_string()); has_rev = true; }
      else if (k == "gen") { m.generation = r.read_uint64(); }
      else if (k == "ts") { m.created_at = r.read_uint64(); }
      else if (k == "validation") { m.validation = parse_validation_state(r.read_string()); }
      else if (k == "artifacts") { auto c = r.begin_seq(); for (std::size_t j=0;j<c;++j) m.artifacts.push_back(r.read_string()); r.end_seq(); }
      else if (k == "roles") { auto c = r.begin_map(); for (std::size_t j=0;j<c;++j) m.roles[r.read_key()] = r.read_string(); r.end_map(); }
      else if (k == "compat") { m.compatibility = CompatibilityKey::from_canonical(r); }
      else if (k == "deps") { auto c = r.begin_seq(); for (std::size_t j=0;j<c;++j) m.dependencies.push_back(r.read_string()); r.end_seq(); }
      else if (k == "provenance") { auto c = r.begin_map(); for (std::size_t j=0;j<c;++j) m.provenance[r.read_key()] = r.read_string(); r.end_map(); }
      else if (k == "source_uri") { if (r.peek_tag() == canon::Tag::null) r.read_null(); else m.source_uri = r.read_string(); }
      else fail(ErrorCode::malformed_metadata, "unexpected Manifest field: " + k);
    }
    r.end_map();
    if (!has_model || !has_rev) fail(ErrorCode::malformed_metadata, "Manifest missing model/revision");
    return m;
  }
};

}  // namespace model_cache
