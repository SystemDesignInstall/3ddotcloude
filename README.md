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
| `pipelines/` | Recipe-driven orchestration and workflow execution. |
| `python/` | Python SDK (`spatial_sdk`), research modules, AI worker stubs. |
| `cli/` | `spatial` command-line interface. |
| `docs/` | Specifications and architecture documentation. |
| `schemas/` | Cross-language contracts: protobuf, JSON Schema, SQL DDL. |
| `tests/` | Kernel, integration, and end-to-end test suites. |
| `benchmarks/` | Benchmark harness and dataset-based evaluation (ADR-029). |

## Documentation

- [Product blueprint](docs/vision/platform-blueprint.md) — strategy and roadmap.
- [Scene model](docs/specifications/scene-model.md) — the central domain object.
- [Geometry model](docs/specifications/geometry-model.md) — `GeometryElement` types.
- [Worker protocol](docs/specifications/worker-protocol.md) — process isolation and IPC.
- [Build guide](docs/development/build.md) — Conan/CMake presets and toolchain policy.
- [Architecture Decision Records](https://github.com/spatial-platform/spatial-rfcs/tree/main/adr) — ADR-001..ADR-037 in the governance repo.
- [Architecture Constitution](https://github.com/spatial-platform/spatial-rfcs/blob/main/CONSTITUTION.md) — supreme governing document.

## M0 status

M0 is the engineering skeleton (ADR-031). Everything below is built, tested,
and shipped; everything after it is deferred.

**In M0**

- Project Core (`.spx` storage container)
- Artifact Store (CAS, SHA-256, atomic temp+rename, UUID manifests)
- Strict coordinates and typed transforms (`WorldFromCamera`, `RigFromSensor`,
  `TimestampNs`, `DistanceMeters`, ...) with a Coordinate Frame Graph
- Scene + Observation Graph (immutable observations, sensor/rig/calibration)
- Scheduler (DAG, retries, cancellation, persisted state, task cache)
- Worker Protocol (separate processes, Protobuf frames, heartbeat, cancellation)
- Python SDK, CLI, and mock adapters

**Explicitly NOT in M0**

- Photogrammetry / SfM / MVS backends
- SLAM / LiDAR registration
- AI models and inference
- Gaussian Splatting
- Qt / Vulkan / CUDA rendering
- Cloud / distributed processing

## Build and test

M0 dependencies are pinned to `eigen`, `protobuf`, `sqlite3`, `nlohmann-json`,
and `gtest` (see `conanfile.txt` and `THIRD_PARTY.yml`); the Conan 2 recipes,
profiles, and CMake presets ship with P1 (ADR-022). The commands below are the
single supported build flow.

**Windows** (Visual Studio 2022 Build Tools, CMake >= 3.28, Python 3.11):

```bat
python -m pip install conan
conan profile show msvc2022-x64
conan install . --build=missing
cmake --preset default
cmake --build --preset default
ctest --test-dir build/default --output-on-failure
pytest
```

**Linux** (gcc-12 / clang-16, CMake >= 3.28, Python 3.11):

```bash
python -m pip install conan
conan profile show linux-gcc12
conan install . --build=missing
cmake --preset default
cmake --build --preset default
ctest --test-dir build/default --output-on-failure
pytest
```

Use `cmake --preset release` for the Release configuration. Configure through
presets only — ad hoc configuration is a review failure (ADR-022).

## Getting started

```bash
python -m pip install -e python/spatial_sdk
spatial project create demo.spx
spatial project info demo.spx
```

`spatial project create` initializes a `.spx` project with the metadata
database and artifact store. From there, importers feed observations into the
Scene and mock adapters round-trip the full pipeline without any third-party
backend (ADR-021).

## Continuous validation

CI (`.github/workflows/ci.yml`) enforces these gates on every change:

| Gate | Script | What it enforces |
|---|---|---|
| Constitution change control | `scripts/check_constitution.py` | Protected paths need an RFC reference |
| RFC / ADR governance | `scripts/check_rfc.py` | ADR/RFC naming, structure, index completeness |
| Domain types | `scripts/check_domain_types.py` | No raw Eigen types outside `core/geometry/` and `adapters/` |
| Architecture debt | `scripts/check_arch_debt.py` | Debt = 0 in the kernel (debt markers are rejected) |
| Dependency registry | `scripts/check_dependencies.py` | Every dependency registered; permissive licenses in the kernel |
| Schema validation | `scripts/check_schemas.py` | Protobuf/JSON-Schema/SQL contract conformance |
