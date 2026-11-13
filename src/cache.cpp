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

// The dependency graph is keyed by logical artifact ("ns/name"). A dependency
// written with a content pin ("ns/name#hex") is a dependency on that artifact.
std::string graph_key(const std::string& key) { return key.substr(0, key.find('#')); }
std::vector<std::string> graph_keys(const std::vector<std::string>& keys) {
  std::vector<std::string> out;
  out.reserve(keys.size());
  for (const auto& k : keys) out.push_back(graph_key(k));
  return out;
}
std::string node_key(const ArtifactId& id) { return logical_key(id.ns.str(), id.name); }

// The attributes a request may constrain, checked the same way everywhere.
template <typename Req>
const char* mismatch(const ArtifactRecord& rec, const Req& req, const std::optional<ArtifactType>& type,
                     const std::optional<ArtifactRole>& role) {
  if (type && rec.type != *type) return "type mismatch";
  if (role && !(rec.role == *role)) return "role mismatch";
  if (req.version && !(rec.version == *req.version)) return "version mismatch";
  if (req.format && !(rec.format && *rec.format == *req.format)) return "format mismatch";
  if (req.architecture && !(rec.architecture && *rec.architecture == *req.architecture)) return "architecture mismatch";
  if (req.model) {
    auto m = rec.provenance.find("model");
    if (m == rec.provenance.end() || m->second != req.model->str()) return "model mismatch";
  }
  if (req.revision) {
    auto rv = rec.provenance.find("revision");
    if (rv == rec.provenance.end() || rv->second != req.revision->str()) return "revision mismatch";
  }
  if (!CompatibilityKey::compatible(req.compatibility, rec.compatibility)) return "incompatible";
  return nullptr;
}

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

  using PendingEvent = std::pair<EventType, std::map<std::string, std::string>>;
  // Events raised while the state lock is held. The caller takes them and emits
  // them only after it has released the lock.
  std::vector<PendingEvent> deferred;
  std::vector<PendingEvent> take_deferred() {
    std::vector<PendingEvent> out;
    out.swap(deferred);
    return out;
  }

  // Artifacts that some other artifact depends on, by logical key.
  std::set<std::string> protected_keys() const {
    std::set<std::string> out;
    for (const auto& kv : artifacts)
      for (const auto& n : graph.ancestors(kv.first)) out.insert(n);
    return out;
  }

  std::uint64_t dependency_edges() const {
    std::uint64_t edges = 0;
    for (const auto& n : graph.nodes()) edges += graph.dependencies(n).size();
    return edges;
  }

  // Every generation that references a backing shares its fate: once the bytes
  // are quarantined, none of them may go on saying VALID.
  void quarantine_sharers(const Digest& d) {
    for (auto& kv : artifacts) {
      ArtifactRecord updated = kv.second;
      bool changed = false;
      for (auto& g : updated.generations) {
        if (!g.backing_key || !(g.content_digest == d)) continue;
        if (g.validation == ValidationState::QUARANTINED || g.validation == ValidationState::RETIRED) continue;
        g.validation = ValidationState::QUARANTINED;
        changed = true;
      }
      if (!changed) continue;
      storage.save_record(updated);
      kv.second = std::move(updated);
      deferred.push_back({EventType::ARTIFACT_QUARANTINED, {{"key", kv.first}, {"reason", "shared backing quarantined"}}});
    }
  }

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
      graph.set_dependencies(nk, graph_keys(rec.dependencies));
    }
    for (const auto& key : storage.list_manifest_keys()) {
      const std::string prefix("manifest:");
      if (key.compare(0, prefix.size(), prefix) != 0) continue;
      auto body = key.substr(prefix.size());
      // "model/revision", where either part may itself contain '/' ("org/model"):
      // the split that names the stored manifest is the one that loads it back.
      for (auto slash = body.find('/'); slash != std::string::npos; slash = body.find('/', slash + 1)) {
        auto m = storage.load_manifest(body.substr(0, slash), body.substr(slash + 1));
        if (m && m->canonical_key() == key) { manifests[key] = std::move(*m); break; }
      }
    }
    // A manifest is VALID only while every artifact it lists is present.
    for (auto& kv : manifests) {
      if (kv.second.validation != ValidationState::VALID) continue;
      for (const auto& a : kv.second.artifacts) {
        if (artifacts.count(graph_key(a)) != 0) continue;
        kv.second.validation = ValidationState::INVALID;
        try { storage.save_manifest(kv.second); } catch (...) { /* still invalid in memory */ }
        break;
      }
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

  // The caller's validator runs with no lock held.
  ValidationState vstate = ValidationState::UNKNOWN;
  if (spec.validation_mode == ValidationMode::External) {
    const bool ok = spec.external_validator ? spec.external_validator(computed, size) : false;
    vstate = ok ? ValidationState::VALID : ValidationState::QUARANTINED;
  } else {
    vstate = ValidationState::VALID;
  }

  // Everything below decides and commits under the state lock. Events are only
  // collected there; they are emitted after the lock is released.
  std::vector<Impl::PendingEvent> emitted;
  bool closed = false;
  {
    std::unique_lock<std::shared_mutex> wg(im.mu);
    auto referenced = [&]() {
      auto b = im.backings.find(computed);
      return b != im.backings.end() && b->second.refcount > 0;
    };
    // An object this call wrote and that nothing references is this call's to
    // take back. A backing that other artifacts share is never touched here.
    auto discard_own_object = [&](bool quarantine) {
      if (!isNew || referenced()) return;
      std::lock_guard<std::mutex> cg(im.cas_mu);
      if (quarantine) im.storage.quarantine_object(computed, "external_validation_failed");
      else im.storage.remove_object(computed);
    };
    auto reject = [&](const char* reason) {
      res.admitted = false; res.rejection_reason = reason;
      im.stats.artifacts_rejected.fetch_add(1, std::memory_order_relaxed);
    };

    if (im.closed) {
      discard_own_object(false);
      closed = true;
    } else if (vstate == ValidationState::QUARANTINED) {
      discard_own_object(true);
      reject("external validator rejected");
      res.validation = ValidationState::QUARANTINED;
      im.stats.corruption_detections.fetch_add(1, std::memory_order_relaxed);
      im.stats.quarantine_events.fetch_add(1, std::memory_order_relaxed);
      emitted.push_back({EventType::CORRUPTION_DETECTED, {{"key", lk}, {"reason", "validation"}}});
      emitted.push_back({EventType::ARTIFACT_QUARANTINED, {{"key", lk}}});
      emitted.push_back({EventType::ARTIFACT_REJECTED, {{"key", lk}, {"reason", res.rejection_reason}}});
    } else {
      // Declared dependencies must not close a cycle, and the graph is bounded.
      const std::vector<std::string> deps = graph_keys(spec.dependencies);
      bool cycle = false;
      for (const auto& d : deps)
        if (d == lk || im.graph.ancestors(d).count(lk) != 0) { cycle = true; break; }
      const std::uint64_t edges_after =
          im.dependency_edges() - im.graph.dependencies(lk).size() + deps.size();

      if (cycle) {
        discard_own_object(false);
        reject("dependency cycle");
        emitted.push_back({EventType::ARTIFACT_REJECTED, {{"key", lk}, {"reason", res.rejection_reason}}});
      } else if (edges_after > im.config.limits.max_dependency_edges) {
        discard_own_object(false);
        reject("dependency edge bound exceeded");
        emitted.push_back({EventType::ARTIFACT_REJECTED, {{"key", lk}, {"reason", res.rejection_reason}}});
      } else if (!ensure_capacity(im, computed, static_cast<std::uint64_t>(size), lk)) {
        discard_own_object(false);
        reject("capacity exceeded and non-evictable");
      } else {
        // The object was written before this lock was taken. Whatever removed it
        // in between (gc, an eviction, making room just now), it is put back
        // before a record points at it.
        if (!im.storage.object_exists(computed)) {
          std::lock_guard<std::mutex> cg(im.cas_mu);
          im.storage.write_object(computed, data, size);
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

        try {
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
            // The bytes were just hashed and are in place again: a generation that
            // had been quarantined or invalidated is usable again. A retired
            // generation stays retired.
            GenerationRecord* cur = updated.current();
            if (cur->validation != ValidationState::RETIRED) cur->validation = vstate;
            const std::uint64_t gen_no = cur->generation;
            const ValidationState gen_state = cur->validation;
            im.storage.save_record(updated);
            rec = std::move(updated);
            im.graph.set_dependencies(lk, deps);
            im.recompute_accounting();
            res.admitted = true; res.generation = gen_no;
            res.validation = gen_state; res.dedup = true;
            im.stats.artifacts_admitted.fetch_add(1, std::memory_order_relaxed);
            emitted.push_back({EventType::ARTIFACT_VALIDATED, {{"key", lk}, {"gen", std::to_string(gen_no)}}});
            emitted.push_back({EventType::ARTIFACT_ADMITTED, {{"key", lk}, {"dedup", "true"}}});
          } else {
            ArtifactRecord updated = rec;
            // What describes the artifact follows the content that is now current.
            updated.type = spec.type; updated.role = spec.role; updated.version = spec.version;
            updated.variant = spec.variant; updated.format = spec.format; updated.architecture = spec.architecture;
            updated.compatibility = spec.compatibility; updated.source_uri = spec.source_uri;
            updated.dependencies = spec.dependencies;
            updated.provenance = spec.provenance;
            if (spec.model) updated.provenance["model"] = spec.model->str();
            if (spec.revision) updated.provenance["revision"] = spec.revision->str();
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
            im.graph.set_dependencies(lk, deps);
            im.recompute_accounting();

            res.admitted = true; res.generation = newgen; res.validation = vstate;
            im.stats.artifacts_admitted.fetch_add(1, std::memory_order_relaxed);
            emitted.push_back({EventType::ARTIFACT_VALIDATED, {{"key", lk}, {"gen", std::to_string(newgen)}}});
            emitted.push_back({EventType::ARTIFACT_ADMITTED, {{"key", lk}}});
          }
        } catch (...) {
          // Nothing was committed: a record that exists only because of this call
          // goes away again, and so does an object nothing references.
          if (inserted) im.artifacts.erase(it);
          discard_own_object(false);
          throw;
        }
      }
    }
    // Evictions (and what they invalidated) come first, in the order they happened.
    auto first = im.take_deferred();
    first.insert(first.end(), emitted.begin(), emitted.end());
    emitted.swap(first);
  }
  for (const auto& ev : emitted) im.events.emit(ev.first, ev.second);
  if (closed) fail(ErrorCode::internal_error, "cache is closed");
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
    if (n > sizeof(chunk)) fail(ErrorCode::invalid_argument, "stream reader returned more than it was given room for");
    if (n > impl_->config.limits.max_artifact_size - buf.size())
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
  // A manifest that lists the artifact is no longer a complete collection.
  for (auto& kv : im.manifests) {
    Manifest& m = kv.second;
    if (m.validation != ValidationState::VALID) continue;
    bool lists = false;
    for (const auto& a : m.artifacts) if (graph_key(a) == lk) { lists = true; break; }
    if (!lists) continue;
    m.validation = ValidationState::INVALID;
    try { im.storage.save_manifest(m); } catch (...) { /* re-derived when the cache is opened */ }
    im.deferred.push_back({EventType::MANIFEST_INVALIDATED, {{"key", kv.first}}});
  }
  im.recompute_accounting();
}

bool ModelCache::ensure_capacity(Impl& im, const Digest& d, std::uint64_t size,
                                    const std::string& write_lk) {
  const auto& lim = im.config.limits;
  // The write adds a logical object only when write_lk has no current content yet.
  auto wit = im.artifacts.find(write_lk);
  const std::uint64_t extra_count = (wit != im.artifacts.end() && wit->second.current()) ? 0 : 1;

  // Evictions are worked out on a copy of the accounting first. When no set of
  // evictions makes room, nothing is evicted at all.
  std::map<Digest, std::uint64_t> refs;
  for (const auto& b : im.backings) refs[b.first] = b.second.refcount;
  std::uint64_t phys = im.physical_bytes;
  std::uint64_t count = im.logical_count;
  auto fits = [&]() {
    auto r = refs.find(d);
    const std::uint64_t extra_phys = (r == refs.end() || r->second == 0) ? size : 0;
    return extra_phys <= lim.max_total_bytes && phys <= lim.max_total_bytes - extra_phys &&
           count + extra_count <= lim.max_object_count;
  };
  if (fits()) return true;

  const std::set<std::string> protected_nodes = im.protected_keys();
  std::vector<std::pair<std::uint64_t, std::string>> candidates;
  for (const auto& kv : im.artifacts) {
    const auto& rec = kv.second;
    if (rec.pinned) continue;
    if (kv.first == write_lk) continue;
    if (protected_nodes.count(kv.first)) continue;
    candidates.push_back({rec.updated_at, kv.first});
  }
  std::sort(candidates.begin(), candidates.end());

  std::vector<std::string> plan;
  for (const auto& [t, key] : candidates) {
    (void)t;
    const ArtifactRecord& rec = im.artifacts.at(key);
    for (const auto& g : rec.generations) {
      if (!g.backing_key) continue;
      Digest gd;
      try { gd = Digest::from_hex(*g.backing_key); } catch (...) { continue; }
      auto r = refs.find(gd);
      if (r == refs.end() || r->second == 0) continue;
      if (--r->second == 0) {
        auto b = im.backings.find(gd);
        const std::uint64_t bytes = b == im.backings.end() ? 0 : b->second.bytes;
        phys -= bytes < phys ? bytes : phys;
      }
    }
    if (rec.current() && count > 0) --count;
    plan.push_back(key);
    if (fits()) break;
  }
  if (!fits()) return false;

  for (const auto& key : plan) {
    drop_record(im, key);
    im.stats.artifacts_evicted.fetch_add(1, std::memory_order_relaxed);
    im.deferred.push_back({EventType::ARTIFACT_EVICTED, {{"key", key}}});
  }
  return true;
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
        // Every attribute the request constrains is checked, not only some of them.
        if (mismatch(rec, req, req.type, req.role)) continue;
        if (req.variant && !(rec.variant && *rec.variant == *req.variant)) continue;
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
        out.found = true;
        out.valid = (gen->validation == ValidationState::VALID);  // also when require_valid is off
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
  std::optional<std::string> reused_key;
  {
    // The records are read under the lock from start to finish: nothing found
    // here is looked at after the lock is gone.
    std::shared_lock<std::shared_mutex> g(im.mu);
    std::vector<std::pair<const ArtifactRecord*, const GenerationRecord*>> candidates;
    for (const auto& kv : im.artifacts) {
      const auto& rec = kv.second;
      if (req.logical_key && kv.first != *req.logical_key) continue;
      if (mismatch(rec, req, std::optional<ArtifactType>(req.type), std::optional<ArtifactRole>(req.role))) continue;
      const auto* gen = rec.current();
      if (!gen) continue;
      candidates.push_back({&rec, gen});
      plan.candidates.push_back(logical_key(rec.ns, rec.name));
    }
    std::sort(plan.candidates.begin(), plan.candidates.end());
    if (candidates.empty()) {
      plan.action = ReuseAction::FETCH_REQUIRED;
      plan.compatibility_reasoning = "no candidate matched type/role/compatibility";
      return plan;
    }
    // A valid candidate always comes before one that is not: a corrupt or
    // unvalidated artifact never hides a reusable one.
    std::sort(candidates.begin(), candidates.end(), [](const auto& x, const auto& y) {
      const bool xv = x.second->validation == ValidationState::VALID;
      const bool yv = y.second->validation == ValidationState::VALID;
      if (xv != yv) return xv;
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
    plan.artifact_key = logical_key(rec->ns, rec->name);

    if (gen->validation == ValidationState::VALID) {
      plan.action = ReuseAction::REUSE_LOCAL;
      if (gen->backing_key) plan.backing_location = im.storage.object_path(gen->content_digest);
      im.stats.bytes_reused.fetch_add(gen->size, std::memory_order_relaxed);
      reused_key = plan.artifact_key;
    } else if (gen->validation == ValidationState::QUARANTINED ||
               gen->validation == ValidationState::INVALID) {
      plan.action = ReuseAction::REJECT_CORRUPT;
      plan.rejected.push_back({*plan.artifact_key, std::string("validation state ") + to_string(gen->validation)});
    } else {
      plan.action = ReuseAction::REVALIDATE_REQUIRED;
    }
  }
  if (reused_key) im.events.emit(EventType::ARTIFACT_REUSED, {{"key", *reused_key}});
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
    if (impl_->manifests.count(ck) == 0 &&
        impl_->manifests.size() >= impl_->config.limits.max_manifest_count)
      fail(ErrorCode::capacity_exceeded, "manifest count bound reached");
    auto valid = ValidationState::VALID;
    for (const auto& a : m.artifacts) if (!key_exists(a)) { valid = ValidationState::INVALID; break; }
    Manifest mm = m;
    mm.validation = valid;
    impl_->storage.save_manifest(mm);  // durable first, then visible
    impl_->manifests[ck] = mm;
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
    Manifest updated = it->second;
    updated.validation = ValidationState::INVALID;
    impl_->storage.save_manifest(updated);  // durable first, then visible
    it->second = std::move(updated);
    emitted.push_back({EventType::MANIFEST_INVALIDATED, {{"key", it->first}}});
  }
  for (const auto& ev : emitted) impl_->events.emit(ev.first, ev.second);
}
// ---------------------------------------------------------------------------
// lifetime / validation operations
// ---------------------------------------------------------------------------
void ModelCache::validate(const ArtifactId& id, bool force) {
  auto& im = *impl_;
  std::vector<Impl::PendingEvent> emitted;
  {
    std::unique_lock<std::shared_mutex> wg(im.mu);
    auto it = im.artifacts.find(logical_key(id.ns.str(), id.name));
    if (it == im.artifacts.end()) fail(ErrorCode::not_found, "artifact not found");
    const auto* found = id.content_pin ? it->second.find_generation(*id.content_pin) : it->second.current();
    if (!found) fail(ErrorCode::not_found, "generation not found");
    const std::uint64_t gen_no = found->generation;
    const Digest digest = found->content_digest;

    // The record is changed on a copy, saved, and only then made current.
    ArtifactRecord updated = it->second;
    GenerationRecord* gen = updated.find_generation(gen_no);
    bool corrupt = false;
    if (force || gen->validation != ValidationState::VALID) {
      if (gen->backing_key) {
        const bool ok = im.storage.verify_object(gen->content_digest);
        if (ok) {
          // Intact bytes make a generation usable again, except one that was retired.
          if (gen->validation != ValidationState::RETIRED) gen->validation = ValidationState::VALID;
        } else {
          gen->validation = ValidationState::QUARANTINED;
          corrupt = true;
        }
      } else if (gen->validation != ValidationState::QUARANTINED &&
                 gen->validation != ValidationState::RETIRED) {
        gen->validation = ValidationState::VALID;
      }
    }
    updated.updated_at = platform::now_millis();
    im.storage.save_record(updated);
    it->second = std::move(updated);
    if (corrupt) {
      im.storage.quarantine_object(digest, "validation_failed");
      im.stats.corruption_detections.fetch_add(1, std::memory_order_relaxed);
      im.stats.quarantine_events.fetch_add(1, std::memory_order_relaxed);
      emitted.push_back({EventType::CORRUPTION_DETECTED, {{"key", it->first}, {"reason", "validation"}}});
      emitted.push_back({EventType::ARTIFACT_QUARANTINED, {{"key", it->first}}});
      im.quarantine_sharers(digest);
    }
    emitted.push_back({EventType::ARTIFACT_VALIDATED, {{"key", it->first}, {"gen", std::to_string(gen_no)}}});
    for (auto& ev : im.take_deferred()) emitted.push_back(std::move(ev));
  }
  for (const auto& ev : emitted) im.events.emit(ev.first, ev.second);
}

void ModelCache::quarantine(const ArtifactId& id, const std::string& reason) {
  auto& im = *impl_;
  std::vector<Impl::PendingEvent> emitted;
  {
    std::unique_lock<std::shared_mutex> wg(im.mu);
    auto it = im.artifacts.find(logical_key(id.ns.str(), id.name));
    if (it == im.artifacts.end()) fail(ErrorCode::not_found, "artifact not found");
    const auto* found = id.content_pin ? it->second.find_generation(*id.content_pin) : it->second.current();
    if (!found) fail(ErrorCode::not_found, "generation not found");
    const std::uint64_t gen_no = found->generation;
    const Digest digest = found->content_digest;
    const bool has_backing = found->backing_key.has_value();

    ArtifactRecord updated = it->second;
    updated.find_generation(gen_no)->validation = ValidationState::QUARANTINED;
    updated.updated_at = platform::now_millis();
    im.storage.save_record(updated);
    it->second = std::move(updated);
    im.stats.quarantine_events.fetch_add(1, std::memory_order_relaxed);
    emitted.push_back({EventType::ARTIFACT_QUARANTINED, {{"key", it->first}, {"reason", reason}}});
    emitted.push_back({EventType::CORRUPTION_DETECTED, {{"key", it->first}, {"reason", reason}}});
    if (has_backing) {
      // The bytes leave the object store, so nothing that shares them stays VALID.
      im.storage.quarantine_object(digest, reason);
      im.quarantine_sharers(digest);
    }
    for (auto& ev : im.take_deferred()) emitted.push_back(std::move(ev));
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
    for (auto& ev : im.take_deferred()) emitted.push_back(std::move(ev));
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
    const auto* found = id.content_pin ? it->second.find_generation(*id.content_pin) : it->second.current();
    if (!found) fail(ErrorCode::not_found, "generation not found");
    ArtifactRecord updated = it->second;
    updated.find_generation(found->generation)->validation = ValidationState::RETIRED;
    updated.updated_at = platform::now_millis();
    im.storage.save_record(updated);  // durable first, then visible
    it->second = std::move(updated);
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
      // The new name is an artifact name like any other: one that could not be
      // read back from disk is refused here, not when the cache is next opened.
      detail::check_identity_string("supersede target name", id.name + suffix);
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
      ArtifactRecord retired = it->second;
      for (auto& g2 : retired.generations) g2.current = false;
      im.storage.save_record(retired);  // durable first, then visible
      it->second = std::move(retired);
      im.graph.set_dependencies(lk2, graph_keys(it->second.dependencies));
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
    im.storage.save_alias(alias, tgt);  // durable first, then visible
    im.aliases[alias] = tgt;
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
  ArtifactRecord updated = it->second;
  updated.pinned = true;
  impl_->storage.save_record(updated);  // durable first, then visible
  it->second = std::move(updated);
}

void ModelCache::unpin(const ArtifactId& id) {
  std::unique_lock<std::shared_mutex> g(impl_->mu);
  auto it = impl_->artifacts.find(logical_key(id.ns.str(), id.name));
  if (it == impl_->artifacts.end()) fail(ErrorCode::not_found, "artifact not found");
  ArtifactRecord updated = it->second;
  updated.pinned = false;
  impl_->storage.save_record(updated);  // durable first, then visible
  it->second = std::move(updated);
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

  // Everything below reads shared state, so the lock comes first.
  std::shared_lock<std::shared_mutex> g(im.mu);
  r.physical_bytes = im.physical_bytes;
  r.dependencies = req.id ? im.graph.dependencies(node_key(*req.id)) : std::vector<std::string>{};

  // The same checks and the same preference a lookup applies.
  std::vector<std::pair<const ArtifactRecord*, const GenerationRecord*>> accepted;
  for (const auto& kv : im.artifacts) {
    const auto& rec = kv.second;
    const auto* gen = rec.current();
    if (!gen) continue;
    r.candidates_evaluated.push_back(kv.first);
    if (const char* why = mismatch(rec, req, req.type, req.role)) {
      r.candidates_rejected.push_back({kv.first, why}); continue;
    }
    if (req.variant && !(rec.variant && *rec.variant == *req.variant)) {
      r.candidates_rejected.push_back({kv.first, "variant mismatch"}); continue;
    }
    if (req.require_valid && gen->validation != ValidationState::VALID) {
      r.candidates_rejected.push_back({kv.first, std::string("validation ") + to_string(gen->validation)}); continue;
    }
    accepted.push_back({&rec, gen});
  }
  std::sort(accepted.begin(), accepted.end(), [](const auto& x, const auto& y) {
    if (x.first->pinned != y.first->pinned) return x.first->pinned;
    if (x.first->updated_at != y.first->updated_at) return x.first->updated_at < y.first->updated_at;
    return logical_key(x.first->ns, x.first->name) < logical_key(y.first->ns, y.first->name);
  });
  if (!accepted.empty()) {
    const auto* rec = accepted.front().first;
    const auto* gen = accepted.front().second;
    r.selected_artifact = logical_key(rec->ns, rec->name);
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
    const std::set<std::string> protected_nodes = im.protected_keys();
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
    for (auto& ev : im.take_deferred()) emitted.push_back(std::move(ev));
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
