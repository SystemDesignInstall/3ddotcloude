# P3-impl-7c — Metric Loop-Closure Pose-Graph Edges: Implementation Specification & Readiness

| | |
|---|---|
| Status | READINESS v2 — decisions D1–D6 **CLOSED** (this revision), **no code changed** |
| Intended work | `P3-impl-7c` (drift correction via metric loop-closure edges) |
| Axes touched | `core`, `adapters/gtsam`, `engine/pipeline`, `schemas`, `tests` |
| Prior work | 7a (offline pose-graph CLI, `fb1a333`), 7b (geometric loop-closure verification, `8418235`) |
| Inherited trait | Build on the 7b seam: `LoopClosure` + `VerificationResult` (incl. `has_relative_pose`) |
| Owner verdict | CONDITIONAL PASS on v1; this revision implements the six conditions (D1–D6). GO gated on owner's look at §14. |

---

## 1. Purpose and normative decisions

This document is the implementation specification for **P3-impl-7c** and a
readiness statement for the owner. It fixes decision **Option B (calibrated
Essential path)** and, on the scale question, decision **Option 1
(trajectory-derived metric scale)** — as the sole normative scale source.

Decisions marked **[NORM]** are binding on the implementation. Section 15 lists
the items the owner must confirm (a "GO") before code starts.

### Decisions in force

- **D-7c-1 [NORM]** The metric scale of a loop-closure relative-pose
  measurement is **never** derived from the Essential matrix itself. The
  Essential/calibrated multi-view estimate provides the rotation (metric,
  unambiguous after cheirality) and the unit translation *direction* only.
  Magnitude comes from the **existing metric trajectory** (Option 1). See §3.
- **D-7c-2 [NORM]** An uncalibrated (`F`) verification result **never** produces
  a metric `PoseGraphEdge`, and identity/zero measurements are never used as a
  substitute for an unavailable relative pose. See §5, §11.
- **D-7c-3 [NORM]** `spatial_separation_m` is the Euclidean separation of the
  source/target **trajectory pose nodes**, and is **not** the metric relative
  translation of a loop edge. They are different quantities and must never be
  conflated. See §6.
- **D-7c-4 [NORM]** The edge measurement convention is fixed as
  `T_source_target` with rotation/quaternion terms as documented in §4.
  Directionality is enforced once, centrally, to prevent inverted translations.
- **D-7c-5 [NORM]** GTSAM stays behind the `TrajectoryOptimizer` core seam
  (§7); the engine never includes a GTSAM header.
- **D-7c-6 [NORM]** The engine produces loop edges only via a new
  `loop_closure_to_pose_graph` stage (§7) which is the single consumer of
  verified metric closures.

### Boundary confirmations (unchanged from 7a/7b plan)

- 7c does **not** implement: executor wiring for the full pipeline (7a.1,
  deferred), multi-session/map-merge, no-CAS/Synthetic-Images path, LiDAR
  handover, GNSS edge injection, or retiming. The synthetic 6c path and the
  real 7a/7b path remain structurally parallel; 7c extends the real path only.
- The pre-existing 6c domain-type debt at `pose_graph_helpers.h:48,69`
  (`check_domain_types` gate) is **out of scope** and must not be "fixed"
  opportunistically in 7c (except where 7c itself must touch those functions —
  see §8).
- No raw-Eigen domain API is introduced. Transforming between representations
  happens inside the adapters; edge payloads are the canonical std-array types.

---

## 2. "Metric-ness" requirement — prose then machine-checkable

> A trajectory is *metric* only if a machine-checkable declaration exists that
> its `position_xyz` values are metres in a physical (or physical-equivalent)
> scale anchored to a calibrated baseline or inertial/odometry scale.

`double xyz ≠ metric`. In the planned E2E the trajectory typically comes from a
COLMAP adapter (`adapters/colmap/colmap_trajectory_adapter.cpp`, `kind="sfm"`),
whose `position_xyz` are **SfM-reconstruction units** with no declared scale:

- `Trajectory` and `TrajectoryPoseNode` (`core/trajectory/trajectory.h`) carry
  **no** scale/unit field. Proveance is `ReconstructionProvenance`.
- The only "meter" declaration in the platform is
  `ArtifactManifest.unit` (default `"meter"`) — a manifest-level label, not a
  scale certificate.
- `coordinate_frame` names a frame; it does not state units.

**Consequence.** Under Option 1, computing `lambda` (scale) from a trajectory
whose metric basis is **undeclared** would itself be fabrication. Therefore:

- **D-7c-7 [NORM]** The metric-basis declaration is a precondition for any
  metric loop edge. A trajectory is eligible only when, **for the frames in
  question**, an explicit metric-basis record (`EligibleTheta`; see §3) is
  present and valid.
- **D-7c-8 [NORM]** The 7c E2E must use an explicitly metric baseline (e.g. a
  synthetic metric odometry trajectory with known metre scale, or COLMAP output
  accompanied by a validated scale-to-metre calibration). A raw COLMAP-only
  trajectory must produce **no** metric loop edge (documented negative test,
  §13).

---

## 3. Option 1 — scale source of truth (math, normative)

### 3.1 Inputs

Frames (normative, D6, see also §4):

- **W** — trajectory world/local frame (`coordinate_frame`, e.g. `"trajectory_0"`).
  `TrajectoryPoseNode.position_xyz` are coordinates **in W**.
- **C_s, C_t** — source and target *camera* frames.
- **R_ws** — rotation part of the source node `T_trajectory_camera`: maps
  C_s → W (a point's coordinates in the source camera frame, expressed in W).
  Derived from `rotation_xyzw` (scalar-last). (I.e. `R_ws` is the "world from
  camera" rotation of the source pose.)

Let frame `s` (source) and frame `t` (target) be the endpoints of an
**accepted calibrated loop closure** whose geometric verification produced a
valid Essential decomposition:

- `R_ess` — 3×3 rotation recovered from the Essential matrix under the
  convention `x_t = R_ess · x_s + t_ess · ρ` (Appendix A), unique after
  cheirality. Maps C_s → C_t. (Metric — real rotation.)
- `t̂_ess` — unit translation direction recovered from `E`, **expressed in the
  source camera frame C_s** (the epipolar translation `t` of Appendix A lives
  in the source-camera coordinate system).
- `cheirality_ok = true` — the chosen `(R_ess, t̂_ess)` paired with
  `λ_provisional` (any positive value, sign only) places points in front of
  both cameras.

And the metric trajectory provides, at frames `s` and `t`:

- `p_s_ww`, `p_t_ww` — `position_xyz` of the pose nodes, i.e. the camera-centre
  coordinates **in W**.
- `q_s_ww`, `q_t_ww` — `rotation_xyzw` (scalar-last); used only for frame
  alignment (`R_ws`), not for magnitude.

### 3.2 Scale definition (the only normative scale) — with explicit frames (D6)

The trajectory-implied separation of the camera centres in W:

```
delta_p_W = p_t_ww − p_s_ww            (vector in W)
```

The Essential translation direction must be brought into the **same frame**
before any scalar product is taken. Since `t̂_ess` is given in C_s:

```
t̂_W = R_ws · t̂_ess                     (t̂ expressed in W)
```

The **trajectory-derived metric scale along the Essential direction** is

```
λ = dot(t̂_W, delta_p_W) = dot(R_ws · t̂_ess, p_t_ww − p_s_ww)
```

with `cos_theta = dot(t̂_W, delta_p_hat_W)`, `delta_p_hat_W = delta_p_W / ||delta_p_W||`.

> Equivalent formulation, both normative-equivalent; implement **one** and
> document it: bring `delta_p` into C_s instead:
> `λ = dot(t̂_ess, R_wsᵀ · delta_p_W)`. The two are identical because rotation is
> an isometry (`dot(R a, R b) = dot(a, b)`). The implementation must pick one,
> state it in code comments, and be covered by T2 (sign/frame test).

Frames of every quantity (normative table, D6):

| Quantity | Frame | Meaning |
|---|---|---|
| `R_ws` | C_s → W | source-camera rotation, from trajectory node `rotation_xyzw` |
| `R_ess` | C_s → C_t | rotation recovered from Essential (Appendix A) |
| `t̂_ess` | C_s | unit translation direction (Essential) |
| `delta_p_W` | W | `p_t_ww − p_s_ww` (trajectory-implied separation) |
| `λ` | scalar (m) | signed projection `dot(t̂_W, delta_p_W)` |
| `t_metric_Cs` | C_s | `λ · t̂_ess` (metric translation, target w.r.t. source) |
| `PoseGraphEdge.relative_position_xyz` | C_s | `t_metric_Cs` (per §4.2 / D6) |
| `PoseGraphEdge.relative_rotation_xyzw` | C_s → C_t basis | `R_ess` (per §4.4 / D6) |

### 3.3 Consistency guard

- **D-7c-9 [NORM]** If `cos_theta < cos_theta_min` (chosen in
  `VerificationOptions`, default `0.5`, i.e. `>= 60 deg`) the Essential
  direction and the trajectory direction disagree beyond tolerance → the
  closure is still *verified*, but **no metric edge is produced** (deterministic
  return of "not metric-eligible"). No scale fallback is invented.
- **D-7c-10 [NORM]** `λ` must be strictly positive and finite; if
  `||delta_p_W|| == 0` (coincident nodes) the edge is not metric-eligible either
  (D1: zero-baseline → rejection, see §14-D1).

### 3.4 Metric relative pose

```
t_metric_Cs = λ · t̂_ess                              (metres, C_s)
R_s2t = R_ess                                        (C_s → C_t)
T_source_target_metric = (R_s2t, t_metric_Cs)
```

The edge is built from `(R_s2t, t_metric_Cs)` exactly as the calibrated
relative pose, then framed per §4 (including any self-inverse alignment needed
because monocular E yields a camera-to-camera map while the edge stores the
`T_source_target` convention).

### 3.5 Why this is not fabrication

- R: measured by the calibrated Essential path (real metric rotation).
- Direction: measured by the calibrated Essential path (unit).
- Magnitude: inherited from the existing metric trajectory's own scale, which
  is the platform's declared metre basis — it is **not** invented, not normalised
  to `||t||=1`, not set to 1/0/`spatial_separation_m`/a constant.

### 3.6 Explicitly forbidden constructions

1. `t_metric = t̂_ess` (unit scale) — inventing "1 metre".
2. `t_metric = 0` or identity rotation — substituting a trivial measurement.
3. `λ = spatial_separation_m` — conflating two quantities (§6).
4. `λ = epsilon` / arbitrary constant tuned to make the graph "work".
5. `R` or `t` taken directly (unverified) from the trajectory at metric
   eligible frames — the rotation and direction must come from the Essential
   path, not from the pose graph prior.
6. Taking a dot product between `t̂_ess` (C_s) and `delta_p_W` (W) **without**
   the frame transformation of §3.2 — a directional error that silently
   changes the meaning of `λ`. Guarded by D-7c-11 [NORM]: every scalar product
   over coordinates must operate on quantities declared in the same frame
   (the §3.2 table).

### 3.7 Frame-algebra requirement (D-7c-11 [NORM])

Implemented and unit-tested together with T2:
- the builder never dots `t̂_ess` with `delta_p_W` directly;
- `R_ws` is derived from the source node `rotation_xyzw` by one documented
  scalar-last→matrix conversion (reused via an existing `core/coordinates`
  helper if present, otherwise a local private one with a `unit` test);
- `relative_rotation_xyzw` on the edge carries the **quaternion of `R_ess`**
  (the measured rotation), not of `R_wsᵀ·R_wt` (the prior).

---

## 4. Coordinate-frame and direction conventions [NORM]

### 4.1 Trajectory pose nodes

`TrajectoryPoseNode.position_xyz` and `rotation_xyzw` represent
`T_trajectory_camera` = world-from-body in the trajectory's **local** frame
(`core/trajectory/trajectory.h:35-36,42-43`), quaternion scalar-last
(ADR-007). These fields are metadata for alignment; they are never the edge
measurement (§3).

### 4.2 Edge payload

`PoseGraphEdge` (`core/trajectory/pose_graph.h`) with:
- `relative_position_xyz` = translation of `T_source_target`, expressed **in
  the source camera frame C_s** (3 metres).

  > Convention (matches the claim in `pose_graph_helpers.h:302-304`): the
  > position is the translation expressed in the **source pose frame** that maps
  > the source camera pose to the target camera pose. Under D6 this is exactly
  > `t_metric_Cs = λ · t̂_ess` of §3.4.
- `relative_rotation_xyzw` = rotation of `T_source_target` = the quaternion of
  `R_ess` (measured), scalar-last `(x, y, z, w)` per ADR-007. The edge's
  rotation is **never** `R_wsᵀ·R_wt` (trajectory prior).
- `information_matrix_6x6` = 6×6 in **row-major**, **translation-first then
  rotation** (see §9; schema `pose-graph.schema.json`),
  `relative_position_xyz` and `relative_rotation_xyzw` are both `required` in
  the schema — so a loop edge always carries a full 6-DOF measurement. (This is
  why F-only closures must not reach edge construction at all, §11.)

### 4.3 Directionality guard

Monocular Essential decomposition yields 4 candidate `(R, t)` under the
epipolar convention `E = [t]_x R` mapping camera-1 coordinates into camera-2 —
the solution that places triangulated points in front of **both** cameras is
chosen (`cheirality`). Under D6 the mapping is `x_t = R_ess · x_s + ρ·t̂_ess`
with `source == camera1`, `target == camera2`, so the epipolar translation
`t` is (sign-)the metric direction in C_s. The edge stores `T_source_target`
per §4.2, which is the same convention, but the binding is centralized:

- the D6 frame map (§3.2) is implemented **once** in `BuildMetricLoopClosureEdge`;
- a dedicated unit test (`InvertedTranslationGuard`, §13 T2) verifies sign:
  a deliberately inverted `t̂_ess` must change the produced edge translation
  sign (and must either be rejected by `cos_theta ≤ 0` or produce a clearly
  opposite `relative_position_xyz`);
- the engine must not reverse signs locally anywhere else.

### 4.4 Rotation-vs-prior rule

`BuildMetricLoopClosureEdge` must produce the edge rotation from
`(R_ess, cheirality)` alone. The trajectory rotation `R_ws` is used **only** for
the D6-alignment of `λ` (§3.2), never as the edge's rotation block. This keeps
the loop constraint an independent measurement (the point of 7c): its rotation
comes from the Essential geometry, not from the (drifting) trajectory prior.

### 4.5 Coordinate-frame provenance

The Essential membership (intrinsics `K`) comes from
`Calibration` (`core/scene/sensor/sensor.h:34`) resolved via
`SceneQuery::ResolveCalibrationAt` (`core/scene/query/scene_query.h:49`) and
materialized by `MaterializeCalibrationArtifact`
(`core/scene/query/calibration_materializer.h:38`) / `WriteCalibrationArtifact`
(`core/artifacts/calibration_artifact.h:71`). Calibrated path only — `F`/no-K
closures never reach this code (§11).

---

## 5. Non-metric trajectory handling [NORM]

If the trajectory is not metric-eligible (§2, §3.3) for the frames in question:

1. The verified closure **may still persist** as a visual verification record
   (`VerificationResult` with `has_relative_pose=false`), consistent with 7b.
2. **No** `PoseGraphEdge` of type `"loop_closure"` is created.
3. **No fallback scale is invented.** Explicitly forbidden (repeated from
   D-7c-1/D-7c-2): scale 1, scale 0, `λ = spatial_separation_m`, arbitrary
   constant, "unit-SfM" reconstruction posing as metric, or
   identity/zero measurement.

The check is performed by `loop_closure_to_pose_graph` (engine, §7) before any
edge is built; the metric-basis predicate lives in core
(`MetricEligibleTrajectoryBasis`, §8) so it is testable without the engine.

### 5.1 D4 — metric-basis declaration (provenance-derived, not caller-asserted)

A metric-basis declaration is **machine-checkable**, carries **provenance**, and
is **derived, not free-form**: any pipeline that can
`{"metric": true}` will just say `true`. Approved (normative) representation:

```
metric_basis: {
  declared:      true|false        // may never be true "by fiat"
  source:        "trajectory" | "reconstruction" | "combined"
  basis:         "odometry_scale" | "calibrated_baseline" | "sfm_metric_aligned"
                | "ground_truth"
  provenance:    { backend, version, configuration_hash, git_commit }  // D-CRM-11
  scale_calibration_ref:  <CAS/artifact ref of the calibration that fixes scale>
}
```

Constraints:
- `declared == true` requires a **matching** `basis` and a non-empty
  `scale_calibration_ref` + `provenance.configuration_hash`. A bare
  `declared: true` (no basis, no ref, no provenance hash) fails validation.
- The field is stored on the **trajectory** (root) alongside
  `ReconstructionProvenance` reuse (`core/reconstruction/reconstruction.h:87`),
  and surfaced in the trajectory CAS payload/schema (`trajectory.schema.json`).
  Deterministic validation lives in core (`ValidateMetricBasis`, §8).
- **Invariant [max-conviction]:** *metric eligibility must be derived from
  trusted provenance, not from a caller assertion.* A component that merely
  wants an edge cannot set the flag; only the scale-producing pipeline
  (odometry adapter, calibrated-baseline stage, metric COLMAP alignment) can;
  its provenance record is the evidence.

If metric basis is absent:

```text
Verified visual closure  →  persistable  →  NO PoseGraph metric edge
```

which the negative E2E (§12) must prove.

---

## 6. `spatial_separation_m` semantics [NORM] (D2 — closed)

- **Meaning.** Euclidean separation of the **trajectory pose nodes** of the
  source and target frames: `|| p_t_ww - p_s_ww ||` in metres
  (the 6c `PositionDistance` / D-LC-06 heritage).
- **It is a diagnostic / quality field, not a translation measurement.** The
  essential invariant:

  ```
  spatial_separation_m  ≠  ||relative_position_xyz||
  ```

  They agree only in contrived aligned cases; in general the edge translation
  is `λ·t̂_ess` (direction-sensitive, cheirality-sign-sensitive) while
  `spatial_separation_m` is a direction-agnostic magnitude of the trajectory
  prior. They must never be conflated, and `spatial_separation_m` is never
  used as the edge's translation magnitude.
- **Required by contract:** `loop-closure.schema.json` requires
  `spatial_separation_m`; it is currently stored as `0.0` everywhere because the
  verifier never computes it (`engine/pipeline/loop_closure_verification.cpp:48`
  and `:114` pass it through untouched). **7c fixes this**: the field is
  filled from the trajectory nodes at the source/target frames, published for
  provenance alongside the metric edge, and a docstring + schema description
  stating the exact semantics above is added.

---

## 7. Architecture: seams and wiring

### 7.1 New core seam `TrajectoryOptimizer`

A core interface (header-only, `core/trajectory/optimizer.h`):

```
struct OptimizationInput {
  PoseGraph graph;                 // edges incl. metric loop closures
  AnchorPriorConfig anchor_prior;  // as today (position-only prior in GTSAM impl)
  std::vector<TrajectoryPoseNode> nodes;   // initial values
};

struct OptimizationOutput {
  std::vector<TrajectoryPoseNode> optimized_nodes;  // canonical types (std::array)
  OptimizationTrace trace;         // per-iteration stats, converged, error
};

class TrajectoryOptimizer {
 public:
  virtual ~TrajectoryOptimizer() = default;
  virtual OptimizationOutput optimize(const OptimizationInput&) = 0;
};
```

The **GTSAM adapter** (`adapters/gtsam/gtsam_optimizer_adapter.h`) implements
`TrajectoryOptimizer`. The existing free function `optimize()` in
`gtsam_optimizer_adapter.cpp` becomes the internal implementation of this
interface (kept private-to-adapter; engine calls only the interface). GTSAM
types stay out of `engine/` (D-7c-5). The 6c synthetic test path may keep its
header-only helpers; the canonical path all use the seam.

### 7.2 New engine stage `loop_closure_to_pose_graph`

`engine/pipeline/loop_closure_to_pose_graph.cpp`:

- Consumes accepted 7b `VerificationResult`s (`has_relative_pose=true`) +
  `Trajectory` + calibration material.
- Resolves source/target frames → trajectory pose nodes
  (`TrajectoryPoseNode.frame_id` bridge, D-TRJ-10).
- Checks metric eligibility (§2, §3.3); for eligible frames computes the metric
  relative pose via the Option-1 formula (§3.4), builds the edge with
  `BuildMetricLoopClosureEdge` (§8), else skips the edge.
- Computes/publishes `spatial_separation_m` (§6).
- Dispatches to `TrajectoryOptimizer` seam, then persists
  `PoseGraph`/`OptimizationResult`/status per the DB model
  (`metadata_db.h`: `PoseGraphRow:215`, `OptimizationResultRow:260`).

**Call graph:** 7b `loop_closure_verification` → **`loop_closure_to_pose_graph`**
→ `TrajectoryOptimizer` seam → GTSAM adapter. Nothing in `engine/` may
`#include` GTSAM.

### 7.3 (Rejected) Option A — identity loop measurement

Explicitly rejected by the owner. The 6c E2E identity loop edge
(`pose_graph_helpers.h:336-337`, `DriftReductionWithLoop` in
`tests/unit/test_gtsam_adapter.cpp:1539-1629`) remains a *synthetic* test of the
optimizer only. The real path never emits an identity/zero measurement and
never uses info-magnitude-inflated identity as a substitute (D-7c-2).

---

## 8. `BuildLoopClosureEdge` scope of change [NORM]

7c replaces/augments the core loop-edge construction in
`core/trajectory/pose_graph_helpers.h`:

- **`BuildLoopClosureEdge` (lines 307-348):** today hard-codes
  `relative_position_xyz = {0,0,0}` and identity rotation with isotropic info
  (`MakeIsotropicInfo6` defaults 50.0/50.0). Under 7c this fabrication is **no
  longer the implementation used for real closures**. The function is amended
  so that:
  - it **requires** a metric-relative-pose measurement parameter (no default),
  - when no measurement is provided it returns `std::nullopt` (documented as
    "must not produce an edge for a visually-verified-only closure"),
  - validation via `ValidateInformationMatrix` stays.
- **New `BuildMetricLoopClosureEdge`**: builds the edge from Option-1
  `(R_s2t, t_s2t)` + information matrix (§9); performs all frame/direction
  normalization per §4 and the consistency guard per §3.3; implements
  `MetricEligibleTrajectoryBasis` (metric-basis check, §5).
- **Untouched:** `DetectCandidates` (6c synthetic, line 211) and
  `VerifyCandidate` (line 242) — the synthetic vs real paths stay parallel.
- **Untouched:** `pose_graph_helpers.h:48,69` (6c domain-type debt) — not part
  of 7c; do not opportunistically fix.

---

## 9. GTSAM covariance / information matrix [NORM] (D3 — closed)

- Payload: `PoseGraphEdge.information_matrix_6x6`, 6×6 row-major, stored on the
  canonical edge type (not invented in the adapter).
- Adapter mapping (exists): `informationToNoiseModel` in
  `gtsam_optimizer_adapter.cpp:91-96` builds
  `noiseModel::Gaussian::Information` (permutes to GTSAM tangent ordering);
  `edge.type == "odometry" | "loop_closure" | "lidar_odometry"` →
  `BetweenFactor<::gtsam::Pose3>` (lines 293, 301-307). No change needed to
  this mapping; the seam (§7.1) wraps it.
- **Edge uncertainty — deterministic, confidence-derived, conservative
  (normative).** This is an **engineering approximation, documented as such** —
  it is *not* a physically derived covariance. It depends only on verifier
  evidence, so every run is reproducible. Inputs:

  | Symbol | Source |
  |---|---|
  | `inlier_count` | Essential RANSAC inliers (verification) |
  | `inlier_ratio` | `inlier_count / matches` (verification) |
  | `geometric_residual` | mean reprojection residual (px) of accepted inliers |
  | `baseline_m` | `||delta_p_W||` from the metric trajectory (scale support) |
  | `cos_theta` | consistency `dot(t̂_W, delta_p̂_W)` (D-7c-9) |
  | `lambda` | option-1 scale (§3.2) |

- **Deterministic formula (normative default):** build a diagonal
  information matrix in (translation×3, rotation×3) row-major order:

  ```
  confidence_c = clamp(inlier_ratio / 0.6, 0, 1)                 // ratio quality
  confidence_n = clamp((inlier_count - 15) / 25.0, 0, 1)          // count support
  confidence_res = clamp(1 - geometric_residual / 2.0, 0, 1)      // residual (px)
  confidence_align = clamp(cos_theta / 0.5, 0, 1)                 // scale/pairing (D-7c-9)
  Q = confidence_c * confidence_n * confidence_res * confidence_align
  Q = max(Q, 0.01)   // floor: never a zero/identity matrix, never an
                     // uninvertible matrix — but only reached when the
                     // consistency gate already passed (cos_theta >= 0.5).

  sigma_t = baseline_m * (0.25 / Q)          // conservative: scale-proportional
  sigma_r = 0.05 rad * (1.0 / Q)            // conservative rotation std

  diagonal entries:  1/(sigma_t^2)   ×3, then  1/(sigma_r^2)  ×3
  ```

- The same formula is used for both translation and rotation blocks with the
  above (documented, tunable in `VerificationOptions`, defaults frozen for the
  milestone) — but it stays **deterministic** and monotone in all five inputs.
- **Must hold (validated by `ValidateInformationMatrix`):** finite, symmetric,
  positive definite; satisfies the existing GTSAM information contract; **no
  magic `50.0`** (the 6c default); **no magic `300.0`** (the E2E constant).
  `ValidateInformationMatrix` (used in `BuildLoopClosureEdge`) is the gate.
- `anchor_prior.information_scale` and the GTSAM "gps" position-only
  `PriorFactor` behavior (lines 278-281, 310-328) are unchanged.

---

## 10. 7b → metric verifier → PoseGraph connection (D5 — closed)

- **`LoopClosure` carries the measured geometry (normative).** After
  verification the result is self-contained:

  ```text
  LoopClosure
   ├── source_frame_id / target_frame_id
   ├── verification       (status, confidence, inlier stats, has_relative_pose)
   ├── R_ess              (cheirality-resolved rotation; C_s → C_t)
   ├── t_direction        (unit t̂_ess, C_s)
   ├── spatial_separation_m
   └── (final, after scale resolution)
        t_metric = λ * t̂_ess  →  PoseGraphEdge
  ```

- **t̂ vs metric translation are never conflated.** `LoopClosure` stores the
  **unit direction** `t_direction` (pre-scale). The metric translation
  `t_metric = λ·t̂_ess` is produced **only** by the scale-resolution step
  (`BuildMetricLoopClosureEdge`, §8), which consumes the metric-basis
  declaration. Downstream PoseGraph/optimizer consumers see only
  `relative_position_xyz` (never raw `t̂`).
- 7b already produces `VerificationResult` with `has_relative_pose`; the 7c
  `loop_closure_to_pose_graph` stage becomes the single consumer of the metric
  branch. This addresses the "unwired seam" identified in the 7a/7b audit.
- The `LoopClosure` type today (`core/trajectory/loop_closure.h`) has **no**
  transform fields; 7c extends it per the tree above (schema
  `loop-closure.schema.json` updated with `R`, `t_direction`, and the metric
  information block; both remain optional — only filled for calibrated-accepted
  closures).
- The DB already offers `PoseGraphRow`/`OptimizationResultRow` and trajectory
  status values (`"reconstructing"|"succeeded"|"failed"|"superseded"`, pose-graph
  `"building"|"ready"|"optimizing"|"optimized"|"failed"`) — 7c must implement an
  UPDATE path (currently only INSERT exists) to record status after edge
  insertion/optimization.

---

## 11. F-only closures

- `VerificationResult` with `has_relative_pose=false` (uncalibrated `F`) is
  **persisted as visual verification** and is the **end** of its processing:
  it never becomes a `PoseGraphEdge`, never reaches `TrajectoryOptimizer`, never
  contributes to pose-graph optimization.
- Identical treatment if the calibrated path fails cheirality or the consistency
  guard (§3.3) — verified but not metric-eligible ⇒ no edge (D-7c-9).

---

## 12. E2E and what it "proves"

The 7c proof is a **closed-loop, genuinely-metric** trajectory: a metric
odometry trajectory (simulated or synthetic-images) with a real calibrated
Essential closure between distant frames, driving:

1. `loop_closure_verification` (7b) accepts the closure with a real relative
   pose; `has_relative_pose=true`;
2. `loop_closure_to_pose_graph` produces **one** metric `PoseGraphEdge` whose
   `relative_position_xyz`, `relative_rotation_xyzw`, and
   `information_matrix_6x6` match the Option-1 formula within documented error;
   `spatial_separation_m` carries the trajectory-node separation and is
   demonstrably **different** from the edge translation;
3. `TrajectoryOptimizer` (GTSAM seam) reduces trajectory position error vs. the
   non-closed baseline.

**Quantitative success criterion (mandatory, D-7c-12 [NORM])** — "GTSAM
returned SUCCESS" is not proof. The E2E must assert, against the ground-truth
trajectory:

```
RMSE_position,GL (after loop)  <  RMSE_position,noLoops (baseline)
```

- before/after are measured on the **same** ground-truth knots;
- the loop edge is the **only** difference between the two runs;
- the claim is reported numerically (e.g. `RMSE 0.040 m → 0.012 m`) and asserts
  a documented improvement margin (default `≥ 30%`), not merely "converged".

And the **negative proof** (mandatory, D-7c-13 [NORM]) — same COLMAP-only
trajectory with undeclared scale (§2, §5.1):

- **no** metric edge is created; run exits `OK`;
- the result document states `metric-basis: absent → no edge`;
- **the trajectory poses remain numerically unmodified** (the optimizer never
  ran on loop edges); asserted by comparing output nodes to input nodes within
  machine epsilon.

What the E2E **does not** prove (must be stated): sub-metre absolute accuracy,
real-world GPS/GNSS calibration, and multi-session consistency. 7c's claim is
"metric loop closure → correct scale → measurable drift reduction on a
genuinely-metric trajectory", with correctness counters at every stage and a
checkable negative case proving the guard rail holds.

---

## 13. Test matrix (draft)

| ID | Area | Assertion |
|----|------|-----------|
| T1 | Option-1 math | `λ = dot(t̂_W, delta_p_W)`; edge translation equals projection, R equals cheirality-resolved `R_ess` (D6 frame table §3.2) |
| T2 | Directionality | `InvertedTranslationGuard`: edge `T_source_target` consistent with trajectory frames; sign-flipped `t̂_ess` changes edge translation sign and is either rejected by `cos_theta ≤ 0` or yields clearly-opposite `relative_position_xyz`; the D6 transformation `R_ws · t̂_ess` is exercised (never a raw dot in C_s vs W) |
| T3 | Consistency guard | `cos_theta < 0.5` → no metric edge, closure still verified |
| T4 | Non-metric (D4) | undeclared-scale COLMAP-only trajectory → no metric edge, no fallback (D-7c-8); `metrics_basis.declared=false` validation rejects `declared:true` with no basis/ref/hash |
| T5 | `spatial_separation_m` (D2) | filled, equals `||p_t-p_s||_W`, ≠ `||relative_position_xyz||`, and metadata distinguishes it from edge translation |
| T6 | `BuildMetricLoopClosureEdge` | identity/no-measurement input → `std::nullopt`, never fabricated edge; no magic info constants present |
| T7 | `TrajectoryOptimizer` seam | engine calls interface; GTSAM impl produces optimised nodes (`test_gtsam_adapter` `RunOptimize :1512` reused) |
| T8 | Info matrix (D3) | formula deterministic in all 5 inputs; finite, symmetric, positive definite; monotone in quality; validated by `ValidateInformationMatrix`; no constant-50/300 provenance in real path |
| T9 | E2E drift (D-7c-12) | metric closed-loop: `RMSE_after < RMSE_before`, margin ≥ 30%, on same ground-truth knots, loop edge the only difference |
| T10 | Negative E2E (D-7c-13) | no metric basis → no edge → trajectory nodes numerically unmodified (machine epsilon), run OK |
| T11 | Concurrency/status | UPDATE status path on `PoseGraphRow` after edge insertion + optimization |
| T12 | `LoopClosure` payload (D5) | cl presents `(R_ess, t̂_ess)` (unit), never `t_metric`; conflation compile-guard/documentation test `t_hat` vs `t_metric` |

---

## 14. Decisions D1–D6 — CLOSED (owner ratified)

### D1 — zero baseline → rejection only [CLOSED]

If `||delta_p_W|| ≈ 0` (coincident nodes / exact revisitation): the Essential
matrix gives no reliable translation direction and projection `λ` is undefined
by construction. **Not metric-eligible**; `BuildMetricLoopClosureEdge` returns
`std::nullopt`; closure persists as verified-visual only. An exact revisitation
is **never** converted into a metric constraint (specifically: no identity-edge
"zero-baseline + high-confidence info" shortcut; that is Option A, rejected).
Effective: `D-7c-10` stands; test T3 extended with a `baseline ≈ 0` case.

### D2 — `spatial_separation_m` is diagnostic only [CLOSED]

Fixed as in §6: `spatial_separation_m = ||p_t_ww − p_s_ww||` (trajectory nodes,
W). It is a **diagnostic / quality field**, never the edge translation, and
never `‖relative_position_xyz‖`. Where it is computed: in
`loop_closure_to_pose_graph` (keeps 7b unchanged). Test T5.

### D3 — deterministic confidence-derived information model [CLOSED]

As in §9 — a **documented engineering approximation**, not a physical
covariance. Diagonal 6×6, `(1/σ_t²)×3, (1/σ_r²)×3`, with `σ_t` and `σ_r` from
the five verifier-quality inputs via the frozen default formula (§9). Finite,
symmetric, positive-definite, monotone in quality, gated by
`ValidateInformationMatrix`. No `50.0`/`300.0` magics in the real path. The
name constraint also applies to *all* of 7c: no component may claim this is a
physically accurate covariance.

### D4 — metric basis is provenance-derived [CLOSED]

As in §5.1: `metric_basis` on the trajectory root, `{declared, source, basis,
provenance, scale_calibration_ref}`; validation rejects bare
`declared:true`. Invariant (max-conviction): *metric eligibility is derived
from trusted provenance, not from a caller assertion.* Absent basis ⇒ no metric
edge (negative E2E T10). Schema form: extend `trajectory.schema.json` with
`metric_basis` (§5.1).

### D5 — `LoopClosure` carries measured geometry [CLOSED]

As in §10: `LoopClosure` stores `(R_ess, t̂_ess (unit))` + verification stats;
`t_metric = λ·t̂_ess` is produced only by the scale-resolution step
(`BuildMetricLoopClosureEdge`). `t̂` and `t_metric` are distinct quantities and
are never conflated; PoseGraph/optimizer never see raw `t̂`. Schema updated
(`loop-closure.schema.json`). Test T12.

### D6 — frame convention for `λ` [CLOSED]

The §3.2 table is normative. Dot products over coordinates act on quantities
in the **same frame**. Primary implementation:

```
p_s_ww, p_t_ww   (W)  ──▶  delta_p_W = p_t_ww − p_s_ww
t̂_ess            (C_s) ──▶  t̂_W = R_ws · t̂_ess
λ = dot(t̂_W, delta_p_W)
```

Equivalent alternative: `λ = dot(t̂_ess, R_wsᵀ·delta_p_W)` — identical under
rotation isometry; choose one, document it, and cover with T2. The edge stores
`relative_position_xyz = λ·t̂_ess` (C_s) and `relative_rotation_xyzw = quat(R_ess)`
(never the trajectory prior `R_wsᵀ·R_wt`).

---

## 15. Status after D1–D6

All six conditions are closed in this revision. **Remaining GO gate:**
owner confirms §12's quantitative criteria (D-7c-12/D-7c-13) and the §9 default
formula constants. On GO, the implementation sequence is: T1/T2 math+guard
unit tests → core seam §7.1 → D4 schema + `LoopClosure` payload (§10) → core
edge builder §8 (+ `spatial_separation_m` fix D2, §6) → D3 info matrix (§9) →
engine stage §7.2 (+ status UPDATE) → adapter seam §7.1 → E2E §12 → gates
(`check_constitution.py --base <7b-commit>`, full ctest Debug+Release). **No
code changes have been made by this document.**

---

## 16. Owner review checklist (maps to the 9 review points, now with D1–D6)

1. **Scale-derivation math** — §3.2-3.6, §14-D6 (projection formula with
   explicit frames `λ = dot(R_ws·t̂_ess, p_t_ww−p_s_ww)`, consistency guard,
   forbidden constructions). Confirm the D6 oriented formula (was
   `lambda = t̂_ess · delta_p` without a frame).
2. **Hidden fabrication** — §2, §3.6, §5.1, §14-D4 (no undeclared scales; no
   identity; no `spatial_separation_m` re-use; metric basis provenance-derived;
   manifest label ≠ certificate).
3. **`TrajectoryOptimizer` seam** — §7.1; D-7c-5; adapter wraps existing free
   function.
4. **7b→verifier→PoseGraph connection** — §10, §14-D5; new stage consumes
   `has_relative_pose=true`; `LoopClosure` carries measured `(R_ess, t̂_ess)`;
   `t_metric = λ·t̂_ess` resolved only at scale-resolution.
5. **F-only closures** — §11; end-of-processing, never a metric edge.
6. **`BuildLoopClosureEdge` fix** — §8; requires measured pose, `nullopt`
   without; zero-baseline rejection (D1); lines 48/69 and 6c helpers untouched.
7. **GTSAM covariance/information** — §9, §14-D3; deterministic
   confidence-derived info model (5 inputs), validated, no constant-50/300 in
   real path; documented as engineering approximation, not physical covariance.
8. **What the E2E proves** — §12; quantitative `RMSE_after < RMSE_before`
   (≥30% margin, D-7c-12) plus negative proof leaving nodes unmodified
   (D-7c-13).
9. **Drift-correction tests** — §13 T9, T10, T1, T2, T12; assert ground-truth
   metric error reduction and the provenance guard rail.

**Request (final):** confirm §9 default formula constants and §12 quantitative
criteria. On that GO, the implementation sequence in §15 applies. No code
changes have been made by this document.

---

## Appendix A — calibrated-relative-pose convention (for the builder)

Reference (consistent with OpenCV/COLMAP pinhole conventions): calibrated
essential `E = K2^T F K1`; decompose `E = [t]_x R`; disambiguate by cheirality
→ unique `(R_ess, t̂_ess)` mapping camera-1 → camera-2 coordinates
(`x_t = R_ess · x_s + ρ·t̂_ess`, `source=camera1`, `target=camera2`, `t̂_ess`
expressed in C_s). The builder must document the mapping from COLMAP's
`images`-record camera-to-world (`q_wc`, `t_wc`) to `TrajectoryPoseNode`
(`T_trajectory_camera`) and to the edge `T_source_target` (D6, §3.2 table) in
tests T1/T2 so a direction error cannot silently slip through.