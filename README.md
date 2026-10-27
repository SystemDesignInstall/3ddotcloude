# Spatial Platform

**Spatial Platform** is a commercial 3D reconstruction platform with a
scene-centric, geometry-first architecture: it ingests any sensor's output
(photos, LiDAR, video, IMU/GNSS, RGB-D) as immutable observations into a
persistent, versioned **Scene**, then turns them into geometry through
replaceable, capability-based adapters — positioned as the layer underneath
(and beyond) pipeline-style tools such as RealityCapture and Metashape. Instead
of shipping a single hard-wired reconstruction pipeline, the platform owns the
data substrate — provenance, uncertainty, versioning, and relationships — and
treats every reconstruction engine as a swappable worker behind an adapter.

## Key principles

- **One scene, one coordinate graph.** Every Scene carries a single,
  versioned coordinate graph; a transform without named source and target
  frames is invalid.
- **AI outputs are priors.** Learned models propose hypotheses (pose, depth,
  focal, masks); the classical geometric core validates and commits them as
  geometry — never the other way around (ADR-006).
- **Capabilities over names.** Components declare what they can do
  (`Capability`), and the engine selects implementations by capability, so
  COLMAP, OpenMVS, VGGT, or a future engine can be swapped without touching
  Core (ADR-034).
- **Immutable, content-addressed artifacts.** A computed result is written
  once, addressed by its SHA-256 content, and never modified (ADR-010).
- **Every geometry element has an explicit frame.** An element whose frame is
  unknown is a defect, and strict domain types replace raw matrices and
  untyped scalars in business logic (ADR-007, ADR-018).

## Repository layout

| Path | Purpose |
|---|---|
| `core/` | Kernel: Scene, coordinates, geometry, artifacts, plugin interfaces. Constitution-protected. |
| `engine/` | Capability implementations that turn observations into geometry. |
| `adapters/` | Third-party backend boundary (COLMAP, OpenMVS, ... behind interfaces). |
| `importers/` | Sensor/capture ingestion into the Scene and Observation Graph. |
| `engine/pipeline/` | Recipe compilation, orchestration and execution manifests. |
| Python SDK | Planned; not implemented in this checkout. Python worker helpers live under `engine/workers/`. |
| `cli/` | `spatial` command-line interface. |
| `docs/` | Specifications and architecture documentation. |
| `schemas/` | Cross-language contracts: protobuf, JSON Schema, SQL DDL. |
| `tests/` | Kernel, integration, and end-to-end test suites. |
| `docs/benchmarks/` | Benchmark framework specification; dataset-based evaluation remains pending. |

## Documentation

- [Product blueprint](docs/vision/platform-blueprint.md) — strategy and roadmap.
- [Scene model](docs/specifications/scene-model.md) — the central domain object.
- [Geometry model](docs/specifications/geometry-model.md) — `GeometryElement` types.
- [Worker protocol](docs/specifications/worker-protocol.md) — process isolation and IPC.
- [Build guide](docs/development/build.md) — Conan/CMake presets and toolchain policy.
- [Architecture Decision Records](https://github.com/SystemDesignInstall/spatial-rfcs/tree/main/adr) — ADR-001..ADR-037 in the governance repo.
- [Architecture Constitution](https://github.com/SystemDesignInstall/spatial-rfcs/blob/main/CONSTITUTION.md) — supreme governing document.

## Implementation status

Current baseline: P3.1 sparse-correction implementation, with production acceptance
still open. The runtime is substantially beyond the M0 skeleton:

- `.spx` project storage, immutable SHA-256 artifacts, SQLite metadata and provenance.
- Image import, sensor registration, canonical calibration and feature artifacts.
- Capability-based pipeline compiler, scheduler, process workers, retries and cache.
- COLMAP CLI adapter and canonical sparse Reconstruction v2 conversion.
- Trajectory / PoseGraph / loop-closure domain types, calibrated essential verification.
- Real GTSAM trajectory optimization and fixed-intrinsic bundle adjustment.
- `p3_sparse_correction` through Engine and CLI: loop verification → graph optimization
  → pose feedback → retriangulation → BA quality gate → revision lineage.

Synthetic geometric tests and a subprocess probe are not validation on a real
capture dataset. Trusted metric-scale provenance, native COLMAP validation and
remaining recovery/publication cases must be closed before production acceptance.
Multi-session registration, LiDAR fusion, dense reconstruction and AI remain
future work.

See the [current verification report](docs/architecture/P3.1-reliability-verification-2026-10-27.md)
and [milestone index](docs/architecture/P3-milestone-status.md).

## Build and test

Use the exact profile, output-folder and preset commands in the
[build guide](docs/development/build.md). Windows uses `default` / `release`;
Linux uses `linux-debug` / `linux-release`. All enable the real GTSAM adapter.

After installing locked dependencies:

```sh
cmake --preset linux-debug
cmake --build --preset linux-debug --parallel 2
ctest --preset linux-debug --parallel 2 --timeout 180
```

The C++ CLI is produced by the build. Project creation and inspection:

```sh
spatial project create demo.spx
spatial project info demo.spx
```

## Continuous validation

CI runs architecture gates and actual C++ configure/build/CTest jobs for
Ubuntu 24.04 and Windows 2022 in Debug and Release. The gates check protected
surface changes, RFC/ADR structure, domain types, worker isolation, architecture
debt, dependencies and schemas. See `.github/workflows/ci.yml` and the build guide.
