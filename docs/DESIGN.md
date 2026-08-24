# Model Cache - Design Overview

Model Cache is a C++20 runtime with a layered architecture. The public surface is the ModelCache facade (include/model_cache/cache.hpp). Everything below it is a component with a clear responsibility and a small, explicit interface.

## Layers

- Identity (identity.hpp, digest.hpp): validated namespace/name/role/format/architecture identities and the authoritative SHA-256 content digest. Identity is a value type, never a pointer or a transient path.
- Canonical serialization (serialization.hpp): a strict, deterministic, length-prefixed wire format with bounded allocations and whole-stream validation. It drives manifest digests, compatibility keys, and persisted records.
- Compatibility (compatibility.hpp): canonical compatibility keys and strict matching.
- Storage (storage.hpp, detail/platform.hpp): the durable layer. It owns content-addressable backing objects (dedup by SHA-256) and atomic metadata files (artifact records, manifests, aliases), plus recovery and quarantine. All file writes are atomic: temp staging plus flush plus atomic commit/rename.
- Dependency graph (dependency.hpp): directed artifact dependencies with cycle rejection and transitive traversal in both directions.
- Observability (events.hpp, stats.hpp): a bounded event bus (callbacks never run under an internal lock) and atomic bounded statistics.
- Facade (cache.cpp): the thread-safe state machine. A single shared_mutex guards logical records, manifests, aliases, backing reference counts, and the dependency graph; read-heavy operations (lookup) run concurrently under the shared lock. Mutation is transactionally ordered: inspect, reserve, stage, hash, validate, commit, publish. Records are persisted atomically before publishing, so a failed transaction never corrupts prior state.

## Thread-safety and determinism

State mutation serializes under one shared_mutex; content-addressable writes serialize under a separate CAS mutex; the event bus uses its own lock. Callbacks are collected under the state lock and invoked only after it is released, so re-entrancy cannot deadlock. Linearizable ordering is obtained with std::map/std::set (no unordered containers in observable results), so all public orderings and digests are deterministic.

## Bounded capacity

Configured limits (total bytes, object count, manifest count, dependency edges, metadata bytes, individual artifact size, staging bytes, concurrent ingests, subscriptions, event backlog, serialized message size, path length, metadata field size) are enforced explicitly. Admission evicts deterministically before admitting; when a bound cannot be met, admission is rejected rather than silently grown.

## Persistence format

Records carry a state version and strict readers. Malformed, truncated, oversized, or unknown-schema metadata is rejected rather than coerced.