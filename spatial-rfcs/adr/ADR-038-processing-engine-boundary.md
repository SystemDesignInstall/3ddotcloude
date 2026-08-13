# ADR-038 — Processing Engine Boundary Definition

- **Status:** ratified
- **Owner:** Spatial Platform Architecture Board
- **Date:** 2026-08-05
- **Supersedes:** none

## Context

RFC-0002 ratified the Spatial Platform as a **permanent spatial data model**: Scene, observations, geometry, provenance, and artifacts persist as immutable, versioned, queryable data. The platform must now become an **executable spatial computing platform**: users describe computation (a pipeline of tasks), and the platform schedules it across isolated worker processes, caches results, records provenance, and survives restarts. Without an explicit boundary, the processing layer would drift into algorithm ownership (COLMAP/GTSAM/Open3D internals), GPU scheduling, storage, and UI — re-introducing the scope creep ADR-031 was ratified to prevent. This ADR fixes the boundary of the Processing Engine (RFC-0003): what it owns, what it delegates, and what it never touches.

## Decision

The Processing Engine (`engine/**`) is the **runtime layer between the data model and the algorithms**. It owns:

- **Process description**: the Task model — `TaskDefinition` (what to do, input/output/parameter schemas, resource requirements) and `TaskInstance` (a concrete run with resolved inputs, execution state, and metadata). Pipeline/Recipe definitions (ADR-026) are a **model surface** of the engine; their execution is deferred past M0.
- **Workflow → Task Graph derivation**: a Workflow (ADR-028) is an ordered user-facing stage progression derived into a **Task DAG**; the DAG is the scheduler's unit of work.
- **DAG execution**: acyclicity/type-match/resource-feasibility validation, dependency resolution, topological ordering, dispatch.
- **Task lifecycle**: the six-state machine (`pending, running, succeeded, failed, cancelled, skipped`), retry with bounded backoff for recoverable errors only (ADR-014), cooperative cancellation as a first-class persisted state.
- **Persistence of run state**: task/runs/dependencies/worker/cache metadata in `project.db` (SQLite WAL, metadata only, exactly one writer), enabling resume after host restart (ADR-020).
- **Task cache**: content-addressed, keyed by (input SHA-256 hashes, config hash, producer/algorithm version, engine git commit); only `deterministic` tasks are served from cache; `cache_policy = cacheable|never` (ADR-020).
- **Worker process supervision**: spawning isolated child processes, Protobuf IPC over stdin/stdout (ADR-011/012), heartbeat, timeout, crash detection, cancellation delivery, deterministic temp workspace cleanup.
- **Execution records / provenance**: `ExecutionRecord` (inputs, outputs, environment, hardware, start/end) persisted per task for reproducibility and audit.
- **Resource capability matching**: typed `ResourceSpec`/`ResourceProfile` (cores, RAM, GPU/VRAM, temp disk) used for capability-based worker selection — the scheduler is the single allocator (ADR-034).

The engine **does not own**:

- **Algorithms**: COLMAP, OpenMVS, GTSAM, KISS-ICP, Open3D, Gaussian/NeRF, and AI models live behind adapters/workers (ADR-011/034); the engine never embeds their logic.
- **Data storage**: `project.db` and the artifact store belong to `core/storage` + `core/artifacts` (ADR-008/009/010); the engine reads/writes them through those APIs and never owns payload bytes.
- **GPU/accelerator management**: the engine tracks VRAM budgets in the resource model; actual device allocation, exclusivity, and per-GPU scheduling are deferred (process-model §5).
- **User interfaces**: CLI/UI/SDK consume the engine through its public surface; the engine owns no rendering or UX.
- **Adaptive intelligence**: strategy selection (ADR-027) and the Quality Engine (ADR-030) are consumers of engine outputs, not engine subsystems.

The engine boundary is ratified together with RFC-0003; `engine/**` becomes a Constitution-protected surface (§2).

## Alternatives

- **Engine as a thin scheduler only:** rejected — would leave process description, cache, provenance, and resource matching without an owner, fragmenting responsibility across teams.
- **Engine as an algorithm container:** rejected — violates ADR-011 (process isolation) and Principle 15 (every algorithm replaceable); couples the runtime to vendor build complexity.
- **Engine owning storage and GPU:** rejected — duplicates `core/storage`/CAS (ADR-009/010) and contradicts the deferred GPU plan in process-model §5; the single-writer invariant (ADR-020) would break.

## Consequences

- Positive: responsibility boundaries are unambiguous across the six domain teams (agent-tasks §1); the engine is testable with mock workers (ADR-021); reproducibility is enforced in one layer (RFC-0003); later milestones (recipes P4, adaptive P4, benchmarks ADR-029) plug into a stable runtime.
- Negative: engine does not produce reconstruction by itself — demo value in M0 comes from the mock pipeline and demo worker (ADR-031); process isolation adds IPC complexity and per-task process cost.
- Risks and mitigations: risk of boundary drift as real adapters arrive — mitigated by the §2 Constitution surface and Architecture Review; risk of duplicate ownership with `core/storage` — mitigated by the explicit delegation above and `check_constitution.py` prefix gates.

## References

- `rfc/RFC-0003-processing-engine-execution-architecture.md`
- `docs/architecture/engine.md`, `docs/architecture/scheduler-design.md` (Spatial Platform)
- `docs/specifications/task-model.md`, `docs/specifications/worker-protocol.md`
- ADR-008/009/010 (storage and CAS), ADR-011/012 (worker isolation and IPC), ADR-014 (errors), ADR-015 (logging/provenance), ADR-020 (scheduler persistence/cache), ADR-021 (mock adapters), ADR-026 (recipes), ADR-028 (workflow), ADR-031 (M0 scope), ADR-034 (capability plugins), ADR-037 (public API stability)
