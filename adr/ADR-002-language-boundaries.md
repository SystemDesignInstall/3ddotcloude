# ADR-002 — Language boundaries

- **Status:** ratified
- **Owner:** Spatial Platform Architecture Board
- **Date:** 2026-08-04
- **Supersedes:** none

## Context

The kernel (core, engine, adapters) must be performant, deterministic, and ABI-stable because it links against third-party geometry libraries. The SDK, research modules, AI workers, and CLI-adjacent tooling benefit from a higher-level language with rapid iteration and a rich AI/ML ecosystem. The platform therefore needs explicit language boundaries and a defined cross-language contract.

## Decision

C++20 is used for the kernel: core, engine, and adapters. Python 3.11 is used for the SDK, research modules, ai_workers, and CLI-adjacent tooling. The cross-language contract is the .spx format plus the Protobuf IPC schemas and the worker protocol. The Python SDK implements the documented .spx contract natively using sqlite3, json, and hashlib, and dispatches scheduling through the CLI subprocess. pybind11 bindings are deliberately deferred; no Python code calls directly into C++ in M0 scope (ADR-031). C++20 is chosen over C++23 to preserve ABI stability with third-party geometry libraries; the migration path to C++23 (std::expected, std::span, std::flat_map) is documented for later evaluation.

## Alternatives

- All-Python core: rejected. Performance and determinism requirements for SfM, bundle adjustment, and mesh processing cannot be met.
- C++23 now: rejected. ABI risk with third-party geometry libraries; the benefits (std::expected, std::span) are incremental.
- Python with pybind11 now: rejected. Binds Python and C++ lifecycles prematurely; deferred until the native .spx contract proves out.

## Consequences

- Positive: kernel performance and ABI stability; fast iteration in Python; a single cross-language contract to test; no dual implementations of the protocol.
- Negative: two languages to maintain; contract changes ripple across both; debugging across the process boundary is harder.
- Risks and mitigations: keep the .spx contract and protobuf schemas versioned and backward compatible; run schema validation in CI; run protocol conformance tests against both the C++ and Python implementations.

## References

- docs/specifications/project-format.md
- docs/specifications/worker-protocol.md
- docs/specifications/task-model.md
- ADR-001 (Monorepo strategy)
- ADR-012 (IPC protocol)
