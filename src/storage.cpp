#include "model_cache/storage.hpp"

#include "model_cache/detail/platform.hpp"
#include "model_cache/error.hpp"

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>

namespace model_cache {

using platform::path_exists;

Storage::Storage(std::string root, Config config)
    : root_(std::move(root)), config_(std::move(config)) {
  if (root_.empty()) fail(ErrorCode::invalid_argument, "storage root must not be empty");
  ensure_layout();
}

Storage::~Storage() = default;

std::string Storage::join(std::initializer_list<std::string> parts) const {
  std::string out = root_;
  for (const auto& p : parts) {
    if (!out.empty() && out.back() != '/' && out.back() != '\\') out.push_back('/');
    out += p;
  }
  return out;
}

void Storage::ensure_layout() {
  platform::ensure_directory(root_);
  platform::ensure_directory(join({"objects"}));
  platform::ensure_directory(join({"meta", "artifacts"}));
  platform::ensure_directory(join({"meta", "manifests"}));
  platform::ensure_directory(join({"meta", "aliases"}));
  platform::ensure_directory(join({"quarantine"}));
  platform::ensure_directory(join({"tmp"}));
}

// ---------------------------------------------------------------------------
// content-addressable store
// ---------------------------------------------------------------------------
std::string Storage::object_path(const Digest& d) const {
  const std::string hex = d.hex();
  return join({"objects", hex.substr(0, 2), hex});
}

std::string Storage::object_meta_path(const Digest& d) const {
  return object_path(d) + ".meta";
}

bool Storage::object_exists(const Digest& d) const noexcept {
  return path_exists(object_path(d));
}

bool Storage::verify_object(const Digest& d) const {
  if (!object_exists(d)) return false;
  return platform::hash_file(object_path(d)) == d;
}

std::uint64_t Storage::object_size(const Digest& d) const {
  if (!object_exists(d)) fail(ErrorCode::not_found, "object size: missing backing");
  return platform::file_size(object_path(d));
}

bool Storage::write_object(const Digest& d, const void* data, std::size_t bytes) {
  const std::string op = object_path(d);
  const std::string dir = op.substr(0, op.find_last_of('/'));
  platform::ensure_directory(dir);
  if (path_exists(op)) {
    if (verify_object(d)) return false;  // dedup hit, content already correct
    // Corrupt existing backing: move aside and re-write.
    quarantine_object(d, "dedup_mismatch");
  }
  platform::write_file_atomic(op, data, bytes);
  return true;
}

void Storage::remove_object(const Digest& d) noexcept {
  platform::remove_file_if_exists(object_path(d));
  platform::remove_file_if_exists(object_meta_path(d));
}

void Storage::quarantine_object(const Digest& d, const std::string& reason) {
  const std::string src = object_path(d);
  if (!path_exists(src)) return;
  const std::string dest = join({"quarantine", "o_" + d.hex() + "." + std::to_string(platform::now_millis())});
  platform::remove_file_if_exists(dest);
  std::error_code ec;
  std::filesystem::rename(src, dest, ec);
  if (ec) {
    // Fall back to copy+delete so quarantine still happens if rename is blocked.
    const auto bytes = platform::read_file(src);
    platform::write_file_atomic(dest, bytes);
    platform::remove_file_if_exists(src);
  }
  (void)reason;
}

void Storage::list_object_digests(std::vector<Digest>& out) const {
  const std::string base = join({"objects"});
  std::error_code ec;
  for (const auto& sub : std::filesystem::directory_iterator(base, ec)) {
    if (ec) break;
    if (!sub.is_directory()) continue;
    for (const auto& f : std::filesystem::directory_iterator(sub.path(), ec)) {
      if (ec) break;
      const auto name = f.path().filename().string();
      if (name.size() == 64 && name.find_first_not_of("0123456789abcdef") == std::string::npos) {
        out.push_back(Digest::from_hex(name));
      }
    }
  }
}

// ---------------------------------------------------------------------------
// metadata: artifact records
// ---------------------------------------------------------------------------
std::string Storage::record_file(const std::string& ns, const std::string& name) const {
  return join({"meta", "artifacts", detail::encode_for_path(ns), detail::encode_for_path(name) + ".mc"});
}

std::optional<ArtifactRecord> Storage::load_record(const std::string& ns,
                                                   const std::string& name) const {
  const std::string path = record_file(ns, name);
  if (!path_exists(path)) return std::nullopt;
  const auto data = platform::read_file(path, config_.limits.max_metadata_bytes);
  canon::Reader r(data.data(), data.size(), canon::Limits{});
  ArtifactRecord rec = ArtifactRecord::from_canonical(r);
  r.require_consumed();
  return rec;
}

void Storage::save_record(const ArtifactRecord& rec) const {
  const std::string path = record_file(rec.ns, rec.name);
  platform::ensure_directory(path.substr(0, path.find_last_of('/')));
  canon::Writer w;
  rec.to_canonical(w);
  if (w.size() > config_.limits.max_metadata_bytes)
    fail(ErrorCode::capacity_exceeded, "artifact record metadata exceeds bound");
  platform::write_file_atomic(path, w.bytes().data(), w.size());
}

void Storage::remove_record(const std::string& ns, const std::string& name) const {
  platform::remove_file_if_exists(record_file(ns, name));
}

std::vector<std::string> Storage::list_record_keys() const {
  std::vector<std::string> out;
  const std::string base = join({"meta", "artifacts"});
  std::error_code ec;
  for (const auto& nsdir : std::filesystem::directory_iterator(base, ec)) {
    if (ec) break;
    if (!nsdir.is_directory()) continue;
    for (const auto& f : std::filesystem::directory_iterator(nsdir.path(), ec)) {
      if (ec) break;
      const auto path = f.path().string();
      if (path.size() < 4 || path.substr(path.size() - 3) != ".mc") continue;
      const auto data = platform::read_file(path, config_.limits.max_metadata_bytes);
      canon::Reader r(data.data(), data.size(), canon::Limits{});
      ArtifactRecord rec = ArtifactRecord::from_canonical(r);
      out.push_back(rec.logical_key());
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

// ---------------------------------------------------------------------------
// metadata: manifest blobs
// ---------------------------------------------------------------------------
std::string Storage::manifest_file(const std::string& model, const std::string& revision) const {
  return join({"meta", "manifests", detail::encode_for_path(model) + "." + detail::encode_for_path(revision) + ".manifest"});
}

std::optional<Manifest> Storage::load_manifest(const std::string& model,
                                               const std::string& revision) const {
  const std::string path = manifest_file(model, revision);
  if (!path_exists(path)) return std::nullopt;
  const auto data = platform::read_file(path, config_.limits.max_metadata_bytes);
  canon::Reader r(data.data(), data.size(), canon::Limits{});
  Manifest m = Manifest::from_canonical(r);
  r.require_consumed();
  return m;
}

void Storage::save_manifest(const Manifest& m) const {
  const std::string path = manifest_file(m.model.str(), m.revision.str());
  platform::ensure_directory(path.substr(0, path.find_last_of('/')));
  canon::Writer w;
  m.to_canonical(w);
  // Nothing is written that could not be read back under the same bound.
  if (w.size() > config_.limits.max_metadata_bytes)
    fail(ErrorCode::capacity_exceeded, "manifest metadata exceeds bound");
  platform::write_file_atomic(path, w.bytes().data(), w.size());
}

void Storage::remove_manifest(const std::string& model, const std::string& revision) const {
  platform::remove_file_if_exists(manifest_file(model, revision));
}

std::vector<std::string> Storage::list_manifest_keys() const {
  std::vector<std::string> out;
  const std::string base = join({"meta", "manifests"});
  std::error_code ec;
  for (const auto& f : std::filesystem::directory_iterator(base, ec)) {
    if (ec) break;
    const auto path = f.path().string();
    if (path.size() < 10 || path.substr(path.size() - 9) != ".manifest") continue;
    const auto data = platform::read_file(path, config_.limits.max_metadata_bytes);
    canon::Reader r(data.data(), data.size(), canon::Limits{});
    Manifest m = Manifest::from_canonical(r);
    out.push_back(m.canonical_key());
  }
  std::sort(out.begin(), out.end());
  return out;
}

// ---------------------------------------------------------------------------
// metadata: aliases
// ---------------------------------------------------------------------------
std::string Storage::alias_file(const std::string& alias) const {
  return join({"meta", "aliases", detail::encode_for_path(alias) + ".alias"});
}

std::optional<std::string> Storage::load_alias(const std::string& alias) const {
  const std::string path = alias_file(alias);
  if (!path_exists(path)) return std::nullopt;
  const auto data = platform::read_file(path, config_.limits.max_metadata_bytes);
  if (data.empty()) fail(ErrorCode::malformed_metadata, "empty alias file");
  return std::string(reinterpret_cast<const char*>(data.data()), data.size());
}

void Storage::save_alias(const std::string& alias, const std::string& target) const {
  if (alias.find('\n') != std::string::npos || alias.find('\0') != std::string::npos)
    fail(ErrorCode::invalid_identity, "alias contains forbidden character");
  const std::string path = alias_file(alias);
  platform::ensure_directory(path.substr(0, path.find_last_of('/')));
  platform::write_file_atomic(path, target.data(), target.size());
}

void Storage::remove_alias(const std::string& alias) const {
  platform::remove_file_if_exists(alias_file(alias));
}

std::vector<std::string> Storage::list_aliases() const {
  std::vector<std::string> out;
  const std::string base = join({"meta", "aliases"});
  std::error_code ec;
  for (const auto& f : std::filesystem::directory_iterator(base, ec)) {
    if (ec) break;
    const auto fname = f.path().filename().string();
    if (fname.size() < 7 || fname.substr(fname.size() - 6) != ".alias") continue;
    const auto raw = fname.substr(0, fname.size() - 6);
    out.push_back(detail::decode_for_path(raw));
  }
  std::sort(out.begin(), out.end());
  return out;
}

// ---------------------------------------------------------------------------
// staging / temp
// ---------------------------------------------------------------------------
std::string Storage::new_temp_path(const std::string& hint) const {
  return join({"tmp", "stg_" + std::to_string(platform::now_millis()) + "_" + std::to_string(reinterpret_cast<std::uintptr_t>(this) & 0xffff) + "_" + hint});
}

// ---------------------------------------------------------------------------
// recovery
// ---------------------------------------------------------------------------
namespace {
// A staging file left behind by write_file_atomic is "<target>.tmp" followed only
// by digits and dots. A stored file whose own name merely contains ".tmp" (an
// artifact called "weights.tmp", say) is not one and must be kept.
bool is_stale_temp(const std::string& name) {
  const auto pos = name.rfind(".tmp");
  if (pos == std::string::npos) return false;
  const std::string rest = name.substr(pos + 4);
  if (rest.find_first_of("0123456789") == std::string::npos) return false;
  return rest.find_first_not_of("0123456789.") == std::string::npos;
}
}  // namespace

void Storage::recover() {
  // 1. Remove stale temp files anywhere under the root.
  {
    std::error_code ec;
    auto purge = [&](const auto& self_ref, const std::string& dir) -> void {
      for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (e.is_directory()) { self_ref(self_ref, e.path().string()); }
        else if (is_stale_temp(e.path().filename().string())) {
          platform::remove_file_if_exists(e.path().string());
        }
      }
    };
    purge(purge, root_);
  }
  // 2. Reconstruct the referenced-digest set from authoritative records.
  std::set<Digest> referenced;
  for (const auto& key : list_record_keys()) {
    auto slash = key.find('/');
    if (slash == std::string::npos) continue;
    auto rec = load_record(key.substr(0, slash), key.substr(slash + 1));
    if (!rec) continue;
    for (const auto& g : rec->generations) {
      if (g.backing_key) referenced.insert(Digest::from_hex(*g.backing_key));
    }
  }
  // 3. Verify objects; quarantine corrupt ones; delete orphans.
  std::vector<Digest> objects;
  list_object_digests(objects);
  for (const auto& d : objects) {
    if (referenced.find(d) == referenced.end()) {
      platform::remove_file_if_exists(object_path(d));  // orphan
      continue;
    }
    if (!verify_object(d)) {
      quarantine_object(d, "recovery_corruption");
    }
  }
}

}  // namespace model_cache
