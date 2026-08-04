# Algorithm Documentation — Index

- **Status:** placeholder
- **References:** ADR-031 (M0 scope), ADR-021 (mock adapters), ADR-034 (capability architecture), ADR-006 (AI = priors)
- **Scope:** documents describing the reconstruction algorithms the platform integrates through adapters.

## Status of this index

**No algorithm is implemented in M0.** M0 ships interfaces and mock adapters only (ADR-031, ADR-021). Every entry below is therefore marked **not implemented in M0; interfaces + mock only**. The documents are placeholders that will be written when the corresponding algorithm milestone lands (Photogrammetry, SLAM, AI). Each algorithm's normative contract today is the capability taxonomy (`SparseReconstruction`, `DenseStereo`, `BundleAdjustment`, `ICP`, `SurfaceReconstruction`, `Texturing`, `GaussianGeneration`, `LidarOdometry`, `LoopClosure`, `GnssIntegration`) and the adapter descriptors that declare it.

## Planned algorithm documents

Each planned document will specify: the algorithm's role in the canonical pipeline (Import → Validate → Optimize → Review → Improve → Finalize), its capabilities, its inputs/outputs against the Scene and artifact contracts, its interface contract and mock behavior, its failure semantics (deterministic vs. recoverable), and its quality criteria (ADR-016, `reconstruction-pipeline.md`). None exist yet; the table below is the intended index.

| Document | Algorithm | Capability | Status |
|---|---|---|---|
| `structure-from-motion.md` | SfM (sparse reconstruction, COLMAP canonical backend, ADR-004) | `SparseReconstruction` | not implemented in M0; interfaces + mock only |
| `dense-multi-view-stereo.md` | dense MVS (depth maps → fused dense points) | `DenseStereo` | not implemented in M0; interfaces + mock only |
| `bundle-adjustment.md` | BA (optimization, GTSAM factor graph, ADR-005) | `BundleAdjustment` | not implemented in M0; interfaces + mock only |
| `icp-registration.md` | ICP (registration/alignment, e.g. survey frame) | `ICP` | not implemented in M0; interfaces + mock only |
| `lidar-odometry.md` | LiDAR odometry (KISS-ICP) | `LidarOdometry`, `LoopClosure` | not implemented in M0; interfaces + mock only |
| `gaussian-splatting.md` | 3D Gaussian splatting (training/rendering, gsplat backend) | `GaussianGeneration` | not implemented in M0; interfaces + mock only |
| `neural-priors.md` | learned priors (VGGT/MASt3R/DUSt3R/LingBot-Map: depth, poses, semantics, gaussians) | priors only, never authoritative geometry | not implemented in M0; interfaces + mock only |

## Cross-cutting rules for all algorithm documents

- **AI = priors.** Neural outputs enter a Scene only as priors attached to observations, validated by the classical core before anything becomes geometry (ADR-006, Constitution Principle 4).
- **Replaceable.** Replacing COLMAP with OpenMVG or VGGT changes only an adapter, never Core (Principle 15). Algorithm documents describe the *interface contract and mock behavior*, not a Core coupling.
- **Capability-driven.** Each algorithm maps to capabilities; the engine selects implementations by capability, never by vendor name (ADR-034).
- **Geometry-first.** Every algorithm contributes geometry by emitting `GeometryElement`s (Point/Triangle/Voxel/Gaussian) through its adapter; Core never depends on a producer's native representation.
- **AI = priors only.** Neural algorithms are documented as prior providers with a validation gate, never as authoritative geometry.
- **M0 interfaces + mocks.** Until a milestone lands, the normative contract is the capability taxonomy and the mock adapters that implement it; golden fixtures generated from a verified backend keep mocks honest (ADR-016, ADR-021).
- **Canonical defaults.** Where a backend is the ratified default (COLMAP for SfM/BA, ADR-004; GTSAM for factor-graph optimization, ADR-005), the algorithm document records that default and the replaceability proof required to swap it (Principle 15).
- **Quality bar.** Each algorithm document states its recipe quality targets (reprojection error, coverage, density, watertightness, view-synthesis metrics) so the Review stage and the adaptive engine can judge it (ADR-027, ADR-030).

## Adding an algorithm document

Write the document when its milestone lands (or when the interface contract changes), referencing its adapter descriptor and capability declarations. Algorithm docs are advisory; the normative contract is the capability/descriptor schema, which is Constitution-protected.

## References

- `docs/specifications/geometry-model.md`, `docs/specifications/reconstruction-pipeline.md`
- `docs/development/adding-adapter.md`, `docs/architecture/process-model.md`
- ADR-004, ADR-005, ADR-006, ADR-021, ADR-031, ADR-034
