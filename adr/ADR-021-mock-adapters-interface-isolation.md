# ADR-021 — Mock Adapters and Interface Isolation

- **Status:** ratified
- **Owner:** Spatial Platform Architecture Board
- **Date:** 2026-08-04
- **Supersedes:** none

## Context

Spatial Platform integrates third-party backends (COLMAP, OpenMVS, Open3D, GTSAM, Ceres, KISS-ICP, VGGT, gsplat, Nerfstudio) that are large, platform-dependent, license-gated, and not installed in most developer and CI environments. If kernel code, the scheduler, or integration tests depend on real backends, development stalls and CI becomes non-deterministic. The plugin chain (ADR-013) must be verifiable end-to-end before any real backend exists. The THIRD_PARTY.yml registry lists several backends as "planned", so the M0 kernel must be complete and testable with none of them present.

## Decision

Every `ProcessingAdapter` interface has a mock implementation, built with GoogleMock, maintained alongside the interface in `core/plugin/**` and the adapter contracts. Rules: (1) no production code in the kernel includes a backend header or links a backend library — backends are reachable only through interfaces; (2) integration tests run against mocks, so the worker protocol, scheduler, artifact store, and scene pipeline are exercised without any real backend; (3) mocks are faithful to the adapter contract — they validate capability negotiation, error propagation (ADR-014), input/output artifact handling, and the deterministic/cache contract (ADR-020); (4) real-backend tests, when added, are explicitly marked and skipped when the backend is absent; (5) golden fixtures generated once from verified backends (ADR-016) keep mocks honest over time. Interface isolation is enforced by CI: the `check-arch-debt` and architecture-review gates reject backend includes outside adapters, and the dep-registry validation ensures only declared M0 deps (`eigen`, `protobuf`, `sqlite3`, `nlohmann-json`, `gtest`) are linked into the kernel.

## Alternatives

- **Test against the real backends when present:** rejected — makes CI depend on externally built binaries and hides contract bugs behind backend behavior.
- **In-memory fakes written ad hoc per test:** rejected — unmaintained fakes drift from the real contract; gmock-based mocks keep the contract in one place.
- **Delay testing until backends exist:** rejected — the M0 kernel must ship and be verified independently of the "planned" backend registry.

## Consequences

- Positive: kernel and pipeline are fully tested in CI with no third-party backends; capability negotiation is validated on the mock matrix; adapter contracts are the single source of truth; onboarding real backends becomes a filling-in exercise rather than a rewrite; license-gated backends (VGGT) can be excluded from test environments entirely.
- Negative: mocks can diverge from real backend edge cases; mock behavior must be maintained as contracts evolve; some capacity questions (performance, memory) cannot be answered by mocks.
- Risks and mitigations: risk of mock divergence — mitigated by golden fixtures and by running real-backend tests in an optional job; risk of mocks becoming too permissive — mitigated by fault-injection and negative-case tests in the mock suite.

## References

- `docs/specifications/plugin-api.md`
- ADR-013 (plugin and adapter strategy)
- ADR-016 (testing strategy)
- ADR-018 (raw Eigen allowed only behind adapter boundaries)
- ADR-031 (M0 scope — mock adapters and demo worker in scope)
