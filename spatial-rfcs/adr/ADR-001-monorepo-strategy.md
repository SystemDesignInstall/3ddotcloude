# ADR-001 — Monorepo strategy

- **Status:** ratified
- **Owner:** Spatial Platform Architecture Board
- **Date:** 2026-08-04
- **Supersedes:** none

## Context

Spatial Platform ships a large number of tightly coupled components: the C++20 kernel (core, engine, adapters), the Python SDK, the CLI, adapters for third-party backends (COLMAP, OpenMVS, Open3D, GTSAM, Ceres, KISS-ICP), and research modules. These components share interface contracts — the .spx format, Protobuf IPC schemas, strict domain types, and coordinate conventions — that change together during cross-cutting refactors. A single source of truth reduces interface drift and version skew between components that must ship as one product.

## Decision

All C++/Python/CLI/UI/adapters live in one monorepo named "spatial-platform". Governance (RFCs, ADRs, constitution) lives in a separate "spatial-rfcs" repository; source code never lives there. CI runs once per repository with a single matrix (Linux + Windows, Debug + Release) and one set of gates. Releases are atomic across the whole product: the .spx format, protobuf schemas, C++ kernel, and Python SDK ship together under one version.

## Alternatives

- Multi-repo per component: rejected. Interface drift between Core, SDK, and adapters, painful cross-cutting refactors, and version skew between released components; coordination cost grows with the number of repositories.
- Vendor/tree-sync workflows: rejected. Brittle, hide the real dependency relationships, and cannot guarantee atomic releases.

## Consequences

- Positive: one dependency graph; atomic cross-component refactors (e.g., error model or coordinate-type changes); one CI pipeline with consistent gates; version consistency for the .spx contract; easier onboarding.
- Negative: large repository that needs careful path ownership; higher CI time and checkout size; risk of accidental cross-layer coupling.
- Risks and mitigations: enforce layering with the constitution-check (protected paths require RFC) and the architecture-review gate; keep governance in spatial-rfcs; use CMake presets and the Conan cache to keep CI fast.

## References

- docs/architecture/coordinate-systems.md
- ADR-002 (Language boundaries)
- ADR-003 (Dependency manager)
- ADR-008 (Project storage format)
