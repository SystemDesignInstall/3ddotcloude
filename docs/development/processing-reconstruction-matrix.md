# P2.3 (RFC-0007) M2: Processing & Reconstruction Architecture Matrix

- **Status:** decision-gate analysis — input to the Architecture Board decision; NOT a ratification
- **Scope:** M2 of the P2.3 exit gate (`docs/development/p2.3-completion-review.md` §10): capability → backend matrix, primary-source license verification, Robbyant gate, embeddability analysis, recommendation
- **Repo:** `spatial-platform` (`main`)
- **Date:** 2026-08-12
- **Next:** M3 — RFC-0008 in `spatial-rfcs` ratifies the first backend capability stack; **no production backend code before ratification**

## 1. Purpose and method

The four fixed M2 questions:

1. **Q1 — Which backend for which capability** (8 class-separated bands; no cross-class "who is better" claims).
2. **Q2 — Actual embeddability** per candidate as `Backend → Adapter → Canonical Input → Canonical Artifact` (adapter complexity, not "backend X can Y").
3. **Q3 — Robbyant gate** for the LingBot family (Map/Vision/Video/VA/World/VLA) + VGGT/gsplat/Nerfstudio, as an AI-neutral check (no AI advantage).
4. **Q4 — RECOMMENDATION** with the exact fields: Primary backend / Capability / Why / Canonical input / Canonical output / Adapter / Runtime / CPU/GPU / License / Risks / Fallback.

Method:

- Licenses verified from **primary sources** (upstream repository license files / README license statements) on 2026-08-12; no conclusions from memory. Citations in §2.
- Capability names come from the ratified taxonomy in `schemas/json/worker-capabilities.schema.json`; canonical artifact types from PPS-0001 §5.3; the adapter seam from `docs/development/adding-adapter.md` (9 steps); the selection framework from `docs/specifications/adaptive-engine.md` §4 and `docs/specifications/reconstruction-pipeline.md` §2.
- Registry semantics from ADR-003 / ADR-031 (`THIRD_PARTY.yml`, `MODEL_LICENSES.yml`): `status: planned` never links into the kernel; AI weights are governed by the MODEL_LICENSES rule — **AI outputs are priors, never authoritative geometry, and no non-commercial model enters the commercial build**.

## 2. Verified licenses (primary sources, 2026-08-12)

| Backend | code_license (verified) | model weights | commercial use | Verification source |
|---|---|---|---|---|
| **COLMAP** | BSD-3-Clause ("new BSD"; copyright ETH Zurich & UNC Chapel Hill) | n/a | yes | `colmap/README.md` ("licensed under the new BSD license", full BSD text) |
| **OpenMVS** | AGPL-3.0 | n/a | conditional — source disclosure / commercial license required | `cdcseacave/openMVS/LICENSE` |
| **GTSAM** | BSD-2-Clause (simplified BSD) | n/a | yes | `borglab/gtsam/LICENSE` (bundled third-party: CCOLAMD BSD-3, Ceres auto-diff BSD-3, Eigen MPL-2.0, METIS Apache-2.0, Spectra MPL-2.0) |
| **Open3D** | MIT | n/a | yes | `isl-org/Open3D/LICENSE` |
| **KISS-ICP** | MIT | n/a | yes | `PRBonn/kiss-icp/LICENSE` |
| **Ceres Solver** | BSD-3-Clause (core) + Apache-2.0 headers + MIT libmv-derived example code | n/a | yes | `ceres-solver/LICENSE` |
| **gsplat** | Apache-2.0 | n/a | yes | `nerfstudio-project/gsplat/LICENSE` |
| **Nerfstudio** | Apache-2.0 | n/a | yes | `nerfstudio-project/nerfstudio/LICENSE` |
| **VGGT** | Custom **"VGGT License" v1 (2025-07-29)** — commercial-use friendly code, **military/ITAR excluded** | **VGGT-1B checkpoint: non-commercial**; **VGGT-1B-Commercial: commercial via application form** (LLaMA-like approval) | conditional | `facebookresearch/vggt/LICENSE.txt` + README update (2025-07-29) |
| **LingBot-Map** | **UNVERIFIABLE — repository does not exist** | n/a | n/a | `https://github.com/LingBot/LingBot-Map` returns **404** (2026-08-12) |

Consequences:

- **VGGT status changed since the registry was written.** The code is no longer plain Apache-2.0: it is a custom Meta license that permits commercial use with an Acceptable Use Policy (no military/warfare, ITAR, weapons), is redistributable only under the same agreement, and can be amended by Meta "effective immediately". A verified **commercial checkpoint path now exists** (`VGGT-1B-Commercial`, via application). The original `VGGT-1B` remains non-commercial.
- **LingBot-Map registration is invalid** — the referenced repository returns 404. There is no verifiable capability, output, license, or artifact for any of the LingBot family. The candidate cannot be evaluated and cannot be planned around. Registry corrected in §7.
- The `check_dependencies` gate reports copyleft/other licenses on `planned` backends as **warnings** (legal review + separate-adapter process before activation); `commercial_use: conditional` also warns. This is by design (ADR-031), not a failure.
- COLMAP's README warns that **building with third-party dependencies may affect the resulting license**; the configured build (Ceres, PoseLib, SIFT-GPU, VLFeat) must be audited for permissive-only components.

## 3. Q1 — Capability → backend (class-separated)

Bands are separate classes; **no cross-band comparison is made**. Candidates listed per class; "primary" = first production slot for that class.

### Band 1 — Feature Extraction (`feature_extraction`, ratified P2.3)

| Candidate | License | Role | Notes |
|---|---|---|---|
| **COLMAP (SIFT)** | BSD-3-Clause | **primary** | `feature.schema.json` already reserves a packed/binary encoding for the COLMAP adapter (schema header). SIFT names are ratified with the COLMAP adapter. |
| OpenCV | Apache-2.0 | utility | Image I/O and feature utilities behind adapters; not a primary detector source. |
| VGGT | custom | prior only | Learned matching/geometry priors; ADR-006 gate, never authoritative features. |

### Band 2 — Pose / SfM (`sparse_reconstruction`, `bundle_adjustment`)

| Candidate | License | Role | Notes |
|---|---|---|---|
| **COLMAP** | BSD-3-Clause | **primary (default, ADR-004)** | Incremental and global (GLOMAP) mappers; Ceres-based BA built in. |
| OpenMVS | AGPL-3.0 | excluded from kernel | SfM capability present but AGPL; separate-adapter process after legal review only. |
| VGGT | custom | prior only | Feed-forward poses enter as validated priors (ADR-006), refine via classical BA. |

### Band 3 — Dense Reconstruction (`dense_stereo`; related `surface_reconstruction`, `texturing`)

| Candidate | License | Role | Notes |
|---|---|---|---|
| **COLMAP (PatchMatch stereo)** | BSD-3-Clause | **primary** | Depth fusion maps depth maps → dense Point elements. |
| OpenMVS | AGPL-3.0 | gated | Generally the stronger densifier/mesher/texturer in the class — but AGPL; separate-adapter process after legal review only. |
| Open3D | MIT | primary for surface | Poisson / ball-pivot surface reconstruction and texture tooling; permissive. |
| Ceres | BSD-3-Clause | supporting | Used inside dense solvers. |

Dense/Mesh/Texture are **out of P2 scope** (PPS-0001 §11, project-context §15); listed here for the stack decision.

### Band 4 — Registration (`icp`)

| Candidate | License | Role | Notes |
|---|---|---|---|
| **Open3D** | MIT | **primary** | Point-to-point / point-to-plane ICP, global registration; LiDAR ↔ survey-frame alignment. |
| Ceres | BSD-3-Clause | supporting | Underlying NLS solver for refined alignment. |
| KISS-ICP | MIT | odometry, not ICP | Registration between sensor frames belongs to odometry/SLAM (Band 6), not the ICP capability. |

### Band 5 — Optimization (`bundle_adjustment` as solver; general nonlinear least squares)

| Candidate | License | Role | Notes |
|---|---|---|---|
| **Ceres** | BSD-3-Clause | **primary solver** | Generic NLS minimizer; the BA engine inside COLMAP; usable standalone. |
| **GTSAM** | BSD-2-Clause | **primary factor-graph** | Probabilistic factor-graph optimization for sensor-fusion pose graphs (ADR-005). Distinct layer from Ceres: graph formulation vs. raw minimizer. |

### Band 6 — Mapping / SLAM (`lidar_odometry`, `loop_closure`, `gnss_integration`)

| Candidate | License | Role | Notes |
|---|---|---|---|
| **KISS-ICP** | MIT | **primary odometry** | Lightweight LiDAR odometry; no motion model dependency. |
| **GTSAM** | BSD-2-Clause | **primary loop closure / pose graph** | Factor graph fuses odometry, loop closures, GNSS constraints (ADR-005). |
| PROJ / GDAL | MIT | georeferencing | CRS/datum transforms for `gnss_integration` and survey-frame anchoring. |

SLAM is **out of P2 scope** (project-context §15); listed for the stack decision.

### Band 7 — Neural / AI Geometry (sparse & dense AI priors)

| Candidate | License | Role | Notes |
|---|---|---|---|
| **VGGT** | custom (commercial path exists) | **prior-only pipeline** | Pose/depth/point-map priors; ADR-006 gate; commercial only via VGGT-1B-Commercial (application). Adaptive engine prefers it when classical matching fails (adaptive-engine.md §4). |
| MASt3R / DUSt3R | non_commercial | excluded | Non-commercial research licenses; never in the commercial build (MODEL_LICENSES governing rule). |
| gsplat / Nerfstudio | Apache-2.0 | Band 8 | Geometry from these is prior/render-only, not authoritative (Band 8). |

### Band 8 — Neural Rendering (`gaussian_generation`; future NeRF)

| Candidate | License | Role | Notes |
|---|---|---|---|
| **gsplat** | Apache-2.0 | **primary** | CUDA 3D-GS rasterizer; consumes classical SfM poses; canonical Gaussian export per reconstruction-pipeline.md §2.7. |
| Nerfstudio | Apache-2.0 | secondary / later | NeRF framework; rendering and extracted mesh priors only; overlaps gsplat for view synthesis. |

Gaussian milestone is **out of P2 scope**.

## 4. Q2 — Embeddability (Backend → Adapter → Canonical Input → Canonical Artifact)

| Backend | Adapter form (adding-adapter.md steps 1–9) | Canonical input | Canonical output / artifact | Adapter complexity |
|---|---|---|---|---|
| **COLMAP** | process-worker (COLMAP CLI subprocess; `feature_extractor` → `matcher` → `mapper` → `model_converter`) | ImageArtifact set (CAS, per session) + intrinsics (`calibration.schema.json` `fov`/`brown_conrady`) | FeatureArtifact (SIFT, packed encoding reserved), SparseModel (poses, tracks, points3D), BA report; poses → `WorldFromSensor` + covariance | **HIGH** — first real adapter; must map COLMAP database/model formats faithfully and convert camera models to the canonical taxonomy |
| **OpenMVS** | separate process worker (AGPL isolation, never in-kernel) | COLMAP / SparseModel | DenseModel (depth/points), Mesh, Texture | HIGH + legal process |
| **GTSAM** | in-process library | pose constraints / odometry / tracks (SparseModel or LiDAR trajectory) | optimized poses (`WorldFromSensor` + covariance) | MEDIUM — factor-graph construction over the canonical constraint surface |
| **Ceres** | in-process library (or inside COLMAP adapter) | BA problem (tracks + projection residuals) | refined poses/points + covariance | LOW–MEDIUM — generic solver; needs problem assembly |
| **Open3D** | in-process library | Point geometry elements / dense model | mesh (Triangle elements), ICP alignment | LOW–MEDIUM |
| **KISS-ICP** | in-process library (ROS-free API) | LiDARObservation frames | trajectory → `WorldFromSensor` + point cloud | MEDIUM — frame-aligned point preprocessing; GNSS anchoring later |
| **VGGT** | out-of-process GPU model worker (PyTorch; weights from HF) | image set / video frames | pose/depth/point **priors** (ADR-006 gate); native COLMAP-format export (`demo_colmap.py`) | HIGH — GPU runtime, model serving, weight-license gate, prior validation |
| **gsplat** | out-of-process GPU training worker | SparseModel poses + images | Gaussian elements → GaussianSplatArtifact | MEDIUM–HIGH — training loop; deterministic checkpoints for resume (ADR-020) |
| **Nerfstudio** | out-of-process framework worker | poses + images | rendered views / mesh priors only | HIGH |
| **LingBot-Map** | **not embeddable** | — | — | n/a — repository 404 |

## 5. Q3 — Robbyant gate

Gate order (AI-neutral): **Actual output → Canonical Artifact feasibility → Metric/relative → Coordinate frame → Confidence → Temporal consistency → Classical geometry coexistence → Adapter feasibility.** No AI advantage is granted. "Not needed for P3" is a valid outcome.

### LingBot family (Map / Vision / Video / VA / World / VLA)

- **Actual output:** unverifiable — no public repository, no release, no documented capability.
- **Gate verdict:** **BLOCKED at the first gate.** `https://github.com/LingBot/LingBot-Map` returns **404 (verified 2026-08-12)**. A candidate with no repository cannot be evaluated on output, artifacts, metrics, frame, confidence, temporal consistency, coexistence, or adapter feasibility.
- **Decision:** NOT ELIGIBLE for any milestone until a repository and a verifiable license exist. Do not plan around the LingBot family; do not reserve adapter directory or registry space. This is the strongest form of the "not needed for P3" outcome: the input cannot even be assessed. If a real repository appears, this gate must be re-run before any registration.

### VGGT

- **Actual output:** feed-forward extrinsic + intrinsic, depth maps, point maps, 3D point tracks, from 1 to hundreds of views, in seconds.
- **Canonical Artifact feasibility:** maps to **camera/depth/point priors** — compatible with the platform only through the ADR-006 validation gate (priors on observations, never authoritative SparseModel). Native COLMAP-format export exists, so conversion to canonical artifacts is demonstrated.
- **Metric/relative:** relative frame by default; **not metric out of the box** — needs scale/GNSS anchoring for survey deliverables.
- **Coordinate frame:** arbitrary/relative; must be anchored to a survey frame via `GnssIntegration`/`ICP`.
- **Confidence:** depth/point heads emit confidence maps (`depth_conf`, `point_conf`) — usable for uncertainty channels (ADR-025); camera confidence not robustly exposed.
- **Temporal consistency:** cross-frame point tracks provide consistency; single-shot has no temporal guarantee; the VGGT-Omega follow-up (2026-05) targets long-sequence consistency.
- **Classical geometry coexistence:** this is VGGT's core value — classical BA (COLMAP/Ceres) refines VGGT poses, and VGGT rescues classical matching-degenerate scenes (textureless, repetitive, low overlap; adaptive-engine.md §4). Coexistence, not replacement.
- **Adapter feasibility:** HIGH complexity but demonstrated (COLMAP-format export, HF weights, GPU worker). License: commercial path exists (`VGGT-1B-Commercial`, application form; military excluded; custom license amendable by Meta).
- **Verdict:** eligible as a **prior-only research pipeline**; **not the first production backend**; commercial entry requires the VGGT-1B-Commercial checkpoint and application. **Not needed to unblock P3** (classical SfM/BA).

### gsplat

- **Actual output:** 3D Gaussian parameters (mean / covariance / SH / opacity / color) trained from posed images.
- **Canonical Artifact feasibility:** Gaussian elements → GaussianSplatArtifact (PPS-0001 §5.3); gsplat-compatible export per reconstruction-pipeline.md §2.7 — mapped.
- **Metric/relative:** view-synthesis PSNR/SSIM on hold-out views (Quality-stage metric); geometry is **not authoritative**.
- **Coordinate frame:** inherits the SfM/posed frame — cannot bootstrap itself.
- **Confidence:** implicit (per-view loss); no geometry confidence channel — acceptable for view synthesis, not measurement.
- **Temporal consistency:** static per-scene model; dynamic content out of scope.
- **Classical geometry coexistence:** consumes classical SfM poses (COLMAP or VGGT); depends on the sparse stack.
- **Adapter feasibility:** MEDIUM–HIGH (GPU training worker, deterministic checkpoint resume).
- **Verdict:** eligible for the **Gaussian milestone (post-P2)**; not needed for P3.

### Nerfstudio

- **Actual output:** NeRF renderings and extracted mesh priors from posed images.
- **Canonical Artifact feasibility:** mesh/rendering priors only — not authoritative geometry (ADR-006).
- **Metric/relative:** PSNR/SSIM; extracted geometry is weaker than explicit MVS for measurement.
- **Coordinate frame:** inherits the posed frame.
- **Confidence:** none.
- **Temporal consistency:** static scenes only (per-scene NeRF).
- **Classical geometry coexistence:** consumes classical poses; priors only.
- **Adapter feasibility:** HIGH (framework, GPU, heavy dependency surface).
- **Verdict:** eligible later for rendering/visualization; **low priority** — overlaps gsplat; not needed for P3.

## 6. Q4 — RECOMMENDATION

- **Primary backend:** COLMAP
- **Capability:** first capability stack — `feature_extraction` (SIFT) + `sparse_reconstruction` + `bundle_adjustment`, extending to `dense_stereo` (PatchMatch) when dense is in scope.
- **Why:** the only candidate that combines (a) **verified BSD-3-Clause** license, (b) **ratified default status** (ADR-004), (c) the full SfM capability set in one auditable toolchain (features → matching → mapping → BA), (d) model formats convertible to canonical artifacts with published converter tooling, (e) active maintenance (2026 releases; GLOMAP global mapper), (f) capability-first selection already wired — the engine selects by capability, never vendor. AI alternatives (VGGT) remain priors only (ADR-006).
- **Canonical input:** ImageArtifact set (CAS, per session) + intrinsics from `calibration.schema.json` (`fov`, `brown_conrady`) + optional GNSS/IMU/LiDAR priors.
- **Canonical output:** FeatureArtifact (SIFT; packed encoding reserved), SparseModel artifact (poses, tracks, points3D), BA report / QualityReport; poses as `WorldFromSensor` with covariance on the Observation Graph.
- **Adapter:** `adapters/colmap` process-worker wrapping the COLMAP CLI (`feature_extractor` → `matcher` → `mapper` → `model_converter`); added via adding-adapter.md steps 1–9 (Step 1 registry, Step 2 capability declaration, Step 3 ProcessingAdapter, Step 9 mock↔real parity).
- **Runtime:** out-of-process worker subprocess (ADR-028); COLMAP CLI per task; scheduler-managed DAG.
- **CPU/GPU:** CPU-first (SIFT + BA); optional CUDA for extraction speed; **GPU not required for P3**.
- **License:** BSD-3-Clause (verified from README, 2026-08-12). Caution: COLMAP's README notes third-party build dependencies may affect the resulting license — the configured build (Ceres / PoseLib / SIFT-GPU / VLFeat, all permissive) must be audited to exclude any GPL component.
- **Risks:** (1) **HIGH adapter complexity** — COLMAP's database/model formats need a faithful canonical mapping (first real adapter); (2) dense stereo CUDA dependency deferred; (3) license caution re: dependency configuration (above); (4) **OpenMVS (stronger dense/mesh) is AGPL** — kernel-excluded; separate-adapter after legal review only; (5) SIFT encoding must match `feature.schema.json`'s reserved packed encoding — decided in RFC-0008.
- **Fallback:** OpenMVS dense/mesh via separate-adapter process after legal review (AGPL); Open3D (MIT) for ICP/surface; GTSAM (BSD-2) for pose-graph/loop-closure alignment; Ceres (BSD-3) standalone BA if COLMAP BA is insufficient; VGGT (custom license, commercial checkpoint) as an AI-prior rescue path for matching-degenerate scenes — priors only, never authoritative.

**What RFC-0008 should ratify (decision summary for the Board):** the first backend capability stack = **COLMAP** (feature extraction + sparse reconstruction + bundle adjustment) with **Ceres / GTSAM / Open3D** as permissive secondary capability backends; **VGGT** as prior-only research pipeline (commercial via VGGT-1B-Commercial application); **gsplat** deferred to the Gaussian milestone; **OpenMVS excluded from the kernel** (AGPL); **LingBot family not eligible** (no repository). A stack, not a single program — the architecture stays backend-independent.

## 7. Registry updates applied in this commit

`THIRD_PARTY.yml`:
- **VGGT:** `code_license` → `VGGT License v1 (2025-07-29, custom; commercial-friendly, military excluded)`; `model_license` → `VGGT-1B non-commercial; VGGT-1B-Commercial commercial (application)`; notes updated with the verified commercial path.
- **LingBot-Map:** `status` → `removed`; notes → repository 404 (verified 2026-08-12); registration invalid.

`MODEL_LICENSES.yml`:
- **VGGT:** `commercial_use` kept `research_only_pending`; note updated — commercial path now exists via `VGGT-1B-Commercial` (application form; military excluded) pending legal/Architecture review.
- **LingBot-Map:** added as `verification_required` / `research_only_pending` with note — repository 404 (2026-08-12); removed from THIRD_PARTY.yml; registration invalid until a real repository appears.

## 8. Exit decision for M2

The Board decides the four questions; this matrix is the input. License verification is complete (primary sources, §2); the capability matrix is class-separated (§3); embeddability is scored per candidate (§4); the Robbyant gate produced two eligible-but-deferred AI outcomes (VGGT prior pipeline, gsplat later) and blocked the LingBot family (§5); the recommendation is a COLMAP-led capability stack (§6).

If accepted: proceed to **M3** — RFC-0008 in `spatial-rfcs` ratifies the stack and the `core/plugin/**` / `adapters/interfaces/**` build. No production backend code before ratification: the backend choice changes the capability contract, adapter interface, artifact contract, runtime/resource requirements, GPU model, and configuration/provenance surfaces.
