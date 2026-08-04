# Testing Strategy

- **Status:** ratified (P0)
- **References:** ADR-016 (testing strategy), ADR-021 (mock adapters), ADR-018 (strict types)
- **Definition of "tested":** a test is complete when it asserts the observed outcome (return value, persisted state, emitted event) **and** covers the error path, not just the happy path.

## 1. Pyramid

Four layers, all first-class in CI:

1. **Unit tests** — fast, per component (GoogleTest + GoogleMock for the C++ kernel).
2. **Integration tests** — exercise real boundaries (worker protocol framing, scheduler persistence, artifact CAS round-trips) against mock adapters (ADR-021).
3. **Property-based tests** — coordinate math, transform composition, round-trips; plus **golden tests** against reference values for the coordinate conventions.
4. **Fault-injection tests** — worker crashes, heartbeat timeouts, cancellation, retry paths, kill-during-write.

The kernel pyramid is **broad and shallow**: most coverage in fast unit tests, few slow integration tests.

## 2. Unit tests (`tests/unit`, GoogleTest)

Cover per component, including:

- **SE(3) composition and inversion** — including round-trips across all strict transform pairs.
- **Quaternion conversion** — `(x,y,z,w)` scalar-last; golden conversion vs. reference values; scalar-first round-trips.
- **Coordinate frame graph** — frame resolution, transform path composition, unknown-frame rejection.
- **Project create/open** — layout, `project.json` validation, read-only mode, portability.
- **Migrations** — `schema_version` driven; each migration transactional; rollback on failure.
- **Artifact hashing** — SHA-256 identity, shard layout, deduplication.
- **Atomic write** — temp-then-rename semantics; reader never observes a partial state.
- **DAG validation** — cycles, orphaned nodes, invalid transitions rejected.
- **Cancellation** — persisted first-class state; never re-run on resume.
- **Retries** — bounded backoff; only `recoverable` codes retried; retry exhaustion → `failed`.
- **Serialization** — protobuf/JSON round-trips for Scene, artifacts, task state.

## 3. Integration tests (`tests/integration`)

- **C++ core + SQLite** — WAL mode, locking, single-writer enforcement, read-only openers.
- **C++ scheduler + Python worker** — end-to-end task dispatch over the IPC protocol.
- **Restart/resume** — pipeline interrupted at task N resumes from N, not zero (ADR-020).
- **Worker crash** — heartbeat timeout + pipe EOF → recoverable failure → retry.
- **Corrupted artifact** — manifest/payload mismatch detected on read and during GC.
- **Schema migration** — upgrade path on a real database; rollback on failure.
- **CLI flow** — create project, import, run mock pipeline, export.

Integration runs against **mock adapters** so CI is deterministic with no backend installed (ADR-021). Real-backend tests are explicitly marked and skipped when the backend is absent.

## 4. Property-based tests

- **Transform round trips** — `world_from_sensor * sensor_from_world = identity` for random poses; composition associativity.
- **Serialization round trips** — arbitrary Scenes/artifacts/task states serialize and deserialize losslessly.
- **Random DAGs** — generated DAGs validate or fail validation with a deterministic error; no crashes, no hangs.
- **Coordinate conversions** — random points through every strict transform pair match reference math.
- **Golden fixtures** — curated reference values for right-handed, meters, scalar-last quaternions, `world_from_sensor`, OpenCV camera convention; generated once from verified backends and kept honest over time.

Seeded randomness and deterministic timeouts keep property/fault tests non-flaky.

## 5. Fault injection

- **Kill during write** — CAS temp-then-rename survives; project remains consistent.
- **Disk full** — atomic save fails cleanly with a recoverable `StorageError`; no partial state.
- **Corrupted DB** — integrity check reports the offending hash/UUID; project is not silently repaired.
- **Lost worker** — heartbeat timeout → retry; scheduler state consistent.
- **Invalid artifact** — hash mismatch detected on read; GC skips it safely.
- **Timeout** — worker deadline exceeded → cancellation/crash path.

## 6. Python SDK

Tested with **pytest / mypy / ruff** from the pinned tooling environment. Mirrors the kernel contract: SDK calls assert the same typed error codes, cache semantics, and Scene query behavior over the serialized contract.

## 7. CI matrix and gates

- Matrix: **Ubuntu + Windows × Debug + Release**; Linux Debug adds **ASan/UBSan**.
- Gates before merge: clang-format, clang-tidy, warnings-as-errors, `check-domain-types`, schema validation, `dep-registry-validation`, `constitution-check`, `rfc-check`, architecture review, and the **architecture-debt** gate (no TODO/FIXME/HACK in `core/**`, `engine/**`, `schemas/**`).
- Coverage of new kernel code must not regress the established bar.

## 8. Commands

```
ctest --test-dir build/<preset> --output-on-failure   # unit + integration (C++ kernel)
pytest                                                 # Python SDK
```

Run the full suite locally before opening a PR; CI runs the same matrix plus the gates.

## References

- `docs/development/build.md`, `docs/architecture/process-model.md`, `docs/architecture/error-model.md`
- ADR-016, ADR-017, ADR-018, ADR-021
