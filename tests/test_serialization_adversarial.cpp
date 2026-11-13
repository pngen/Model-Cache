#include "test_helpers.hpp"
#include "model_cache/serialization.hpp"

namespace {
using canon::Limits;
using canon::Reader;
using canon::Tag;
}

TEST(digest_rejects_malformed) {
  CHECK_THROWS(Digest::from_hex("abc"), ErrorCode::invalid_digest);
  CHECK_THROWS(Digest::from_hex(std::string(64, 'z')), ErrorCode::invalid_digest);
  // Correct 64-char lowercase/uppercase valid.
  const std::string good = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  CHECK_EQ(Digest::from_hex(good).hex(), good);
}

TEST(reader_rejects_truncated_string) {
  const std::vector<std::uint8_t> data = {0x06, 0, 0, 0, 0, 0, 0, 0, 10};  // string, len=10, no bytes
  Reader r(data.data(), data.size());
  CHECK_THROWS(r.read_string(), ErrorCode::malformed_metadata);
}

TEST(reader_rejects_nan_double) {
  const std::vector<std::uint8_t> data = {0x05, 0x7f, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  Reader r(data.data(), data.size());
  CHECK_THROWS(r.read_double(), ErrorCode::malformed_metadata);
}

TEST(reader_rejects_oversized_length) {
  const std::vector<std::uint8_t> data = {0x06, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  Reader r(data.data(), data.size());
  CHECK_THROWS(r.read_string(), ErrorCode::malformed_metadata);
}

TEST(reader_rejects_bad_utf8) {
  const std::vector<std::uint8_t> data = {0x06, 0, 0, 0, 0, 0, 0, 0, 1, 0xff};
  Reader r(data.data(), data.size());
  CHECK_THROWS(r.read_string(), ErrorCode::malformed_metadata);
}

TEST(reader_rejects_trailing_bytes) {
  const std::vector<std::uint8_t> data = {0x06, 0, 0, 0, 0, 0, 0, 0, 1, 'x', 0x7f};
  Reader r(data.data(), data.size());
  r.read_string();
  CHECK_THROWS(r.require_consumed(), ErrorCode::malformed_metadata);
}

TEST(record_rejects_missing_fields) {
  const std::vector<std::uint8_t> data = {0x09, 0, 0, 0, 0, 0, 0, 0, 0};  // empty map
  Reader r(data.data(), data.size());
  CHECK_THROWS(ArtifactRecord::from_canonical(r), ErrorCode::malformed_metadata);
}

TEST(record_rejects_unknown_field) {
  // map(1) { "bogus" : null }
  std::vector<std::uint8_t> data;
  data.push_back(0x09);
  for (int i = 0; i < 8; ++i) data.push_back(i == 7 ? 1 : 0);   // count = 1
  data.push_back(0x06);                                         // key string
  for (int i = 0; i < 8; ++i) data.push_back(i == 7 ? 5 : 0);   // key len 5
  data.insert(data.end(), {'b', 'o', 'g', 'u', 's'});
  data.push_back(0x00);                                         // null value
  Reader r(data.data(), data.size());
  CHECK_THROWS(ArtifactRecord::from_canonical(r), ErrorCode::malformed_metadata);
}

TEST(enum_parser_rejects_unknown) {
  CHECK_THROWS(parse_artifact_type("BOGUS_TYPE"), ErrorCode::invalid_argument);
  CHECK_THROWS(parse_validation_state("UNKNOWN_STATE"), ErrorCode::invalid_argument);
  CHECK_THROWS(parse_reuse_action("NOT_A_REUSE"), ErrorCode::invalid_argument);
}

TEST(reader_rejects_truncated_key) {
  // A key that claims ten bytes and brings none.
  const std::vector<std::uint8_t> data = {0x06, 0, 0, 0, 0, 0, 0, 0, 10};
  Reader r(data.data(), data.size());
  CHECK_THROWS(r.read_key(), ErrorCode::malformed_metadata);
}

TEST(stream_reader_cannot_overrun_the_buffer) {
  TempDir td;
  ModelCache cache(td.join("root"));
  const ArtifactId id(ArtifactNamespace("n"), "s");
  int calls = 0;
  auto reader = [&calls](std::uint8_t* buf, std::size_t max) -> std::size_t {
    if (++calls == 1) { for (std::size_t i = 0; i < 16 && i < max; ++i) buf[i] = 'x'; return 16; }
    return static_cast<std::size_t>(-1);   // an error code returned as a size
  };
  CHECK_THROWS(cache.ingest_stream(id, spec(ArtifactType::MODEL_WEIGHTS, "weights", "1"), reader),
               ErrorCode::invalid_argument);
  CHECK(!cache.exists(id));
}
