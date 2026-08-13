# ADR-028 — Workflow Durability and Replay

- **Status:** accepted
- **Owner:** Spatial Platform Architecture Board
- **Date:** 2026-08-04
- **Supersedes:** none

## Context

A commercial reconstruction session is not a single batch job: users import, validate, optimize, inspect, improve, and finalize over many iterations and across sessions and machines. Intermediate results are too expensive to recompute, and mistakes must be reversible. Immutable artifacts (ADR-010) and the persisted scheduler (ADR-020) give us durable primitives, but the user-facing concept of a workflow — an ordered, named progression of stages with checkpoints — does not yet exist. Without it, "undo that alignment" or "compare last night's dense run" are impossible to express.

## Decision

A Workflow is an ordered sequence of stages: Import → Validate → Optimize → Review → Improve → Finalize. Every stage is durable, replayable, undoable, and comparable. Durability means stage results persist as immutable artifacts and scheduler state, so a session survives restarts and machine hand-off. Replayability means re-running a stage with identical inputs and recipe produces identical results via the scheduler cache (ADR-020, ADR-026). Undoability is realized through scene versioning: each stage produces a new immutable Scene version, so stepping back is moving the active pointer to a parent version (ADR-033). Comparability means adjacent versions and stage outputs can be diffed through the Scene Query API (ADR-035) and quantified by the Quality Engine (ADR-030). The workflow layer is a thin, user-facing ordering on top of scheduler tasks; it does not replace the DAG scheduler.

**Deferred to:** post-M0 (Photogrammetry milestone) — workflow stage semantics, checkpoint UI, and replay orchestration; the version-chain metadata that underpins undoability ships in M0 (ADR-033).

## Alternatives

- **Workflow as a single long-running pipeline:** rejected — no checkpoints, no undo, and no cross-session resilience.
- **Workflow as purely a UI concept:** rejected — durability must live in persisted data, not in view state, or replay and comparison are impossible.
- **Recompute-on-demand for undo:** rejected — contradicts the reproducibility and cache semantics of ADR-020 and wastes compute.

## Consequences

- Positive: long sessions are safe and resumable; undo and compare become data operations, not UI tricks; each workflow stage records provenance; teams collaborate on a shared session.
- Negative: additional version and snapshot bookkeeping; some stages cannot be reversed if their inputs were manually deleted — undo is bounded by retained versions; workflow naming must be stable for comparability.
- Risks and mitigations: risk of unbounded storage growth from retained versions — mitigated by CAS dedup and GC policy (ADR-010, ADR-033); risk of user confusion about which version is active — mitigated by explicit version pointer and rename/rebase guidance in the workflow layer.

## References

- `docs/specifications/reconstruction-pipeline.md`
- `docs/specifications/task-model.md`
- ADR-020 (scheduler persistence), ADR-026 (recipes), ADR-030 (quality engine), ADR-033 (immutable scene versioning), ADR-035 (scene query API)
