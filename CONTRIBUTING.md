# Contributing

Thanks for contributing to Model Cache. This document explains expectations for a healthy,
deterministic, warning-free codebase.

## Build requirements

- Windows 11 x64, MSVC (Visual Studio 2022 Build Tools), C++20, CMake >= 3.25.
- The primary configuration is the Visual Studio CMake generator. Release and Debug must both
  build with zero warnings under /W4 /WX /EHsc /utf-8.
- Keep dependencies modest and standards-based. Do not introduce an ML framework, GPU, or heavy
  dependency for a small subsystem.

## Tests

- The test suite lives in tests/ and uses a small dependency-free harness (tests/framework.hpp).
  Tests must run to natural completion; never add a test timeout or rely on one.
- New behavior must come with tests in the appropriate category (identity, compatibility,
  manifest, ingest, persistence, corruption, recovery, deduplication, reference counting, alias,
  generation, dependency graph, cycle rejection, admission, eviction, pinning, quarantine,
  serialization, concurrency, property/randomized, adversarial, CLI, install/downstream,
  benchmark sanity).
- Concurrency is exercised with real threads; the runtime must remain race-free and never invoke
  callbacks while holding an internal lock.
- Randomized/property tests must run thousands of operations and preserve the documented
  invariants (deterministic digests, non-negative reference counts, bounded capacity, no false
  compatibility/reuse, persistence round-trips).

## Coding style

- Follow the existing style: 2-space indentation, braces on the same line, clear names.
- Compile with zero warnings in both configurations. Treat every warning as an error.
- Platform-specific code lives behind the platform abstraction (include/model_cache/detail/
  platform.hpp); do not sprinkle OS-specific calls into the core.
- No framework-specific assumptions. Keep the core vendor-neutral and format-neutral.

## Deterministic behavior

- For identical inputs and policy, ArtifactId, manifest digest, compatibility key, candidate
  ordering, admission/eviction decisions, serialization, and snapshot ordering must all match.
- Never expose the iteration order of a std::map/std::set or other unordered container in a
  public result.
- Serialization must be canonical and strictly validated; malformed persisted metadata is
  rejected, not coerced to a default.

## Adding artifact types

- Artifact types are an explicit enum (types.hpp). Add a value to ArtifactType and its canonical
  string mapping, plus a strict parser case. Do not hardcode a vendor naming scheme into the core.

## Compatibility fields

- Compatibility dimensions are canonical strings (compatibility.hpp). Add a documented dimension
  constant and keep matching strict so similar-but-incompatible artifacts never reuse.

## Persistence format evolution

- Persisted structures carry an explicit version (record.hpp kStateVersion). Bump it and handle
  readers for older versions explicitly. Never silently coerce malformed input.

## Regression and benchmark expectations

- Every defect fixed must include a regression test, and affected plus full validation must be
  re-run.
- Benchmarks measure completed work (ns/op, throughput, busy bytes). Sanity-check numbers before
  publishing them; do not report submission time as completion time.

## Public claims

- Do not claim support for a platform, backend, or integration that has not been compiled and
  tested. Only claim what the tests prove.
- Keep public copy positive and boundary-focused. Do not tease or reference unreleased/private
  roadmap systems, and do not front-run a release sequence.
