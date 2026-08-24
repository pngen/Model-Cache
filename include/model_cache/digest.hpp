#pragma once
// Model Cache — authoritative content digest. Always SHA-256 (32 bytes).
#include "model_cache/detail/sha256.hpp"
#include "model_cache/error.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <string>

namespace model_cache {

/// A 32-byte SHA-256 digest. This is the *authoritative* content identity for
/// correctness-critical content. Weak hashes (CRC-32C etc.) are permitted only
/// as fast non-authoritative block checks.
struct Digest {
  static constexpr std::size_t kSize = 32;
  std::array<std::uint8_t, kSize> bytes{};

  bool operator==(const Digest& o) const noexcept { return bytes == o.bytes; }
  bool operator!=(const Digest& o) const noexcept { return !(*this == o); }
  bool operator<(const Digest& o) const noexcept { return bytes < o.bytes; }
  bool operator>(const Digest& o) const noexcept { return o < *this; }

  bool is_zero() const noexcept {
    for (auto b : bytes) { if (b != 0) return false; }
    return true;
  }

  /// Lowercase 64-char hex.
  std::string hex() const {
    static const char* h = "0123456789abcdef";
    std::string s;
    s.reserve(kSize * 2);
    for (std::uint8_t b : bytes) {
      s.push_back(h[b >> 4]);
      s.push_back(h[b & 0x0fu]);
    }
    return s;
  }

  static Digest from_hex(const std::string& s) {
    Digest d;
    if (s.size() != kSize * 2) fail(ErrorCode::invalid_digest,
                                    "digest hex must be 64 chars, got " + std::to_string(s.size()));
    auto nib = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    for (std::size_t i = 0; i < kSize; ++i) {
      const int hi = nib(s[2 * i]);
      const int lo = nib(s[2 * i + 1]);
      if (hi < 0 || lo < 0) fail(ErrorCode::invalid_digest, "digest hex contains non-hex bytes");
      d.bytes[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return d;
  }
};

/// Compute SHA-256 of a memory buffer.
inline Digest sha256(const void* data, std::size_t len) {
  detail::Sha256 h;
  h.update(data, len);
  Digest d;
  d.bytes = h.final();
  return d;
}

inline Digest sha256(const std::string& s) { return sha256(s.data(), s.size()); }

/// Streaming hasher wrapper producing a Digest.
class DigestHasher {
 public:
  void update(const void* data, std::size_t len) { inner_.update(data, len); }
  void update(const std::string& s) { inner_.update(s); }
  Digest finish() { Digest d; d.bytes = inner_.final(); return d; }

 private:
  detail::Sha256 inner_;
};

}  // namespace model_cache

namespace std {
template <>
struct hash<model_cache::Digest> {
  std::size_t operator()(const model_cache::Digest& d) const noexcept {
    std::size_t h = 1469598103934665603ull;
    for (std::uint8_t b : d.bytes) {
      h ^= static_cast<std::size_t>(b);
      h *= 1099511628211ull;
    }
    return h;
  }
};
}  // namespace std
