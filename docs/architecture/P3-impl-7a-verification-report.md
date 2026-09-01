# P3-impl-7a — Verification Report

**Milestone:** P3-impl-7a — Visual Loop Closure Candidate Generation
**Date:** 2026-08-31
**Verdict:** **PASS** (Debug 549/549 · Release 549/549)
**Status:** IMPLEMENTED — awaiting review/acceptance by the user (do NOT auto-start 7b)

---

## 1. What was built

Bounded/windowed, deterministic **visual loop-closure candidate generation** over the existing
FeatureArtifact pipeline, following the AUTHORIZED Q1–Q5 decisions.

| Layer | File | Purpose |
|-------|------|---------|
| Core contract | `core/loop_closure/feature_matcher.h` | `MatchingFrameDescriptors` + `LoopClosureFeatureMatcher` interface (backend-independent seam) |
| Core canonical gen | `core/loop_closure/loop_closure_candidate_gen.{h,cpp}` | `LoadFeatureDescriptors` (FeatureArtifact → canonical set) + `GenerateLoopClosureCandidates` (windowed + temporal exclusion + score floor + top-k) |
| Adapter | `adapters/visual_matching/visual_matcher_adapter.{h,cpp}` | `L2NearestMatcher`, the classical deterministic matcher (std-only, no OpenCV/FLANN) |
| Engine stage | `engine/pipeline/loop_closure_detection.{h,cpp}` | `DetectLoopClosureCandidates` (resolves FeatureArtifacts, runs matcher, persists rows + CAS payload); `RegisterLoopClosureDetection` (capability `"loop_closure"`) |
| Tests | `tests/unit/test_loop_closure_detection.cpp` | 12 tests (see §4) |

---

## 2. Test-count record

| Config | Before (6c) | After (7a) |
|--------|-------------|------------|
| Debug | 537/537 | **549/549** (+12) |
| Release | 537/537 | **549/549** (+12) |
| spatial_gtsam_tests | 39/39 | 39/39 (unchanged) |

The 12 new tests run under a new executable `spatial_loop_closure_tests`. GTSAM tests still pass;
the `spatial_gtsam_tests` count is unchanged at 39.

---

## 3. Candidate-generation examples (deterministic evidence)

Descriptor space: 16-dim float rows (mock_16). Revisit = same descriptor set; unrelated = different
set (see §4 test rigor).

**A. Revisit detected (the core result).** Three frames:
- f0 at t=1000, features {1,2,3,4,5}
- f1 at t=2000, features {101,102,103}  (unrelated)
- f2 at t=3000, features {1,2,3,4,5}  (revisit of f0)

`DetectLoopClosureCandidates(..., minimum_match_score=0.5)` → **exactly one candidate**:

```json
{
  "candidate_id": "<uuidv4>",
  "trajectory_id": "<traj>",
  "source_frame_id": "<f2>",      // the newer frame
  "target_frame_id": "<f0>",      // the older, revisited frame
  "feature_match_score": 5.0,
  "matcher": "visual_l2_nearest",
  "created_at_ns": "<now>"
}
```

Source is newer, target is older (LoopClosureCandidate convention). The unrelated f1 produces zero
candidates (score 0 < 0.5).

**B. Score separation.** Identical set → score = count = 5. Unrelated sets → 0. Partial overlap
({1,2}) → 2. Deterministic: repeated calls identical.

**C. Temporal exclusion.** min separation 1000 ns removes the (f1,f0) 50 ns near-pair; only
well-separated pairs survive.

**D. Bounded window.** 4 frames, no window → 6 candidates; window=1 → 3; window=2 → 5.

**E. Top-k.** `max_candidates_per_source=1` keeps only the highest-scoring match per source frame.

---

## 4. What the 12 tests assert

- **VisualMatcherAdapterTest (4):** identical→high score; unrelated→low; partial overlap in between;
  determinism for identical inputs.
- **LoopClosureCandidateGenTest (5):** source-newer/target-older; temporal exclusion; score floor;
  bounded window counts; top-k per source.
- **LoopClosureDetectionTest (3):** real FeatureArtifacts → persisted rows (read back by
  trajectory) matching the canonical candidates; the CAS **loop_closure** payload validates against
  `loop-closure.schema.json` (candidates populated, closures empty); the CAS manifest carries
  provenance (input_artifact_hashes = the two FeatureArtifact content hashes consumed).

---

## 5. Decisions honoured

| Decision | How |
|----------|-----|
| Q1 adapter boundary | Core holds only the contract + canonical generation; the concrete matcher is the `visual_matching` adapter implementing `LoopClosureFeatureMatcher`. |
| Q2 capability/pipeline | Single-stage pipeline `loop_closure_detection` registered with the **existing** capability name `"loop_closure"` (worker-capabilities.schema.json); candidate generation only — no verification/PoseGraph/GTSAM. |
| Q3 FeatureArtifact contract | Matcher consumes `FeatureArtifact.descriptors[]`; `mock_16` used for deterministic tests only, **not** declared a production descriptor. |
| Q4 existing persistence | Reuses the existing `loop_closure_candidates` metadata table (migration 0008) + `loop-closure.schema.json` CAS payload + ArtifactStore manifest provenance. No new storage subsystem. |
| Q5 bounded/windowed | `search_window` lookback + `minimum_temporal_separation_ns`; no global retrieval/indexing. |

---

## 6. Boundary / non-goals confirmation

- **GTSAM boundary unchanged:** the 7a code (core/loop_closure, adapters/visual_matching,
  engine/pipeline/loop_closure_detection) has **no GTSAM includes or uses** (grep-verified); the
  GTSAM adapter and `pose_graph_helpers.h` are untouched; `spatial_gtsam_tests` still 39/39.
- **No protected canonical contract changed:** the candidate generation extends existing types; the
  single addition to an existing header is a default argument (`trajectory_id = ""`) on the new
  `GenerateLoopClosureCandidates` function — no existing signature changed.
- **Not implemented here:** geometric verification (7b), PoseGraph assembly, GTSAM integration (7c),
  multi-session, TEASER++, Open3D, AI/neural, no new repository research.

### Runnable-capability boundary (transparent disclosure)

The `"loop_closure"` capability is **registered as a declarative pipeline stage**
(`RegisterLoopClosureDetection`) and the stage producer (`DetectLoopClosureCandidates`) is a tested,
callable function. However, the full `Engine::RunPipeline` **in-process executor path is NOT bound**
to `"loop_closure"` in this increment: that path dispatches a worker `task_type` and requires the
worker profile to declare the capability (`DemoWorkerProfile().capabilities` currently lists only
`feature_extraction, reconstruction, validation`), and it operates on a single input ref per stage,
whereas candidate generation consumes multiple FeatureArtifacts per run. Wiring the executor path is
a deliberate deferral that touches the protected worker/task infrastructure; it can be added as a
small, separately-authorized follow-up if consumed via `RunPipeline` is required.

---

## 7. Files created/modified

**Created**
- `core/loop_closure/feature_matcher.h`
- `core/loop_closure/loop_closure_candidate_gen.h`, `.cpp`
- `adapters/visual_matching/visual_matcher_adapter.h`, `.cpp`, `CMakeLists.txt`
- `engine/pipeline/loop_closure_detection.h`, `.cpp`
- `tests/unit/test_loop_closure_detection.cpp`
- `docs/architecture/P3-impl-7a-verification-report.md` (this file)

**Modified**
- `core/CMakeLists.txt` (add loop_closure source)
- `engine/CMakeLists.txt` (add loop_closure_detection source)
- `adapters/CMakeLists.txt` (add visual_matching subdir)
- `tests/CMakeLists.txt` (add spatial_loop_closure_tests)
- `core/loop_closure/loop_closure_candidate_gen.h` (default arg)
- `docs/architecture/P3-impl-7a-implementation-readiness.md` (resolved Q1–Q5 + authorization)
- `docs/architecture/P3-milestone-status.md` (7a status + next = 7b)
