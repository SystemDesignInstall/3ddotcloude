# P3-impl-7a — Implementation Readiness Report

**Status:** READY TO SCOPE (awaiting authorization)
**Date:** 2026-08-31
**Depends on:** P2.3 feature extraction (✅), P3 trajectory/pose-graph types (✅), P3-impl-6c E2E (✅),
ADR-P3-GTSAM-001 (✅)
**Scope:** **Visual Loop Closure Candidate Generation only** — connect the existing
FeatureArtifact pipeline to the P3 loop-closure types.

---

## 0. What this milestone is NOT

> Grounded only in what already exists. **No new repository research.** **No TEASER++ / Open3D /
> AI / neural / multi-session increment at this step.**

7a does **not** do geometric verification (that is 7b), even though the earlier design (D-LC-04)
lists verification criteria. 7a's output is the confirmed *candidate set* as `LoopClosureCandidate`
records, ready for a 7b geometric verifier. 6c remains the **golden integration test** for 7c.

---

## 1. The contour this milestone completes

```text
          Images
            ↓
   Feature Extraction (P2.3, EXISTS)      FeatureArtifact per frame
            ↓
     Descriptor retrieval (EXISTS)
            ↓
     Visual / descriptor matching          ← 7a NEW (candidate generation)
            ↓
        Candidate pairs
            ↓
        Temporal exclusion                 ← reuse 6c helper semantics
            ↓
     LoopClosureCandidate                  (canonical type, EXISTS)
```

7a turns the **early** part of the pipeline (images → features) into the **late** P3 loop-closure
contract (LoopClosureCandidate → eventually PoseGraph → GTSAM).

---

## 2. What already exists (no new work required here)

| Provided by | Type / facility | Gap for 7a |
|-------------|-----------------|-----------|
| `core/scene/feature/feature_set.h` | `FeatureSet` (frame_id → `artifact_ref`, detector, descriptor_type, count) | canonical id + CAS link exists |
| `engine/pipeline/feature_extraction.h` | `FeatureExtractionResult`, `FeatureArtifact` payload writer | produces the per-frame feature artifact |
| `schemas/json/feature.schema.json` | FeatureArtifact payload | **keypoints[x,y,size,angle,response]** + **descriptors[rows of numbers]** per frame — the matcher input |
| `core/scene/observation_graph/image_observation.h` | `ImageObservation` (frame_id, timestamp_ns, session_id, sensor_id) | per-frame time + session; frame→observation bridge |
| `core/scene/frame.h` | `Frame` (kinematic, pose_ref) | bridge key |
| `core/scene/query/scene_query.h` | `SceneQuery` (Observations(), artifact resolution) | iterate frames/observations per session |
| `core/trajectory/loop_closure.h` | `LoopClosureCandidate` (candidate_id, trajectory_id, source_frame_id, target_frame_id, feature_match_score, matcher, created_at_ns) | **the exact output record** |
| `core/utils/uuid.h` / `core/artifacts/artifact_store.h` | UUIDs, CAS put/get | persistence + provenance |
| `core/trajectory/pose_graph_helpers.h` (6c) | `DetectCandidates`, `min_temporal_separation`, `VerifyCandidate` | **temporal-exclusion + verification scaffolding reusable for 7a/7b** |
| `core/reconstruction/...` provenance | `ReconstructionProvenance` | lineage on the produced artifacts |
| P3 capability vocabulary (§18.1) | `"loop_closure": {trajectory, feature} -> {loop_closure, pose_graph}` | target capability contract |

**Conclusion:** every input type and every output type 7a needs already exists. The missing piece is
the **matching algorithm itself** plus the adapter boundary that produces
`LoopClosureCandidate`s from two frames' `FeatureArtifact`s.

---

## 3. What 7a must add (candidate generation only)

1. **A backend-independent matching contract** in core (do not put the matcher algorithm in core):
   - Input: two `FeatureSet`/`FeatureArtifact` payloads (`keypoints` + `descriptors`) + frames'
     timestamps.
   - Output: a `feature_match_score` (raw, [0, ∞), backend-specific per D-LC-02) or rejection.
   - The adapter boundary mirrors the existing pattern (a `..._adapter.h` in `adapters/...`).

2. **A classical visual matcher backend** (minimal, deterministic):
   - Descriptor distance (e.g. L2) nearest-neighbor matching between two frames' descriptor rows.
   - Ratio/threshold test to produce a similarity score → candidate ranking.
   - Choose the **simplest** matcher that produces real candidate pairs from `feature.schema.json`
     descriptors. (Concrete detector/descriptor: whatever the existing P2.3 pipeline can produce —
     the `mock_16` descriptor rows already carry 16-dim floats usable for L2 matching; no new
     detector required for the first classical proof.)

3. **Candidate ranking + temporal exclusion** driven by existing D-LC-06/D-LC-08 semantics:
   - Only non-consecutive, temporally-separated pairs become candidates
     (`minimum_temporal_separation_ns`, reuse 6c helper).
   - Each surviving pair becomes a `LoopClosureCandidate` (canonical fields + `matcher` label),
     persisted for audit (D-LC-02/D-LC-03).

4. **Tests** (mirroring 6c style): deterministic synthetic feature pairs (two frames sharing a
   subset descriptor set → high score; unrelated frames → low/no candidate), temporal exclusion,
   cross-session exclusion if applicable, determinism of candidates given deterministic features
   (D-DI-02), and schema compliance with `loop-closure.schema.json`.

---

## 4. Resolved decisions for 7a (AUTHORIZED, 2026-08-31)

> These replace the earlier open questions. A single source of truth; do not re-open unless new
> protected-contract impact is discovered (then report `ARCHITECTURE CHANGE REQUIRED`).

**Q1 — matcher location → adapter.**
Core holds only a backend-independent matching **contract** and the canonical candidate-generation
logic (`core/loop_closure/`). The concrete matching implementation lives behind an adapter boundary
(`adapters/visual_matching/`) exactly as GTSAM is an adapter. Core says "I need a visual descriptor
matcher"; it never names OpenCV / FLANN / BFMatcher / a specific algorithm.

```
Core
  │
  ▼
FeatureMatcher contract (interface)
  │
  ▼
Visual Matching Adapter
  │
  ▼
classical matcher
```

**Q2 — capability vs library → capability.**
7a is a real single-stage pipeline stage (not bare helpers), registered with the **existing** capability
name from `worker-capabilities.schema.json`: **`"loop_closure"`**. Its contract is **candidate
generation only**:

```
Frame/FeatureArtifact → descriptor matching → candidate ranking → temporal exclusion →
LoopClosureCandidate[] → CAS + metadata rows
```

It SHALL NOT produce PoseGraph/GTSAM work at this stage (that is 7b/7c).

**Q3 — descriptor → existing FeatureArtifact contract; `mock_16` for tests only.**
The matcher operates against `FeatureArtifact.descriptors[rows]` (feature.schema.json) and is
independent of any specific detector/descriptor. The existing `mock_16` (16-dim float rows) is used for the
**deterministic integration tests**. It is **NOT** declared a production visual descriptor — a production
descriptor is a separate, later stage.

**Q4 — persistence → yes, existing pattern.**
Candidates are NOT a transient helper. Use the existing ArtifactStore / canonical IDs / provenance /
metadata-row pattern (the `loop_closure_candidates` metadata table already exists — migration 0008 —
and `loop-closure.schema.json` already defines the candidates CAS payload). No new storage subsystem.

**Q5 — search scope → bounded/windowed.**
For 7a use a **bounded/windowed** candidate search: each source frame is matched only against target
frames within a configured lookback window that also satisfies `minimum_temporal_separation_ns`.
Deterministic, bounded, configurable, testable. No large-scale global retrieval/indexing
(e.g. vocab trees) in 7a.

---

## 6. AUTHORIZATION — P3-impl-7a (2026-08-31)

> **AUTHORIZE P3-impl-7a.** Implement only P3-impl-7a. Reuse existing canonical types and the existing
> adapter/capability architecture.
>
> - Q1: matcher implementation SHALL live behind an adapter boundary. Core contains only the
>   backend-independent matching contract and canonical candidate-generation logic.
> - Q2: implement 7a as a capability/pipeline stage for visual loop-closure **candidate generation
>   only**. Output `LoopClosureCandidate` records; SHALL NOT perform geometric verification,
>   PoseGraph construction, or GTSAM optimization.
> - Q3: matcher SHALL operate against the existing `FeatureArtifact` descriptor contract. Existing
>   `mock_16` MAY be used for deterministic integration tests, but SHALL NOT be declared a production
>   visual descriptor.
> - Q4: candidates SHALL follow the existing ArtifactStore / provenance / metadata-row persistence
>   pattern. Do not create a new storage subsystem.
> - Q5: use bounded/windowed candidate search with configurable temporal exclusion. Do not implement
>   large-scale global retrieval/indexing in 7a.
>
> Do not perform new repository research. Do not implement TEASER++, Open3D, AI/neural methods,
> multi-session registration, geometric verification, or GTSAM integration.
>
> If implementation requires changing a protected canonical contract, STOP and report
> `ARCHITECTURE CHANGE REQUIRED`.
>
> At completion provide: Debug/Release test counts, modified files, candidate-generation examples,
> deterministic-test evidence, schema validation, provenance/CAS evidence, and explicit confirmation
> that the GTSAM/Core boundaries remain unchanged.

---

## 5. Explicitly deferred (not in 7a)

- **7b** geometric verification (inliers, relative pose, epipolar) — consumes 7a candidates and the
  6c `VerifyCandidate`/`BuildLoopClosureEdge` scaffolding.
- **7c** visual loop → canonical PoseGraph → GTSAM → optimized trajectory (reuse 6c E2E as golden
  test).
- **Multi-session registration**, LiDAR fusion, dense reconstruction, AI/neural reconstruction.

---

## 6. Recommendation

7a is **ready to scope**. It can be implemented purely on existing canonical types + the existing
feature pipeline, producing real (non-synthetic) `LoopClosureCandidate`s — the first block of the
visual loop-closure path that the 6c "synthetic" E2E currently stands in for.

**Do not begin implementation automatically.** Await explicit authorization and a decision on Q1–Q3
above. Prepare no further repository research.
