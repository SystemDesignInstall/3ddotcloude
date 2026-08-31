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

## 4. Open design questions for 7a (resolve before implementation)

| # | Question | Proposed default |
|---|----------|------------------|
| Q1 | Where does the matcher backend live? | New `adapters/<matcher>/` following the adapter-seam pattern; core holds only the contract + candidate vector. |
| Q2 | Does 7a need a new capability `"loop_closure"` registered in a pipeline, or is it a library/CLI step for now? | Mirror the feature-extraction single-stage pipeline pattern; register a `loop_closure` capability producing candidates (no PoseGraph/GTSAM yet — that is 7b/7c). |
| Q3 | Which descriptor/detector for the first classical proof? | Whatever existing P2.3 produces (`mock_16` rows), decided with the user before locking a detector name in the schema vocabulary. |
| Q4 | Candidate output: standalone CAS artifacts + DB rows, consistent with LoopClosureCandidate persistence? | Yes — follow the artifact + metadata-row pattern used by trajectory/pose-graph. |
| Q5 | Scope of temporally-rich sessions (large N): brute-force O(N²) or windowed/sequence-based search? | Start windowed (bounded lookback) to keep the first proof deterministic and cheap; document the full-search option for later. |

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
