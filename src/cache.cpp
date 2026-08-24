#include "model_cache/cache.hpp"

#include "model_cache/detail/platform.hpp"
#include "model_cache/error.hpp"
#include "model_cache/storage.hpp"
#include "model_cache/dependency.hpp"

#include <algorithm>
#include <chrono>
#include <map>
#include <memory>
#include <set>
#include <shared_mutex>
#include <sstream>

namespace model_cache {

namespace {
std::string logical_key(const std::string& ns, const std::string& name) { return ns + "/" + name; }

std::pair<std::string, std::string> split_logical(const std::string& key) {
  auto slash = key.find('/');
  if (slash == std::string::npos) fail(ErrorCode::invalid_identity, "malformed logical key: " + key);
  return {key.substr(0, slash), key.substr(slash + 1)};
}

std::string node_key(const ArtifactId& id) { return id.canonical(); }

struct BackingInfo {
  std::uint64_t refcount = 0;
  std::uint64_t bytes = 0;
  ValidationState validation = ValidationState::UNKNOWN;
};
}  // namespace

struct ModelCache::Impl {
  Config config;
  Storage storage;
  EventBus events;
  Stats stats;

  mutable std::shared_mutex mu;
  std::mutex cas_mu;

  std::map<std::string, ArtifactRecord> artifacts;
  std::map<std::string, Manifest> manifests;
  std::map<std::string, std::string> aliases;
  std::map<Digest, BackingInfo> backings;
  DependencyGraph graph;

  std::uint64_t cache_generation = 0;
  std::uint64_t logical_bytes = 0;
  std::uint64_t physical_bytes = 0;
  std::uint64_t logical_count = 0;
  bool closed = false;

  Impl(std::string root, Config cfg)
      : config(std::move(cfg)),
        storage(root, config),
        events(config.limits.max_subscriptions, config.limits.max_event_backlog) {}

  std::uint64_t recompute_accounting() {
    logical_bytes = 0;
    logical_count = 0;
    for (const auto& kv : artifacts) {
      const auto* gen = kv.second.current();
      if (gen) { logical_bytes += gen->size; ++logical_count; }
    }
    physical_bytes = 0;
    for (const auto& b : backings) if (b.second.refcount > 0) physical_bytes += b.second.bytes;
    return cache_generation;
  }

  void load_state() {
    std::unique_lock<std::shared_mutex> g(mu);
    for (const auto& key : storage.list_record_keys()) {
      auto [ns, name] = split_logical(key);
      auto rec = storage.load_record(ns, name);
      if (!rec) continue;
      artifacts[logical_key(ns, name)] = std::move(*rec);
    }
    backings.clear();
    for (const auto& kv : artifacts) {
      for (const auto& gen : kv.second.generations) {
        if (!gen.backing_key) continue;
        Digest d = Digest::from_hex(*gen.backing_key);
        auto& bi = backings[d];
        bi.refcount += 1;
        if (bi.bytes == 0) bi.bytes = gen.size;
        if (gen.validation == ValidationState::VALID) bi.validation = ValidationState::VALID;
      }
    }
    // Validation-consistency pass: a generation whose backing was quarantined or
    // is otherwise missing must never be served as authoritative/valid.
    for (auto& kv : artifacts) {
      bool changed = false;
      for (auto& gen : kv.second.generations) {
        if (gen.backing_key && gen.validation == ValidationState::VALID) {
          Digest d = Digest::from_hex(*gen.backing_key);
          if (!storage.object_exists(d)) { gen.validation = ValidationState::QUARANTINED; changed = true; }
        }
      }
      if (changed) storage.save_record(kv.second);
    }
    for (const auto& kv : artifacts) {
      const auto& rec = kv.second;
      const std::string nk = logical_key(rec.ns, rec.name);
      graph.set_dependencies(nk, rec.dependencies);
    }
    for (const auto& key : storage.list_manifest_keys()) {
      const std::string prefix("manifest:");
      if (key.compare(0, prefix.size(), prefix) != 0) continue;
      auto body = key.substr(prefix.size());
      auto slash = body.find('/');
      if (slash == std::string::npos) continue;
      auto m = storage.load_manifest(body.substr(0, slash), body.substr(slash + 1));
      if (m) manifests[key] = std::move(*m);
    }
    for (const auto& alias : storage.list_aliases()) {
      auto target = storage.load_alias(alias);
      if (target) aliases[alias] = *target;
    }
    recompute_accounting();
  }
};
// ---------------------------------------------------------------------------
// construction / lifecycle
// ---------------------------------------------------------------------------
ModelCache::ModelCache(std::string root, Config config)
    : impl_(std::make_unique<Impl>(std::move(root), std::move(config))) {
  validate_config(impl_->config);
  impl_->storage.recover();
  impl_->load_state();
}

ModelCache::~ModelCache() { close(); }

const Config& ModelCache::config() const noexcept { return impl_->config; }
const std::string& ModelCache::root() const noexcept { return impl_->storage.root(); }

void ModelCache::close() {
  if (!impl_) return;
  std::unique_lock<std::shared_mutex> g(impl_->mu);
  impl_->closed = true;
}

// ---------------------------------------------------------------------------
// ingest
// ---------------------------------------------------------------------------
IngestResult ModelCache::ingest(const ArtifactId& id, const IngestSpec& spec,
                                const std::uint8_t* data, std::size_t size) {
  auto& im = *impl_;
  const std::string lk = logical_key(id.ns.str(), id.name);
  IngestResult res;
  res.artifact_key = lk;

  if (size > im.config.limits.max_artifact_size)
    fail(ErrorCode::invalid_argument, "artifact exceeds max_artifact_size");

  const Digest computed = sha256(data, size);
  res.digest = computed;
  res.size = size;

  im.events.emit(EventType::ARTIFACT_INGEST_STARTED,
                 {{"key", lk}, {"type", to_string(spec.type)}});

  if (spec.expected_digest && *spec.expected_digest != computed) {
    res.admitted = false; res.rejection_reason = "expected digest mismatch";
    im.stats.artifacts_rejected.fetch_add(1, std::memory_order_relaxed);
    im.events.emit(EventType::ARTIFACT_REJECTED, {{"key", lk}, {"reason", res.rejection_reason}});
    return res;
  }
  if (spec.expected_size != 0 && spec.expected_size != size) {
    res.admitted = false; res.rejection_reason = "expected size mismatch";
    im.stats.artifacts_rejected.fetch_add(1, std::memory_order_relaxed);
    im.events.emit(EventType::ARTIFACT_REJECTED, {{"key", lk}, {"reason", res.rejection_reason}});
    return res;
  }

  bool isNew = false;
  {
    std::lock_guard<std::mutex> cg(im.cas_mu);
    isNew = im.storage.write_object(computed, data, size);
  }
  res.dedup = !isNew;
  res.new_backing = isNew;
  im.stats.bytes_ingested.fetch_add(size, std::memory_order_relaxed);

  ValidationState vstate = ValidationState::UNKNOWN;
  if (spec.validation_mode == ValidationMode::External) {
    const bool ok = spec.external_validator ? spec.external_validator(computed, size) : false;
    vstate = ok ? ValidationState::VALID : ValidationState::QUARANTINED;
  } else {
    vstate = ValidationState::VALID;
  }

  if (vstate == ValidationState::QUARANTINED) {
    std::lock_guard<std::mutex> cg(im.cas_mu);
    im.storage.quarantine_object(computed, "external_validation_failed");
    res.admitted = false; res.rejection_reason = "external validator rejected";
    res.validation = ValidationState::QUARANTINED;
    im.stats.corruption_detections.fetch_add(1, std::memory_order_relaxed);
    im.stats.quarantine_events.fetch_add(1, std::memory_order_relaxed);
    im.events.emit(EventType::CORRUPTION_DETECTED, {{"key", lk}, {"reason", "validation"}});
    im.events.emit(EventType::ARTIFACT_QUARANTINED, {{"key", lk}});
    im.events.emit(EventType::ARTIFACT_REJECTED, {{"key", lk}, {"reason", res.rejection_reason}});
    return res;
  }

  std::vector<std::pair<EventType, std::map<std::string, std::string>>> emitted;
  {
    std::unique_lock<std::shared_mutex> wg(im.mu);
    if (im.closed) fail(ErrorCode::internal_error, "cache is closed");

    const bool fits = ensure_capacity(im, computed, static_cast<std::uint64_t>(size), lk);
    if (!fits) {
      res.admitted = false; res.rejection_reason = "capacity exceeded and non-evictable";
      im.stats.artifacts_rejected.fetch_add(1, std::memory_order_relaxed);
      for (const auto& ev : emitted) im.events.emit(ev.first, ev.second);
      return res;
    }

    auto [it, inserted] = im.artifacts.try_emplace(lk);
    ArtifactRecord& rec = it->second;
    if (inserted) {
      rec.ns = id.ns.str(); rec.name = id.name;
      rec.type = spec.type; rec.role = spec.role; rec.version = spec.version;
      rec.variant = spec.variant; rec.format = spec.format; rec.architecture = spec.architecture;
      rec.compatibility = spec.compatibility; rec.source_uri = spec.source_uri;
      rec.created_at = platform::now_millis(); rec.provenance = spec.provenance;
      if (spec.model) rec.provenance["model"] = spec.model->str();
      if (spec.revision) rec.provenance["revision"] = spec.revision->str();
      rec.dependencies = spec.dependencies;
    }

    const GenerationRecord* existing = rec.find_generation(computed);
    if (existing && existing->current) {
      ArtifactRecord updated = rec;
      updated.role = spec.role; updated.version = spec.version;
      updated.compatibility = spec.compatibility;
      updated.dependencies = spec.dependencies;
      updated.provenance = spec.provenance;
      if (spec.model) updated.provenance["model"] = spec.model->str();
      if (spec.revision) updated.provenance["revision"] = spec.revision->str();
      updated.updated_at = platform::now_millis();
      im.storage.save_record(updated);
      rec = std::move(updated);
      im.graph.set_dependencies(lk, spec.dependencies);
      im.recompute_accounting();
      res.admitted = true; res.generation = existing->generation;
      res.validation = existing->validation; res.dedup = true;
      im.stats.artifacts_admitted.fetch_add(1, std::memory_order_relaxed);
      emitted.push_back({EventType::ARTIFACT_VALIDATED, {{"key", lk}, {"gen", std::to_string(existing->generation)}}});
      emitted.push_back({EventType::ARTIFACT_ADMITTED, {{"key", lk}, {"dedup", "true"}}});
      for (const auto& ev : emitted) im.events.emit(ev.first, ev.second);
      return res;
    }

    ArtifactRecord updated = rec;
    const std::uint64_t newgen = updated.current_generation + 1;
    updated.current_generation = newgen;
    for (auto& g : updated.generations) g.current = false;
    GenerationRecord gen;
    gen.generation = newgen;
    gen.content_digest = computed;
    gen.size = size;
    gen.validation = vstate;
    gen.backing_key = computed.hex();
    gen.current = true;
    const std::uint64_t ts = platform::now_millis();
    gen.created_at = ts;
    updated.updated_at = ts;
    updated.generations.push_back(std::move(gen));

    im.storage.save_record(updated);
    rec = std::move(updated);
    im.backings[computed].refcount += 1;
    im.backings[computed].bytes = size;
    if (vstate == ValidationState::VALID) im.backings[computed].validation = ValidationState::VALID;
    im.graph.set_dependencies(lk, spec.dependencies);
    im.recompute_accounting();

    res.admitted = true; res.generation = newgen; res.validation = vstate;
    im.stats.artifacts_admitted.fetch_add(1, std::memory_order_relaxed);
    emitted.push_back({EventType::ARTIFACT_VALIDATED, {{"key", lk}, {"gen", std::to_string(newgen)}}});
    emitted.push_back({EventType::ARTIFACT_ADMITTED, {{"key", lk}}});
  }
  for (const auto& ev : emitted) im.events.emit(ev.first, ev.second);
  return res;
}

IngestResult ModelCache::ingest_file(const ArtifactId& id, const IngestSpec& spec,
                                     const std::string& path) {
  const auto data = platform::read_file(path, impl_->config.limits.max_artifact_size);
  return ingest(id, spec, data.data(), data.size());
}

IngestResult ModelCache::ingest_stream(const ArtifactId& id, const IngestSpec& spec,
                                       StreamReadFn reader) {
  std::vector<std::uint8_t> buf;
  buf.reserve(1 << 20);
  std::uint8_t chunk[256 * 1024];
  while (true) {
    const auto n = reader(chunk, sizeof(chunk));
    if (n == 0) break;
    if (buf.size() + n > impl_->config.limits.max_artifact_size)
      fail(ErrorCode::capacity_exceeded, "stream exceeds max_artifact_size");
    buf.insert(buf.end(), chunk, chunk + n);
  }
  return ingest(id, spec, buf.data(), buf.size());
}
// ---------------------------------------------------------------------------
// capacity / eviction helpers
// ---------------------------------------------------------------------------
void ModelCache::release_backing(Impl& im, const ArtifactRecord& rec) {
  for (const auto& g : rec.generations) {
    if (!g.backing_key) continue;
    Digest d;
    try { d = Digest::from_hex(*g.backing_key); } catch (...) { continue; }
    auto it = im.backings.find(d);
    if (it == im.backings.end()) continue;
    if (it->second.refcount > 0) --it->second.refcount;
    if (it->second.refcount == 0) {
      im.storage.remove_object(d);
      im.backings.erase(it);
    }
  }
}

void ModelCache::drop_record(Impl& im, const std::string& lk) {
  auto it = im.artifacts.find(lk);
  if (it == im.artifacts.end()) return;
  release_backing(im, it->second);
  im.graph.remove_node(lk);
  auto [ns, name] = split_logical(lk);
  im.storage.remove_record(ns, name);
  im.artifacts.erase(it);
  im.recompute_accounting();
}

bool ModelCache::ensure_capacity(Impl& im, const Digest& d, std::uint64_t size,
                                    const std::string& write_lk) {
  const auto& lim = im.config.limits;
  auto bit = im.backings.find(d);
  const std::uint64_t extra_phys =
      (bit == im.backings.end() || bit->second.refcount == 0) ? size : 0;
  const bool fits = (im.physical_bytes + extra_phys <= lim.max_total_bytes) &&
                    (im.logical_count + 1 <= lim.max_object_count);
  if (fits) return true;

  std::set<std::string> protected_nodes;
  for (const auto& kv : im.artifacts)
    for (const auto& n : im.graph.ancestors(kv.first)) protected_nodes.insert(n);

  std::vector<std::pair<std::uint64_t, std::string>> candidates;
  for (const auto& kv : im.artifacts) {
    const auto& rec = kv.second;
    if (rec.pinned) continue;
    if (kv.first == write_lk) continue;
    if (protected_nodes.count(kv.first)) continue;
    candidates.push_back({rec.updated_at, kv.first});
  }
  std::sort(candidates.begin(), candidates.end());

  for (const auto& [t, key] : candidates) {
    (void)t;
    drop_record(im, key);
    im.stats.artifacts_evicted.fetch_add(1, std::memory_order_relaxed);
    im.events.emit(EventType::ARTIFACT_EVICTED, {{"key", key}});
    const bool ok = (im.physical_bytes + extra_phys <= lim.max_total_bytes) &&
                    (im.logical_count + 1 <= lim.max_object_count);
    if (ok) return true;
  }
  return false;
}

const ArtifactRecord* ModelCache::lookup_record(const Impl& im, const ArtifactId& id) const {
  auto lk = logical_key(id.ns.str(), id.name);
  auto it = im.artifacts.find(lk);
  if (it == im.artifacts.end()) return nullptr;
  if (id.content_pin && !it->second.find_generation(*id.content_pin)) return nullptr;
  return &it->second;
}

// ---------------------------------------------------------------------------
// lookup
// ---------------------------------------------------------------------------
bool ModelCache::exists(const ArtifactId& id) const {
  std::shared_lock<std::shared_mutex> g(impl_->mu);
  return lookup_record(*impl_, id) != nullptr;
}

LookupResult ModelCache::lookup(const LookupRequest& req) const {
  auto& im = *impl_;
  LookupResult out;
  std::vector<std::pair<EventType, std::map<std::string, std::string>>> hitmiss;
  {
    std::shared_lock<std::shared_mutex> g(im.mu);
    im.stats.lookups.fetch_add(1, std::memory_order_relaxed);
    const auto t0 = std::chrono::steady_clock::now();

    if (req.id) {
      const auto* rec = lookup_record(im, *req.id);
      if (!rec) {
        im.stats.misses.fetch_add(1, std::memory_order_relaxed);
        hitmiss.push_back({EventType::CACHE_MISS, {{"key", req.id->canonical()}}});
      } else {
        const GenerationRecord* gen = req.id->content_pin ? rec->find_generation(*req.id->content_pin)
                                                          : rec->current();
        if (gen) {
          out.found = true; out.artifact_key = logical_key(rec->ns, rec->name);
          out.validation = gen->validation; out.generation = gen->generation;
          out.size = gen->size;
          out.content_digest = gen->content_digest;
          if (gen->backing_key) out.backing_location = im.storage.object_path(gen->content_digest);
          out.dependencies = rec->dependencies;
          out.valid = (gen->validation == ValidationState::VALID);
          im.stats.hits.fetch_add(1, std::memory_order_relaxed);
          hitmiss.push_back({EventType::CACHE_HIT, {{"key", out.artifact_key}}});
        } else {
          im.stats.misses.fetch_add(1, std::memory_order_relaxed);
          hitmiss.push_back({EventType::CACHE_MISS, {{"key", req.id->canonical()}}});
        }
      }
      const auto t1 = std::chrono::steady_clock::now();
      im.stats.record_lookup_latency(static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
    } else {
      // model/revision + type + role + compatibility scan
      std::vector<std::pair<const ArtifactRecord*, const GenerationRecord*>> hits;
      for (const auto& kv : im.artifacts) {
        const auto& rec = kv.second;
        if (req.type && rec.type != *req.type) continue;
        if (req.role && !(rec.role == *req.role)) continue;
        if (req.model) {
          auto m = rec.provenance.find("model");
          if (m == rec.provenance.end() || m->second != req.model->str()) continue;
        }
        if (req.revision) {
          auto rv = rec.provenance.find("revision");
          if (rv == rec.provenance.end() || rv->second != req.revision->str()) continue;
        }
        if (!CompatibilityKey::compatible(req.compatibility, rec.compatibility)) continue;
        const auto* gen = rec.current();
        if (!gen) continue;
        if (req.require_valid && gen->validation != ValidationState::VALID) continue;
        hits.push_back({&rec, gen});
      }
      std::sort(hits.begin(), hits.end(), [](const auto& x, const auto& y) {
        if (x.first->pinned != y.first->pinned) return x.first->pinned;
        if (x.first->updated_at != y.first->updated_at) return x.first->updated_at < y.first->updated_at;
        return logical_key(x.first->ns, x.first->name) < logical_key(y.first->ns, y.first->name);
      });
      if (!hits.empty()) {
        const auto* rec = hits.front().first;
        const auto* gen = hits.front().second;
        out.found = true; out.valid = true;
        out.artifact_key = logical_key(rec->ns, rec->name);
        out.validation = gen->validation; out.generation = gen->generation; out.size = gen->size;
        out.content_digest = gen->content_digest;
        if (gen->backing_key) out.backing_location = im.storage.object_path(gen->content_digest);
        out.dependencies = rec->dependencies;
        im.stats.compatible_hits.fetch_add(1, std::memory_order_relaxed);
        im.stats.hits.fetch_add(1, std::memory_order_relaxed);
        hitmiss.push_back({EventType::CACHE_HIT, {{"key", out.artifact_key}}});
        for (std::size_t i = 1; i < hits.size(); ++i)
          out.variants.push_back(logical_key(hits[i].first->ns, hits[i].first->name));
      } else {
        im.stats.misses.fetch_add(1, std::memory_order_relaxed);
        hitmiss.push_back({EventType::CACHE_MISS, {{"key", std::string("?")}}});
      }
      const auto t1 = std::chrono::steady_clock::now();
      im.stats.record_lookup_latency(static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
    }
  }
  for (const auto& ev : hitmiss) im.events.emit(ev.first, ev.second);
  return out;
}

std::optional<ArtifactRecord> ModelCache::record(const ArtifactId& id) const {
  std::shared_lock<std::shared_mutex> g(impl_->mu);
  auto it = impl_->artifacts.find(logical_key(id.ns.str(), id.name));
  if (it == impl_->artifacts.end()) return std::nullopt;
  if (id.content_pin && !it->second.find_generation(*id.content_pin)) return std::nullopt;
  return it->second;
}

std::vector<std::string> ModelCache::list(const std::optional<ArtifactType>& type) const {
  std::shared_lock<std::shared_mutex> g(impl_->mu);
  std::vector<std::string> out;
  for (const auto& kv : impl_->artifacts) {
    if (type && kv.second.type != *type) continue;
    out.push_back(kv.first);
  }
  std::sort(out.begin(), out.end());
  return out;
}
// ---------------------------------------------------------------------------
// reuse planning
// ---------------------------------------------------------------------------
ReusePlan ModelCache::reuse_plan(const ReuseRequest& req) const {
  auto& im = *impl_;
  ReusePlan plan;
  std::vector<std::pair<const ArtifactRecord*, const GenerationRecord*>> candidates;
  {
    std::shared_lock<std::shared_mutex> g(im.mu);
    for (const auto& kv : im.artifacts) {
      const auto& rec = kv.second;
      if (rec.type != req.type) continue;
      if (!(rec.role == req.role)) continue;
      if (req.version && !(rec.version == *req.version)) continue;
      if (req.format && !(rec.format && rec.format == *req.format)) continue;
      if (req.architecture && !(rec.architecture && rec.architecture == *req.architecture)) continue;
      if (!CompatibilityKey::compatible(req.compatibility, rec.compatibility)) continue;
      if (req.model) {
        auto m = rec.provenance.find("model");
        if (m == rec.provenance.end() || m->second != req.model->str()) continue;
      }
      if (req.revision) {
        auto rv = rec.provenance.find("revision");
        if (rv == rec.provenance.end() || rv->second != req.revision->str()) continue;
      }
      const auto* gen = rec.current();
      if (!gen) continue;
      candidates.push_back({&rec, gen});
      plan.candidates.push_back(logical_key(rec.ns, rec.name));
    }
  }
  std::sort(plan.candidates.begin(), plan.candidates.end());
  if (candidates.empty()) {
    plan.action = ReuseAction::FETCH_REQUIRED;
    plan.compatibility_reasoning = "no candidate matched type/role/compatibility";
    return plan;
  }
  std::sort(candidates.begin(), candidates.end(), [](const auto& x, const auto& y) {
    if (x.first->pinned != y.first->pinned) return x.first->pinned;
    if (x.first->updated_at != y.first->updated_at) return x.first->updated_at < y.first->updated_at;
    return logical_key(x.first->ns, x.first->name) < logical_key(y.first->ns, y.first->name);
  });
  const auto* rec = candidates.front().first;
  const auto* gen = candidates.front().second;
  plan.compatibility_reasoning = "compatibility key matched";
  plan.validation = gen->validation;
  plan.generation = gen->generation;
  plan.size = gen->size;
  plan.content_digest = gen->content_digest;
  plan.dependencies = rec->dependencies;
  plan.bytes_reusable = gen->size;

  if (gen->validation == ValidationState::VALID) {
    plan.action = ReuseAction::REUSE_LOCAL;
    plan.artifact_key = logical_key(rec->ns, rec->name);
    if (gen->backing_key) plan.backing_location = impl_->storage.object_path(gen->content_digest);
    impl_->stats.bytes_reused.fetch_add(gen->size, std::memory_order_relaxed);
    impl_->events.emit(EventType::ARTIFACT_REUSED, {{"key", *plan.artifact_key}});
  } else if (gen->validation == ValidationState::QUARANTINED ||
             gen->validation == ValidationState::INVALID) {
    plan.action = ReuseAction::REJECT_CORRUPT;
    plan.artifact_key = logical_key(rec->ns, rec->name);
    plan.rejected.push_back({*plan.artifact_key, std::string("validation state ") + to_string(gen->validation)});
  } else {
    plan.action = ReuseAction::REVALIDATE_REQUIRED;
    plan.artifact_key = logical_key(rec->ns, rec->name);
  }
  return plan;
}

std::vector<std::string> ModelCache::variants(const ArtifactId& id) const {
  std::shared_lock<std::shared_mutex> g(impl_->mu);
  const auto* target = lookup_record(*impl_, id);
  if (!target) return {};
  std::vector<std::string> out;
  for (const auto& kv : impl_->artifacts) {
    const auto& rec = kv.second;
    if (rec.ns != target->ns) continue;
    if (rec.name == target->name) continue;
    if (rec.role != target->role && rec.type != target->type) continue;
    const auto* gen = rec.current();
    if (!gen) continue;
    if (gen->validation != ValidationState::VALID) continue;
    out.push_back(logical_key(rec.ns, rec.name));
  }
  std::sort(out.begin(), out.end());
  return out;
}

// ---------------------------------------------------------------------------
// manifests
// ---------------------------------------------------------------------------
bool ModelCache::key_exists(const std::string& key) const {
  auto hash = key.find('#');
  std::string base = hash == std::string::npos ? key : key.substr(0, hash);
  auto slash = base.find('/');
  if (slash == std::string::npos) return false;
  const std::string ns = base.substr(0, slash), name = base.substr(slash + 1);
  auto it = impl_->artifacts.find(logical_key(ns, name));
  if (it == impl_->artifacts.end()) return false;
  if (hash != std::string::npos) {
    Digest pin = Digest::from_hex(key.substr(hash + 1));
    if (!it->second.find_generation(pin)) return false;
  }
  return true;
}

void ModelCache::manifest_add(const Manifest& m) {
  std::vector<std::pair<EventType, std::map<std::string, std::string>>> emitted;
  const std::string ck = "manifest:" + m.model.str() + "/" + m.revision.str();
  {
    std::unique_lock<std::shared_mutex> g(impl_->mu);
    auto valid = ValidationState::VALID;
    for (const auto& a : m.artifacts) if (!key_exists(a)) { valid = ValidationState::INVALID; break; }
    Manifest mm = m;
    mm.validation = valid;
    impl_->manifests[ck] = mm;
    impl_->storage.save_manifest(mm);
    emitted.push_back({EventType::MANIFEST_ADDED, {{"key", ck}}});
  }
  for (const auto& ev : emitted) impl_->events.emit(ev.first, ev.second);
}

std::optional<Manifest> ModelCache::manifest_get(const ModelIdentity& model,
                                                 const ModelRevision& revision) const {
  std::shared_lock<std::shared_mutex> g(impl_->mu);
  auto it = impl_->manifests.find("manifest:" + model.str() + "/" + revision.str());
  if (it == impl_->manifests.end()) return std::nullopt;
  return it->second;
}

std::vector<std::string> ModelCache::manifest_list() const {
  std::shared_lock<std::shared_mutex> g(impl_->mu);
  std::vector<std::string> out;
  for (const auto& kv : impl_->manifests) out.push_back(kv.first);
  std::sort(out.begin(), out.end());
  return out;
}

void ModelCache::manifest_invalidate(const ModelIdentity& model, const ModelRevision& revision) {
  std::vector<std::pair<EventType, std::map<std::string, std::string>>> emitted;
  {
    std::unique_lock<std::shared_mutex> g(impl_->mu);
    auto it = impl_->manifests.find("manifest:" + model.str() + "/" + revision.str());
    if (it == impl_->manifests.end()) fail(ErrorCode::not_found, "manifest not found");
    it->second.validation = ValidationState::INVALID;
    impl_->storage.save_manifest(it->second);
    emitted.push_back({EventType::MANIFEST_INVALIDATED, {{"key", it->first}}});
  }
  for (const auto& ev : emitted) impl_->events.emit(ev.first, ev.second);
}
// ---------------------------------------------------------------------------
// lifetime / validation operations
// ---------------------------------------------------------------------------
void ModelCache::validate(const ArtifactId& id, bool force) {
  auto& im = *impl_;
  std::vector<std::pair<EventType, std::map<std::string, std::string>>> emitted;
  {
    std::unique_lock<std::shared_mutex> wg(im.mu);
    auto it = im.artifacts.find(logical_key(id.ns.str(), id.name));
    if (it == im.artifacts.end()) fail(ErrorCode::not_found, "artifact not found");
    auto* gen = id.content_pin ? it->second.find_generation(*id.content_pin) : it->second.current();
    if (!gen) fail(ErrorCode::not_found, "generation not found");
    if (force || gen->validation != ValidationState::VALID) {
      if (gen->backing_key) {
        const bool ok = im.storage.verify_object(gen->content_digest);
        gen->validation = ok ? ValidationState::VALID : ValidationState::QUARANTINED;
        if (!ok) {
          im.storage.quarantine_object(gen->content_digest, "validation_failed");
          im.stats.corruption_detections.fetch_add(1, std::memory_order_relaxed);
          im.stats.quarantine_events.fetch_add(1, std::memory_order_relaxed);
          emitted.push_back({EventType::CORRUPTION_DETECTED, {{"key", it->first}, {"reason", "validation"}}});
          emitted.push_back({EventType::ARTIFACT_QUARANTINED, {{"key", it->first}}});
        }
      } else if (gen->validation != ValidationState::QUARANTINED &&
                 gen->validation != ValidationState::RETIRED) {
        gen->validation = ValidationState::VALID;
      }
    }
    it->second.updated_at = platform::now_millis();
    im.storage.save_record(it->second);
    emitted.push_back({EventType::ARTIFACT_VALIDATED, {{"key", it->first}, {"gen", std::to_string(gen->generation)}}});
  }
  for (const auto& ev : emitted) im.events.emit(ev.first, ev.second);
}

void ModelCache::quarantine(const ArtifactId& id, const std::string& reason) {
  auto& im = *impl_;
  std::vector<std::pair<EventType, std::map<std::string, std::string>>> emitted;
  {
    std::unique_lock<std::shared_mutex> wg(im.mu);
    auto it = im.artifacts.find(logical_key(id.ns.str(), id.name));
    if (it == im.artifacts.end()) fail(ErrorCode::not_found, "artifact not found");
    auto* gen = id.content_pin ? it->second.find_generation(*id.content_pin) : it->second.current();
    if (!gen) fail(ErrorCode::not_found, "generation not found");
    gen->validation = ValidationState::QUARANTINED;
    if (gen->backing_key) im.storage.quarantine_object(gen->content_digest, reason);
    it->second.updated_at = platform::now_millis();
    im.storage.save_record(it->second);
    im.stats.quarantine_events.fetch_add(1, std::memory_order_relaxed);
    emitted.push_back({EventType::ARTIFACT_QUARANTINED, {{"key", it->first}, {"reason", reason}}});
    emitted.push_back({EventType::CORRUPTION_DETECTED, {{"key", it->first}, {"reason", reason}}});
  }
  for (const auto& ev : emitted) im.events.emit(ev.first, ev.second);
}

void ModelCache::purge(const ArtifactId& id) {
  auto& im = *impl_;
  std::vector<std::pair<EventType, std::map<std::string, std::string>>> emitted;
  {
    std::unique_lock<std::shared_mutex> wg(im.mu);
    const auto lk = logical_key(id.ns.str(), id.name);
    if (im.artifacts.count(lk) == 0) fail(ErrorCode::not_found, "artifact not found");
    drop_record(im, lk);
    emitted.push_back({EventType::ARTIFACT_RETIRED, {{"key", lk}}});
  }
  for (const auto& ev : emitted) im.events.emit(ev.first, ev.second);
}

void ModelCache::retire(const ArtifactId& id) {
  auto& im = *impl_;
  std::vector<std::pair<EventType, std::map<std::string, std::string>>> emitted;
  {
    std::unique_lock<std::shared_mutex> wg(im.mu);
    auto it = im.artifacts.find(logical_key(id.ns.str(), id.name));
    if (it == im.artifacts.end()) fail(ErrorCode::not_found, "artifact not found");
    auto* gen = id.content_pin ? it->second.find_generation(*id.content_pin) : it->second.current();
    if (!gen) fail(ErrorCode::not_found, "generation not found");
    gen->validation = ValidationState::RETIRED;
    it->second.updated_at = platform::now_millis();
    im.storage.save_record(it->second);
    emitted.push_back({EventType::ARTIFACT_RETIRED, {{"key", it->first}}});
  }
  for (const auto& ev : emitted) im.events.emit(ev.first, ev.second);
}

void ModelCache::supersede(const ArtifactId& id, const std::string& suffix) {
  auto& im = *impl_;
  std::vector<std::pair<EventType, std::map<std::string, std::string>>> emitted;
  {
    std::unique_lock<std::shared_mutex> wg(im.mu);
    auto it = im.artifacts.find(logical_key(id.ns.str(), id.name));
    if (it == im.artifacts.end()) fail(ErrorCode::not_found, "artifact not found");
    auto* cur = it->second.current();
    if (!cur) fail(ErrorCode::not_found, "no current generation");
    if (!suffix.empty()) {
      if (suffix.find('/') != std::string::npos || suffix.find('\\') != std::string::npos)
        fail(ErrorCode::invalid_argument, "supersede suffix must not contain path separators");
      const std::string lk2 = logical_key(id.ns.str(), id.name + suffix);
      if (im.artifacts.count(lk2)) fail(ErrorCode::already_exists, "supersede target already exists");
      ArtifactRecord rec2 = it->second;
      rec2.name = id.name + suffix;
      rec2.current_generation = 1;
      rec2.generations.clear();
      const std::uint64_t ts = platform::now_millis();
      rec2.created_at = ts; rec2.updated_at = ts;
      GenerationRecord g = *cur;
      g.generation = 1; g.current = true; g.created_at = ts;
      rec2.generations.push_back(std::move(g));
      im.storage.save_record(rec2);
      im.artifacts[lk2] = std::move(rec2);
      if (cur->backing_key) {
        Digest d = Digest::from_hex(*cur->backing_key);
        im.backings[d].refcount += 1;
        im.backings[d].bytes = cur->size;
        if (cur->validation == ValidationState::VALID) im.backings[d].validation = ValidationState::VALID;
      }
      for (auto& g2 : it->second.generations) g2.current = false;
      im.storage.save_record(it->second);
      im.graph.set_dependencies(lk2, it->second.dependencies);
      emitted.push_back({EventType::ARTIFACT_SUPERSEDED, {{"key", it->first}, {"new", lk2}}});
    } else {
      ArtifactRecord updated = it->second;
      const std::uint64_t newgen = updated.current_generation + 1;
      for (auto& g2 : updated.generations) g2.current = false;
      GenerationRecord g = *cur;
      g.generation = newgen; g.current = true;
      const std::uint64_t ts = platform::now_millis();
      g.created_at = ts;
      updated.generations.push_back(std::move(g));
      updated.current_generation = newgen;
      updated.updated_at = ts;
      if (cur->backing_key) {
        Digest d = Digest::from_hex(*cur->backing_key);
        im.backings[d].refcount += 1;
        im.backings[d].bytes = cur->size;
        if (cur->validation == ValidationState::VALID) im.backings[d].validation = ValidationState::VALID;
      }
      im.storage.save_record(updated);
      it->second = std::move(updated);
      emitted.push_back({EventType::ARTIFACT_SUPERSEDED, {{"key", it->first}, {"gen", std::to_string(newgen)}}});
    }
    im.recompute_accounting();
  }
  for (const auto& ev : emitted) im.events.emit(ev.first, ev.second);
}

std::uint64_t ModelCache::generations(const ArtifactId& id) const {
  std::shared_lock<std::shared_mutex> g(impl_->mu);
  auto it = impl_->artifacts.find(logical_key(id.ns.str(), id.name));
  if (it == impl_->artifacts.end()) return 0;
  return it->second.generations.size();
}
// ---------------------------------------------------------------------------
// aliases
// ---------------------------------------------------------------------------
void ModelCache::alias_set(const std::string& alias, const ArtifactId& target) {
  auto& im = *impl_;
  if (alias.empty()) fail(ErrorCode::invalid_argument, "alias must be non-empty");
  if (alias.find('\n') != std::string::npos || alias.find('\0') != std::string::npos)
    fail(ErrorCode::invalid_argument, "alias contains forbidden character");
  std::vector<std::pair<EventType, std::map<std::string, std::string>>> emitted;
  {
    std::unique_lock<std::shared_mutex> wg(im.mu);
    const auto tgt = target.canonical();
    im.aliases[alias] = tgt;
    im.storage.save_alias(alias, tgt);
    emitted.push_back({EventType::ALIAS_UPDATED, {{"alias", alias}, {"target", tgt}}});
  }
  for (const auto& ev : emitted) im.events.emit(ev.first, ev.second);
}

std::optional<std::string> ModelCache::alias_get(const std::string& alias) const {
  std::shared_lock<std::shared_mutex> g(impl_->mu);
  auto it = impl_->aliases.find(alias);
  if (it == impl_->aliases.end()) return std::nullopt;
  return it->second;
}

std::vector<std::string> ModelCache::alias_list() const {
  std::shared_lock<std::shared_mutex> g(impl_->mu);
  std::vector<std::string> out;
  for (const auto& kv : impl_->aliases) out.push_back(kv.first);
  std::sort(out.begin(), out.end());
  return out;
}

// ---------------------------------------------------------------------------
// pinning
// ---------------------------------------------------------------------------
void ModelCache::pin(const ArtifactId& id) {
  std::unique_lock<std::shared_mutex> g(impl_->mu);
  auto it = impl_->artifacts.find(logical_key(id.ns.str(), id.name));
  if (it == impl_->artifacts.end()) fail(ErrorCode::not_found, "artifact not found");
  it->second.pinned = true;
  impl_->storage.save_record(it->second);
}

void ModelCache::unpin(const ArtifactId& id) {
  std::unique_lock<std::shared_mutex> g(impl_->mu);
  auto it = impl_->artifacts.find(logical_key(id.ns.str(), id.name));
  if (it == impl_->artifacts.end()) fail(ErrorCode::not_found, "artifact not found");
  it->second.pinned = false;
  impl_->storage.save_record(it->second);
}

bool ModelCache::is_pinned(const ArtifactId& id) const {
  std::shared_lock<std::shared_mutex> g(impl_->mu);
  auto it = impl_->artifacts.find(logical_key(id.ns.str(), id.name));
  if (it == impl_->artifacts.end()) return false;
  return it->second.pinned;
}

// ---------------------------------------------------------------------------
// dependency graph queries
// ---------------------------------------------------------------------------
std::vector<std::string> ModelCache::dependencies(const ArtifactId& id) const {
  std::shared_lock<std::shared_mutex> g(impl_->mu);
  return impl_->graph.dependencies(node_key(id));
}

std::vector<std::string> ModelCache::dependents(const ArtifactId& id) const {
  std::shared_lock<std::shared_mutex> g(impl_->mu);
  return impl_->graph.dependents(node_key(id));
}

std::vector<std::string> ModelCache::ancestors(const ArtifactId& id) const {
  std::shared_lock<std::shared_mutex> g(impl_->mu);
  std::vector<std::string> out;
  for (const auto& a : impl_->graph.ancestors(node_key(id))) out.push_back(a);
  return out;
}

// ---------------------------------------------------------------------------
// explainability
// ---------------------------------------------------------------------------
ExplainResult ModelCache::explain(const LookupRequest& req) const {
  auto& im = *impl_;
  ExplainResult r;
  r.requested_identity = req.id ? req.id->canonical() : std::string("*");
  r.compatibility_key = req.compatibility.canonical();
  r.cache_budget_bytes = im.config.limits.max_total_bytes;
  r.physical_bytes = im.physical_bytes;
  r.dependencies = req.id ? im.graph.dependencies(node_key(*req.id)) : std::vector<std::string>{};

  std::shared_lock<std::shared_mutex> g(im.mu);
  for (const auto& kv : im.artifacts) {
    const auto& rec = kv.second;
    const auto* gen = rec.current();
    if (!gen) continue;
    r.candidates_evaluated.push_back(kv.first);
    if (req.type && rec.type != *req.type) {
      r.candidates_rejected.push_back({kv.first, "type mismatch"}); continue;
    }
    if (req.role && !(rec.role == *req.role)) {
      r.candidates_rejected.push_back({kv.first, "role mismatch"}); continue;
    }
    if (!CompatibilityKey::compatible(req.compatibility, rec.compatibility)) {
      r.candidates_rejected.push_back({kv.first, "incompatible"}); continue;
    }
    if (req.require_valid && gen->validation != ValidationState::VALID) {
      r.candidates_rejected.push_back({kv.first, std::string("validation ") + to_string(gen->validation)}); continue;
    }
    if (r.selected_artifact) continue;
    r.selected_artifact = kv.first;
    r.validation = gen->validation;
    r.generation = gen->generation;
    r.expected_reuse_bytes = gen->size;
    r.dedup = gen->backing_key ? (im.backings.count(gen->content_digest) > 0) : false;
  }
  if (r.selected_artifact) r.selected_action = ReuseAction::REUSE_LOCAL;
  else if (req.type || req.role || !req.compatibility.empty()) r.selected_action = ReuseAction::REJECT_INCOMPATIBLE;
  else r.selected_action = ReuseAction::FETCH_REQUIRED;
  return r;
}

ExplainResult ModelCache::explain_reuse(const ReuseRequest& req) const {
  LookupRequest lr;
  lr.type = req.type; lr.role = req.role; lr.version = req.version;
  lr.format = req.format; lr.architecture = req.architecture;
  lr.compatibility = req.compatibility; lr.model = req.model; lr.revision = req.revision;
  lr.require_valid = req.allow_valid_only;
  auto r = explain(lr);
  r.selected_action = reuse_plan(req).action;
  return r;
}
// ---------------------------------------------------------------------------
// observability
// ---------------------------------------------------------------------------
Stats& ModelCache::stats() noexcept { return impl_->stats; }
EventBus& ModelCache::events() noexcept { return impl_->events; }

Snapshot ModelCache::snapshot() const {
  std::shared_lock<std::shared_mutex> g(impl_->mu);
  Snapshot s;
  s.cache_generation = impl_->cache_generation;
  s.logical_artifact_count = impl_->artifacts.size();
  std::uint64_t backing_count = 0;
  for (const auto& b : impl_->backings) if (b.second.refcount > 0) ++backing_count;
  s.physical_backing_count = backing_count;
  s.total_logical_bytes = impl_->logical_bytes;
  s.total_physical_bytes = impl_->physical_bytes;
  std::uint64_t dedup = 0;
  for (const auto& b : impl_->backings) if (b.second.refcount > 1) dedup += (b.second.refcount - 1) * b.second.bytes;
  s.dedup_bytes_saved = dedup;
  for (const auto& kv : impl_->artifacts) {
    const auto* gen = kv.second.current();
    if (!gen) continue;
    if (gen->validation == ValidationState::VALID) ++s.valid_count;
    else if (gen->validation == ValidationState::INVALID) ++s.invalid_count;
    else if (gen->validation == ValidationState::QUARANTINED) ++s.quarantined_count;
    if (kv.second.pinned) ++s.pinned_count;
  }
  s.manifest_count = impl_->manifests.size();
  s.alias_count = impl_->aliases.size();
  std::uint64_t edges = 0;
  for (const auto& n : impl_->graph.nodes()) edges += impl_->graph.dependencies(n).size();
  s.dependency_edge_count = edges;
  s.capacity_bytes = impl_->config.limits.max_total_bytes;
  s.lookups = impl_->stats.lookups.load(std::memory_order_relaxed);
  s.hits = impl_->stats.hits.load(std::memory_order_relaxed);
  s.misses = impl_->stats.misses.load(std::memory_order_relaxed);
  s.evicted = impl_->stats.artifacts_evicted.load(std::memory_order_relaxed);
  return s;
}

// ---------------------------------------------------------------------------
// maintenance
// ---------------------------------------------------------------------------
std::size_t ModelCache::gc() {
  auto& im = *impl_;
  std::unique_lock<std::shared_mutex> wg(im.mu);
  std::vector<Digest> objects;
  im.storage.list_object_digests(objects);
  std::size_t removed = 0;
  for (const auto& d : objects) {
    auto it = im.backings.find(d);
    const bool referenced = (it != im.backings.end() && it->second.refcount > 0);
    if (!referenced) { im.storage.remove_object(d); ++removed; }
  }
  im.recompute_accounting();
  return removed;
}

std::vector<EvictionRecord> ModelCache::evict(std::optional<std::uint64_t> max_bytes) {
  auto& im = *impl_;
  std::vector<std::pair<EventType, std::map<std::string, std::string>>> emitted;
  std::vector<EvictionRecord> out;
  {
    std::unique_lock<std::shared_mutex> wg(im.mu);
    std::set<std::string> protected_nodes;
    for (const auto& kv : im.artifacts)
      for (const auto& n : im.graph.ancestors(kv.first)) protected_nodes.insert(n);
    std::vector<std::pair<std::uint64_t, std::string>> candidates;
    for (const auto& kv : im.artifacts) {
      if (kv.second.pinned) continue;
      if (protected_nodes.count(kv.first)) continue;
      candidates.push_back({kv.second.updated_at, kv.first});
    }
    std::sort(candidates.begin(), candidates.end());
    std::uint64_t freed = 0;
    for (const auto& [t, key] : candidates) {
      (void)t;
      auto it = im.artifacts.find(key);
      if (it == im.artifacts.end()) continue;
      EvictionRecord er;
      er.artifact_key = key;
      const auto* gen = it->second.current();
      if (gen) { er.digest = gen->content_digest; er.bytes = gen->size; er.generation = gen->generation; }
      const bool shared = gen && gen->backing_key
          ? (im.backings.count(gen->content_digest) && im.backings[gen->content_digest].refcount > 1)
          : false;
      drop_record(im, key);
      er.freed_backing = !shared;
      er.policy_reason = "eviction";
      out.push_back(er);
      freed += er.bytes;
      im.stats.artifacts_evicted.fetch_add(1, std::memory_order_relaxed);
      emitted.push_back({EventType::ARTIFACT_EVICTED, {{"key", key}}});
      if (max_bytes && freed >= *max_bytes) break;
    }
  }
  for (const auto& e : emitted) im.events.emit(e.first, e.second);
  return out;
}

std::size_t ModelCache::verify() const {
  std::shared_lock<std::shared_mutex> g(impl_->mu);
  std::vector<Digest> objects;
  impl_->storage.list_object_digests(objects);
  std::size_t corrupt = 0;
  for (const auto& d : objects) if (!impl_->storage.verify_object(d)) ++corrupt;
  return corrupt;
}

void ModelCache::save() {
  std::unique_lock<std::shared_mutex> wg(impl_->mu);
  for (const auto& kv : impl_->artifacts) impl_->storage.save_record(kv.second);
  for (const auto& kv : impl_->manifests) impl_->storage.save_manifest(kv.second);
  for (const auto& kv : impl_->aliases) impl_->storage.save_alias(kv.first, kv.second);
  ++impl_->cache_generation;
}

void ModelCache::restore() {
  {
    std::unique_lock<std::shared_mutex> wg(impl_->mu);
    impl_->artifacts.clear();
    impl_->manifests.clear();
    impl_->aliases.clear();
    impl_->backings.clear();
    impl_->graph.clear();
    ++impl_->cache_generation;
  }
  impl_->load_state();
}

}  // namespace model_cache
