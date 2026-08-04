# ADR-037 — Public API Stability Policy

- **Status:** ratified
- **Owner:** Spatial Platform Architecture Board
- **Date:** 2026-08-04
- **Supersedes:** none

## Context

The platform is commercial and will be consumed through many surfaces — Desktop application, CLI, Python SDK, C++ SDK, REST API, gRPC API, Cloud Worker, Distributed Scheduler, Mobile Capture, and Web Viewer. Partners and enterprises build on these surfaces and must trust that a minor upgrade does not break their tooling or their automated workflows. At the same time, the platform moves fast: new capabilities, backends, and pipeline semantics (ADR-026, ADR-027, ADR-034) will reshape behavior. Without a stability policy, we either freeze everything (stifling progress) or break everyone (destroying trust). The internal APIs already have change control through the Constitution; public APIs need an equivalent, contract-level rule.

## Decision

The ten public API surfaces — Desktop, CLI, Python SDK, C++ SDK, REST API, gRPC API, Cloud Worker, Distributed Scheduler, Mobile Capture, Web Viewer — are governed by semantic versioning. Within a MAJOR version, backward compatibility is guaranteed: additive changes are allowed, breaking changes are not. Deprecation is explicit and time-boxed: an API is marked deprecated, kept working for a defined deprecation window spanning at least two milestones, and removed only at the next MAJOR release. A clear internal-versus-public distinction is published: public surfaces are listed above and covered by this policy; everything under `core/**`, `engine/**`, `schemas/**`, adapters, and plugins is internal and governed by the Constitution, not by semver — internal APIs may change freely between milestones behind the public contract. Breaking changes to any public surface require a numbered RFC, Architecture Review, and the two-milestone deprecation notice. Protocol buffers for REST/gRPC/worker IPC use explicit versioned field evolution (additive fields, reserved tags) to avoid breaking on wire.

## Alternatives

- **All APIs versioned identically with full semver:** rejected — conflates the public contract with internal churn and over-constrains the kernel.
- **No stability guarantee:** rejected — untenable for a commercial platform with enterprise and partner dependencies.
- **Single unified version with no deprecation windows:** rejected — forces either lockstep breaks or silent breakage; deprecation windows are required for trust.

## Consequences

- Positive: customers and partners can plan upgrades; automation built on CLI/SDK/REST/gRPC survives minor releases; the internal kernel stays free to evolve rapidly; breaking changes are deliberate, visible, and RFC-gated.
- Negative: maintenance burden of deprecated surfaces during the window; versioning discipline must be enforced in CI; every public surface needs a compatibility test suite; semver majors become expensive and must be scheduled.
- Risks and mitigations: risk of accidental breaking changes — mitigated by compatibility tests against each public surface and CI gates; risk of deprecation-window drift — mitigated by a published deprecation register; risk of public surface proliferation — mitigated by requiring RFC approval before a new surface is declared public.

## References

- `docs/specifications/plugin-api.md`
- `docs/specifications/task-model.md`
- `docs/benchmarks/benchmark-framework.md` (evidence that API behavior is stable)
- ADR-013 (plugin API), ADR-031 (M0 scope), ADR-034 (capability API), ADR-035 (scene query API as the shared read contract)
