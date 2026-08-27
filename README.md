# Model Cache

Model Cache 1.0.0 is an open-source, vendor-neutral C++20 runtime for caching, validating, versioning, persisting, and reusing model weights, adapters, tokenizer assets, compiled artifacts, and model components across AI infrastructure. It turns model artifacts into explicit, reusable infrastructure.

> Core systems question - "What reusable model artifacts already exist, are they compatible and trustworthy, and is reuse cheaper than fetching, rebuilding, recompiling, or reconstructing them?"

Model Cache owns reusable model artifacts as cache objects and metadata. It does not execute model loading, inference, compilation, network fetching, or serving. It identifies reusable artifacts and their state so a downstream engine can decide to reuse instead of rebuild.

## Why a dedicated boundary

Modern inference and training environments repeatedly acquire, generate, compile, transform, validate, and reload expensive artifacts: weight files, shards, quantized weights, adapters, tokenizer assets, vocabularies, configuration, compiled/engine artifacts, kernel bundles, graph/compiler outputs, pre/postprocessing assets, manifests, metadata, and application-defined model components. Model Cache gives them a single, neutral substrate with stable identity, canonical manifests, content-addressable backing, trusted validation, compatibility keys, bounded durable storage with recovery, generations, aliases, dependencies, deduplication, pinning, eviction, and reuse planning.

### Ownership boundary

| Runtime | Owns |
| --- | --- |
| FlashTier | physical residency |
| Context Fabric | reusable computational state |
| Compute Fabric | execution placement |
| Reclaim Fabric | state reclamation |
| Checkpoint Fabric | execution survival |
| KV Fabric | reusable KV inference state |
| Tensor Cache | reusable tensor-shaped state |
| Unified Buffer | heterogeneous buffer objects |
| Transfer Fabric | data movement |
| Model Cache | reusable model artifacts |

Model Cache governs reusable model artifacts: their identity, compatibility, validation, persistence, deduplication, versioning, dependencies, and reuse.

## Independence

Model Cache stands alone. It does not depend on PyTorch, TensorFlow, JAX, vLLM, SGLang, TensorRT-LLM, llama.cpp, any Hugging Face library, CUDA, ROCm, any specific model format, any specific registry, or any specific serving engine. Clean integration seams are provided without making any external system a requirement. The core remains format-neutral; no complex format is pretended to be fully parsed unless a real parser is implemented and tested.

## Building

On Windows 11 x64 with MSVC (Visual Studio 2022 Build Tools) and CMake 3.25 or newer:

~~~sh
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix <install-dir>
~~~

The library builds with /W4 /WX /EHsc /utf-8 and compiles with zero warnings in both Release and Debug. Debug builds enable Microsoft STL iterator and lifetime checks.

## Using as a dependency

After installation, an independent project can consume the exported CMake package:

~~~cmake
find_package(model_cache CONFIG REQUIRED)
target_link_libraries(my_target PRIVATE ModelCache::model_cache)
~~~

## Concepts

### Artifact identity
Every cached artifact carries stable, explicit identity independent of process-local pointers, temporary paths, or runtime handles. Identity can be derived from a caller-provided namespace/name, from canonical metadata, from a content digest, or from model/revision/role/format/architecture/compatibility attributes. SHA-256 is the authoritative content identity; weak hashes are never used as authority.

### Manifests
A manifest represents a model or reusable artifact collection. It is serialized canonically and deterministically; semantically identical manifests produce byte-identical representation and an identical digest. Manifest membership is validated on publication.

### Content-addressable storage
Identical byte content may share one backing object while preserving separate logical identities, namespaces, generations, ownership metadata, and invalidation state. Reference accounting is correct: no double-free, no leaked backing, no premature deletion while any reference remains.

### Validation, quarantine, integrity
Artifacts carry explicit validation state (UNKNOWN, VALIDATING, VALID, INVALID, STALE, QUARANTINED, RETIRED). Validation covers SHA-256 digest, expected size, format identifier, caller-provided validators, dependency and compatibility checks, manifest membership, and external signature/verifier results through a callback seam. Corrupt or suspicious artifacts enter quarantine and are never returned as reusable. Integrity is verified at ingest, optionally at lookup, at recovery, and before quarantined data is promoted.

### Compatibility
Compatibility keys are canonical and deterministic over configurable dimensions (model architecture/revision, artifact format, quantization, tokenizer/vocabulary identity, adapter base-model identity, backend/ABI/compiler version, target architecture, device capability, precision, layout, build flags, and caller-defined fields). Matching is strict: an artifact that merely looks similar is never reported compatible.

### Persistence and recovery
Persistent storage uses a bounded, validated cache root. Writes are atomic (temp-file staging plus fsync/FlushFileBuffers plus atomic commit/rename). Restart recovery removes stale temp files, detects and quarantines corrupted backing, and reconstructs reference counts from authoritative records. No partially admitted artifact appears valid.

### Generations and aliases
Every mutable logical artifact has explicit generation semantics: immutable content generations, a logical alias pointing to the current generation, stale-generation rejection, replacement, supersession, retirement, and rollback to a prior valid generation where policy permits. Aliases update atomically.

### Dependencies
Artifacts may depend on other artifacts. The dependency graph supports parents, children, transitive and reverse dependency traversal, dependency validation, dependency-aware eviction, and rejects dependency cycles. Backing required by surviving dependents is never evicted.

### Admission, eviction, pinning, deduplication
Admission considers available budget, size, validation state, expected reuse, regeneration/fetch cost, dependency value, pinning, caller priority, and existing duplicate backing, and is deterministic for identical inputs. Eviction is deterministic (least-recently-updated with a canonical tie-break) and never evicts pinned artifacts or dependencies of retained artifacts. Deduplication shares identical content while accounting physical and logical bytes separately.

### Concurrency and determinism
The runtime is genuinely thread-safe, with lock ordering defined and external callbacks never invoked while internal locks are held. For identical inputs, identity, manifest digests, compatibility keys, candidate ordering, admission/eviction choices, serialization, and snapshot ordering all match; unordered-container iteration order is never observable.

### Observability
Bounded statistics, a bounded event bus, deterministic snapshots, and explain APIs make behavior explainable without an opaque boolean-only interface.

## CLI

The model-cache executable exposes a serious command surface with JSON output. Run model-cache --help for the full list. Common commands:

~~~sh
model-cache info
model-cache ingest --key ns/name --file weights.bin
model-cache get --key ns/name
model-cache lookup --type MODEL_WEIGHTS --role weights
model-cache list
model-cache manifest-add --model M --revision R --artifacts a,b
model-cache manifest-get --model M --revision R
model-cache validate|quarantine|purge|retire|supersede --key ns/name
model-cache alias-set|alias-get
model-cache pin|unpin --key ns/name
model-cache dependencies|dependents|ancestors --key ns/name
model-cache stats|snapshot|verify|gc|save|restore
model-cache benchmark
~~~

Pass --root <dir> to select the cache root and --json for JSON output.

## Examples and benchmarks
Runnable examples demonstrate ingest, exact lookup, model/revision lookup, compatibility rejection, content deduplication, manifest creation/validation, alias update, generation supersession, adapter dependency on a base model, tokenizer/vocabulary dependency, compiled-artifact compatibility, corruption rejection, quarantine, persistence restart/recovery, deterministic eviction, and concurrent lookup/ingest.

The benchmark measures completed work across scales (1, 10, 100, 1,000, plus snapshot and eviction at 5,000) and reports throughput and latency for hashing, ingest, lookup, dedup, manifest serialization, snapshot, eviction selection, persistence save, and recovery load.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
