#pragma once
// Model Cache — strict, versioned, deterministic canonical serialization.
//
// Canonical form guarantees that semantically identical values produce identical
// byte streams, so canonical hashes and compatibility keys are reproducible.
// Reading is strict: it bounds all allocations, validates types/lengths/UTF-8,
// rejects malformed enumerations, trailing bytes, and impossible sizes, and
// never silently coerces malformed input to zero/default values.

#include "model_cache/digest.hpp"
#include "model_cache/error.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace model_cache {
namespace canon {

enum class Tag : std::uint8_t {
  null = 0x00,
  boolean = 0x01,
  int64 = 0x03,
  uint64 = 0x04,
  double_ = 0x05,
  string = 0x06,
  bytes = 0x07,
  digest = 0x08,
  map = 0x09,
  seq = 0x0a,
};

struct Limits {
  std::size_t max_string_bytes = 16 * 1024 * 1024;
  std::size_t max_seq_len = 4 * 1024 * 1024;
  std::size_t max_map_len = 4 * 1024 * 1024;
  std::size_t max_depth = 128;
  std::size_t max_total_bytes = 1024 * 1024 * 1024;
};

namespace detail {
inline void put_u64(std::vector<std::uint8_t>& out, std::uint64_t v) {
  for (int i = 7; i >= 0; --i) out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xffu));
}
}  // namespace detail

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------
class Writer {
 public:
  void write_null() { buf_.push_back(static_cast<std::uint8_t>(Tag::null)); }
  void write_bool(bool b) {
    buf_.push_back(static_cast<std::uint8_t>(Tag::boolean));
    buf_.push_back(b ? 1 : 0);
  }
  void write_i64(std::int64_t v) {
    buf_.push_back(static_cast<std::uint8_t>(Tag::int64));
    std::uint64_t u = static_cast<std::uint64_t>(v);
    for (int i = 7; i >= 0; --i) buf_.push_back(static_cast<std::uint8_t>((u >> (8 * i)) & 0xffu));
  }
  void write_uint64(std::uint64_t v) {
    buf_.push_back(static_cast<std::uint8_t>(Tag::uint64));
    for (int i = 7; i >= 0; --i) buf_.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xffu));
  }
  void write_double(double v) {
    if (!std::isfinite(v)) fail(ErrorCode::malformed_metadata, "cannot serialize non-finite double");
    buf_.push_back(static_cast<std::uint8_t>(Tag::double_));
    std::uint64_t u;
    static_assert(sizeof(u) == sizeof(v), "double/uint64 size mismatch");
    std::memcpy(&u, &v, sizeof(u));
    for (int i = 7; i >= 0; --i) buf_.push_back(static_cast<std::uint8_t>((u >> (8 * i)) & 0xffu));
  }
  void write_string(std::string_view sv) {
    buf_.push_back(static_cast<std::uint8_t>(Tag::string));
    detail::put_u64(buf_, static_cast<std::uint64_t>(sv.size()));
    buf_.insert(buf_.end(), sv.begin(), sv.end());
  }
  void write_bytes(const std::uint8_t* p, std::size_t n) {
    buf_.push_back(static_cast<std::uint8_t>(Tag::bytes));
    detail::put_u64(buf_, static_cast<std::uint64_t>(n));
    if (n) buf_.insert(buf_.end(), p, p + n);
  }
  void write_digest(const Digest& d) {
    buf_.push_back(static_cast<std::uint8_t>(Tag::digest));
    buf_.insert(buf_.end(), d.bytes.begin(), d.bytes.end());
  }
  void begin_map(std::size_t n) {
    buf_.push_back(static_cast<std::uint8_t>(Tag::map));
    detail::put_u64(buf_, static_cast<std::uint64_t>(n));
  }
  void begin_seq(std::size_t n) {
    buf_.push_back(static_cast<std::uint8_t>(Tag::seq));
    detail::put_u64(buf_, static_cast<std::uint64_t>(n));
  }
  void write_key(std::string_view k) { write_string(k); }
  // The Writer is a flat length-prefixed stream, so map/sequence ends are
  // implicit; these no-ops keep the API symmetric with the Reader.
  void end_map() {}
  void end_seq() {}

  const std::vector<std::uint8_t>& bytes() const { return buf_; }
  std::vector<std::uint8_t> take() { return std::move(buf_); }
  std::size_t size() const { return buf_.size(); }

  /// Hash the accumulated canonical bytes with SHA-256.
  Digest digest() const { return model_cache::sha256(buf_.data(), buf_.size()); }

 private:
  std::vector<std::uint8_t> buf_;
};

// ---------------------------------------------------------------------------
// Reader (strict)
// ---------------------------------------------------------------------------
class Reader {
 public:
  explicit Reader(const std::uint8_t* data, std::size_t len, Limits lim = Limits{})
      : p_(data), end_(data + len), lim_(lim) {}

  Reader(const std::vector<std::uint8_t>& v) : p_(v.data()), end_(v.data() + v.size()) {}
  Reader(std::string_view sv) : p_(reinterpret_cast<const std::uint8_t*>(sv.data())), end_(p_ + sv.size()) {}

  bool at_end() const { return p_ == end_; }
  std::size_t remaining() const { return static_cast<std::size_t>(end_ - p_); }

  bool ok() const { return !failed_; }
  const char* failure_reason() const { return reason_.c_str(); }

  void require_consumed() {
    if (!at_end()) fail(ErrorCode::malformed_metadata, "trailing bytes in canonical stream");
  }

  Tag peek_tag() {
    need(1);
    return static_cast<Tag>(*p_);
  }

  void read_null() { byte_checked(static_cast<std::uint8_t>(Tag::null)); }
  bool read_bool() {
    byte_checked(static_cast<std::uint8_t>(Tag::boolean));
    need(1);
    std::uint8_t v = *p_++;
    if (v > 1) malformed("bad boolean value");
    return v != 0;
  }
  std::int64_t read_i64() {
    byte_checked(static_cast<std::uint8_t>(Tag::int64));
    std::uint64_t u = read_uint64_raw();
    return static_cast<std::int64_t>(u);
  }
  std::uint64_t read_uint64() {
    byte_checked(static_cast<std::uint8_t>(Tag::uint64));
    return read_uint64_raw();
  }
  double read_double() {
    byte_checked(static_cast<std::uint8_t>(Tag::double_));
    std::uint64_t u = read_uint64_raw();
    double v;
    std::memcpy(&v, &u, sizeof(v));
    if (!std::isfinite(v)) malformed("non-finite double in canonical stream");
    return v;
  }
  std::string read_string() {
    byte_checked(static_cast<std::uint8_t>(Tag::string));
    const auto n = read_len();
    if (n > lim_.max_string_bytes) malformed("string exceeds max_string_bytes");
    if (static_cast<std::size_t>(end_ - p_) < n) malformed("truncated string");
    if (!validate_utf8(p_, n)) malformed("invalid UTF-8 in string");
    std::string s(reinterpret_cast<const char*>(p_), n);
    p_ += n;
    return s;
  }
  std::vector<std::uint8_t> read_bytes() {
    byte_checked(static_cast<std::uint8_t>(Tag::bytes));
    const auto n = read_len();
    if (n > lim_.max_string_bytes) malformed("bytes length exceeds bound");
    if (static_cast<std::size_t>(end_ - p_) < n) malformed("truncated bytes");
    std::vector<std::uint8_t> v(p_, p_ + n);
    p_ += n;
    return v;
  }
  Digest read_digest() {
    byte_checked(static_cast<std::uint8_t>(Tag::digest));
    need(Digest::kSize);
    Digest d;
    std::memcpy(d.bytes.data(), p_, Digest::kSize);
    p_ += Digest::kSize;
    return d;
  }
  std::size_t begin_map() {
    byte_checked(static_cast<std::uint8_t>(Tag::map));
    auto n = read_len();
    if (n > lim_.max_map_len) malformed("map length exceeds bound");
    if (++depth_ > lim_.max_depth) malformed("nesting depth exceeded");
    return static_cast<std::size_t>(n);
  }
  std::size_t begin_seq() {
    byte_checked(static_cast<std::uint8_t>(Tag::seq));
    auto n = read_len();
    if (n > lim_.max_seq_len) malformed("seq length exceeds bound");
    if (++depth_ > lim_.max_depth) malformed("nesting depth exceeded");
    return static_cast<std::size_t>(n);
  }
  std::string read_key() {
    // Keys are strings immediately followed by a value; canonical order is by
    // raw key bytes. This reads the key string.
    byte_checked(static_cast<std::uint8_t>(Tag::string));
    const auto n = read_len();
    if (n > lim_.max_string_bytes) malformed("key string exceeds max_string_bytes");
    if (static_cast<std::size_t>(end_ - p_) < n) malformed("truncated key");
    std::string s(reinterpret_cast<const char*>(p_), n);
    p_ += n;
    return s;
  }
  void end_map() { if (depth_ > 0) --depth_; }
  void end_seq() { if (depth_ > 0) --depth_; }

 private:
  void need(std::size_t n) {
    if (failed_) malformed("stream already failed");
    if (static_cast<std::size_t>(end_ - p_) < n) malformed("truncated canonical stream");
  }
  std::uint64_t read_len() {
    need(8);
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | static_cast<std::uint8_t>(*p_++);
    if (v > lim_.max_total_bytes) malformed("length exceeds max_total_bytes");
    return v;
  }
  std::uint64_t read_uint64_raw() {
    need(8);
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | static_cast<std::uint8_t>(*p_++);
    return v;
  }
  void byte_checked(std::uint8_t expected) {
    need(1);
    if (*p_ != expected) malformed("unexpected tag in canonical stream");
    ++p_;
  }
  [[noreturn]] void malformed(const char* why) {
    failed_ = true;
    reason_ = why;
    fail(ErrorCode::malformed_metadata, std::string("malformed canonical metadata: ") + why);
  }
  static bool validate_utf8(const std::uint8_t* p, std::size_t n) {
    // Bounded UTF-8 validator (rejects overlong forms, surrogates, >U+10FFFF).
    std::size_t i = 0;
    while (i < n) {
      const std::uint8_t c = p[i];
      if (c < 0x80) { ++i; continue; }
      std::size_t extra = 0; std::uint32_t cp = 0;
      if ((c & 0xe0u) == 0xc0u) { extra = 1; cp = c & 0x1fu; }
      else if ((c & 0xf0u) == 0xe0u) { extra = 2; cp = c & 0x0fu; }
      else if ((c & 0xf8u) == 0xf0u) { extra = 3; cp = c & 0x07u; }
      else return false;
      if (i + extra >= n) return false;
      for (std::size_t k = 1; k <= extra; ++k) {
        const std::uint8_t cc = p[i + k];
        if ((cc & 0xc0u) != 0x80u) return false;
        cp = (cp << 6) | (cc & 0x3fu);
      }
      // Reject overlong, surrogate, and out-of-range.
      if ((extra == 1 && cp < 0x80) ||
          (extra == 2 && cp < 0x800) ||
          (extra == 3 && cp < 0x10000) ||
          cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
      i += extra + 1;
    }
    return true;
  }

  const std::uint8_t* p_;
  const std::uint8_t* end_;
  Limits lim_;
  bool failed_ = false;
  std::string reason_;
  std::size_t depth_ = 0;
};

}  // namespace canon
}  // namespace model_cache
