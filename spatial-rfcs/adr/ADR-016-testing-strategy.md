# ADR-016 — Testing Strategy

- **Status:** ratified
- **Owner:** Spatial Platform Architecture Board
- **Date:** 2026-08-04
- **Supersedes:** none

## Context

Spatial Platform's kernel handles coordinate transforms, observation ingestion, scheduling, and artifact storage — exactly the code where silent unit/frame/convention bugs corrupt data. Backends (COLMAP, GTSAM, etc.) are third-party and not available in every test environment, so most logic must be testable without them. CI must run on both Ubuntu and Windows, in Debug and Release, with strict static analysis. "Tested" must have a precise meaning so the Architecture Debt gate and the review gate can enforce it. The test pyramid must be defined for the C++ kernel (GoogleTest/gmock) and for the Python SDK (pytest).

## Decision

Testing has four layers, all first-class in CI: (1) unit tests per component with GoogleTest and GoogleMock; (2) integration tests that exercise real boundaries (worker protocol framing, scheduler persistence, artifact CAS round-trips) with mock adapters (ADR-021); (3) property-based tests for coordinate math, transform composition, and round-trips, plus golden tests against reference values for the coordinate conventions (right-handed, meters, quaternion scalar-last, world_from_sensor, OpenCV camera convention); (4) fault-injection tests for worker crashes, heartbeat timeouts, cancellation, and retry paths. The kernel test pyramid is broad and shallow: most coverage in fast unit tests, fewer slow integration tests. The Python SDK is tested with pytest/mypy/ruff. DAG construction is fuzzed for cycles, orphaned nodes, and invalid transitions. A test is "complete" when it asserts the observed outcome (return value, persisted state, emitted event) and covers the error path, not just the happy path. CI wiring: the GitHub Actions matrix runs Ubuntu+Windows x Debug+Release; Linux adds ASan/UBSan; clang-format, clang-tidy, warnings-as-errors, check-domain-types, schema validation, dep-registry validation, constitution-check, rfc-check, and architecture-review all run as gates before merge. Coverage of new kernel code must not regress the established bar, and any test that depends on a real backend must be explicitly marked and skipped when that backend is absent.

## Alternatives

- **Heavy end-to-end tests as the primary layer:** rejected — real backends are unavailable in most environments and E2E failures are slow to localize.
- **Snapshot/regression testing only:** rejected — snapshots obscure intention and go stale silently; golden tests are limited to coordinate conventions where they add value.
- **No fault-injection layer:** rejected — the scheduler and worker protocol only earn confidence by killing workers mid-task.

## Consequences

- Positive: unit failures localize to a single component; property tests pin down the coordinate conventions against regressions; mock adapters make integration and CI deterministic; fault-injection proves crash recovery; definition of "tested" is enforceable by the architecture gate.
- Negative: a large matrix of CI jobs costs time; property/golden tests require curated reference fixtures; marking backend-dependent tests adds bookkeeping.
- Risks and mitigations: risk of tests drifting from real backend behavior — mitigated by golden fixtures generated once from verified backends; risk of flaky fault-injection tests — mitigated by deterministic timeouts and seeded randomness.

## References

- `docs/specifications/geometry-model.md`
- `docs/specifications/task-model.md`
- `docs/architecture/process-model.md`
- ADR-017 (cross-platform CI matrix)
- ADR-018 (strict types — enforced by check-domain-types in CI)
- ADR-021 (mock adapters and interface isolation)
