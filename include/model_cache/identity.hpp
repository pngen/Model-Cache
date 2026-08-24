#pragma once
// Model Cache — artifact identity.
//
// Every cached artifact has stable, explicit identity independent of pointers,
// temporary paths, or unstable runtime handles. Identity is derived from a
// stable caller-provided namespace+name, optionally pinned to a content digest,
// or composed from model/revision/role/format/architecture/compatibility data.
//
// String components are validated: non-empty, valid UTF-8, bounded length, no
// path separators, and never "." or "..". This blocks path traversal / injection
// from untrusted metadata.

#include "model_cache/digest.hpp"
#include "model_cache/error.hpp"
#include "model_cache/serialization.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace model_cache {

namespace detail {
inline void check_identity_string(std::string_view what, const std::string& v,
                                  std::size_t maxlen = 256, bool allow_slash = false) {
  if (v.empty()) fail(ErrorCode::invalid_identity, std::string(what) + " must be non-empty");
  if (v.size() > maxlen) fail(ErrorCode::invalid_identity, std::string(what) + " too long");
  if (v.find('\0') != std::string::npos) {
    fail(ErrorCode::invalid_identity, std::string(what) + " contains NUL");
  }
  if (v == "." || v == "..") {
    fail(ErrorCode::invalid_identity, std::string(what) + " must not be '.' or '..'");
  }
  if (!allow_slash) {
    // For filesystem-path components we require no path separator. Model
    // identity/revision may contain '/' (e.g. "org/model") and are encoded
    // separately for filesystem use.
    if (v.find('/') != std::string::npos || v.find('\\') != std::string::npos) {
      fail(ErrorCode::invalid_identity,
           std::string(what) + " must not contain a path separator");
    }
  }
}

/// Injective encoding of an arbitrary UTF-8 string into a safe, reversible
/// filesystem path component. Any byte outside [A-Za-z0-9._-] becomes '_' + 2
/// hex digits. Used only for on-disk layout, never for identity.
inline std::string encode_for_path(const std::string& s) {
  static const char* h = "0123456789abcdef";
  std::string out;
  out.reserve(s.size());
  auto safe = [](char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '.' || c == '-';
  };
  for (unsigned char c : s) {
    if (safe(static_cast<char>(c))) out.push_back(static_cast<char>(c));
    else {
      out.push_back('_');
      out.push_back(h[c >> 4]);
      out.push_back(h[c & 0x0fu]);
    }
  }
  return out;
}

/// Inverse of encode_for_path. Decodes '_hh' escapes back to bytes. Because an
/// underscore is always escaped as '_5f', decoding is unambiguous.
inline std::string decode_for_path(const std::string& s) {
  auto hexv = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  std::string out;
  out.reserve(s.size());
  std::size_t i = 0;
  while (i < s.size()) {
    if (s[i] == '_') {
      if (i + 2 < s.size()) {
        int hi = hexv(s[i + 1]), lo = hexv(s[i + 2]);
        if (hi >= 0 && lo >= 0) {
          out.push_back(static_cast<char>((hi << 4) | lo));
          i += 3;
          continue;
        }
      }
      out.push_back('_');
      ++i;
    } else {
      out.push_back(s[i]);
      ++i;
    }
  }
  return out;
}
}  // namespace detail

/// A validated, non-empty string identity component (namespace, role, format...).
template <typename Derived, bool AllowSlash = false>
struct ValidatedString {
  std::string value;

  ValidatedString() = default;
  explicit ValidatedString(std::string v, std::string_view what = "string") {
    detail::check_identity_string(what, v, 256, AllowSlash);
    value = std::move(v);
  }

  bool operator==(const ValidatedString& o) const noexcept { return value == o.value; }
  bool operator!=(const ValidatedString& o) const noexcept { return value != o.value; }
  bool operator<(const ValidatedString& o) const noexcept { return value < o.value; }
  bool empty() const noexcept { return value.empty(); }
  const std::string& str() const noexcept { return value; }
};

#define MODEL_CACHE_ID_ALIAS(N)                                    \
  struct N : ValidatedString<N> {                                  \
    using ValidatedString<N>::ValidatedString;                     \
    N() = default;                                                 \
    explicit N(std::string v) : ValidatedString<N>(std::move(v), #N) {} \
  }

MODEL_CACHE_ID_ALIAS(ArtifactNamespace);
MODEL_CACHE_ID_ALIAS(ArtifactRole);
MODEL_CACHE_ID_ALIAS(ArtifactVariant);
MODEL_CACHE_ID_ALIAS(ArtifactFormat);
MODEL_CACHE_ID_ALIAS(ArtifactArchitecture);
MODEL_CACHE_ID_ALIAS(ArtifactVersion);

#undef MODEL_CACHE_ID_ALIAS

// Model identity/revision may use '/' (e.g. "org/model"), so allow a slash.
struct ModelIdentity : ValidatedString<ModelIdentity, true> {
  using ValidatedString<ModelIdentity, true>::ValidatedString;
  ModelIdentity() = default;
  explicit ModelIdentity(std::string v) : ValidatedString<ModelIdentity, true>(std::move(v), "ModelIdentity") {}
};
struct ModelRevision : ValidatedString<ModelRevision, true> {
  using ValidatedString<ModelRevision, true>::ValidatedString;
  ModelRevision() = default;
  explicit ModelRevision(std::string v) : ValidatedString<ModelRevision, true>(std::move(v), "ModelRevision") {}
};

// ---------------------------------------------------------------------------
// ArtifactId — stable logical identity, optionally pinned to a content digest.
// ---------------------------------------------------------------------------
struct ArtifactId {
  ArtifactNamespace ns;
  std::string name;
  std::optional<Digest> content_pin;

  ArtifactId() = default;
  ArtifactId(ArtifactNamespace ns_, std::string name_)
      : ns(std::move(ns_)), name(std::move(name_)) {
    detail::check_identity_string("ArtifactId.name", name);
  }
  ArtifactId(ArtifactNamespace ns_, std::string name_, Digest pin)
      : ns(std::move(ns_)), name(std::move(name_)), content_pin(pin) {
    detail::check_identity_string("ArtifactId.name", name);
  }

  bool operator==(const ArtifactId& o) const noexcept {
    return ns == o.ns && name == o.name && content_pin == o.content_pin;
  }
  bool operator!=(const ArtifactId& o) const noexcept { return !(*this == o); }
  bool operator<(const ArtifactId& o) const noexcept {
    if (ns != o.ns) return ns < o.ns;
    if (name != o.name) return name < o.name;
    return content_pin < o.content_pin;
  }

  /// Canonical textual form. With no pin: "<ns>/<name>"; with pin: "<ns>/<name>#<hex>".
  std::string canonical() const {
    std::string s = ns.str() + "/" + name;
    if (content_pin) { s.push_back('#'); s += content_pin->hex(); }
    return s;
  }

  void to_canonical(canon::Writer& w) const {
    w.begin_map(3);
    w.write_key("ns"); w.write_string(ns.str());
    w.write_key("name"); w.write_string(name);
    w.write_key("pin");
    if (content_pin) w.write_digest(*content_pin);
    else w.write_null();
    w.end_map();
  }

  static ArtifactId from_canonical(canon::Reader& r) {
    ArtifactId a;
    const auto n = r.begin_map();
    bool has_ns = false, has_name = false;
    for (std::size_t i = 0; i < n; ++i) {
      auto k = r.read_key();
      if (k == "ns") { a.ns = ArtifactNamespace(r.read_string()); has_ns = true; }
      else if (k == "name") { a.name = r.read_string(); has_name = true; }
      else if (k == "pin") {
        if (r.peek_tag() == canon::Tag::null) r.read_null();
        else a.content_pin = r.read_digest();
      } else fail(ErrorCode::malformed_metadata, "unexpected ArtifactId field: " + k);
    }
    r.end_map();
    if (!has_ns || !has_name) fail(ErrorCode::malformed_metadata, "ArtifactId missing required fields");
    detail::check_identity_string("ArtifactId.name", a.name);
    return a;
  }
};

}  // namespace model_cache

namespace std {
template <>
struct hash<model_cache::ArtifactId> {
  std::size_t operator()(const model_cache::ArtifactId& a) const noexcept {
    std::size_t h = std::hash<std::string>()(a.ns.str());
    h ^= std::hash<std::string>()(a.name) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    if (a.content_pin) h ^= std::hash<model_cache::Digest>()(*a.content_pin) + 0x9e3779b97f4a7c15ull;
    return h;
  }
};
}  // namespace std
