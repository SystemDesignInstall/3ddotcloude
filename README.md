# spatial-rfcs — Architecture Governance Repository

This repository is the **single source of truth** for architecture governance of the Spatial Platform. It follows the Rust RFC model.

- `adr/` — Architecture Decision Records (`ADR-001` … `ADR-037`). Decisions are numbered and final.
- `rfc/` — RFCs. Proposals for any change to the Constitution-protected surfaces.
- `proposals/` — Early, unnumbered ideas. May be promoted to RFC.
- `experimental/` — Experimental specifications. Tested outside the kernel; promoted to a ratified spec or abandoned.
- `CONSTITUTION.md` — The supreme governing document. Protected surfaces, the 16 Architecture Principles, and change control.

## RFC Lifecycle

```
proposal → draft → proposed → accepted → ratified → superseded
                                   ↘ rejected
```

1. **Draft** — the RFC author drafts `rfc/RFC-NNNN-title.md` using the template.
2. **Proposed** — the RFC is submitted for Architecture Review.
3. **Accepted** — Architecture Board accepts the direction; implementation may begin in a feature branch.
4. **Ratified** — the change is merged and, if it touches the Constitution, the Constitution is amended.
5. **Superseded** — replaced by a newer RFC. Never deleted; history is preserved.

## Rules

- **No agent or team may modify a Constitution-protected path without a ratified RFC** (see `CONSTITUTION.md` §2).
- RFC numbers are assigned sequentially and never reused.
- Every ADR has an owner, a status, and references from the specifications that use it.
- Commit/PR bodies touching protected paths must cite `RFC-NNNN`.

## ADR Index

| ID | Title |
|---|---|
| ADR-001 | Monorepo strategy |
| ADR-002 | Language boundaries: C++ and Python |
| ADR-003 | Dependency manager: Conan 2 |
| ADR-004 | COLMAP as canonical SfM backend |
| ADR-005 | GTSAM as unified sensor factor graph |
| ADR-006 | AI outputs are priors |
| ADR-007 | Coordinate-frame conventions |
| ADR-008 | Project storage format |
| ADR-009 | SQLite metadata and artifact separation |
| ADR-010 | Content-addressed artifact store |
| ADR-011 | Process worker isolation |
| ADR-012 | IPC protocol |
| ADR-013 | Plugin and adapter strategy |
| ADR-014 | Error handling |
| ADR-015 | Logging and provenance |
| ADR-016 | Testing strategy |
| ADR-017 | Cross-platform support |
| ADR-018 | Strict scalar and transform types |
| ADR-019 | Eigen adoption |
| ADR-020 | Scheduler persistence and task cache |
| ADR-021 | Mock adapters and interface isolation |
| ADR-022 | Toolchain bootstrapping |
| ADR-023 | Scene is the central domain object |
| ADR-024 | Observation Graph as canonical measurement substrate |
| ADR-025 | Per-point provenance and uncertainty as first-class data |
| ADR-026 | Recipes as versioned pipeline definitions |
| ADR-027 | Adaptive Reconstruction Intelligence |
| ADR-028 | Workflow durability and replay |
| ADR-029 | Benchmark framework as product evidence base |
| ADR-030 | Quality Engine as first-class |
| ADR-031 | M0 scope boundary |
| ADR-032 | Geometry Graph as independent domain layer |
| ADR-033 | Immutable Scene versioning |
| ADR-034 | Capability-based plugin architecture |
| ADR-035 | Scene Query API |
| ADR-036 | Digital Twin and temporal epochs |
| ADR-037 | Public API stability policy |
