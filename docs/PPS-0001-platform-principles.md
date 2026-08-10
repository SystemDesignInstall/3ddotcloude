# PPS-0001 — Platform Principles Specification

- **Status:** draft (P2)
- **References:** CONSTITUTION.md, RFC-0002, RFC-0003, ADR-004, ADR-006, ADR-007, ADR-010, ADR-013, ADR-018, ADR-020, ADR-025, ADR-030, ADR-033, ADR-034, `docs/specifications/reconstruction-pipeline.md`, `docs/specifications/sensor-model.md`, `docs/specifications/artifact-format.md`, `schemas/json/calibration.schema.json`
- **Protected surface:** the contracts fixed here live under `schemas/**`, `core/scene/**`, `core/artifacts/**`, `adapters/interfaces/**`, `importers/images/**`, `pipelines/**`; changes require a ratified RFC cited in the commit/PR body (CONSTITUTION §2).

## 1. Summary

The Spatial Platform is a system for managing **permanent spatial data**, not a reconstruction system. Reconstruction algorithms are temporary interpretations of permanent observations: *every algorithm is ephemeral, every observation is permanent* (RFC-0002). PPS-0001 fixes the architectural invariants that make this true — a canonical spatial data model, an artifact-centric and provenance-rich storage model, and strict isolation between external libraries and the platform core.

PPS-0001 is the architectural layer that binds the two ratified architecture pillars:

```
CONSTITUTION.md
        │
        ▼
PPS-0001 — Platform Principles Specification
        │
   ┌────┴────┐
   ▼         ▼
RFC-0002    RFC-0003
Spatial     Processing
Model       Engine
```

- **CONSTITUTION.md** — the project's philosophy, principles, and change control;
- **PPS-0001** — the architectural invariants of the domain layer (this document);
- **RFC-0002** — the permanent spatial data model;
- **RFC-0003** — the processing engine and execution architecture.

PPS-0001 does not introduce new ideas wholesale — it **formalizes** constraints already present in the ratified foundations: every processing result is a separate artifact, every artifact carries provenance, and processing is independent of any specific algorithm.

## 2. Motivation

The platform will outlive every reconstruction algorithm it hosts. COLMAP, OpenMVG, AliceVision, neural reconstructions, and Gaussian Splatting will come and go; the observations they consume and the artifacts they produce must remain valid forever. Without an invariant layer, two failures are guaranteed:

1. **Engine coupling** — external library types leak into the core, making every algorithm replacement a core rewrite.
2. **Model drift** — ad hoc artifact shapes, implicit coordinate frames, and unreferenced results accumulate until the data model can no longer be relied upon.

PPS-0001 exists to protect the long-term architecture, guarantee algorithm replaceability, preserve reproducibility, and ensure compatibility of future components. It is deliberately algorithm-agnostic and hardware-agnostic: the document never describes a specific reconstruction algorithm, a Processing Engine implementation, or an external library API.

## 3. Scope

PPS-0001 applies to every component that produces, consumes, or stores spatial data: the kernel (`core/**`), the engine (`engine/**`), adapters (`adapters/**`), importers (`importers/**`), pipelines (`pipelines/**`), schemas (`schemas/**`), and the CLI/SDK. It binds at the domain layer — one level above the Constitution's 16 principles, one level below concrete specifications such as `reconstruction-pipeline.md`.

Out of scope: domain-specific contracts of individual pipelines (camera-model parameterization, artifact payload schemas, adapter I/O for specific backends). These belong to pipeline and P2.x specifications and must not contradict this document.

## 4. Architecture Philosophy

The platform's architecture is governed by the following principles. They are the "mini-constitution" of the domain layer and are evaluated against the top-level Constitution (§1 of CONSTITUTION.md).

1. **All spatial artifacts are immutable.** An artifact is created once, receives a unique identity, and is never changed; a change produces a new artifact (RFC-0002, §5.3).
2. **Calibration is versioned independently.** Intrinsics/extrinsics live on versioned `Calibration` records (`sensor-model.md` §3.1), never inside image metadata; every observation references the calibration valid at capture time.
3. **Every processing stage produces a new artifact.** Stages are pure functions over scene versions and artifacts; nothing is mutated in place (ADR-033).
4. **The platform owns its spatial data model.** No external library type crosses the Adapter Boundary (§5.1, §5.6).
5. **Algorithms are interchangeable through Capability interfaces.** Adapters declare capabilities, never vendor names; the engine selects by capability (ADR-034).
6. **Quality is evaluated independently of reconstruction.** Quality measurement is a separate stage, never a by-product of a reconstruction stage (ADR-030).
7. **Every reconstruction is reproducible.** Identical inputs, recipe, code version, and configuration reproduce identical outputs (ADR-020).
8. **Provenance is never discarded.** Every artifact retains its input chain, producer, and configuration (§5.7).
9. **Uncertainty propagates forward.** Every stage either reduces uncertainty or passes it downstream explicitly (ADR-025, §6.2).
10. **Determinism.** Processing Engine stages are deterministic to the extent the algorithm, hardware platform, and external dependencies allow (§6.3).

---

## 5. Normative Principles

Violation of any rule in this part is an **architectural violation** of the platform.

### 5.1 Canonical Spatial Data Model

**Principle.** The Spatial Platform uses its own canonical spatial data model. External libraries are only sources of computation.

```
                 External World

   COLMAP      OpenMVG      Open3D      GTSAM      Custom Algorithms

          │
          ▼

      Adapter Layer

          │
          ▼

  Canonical Spatial Model

          │
          ▼

     Processing Engine

          │
          ▼

     Spatial Artifacts
```

**Rule.** No external data type may cross the Adapter Boundary.

Forbidden:

```cpp
engine.process(colmap::Image);      // external type reaches the engine
storage.save(openmvg::SfM_Data);    // external type reaches storage
```

Allowed:

```cpp
engine.process(CameraFrame);        // canonical type
storage.save(SparseModelArtifact);  // canonical type
```

Only platform-native types live inside the platform. Replacing COLMAP with OpenMVG, AliceVision, or a custom engine must not require changes to Core, Scene, or the Artifact Store (CONSTITUTION principles 8, 15).

### 5.2 Artifact-Centric Architecture

**Principle.** The platform is organized around artifacts. Raw data is ingested, processed, and every result of processing is a separate artifact.

```
Raw data → Processing → Artifact → Derived Result
```

**Rule.** All spatial entities MUST exist inside a **Capture Session** or a derived **Spatial Context** (RFC-0002: mandatory session context for data creation).

```
Observation
      ↓
Capture Session
      ↓
Artifact
      ↓
Derived World Model
```

An observation is a fact recorded by a sensor at a time and place; it is never mutated. Derived results are artifacts that reference their inputs by content hash and their originating context by identifier.

### 5.3 Artifact Immutability

**Principle.** All spatial artifacts — `image`, `feature`, `match`, `sparse_model`, `dense_model`, `mesh`, `texture`, `quality_report` — are immutable. An artifact:

- is created once;
- receives a unique identifier;
- is never changed.

A state change produces a **new** artifact.

```
SparseModel v1
      |
      | bundle adjustment
      ▼
SparseModel v2
```

Not:

```
SparseModel v1
      |
      | UPDATE
      ▼
SparseModel v1
```

Reasons: reproducibility, audit trail, rollback, distributed processing, cache correctness (ADR-010, ADR-020).

### 5.4 Stable Identity System

Every permanent platform object carries a **Stable ID**.

| ID | Entity |
|---|---|
| `CaptureSessionID` | capture session |
| `SensorID` | sensor (device) |
| `FrameID` | frame / image |
| `ObservationID` | observation record |
| `ArtifactID` | artifact (`artifact_uuid`) |
| `ProcessingRunID` | processing run / execution manifest |

**Requirements.** An ID must be:

- globally unique;
- permanent — stable across sessions and runs;
- independent of memory addresses;
- independent of processing order;
- independent of the producing algorithm.

Forbidden:

```cpp
uint64_t objectAddress;   // a pointer address is never an identifier
```

### 5.5 Coordinate Frame Rules

The coordinate system is part of the data model. There is **no implicit coordinate system** in the platform.

**World Frame.** Right-handed; axes `X`, `Y`, `Z` per project convention (ECEF or local tangent / survey frame); origin recorded on the scene. Rotation convention: quaternion `(x, y, z, w)`, right-handed (ADR-007, ADR-018).

**Units (normative).**

| Domain | Canonical unit |
|---|---|
| World coordinates | meters |
| Pixel coordinates | pixels |
| Angles | radians |
| Time | nanoseconds |
| Confidence | [0, 1] |
| Probability | [0, 1] |

**Transform convention.** All transforms are named `T_from_to` (e.g. `T_world_camera`) and use the strict transform types (quaternion + translation), never raw matrices:

```
P_world = T_world_camera × P_camera
```

Rules:

- Every element/artifact carries an explicit `coordinate_frame` (CONSTITUTION principle 13; `artifact-format.md` §3).
- `WorldFromCamera` is the canonical pose type on cameras.
- A frame boundary is crossed only through a named transform that appears in the provenance chain.

### 5.6 Adapter Isolation

The Adapter is the **single point of contact** with an external library. The Scheduler and the Engine manage **Processing Capabilities**, never libraries.

```
✅
Scheduler
   |
   ▼
Processing Capability
   |
   ▼
Adapter
   |
   ▼
Canonical Artifact
```

```
❌
Scheduler
   |
   ▼
COLMAP Task
```

The Adapter must perform:

- **type conversion** — external structures → canonical types (§5.1), following the canonical naming rules (§5.8) and unit conventions (§5.5);
- **unit normalization** — all values normalized to canonical units before crossing the boundary;
- **validity checks** — reject or correct values outside the canonical model before they enter the platform;
- **metadata transfer** — provenance, configuration, and parameters carried into the canonical representation.

An adapter is the **only** component allowed to mention an external library by name.

### 5.7 Provenance Requirement

Every artifact must carry provenance. Minimum:

```
Artifact
 ├── CreatedBy
 ├── ProcessingRun
 ├── InputArtifacts
 ├── Parameters
 ├── SoftwareVersion
 └── Timestamp
```

Example:

```
SparseModelArtifact
CreatedBy:     COLMAP Adapter
Input:         FeatureArtifact, MatchArtifact
Parameters:    SIFT, RANSAC, BA iterations=100
Software:      spatial-core 1.2.0
```

Provenance is stored as a DAG of `input_artifact_hashes` + `producer` + `configuration_hash` (`artifact-format.md` §7). It is queryable from any artifact back to raw observations, and a cache hit (ADR-020) is recorded as a synthetic producer so cached outputs remain fully auditable.

### 5.8 Canonical Naming Rules

Internal domain concepts are independent of external libraries. Canonical names:

```
ImageArtifact
FeatureArtifact
MatchArtifact
SparseModelArtifact
DenseModelArtifact
MeshArtifact
TextureArtifact
QualityReport
```

Never:

```
ColmapImage
OpenMVGFeature
Open3DMesh
GtsamPose
```

### 5.9 Algorithm Independence

**Principle.** The Spatial Platform architecture MUST remain valid regardless of the algorithm used to produce spatial results.

Today:

```
COLMAP
   ↓
SparseModelArtifact
```

Tomorrow:

```
Neural Reconstruction
   ↓
SparseModelArtifact
```

For the platform the result is identical: a canonical `SparseModelArtifact` with provenance. The choice of algorithm affects only the Adapter layer, never the Core, the Scene, the Artifact Store, or the Scheduler (CONSTITUTION principles 8, 15, 16).

---

## 6. Recommended Policies

The following sections are recommended practice, not hard invariants.

### 6.1 Numeric Representation

**Default.** The canonical mathematical representation is `float64` for coordinates, poses, and calibration parameters.

**Exception.** A component may use another format inside its implementation — GPU, SIMD, Tensor backends, half precision. The Canonical Model boundary must preserve the required precision when converting to and from `float64`.

Descriptor vectors default to `uint8` or `float32`; images retain their original bit depth.

### 6.2 Uncertainty Propagation

Every processing stage must either:

```
reduce uncertainty
```

or:

```
preserve and propagate uncertainty
```

Pipeline:

```
Calibration → Feature Extraction → Matching → Pose Estimation →
Bundle Adjustment → Sparse Reconstruction → Dense Reconstruction →
Quality Evaluation
```

Every artifact may carry `Measurement + Uncertainty + Confidence` (ADR-025). This is why the Quality Report exists: error never disappears — it propagates and is measured.

### 6.3 Deterministic Execution

Processing Engine stages must be deterministic to the extent the algorithm, hardware platform, and external dependencies allow. A future `RFC-XXXX Deterministic Execution Model` may define levels (best effort, deterministic pipeline, bitwise reproducible) once real GPU and distributed backends exist.

---

## 7. Extension Guidelines

Non-normative direction for future evolution.

### 7.1 Capability Evolution

The Capability namespace is an **extensible public namespace**, never a closed list.

Current examples:

```
Import
FeatureExtraction
FeatureMatching
SparseReconstruction
Validation
```

Future examples (non-normative):

```
DenseReconstruction
Meshing
Texturing
Registration
Alignment
SemanticSegmentation
ChangeDetection
DigitalTwinGeneration
Export
```

(The stage names used by `reconstruction-pipeline.md` — `DenseStereo`, `SurfaceReconstruction`, `Texturing`, `GaussianGeneration`, `ICP`, `GnssIntegration`, `LoopClosure`, `LidarOdometry` — map onto these examples. Neither list is exhaustive.)

### 7.2 Future Artifact Types

Reserved directions only — not commitments:

```
NeuralSceneArtifact
GaussianSplatArtifact
VoxelArtifact
SemanticSceneArtifact
DigitalTwinArtifact
```

A reserved type may be promoted to an implemented artifact only through the normal change-control path: a specification section plus, for its schema, a ratified RFC.

### 7.3 Long-Term Vision

The Spatial Platform may evolve through:

```
Capture
   ↓
Understanding
   ↓
Simulation
   ↓
Digital Twin
   ↓
Autonomous Reasoning
```

This is direction, not an architectural obligation.

---

## 8. Compliance Criteria

An architectural change is valid only if every applicable criterion below passes. An agent or team proposing a change MUST evaluate it against this checklist before touching the architecture.

### Data Model

☐ new entities are represented through the Canonical Spatial Model;

☐ there are no dependencies on external library types.

### Artifact

☐ every result of processing is an Artifact;

☐ every Artifact has provenance (CreatedBy, ProcessingRun, InputArtifacts, Parameters, SoftwareVersion, Timestamp);

☐ every Artifact is immutable — a change produces a new Artifact.

### Processing

☐ the Pipeline is independent of any specific algorithm;

☐ every Processing Step produces a new Artifact;

☐ the Scheduler manages Processing Capabilities, not libraries.

### Coordinates

☐ all spatial data has an explicit Coordinate Frame;

☐ units of measurement are explicit.

A change that fails any applicable criterion is rejected until the criterion is satisfied or a ratified RFC formally amends this specification.

## 9. References

- CONSTITUTION.md — §1 (principles 8, 13, 15, 16), §2 (protected surfaces), §3 (what may change without an RFC).
- RFC-0002 — Permanent Spatial Data Model (session context, artifact provenance, observation permanence).
- RFC-0003 — Processing Engine and Execution Architecture (Task model, Scheduler, Workers).
- ADR-004 (COLMAP as canonical SfM backend), ADR-006 (AI outputs are priors), ADR-007 (coordinate-frame conventions), ADR-010 (content-addressed artifact store), ADR-013 (plugin and adapter strategy), ADR-018 (strict scalar and transform types), ADR-020 (scheduler persistence and task cache), ADR-025 (per-point provenance and uncertainty), ADR-030 (Quality Engine as first-class), ADR-033 (immutable scene versioning), ADR-034 (capability-based plugin architecture).
- `docs/specifications/reconstruction-pipeline.md`, `docs/specifications/sensor-model.md`, `docs/specifications/artifact-format.md`.
- `schemas/json/calibration.schema.json`.
