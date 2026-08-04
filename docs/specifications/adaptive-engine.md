# Adaptive Reconstruction Engine Specification

- **Status:** ratified (P1)
- **References:** ADR-027, ADR-026, ADR-030, ADR-006, ADR-034, ADR-035, `docs/specifications/scene-model.md`, `docs/specifications/reconstruction-pipeline.md`
- **Protected surface:** `core/adaptive/**` (Constitution §2)

## 1. Problem

Photogrammetry tooling today forces the operator to become an expert in algorithm selection: *which* matcher (SIFT, SuperPoint, LightGlue), *which* depth engine, *which* meshing method, *which* gaussian or NeRF stack, *which* parameter values — and the answer depends on the data (drone aerial over a field vs. handheld indoor vs. vehicle LiDAR corridor). Wrong choices mean wasted hours, wasted cloud spend, or silently degraded geometry.

The **Adaptive Reconstruction Engine** removes that burden: **the system — not the user — chooses algorithms and parameters** from the scene's measured characteristics and the user's stated goal. The user says "reconstruct by recipe RC High Accuracy" (or "fast preview", or "georeferenced mesh"); the engine picks the strategy. Users who want control always can: human override remains available at every decision point (§5).

The engine lives in the pipeline's **Improve** stage (`reconstruction-pipeline.md` §1): after every `Quality` measurement it compares observed quality against the goal, and either selects the next recipe, re-runs a stage with different parameters, or declares the scene ready to finalize. It is a **selection layer**, not a reconstruction layer — it changes which recipe runs, never the geometry the adapters produce.

## 2. Inputs

The engine's decision is a function of two inputs.

### 2.1 SceneCharacteristics (extracted, not declared)

Produced by a **feature extractor** that runs inside the `Validate` stage of the reconstruction pipeline (its extraction step is the `SceneCharacteristicExtractor` interface, §6; a mock ships in M0). Characteristics are measured facts, never user claims:

| Characteristic | Meaning |
|---|---|
| `sensor_mix` | camera / lidar / rgbd / panoramic / gnss-imu combinations |
| `image_count`, `image_resolution` | cardinality and resolution of image observations |
| `overlap` | estimated pairwise image overlap distribution |
| `motion_blur` | estimated blur/roll-off per view group |
| `gnss_availability` | fix quality, RTK status, georeferencing affordance |
| `lidar_availability` | LiDAR observations present and usable |
| `texture_richness` | texture energy, repetitive-pattern risk |
| `scene_scale` | extent, object size, unit confidence |
| `lighting` | uniformity, shadows, HDR risk |
| `dynamic_content` | moving-object fraction, ghosting risk |

### 2.2 UserGoal (declared by the user)

| Field | Meaning |
|---|---|
| `accuracy_budget` | maximum acceptable reprojection error / absolute accuracy |
| `time_budget` | maximum wall-clock or worker-seconds budget |
| `fidelity_target` | fidelity tier (preview / standard / high accuracy / highest) |
| `output_types` | sparse points, mesh, textured mesh, gaussians, orthophoto, ... |
| `max_cost` | maximum compute cost in credits/worker-seconds |

`UserGoal` has a documented default (fidelity `standard`, no hard time/cost budget, mesh + orthophoto outputs) so that a bare "reconstruct" request is valid. Goals are per-reconstruction; they may be overridden mid-pipeline by the operator.

## 3. Decision model

A **strategy** is an *ordered list of candidate Recipes* ranked by predicted quality/cost. The engine produces exactly one recommended strategy per decision; it never executes it directly — the pipeline does (`reconstruction-pipeline.md` §3).

- **Candidate generation:** recipes matching the `output_types` + capabilities present in the scene (COLMAP vs VGGT requires a `SparseReconstruction` adapter, etc.).
- **Ranking:** predicted quality and cost come from the **Quality Engine** (ADR-030) plus **benchmark priors** (ADR-029) — measured behavior of each recipe on datasets similar to `SceneCharacteristics`.
- **Every decision carries a rationale** (which characteristics dominated, which recipes were considered and rejected, why) and a **confidence** score. Rationales are auditable: they are logged with full provenance (input characteristics hash, extractor version, recipe set, ranking model version).
- **The output of the engine is a recipe reference** — not geometry. Geometry decisions remain inside adapters and the validation gate (ADR-006).

The decision procedure per pipeline iteration:

1. **Extract** `SceneCharacteristics` from the current scene version (`SceneCharacteristicExtractor`).
2. **Generate candidates** from the recipe index filtered by `output_types` and available capabilities (`RecipeSelector`).
3. **Rank** candidates by predicted quality under the cost/time budget (`QualityPredictor`).
4. **Pick** the top recipe and attach a rationale + confidence (`StrategyProvider`).
5. **Log** the decision with full provenance, then hand the recipe to the scheduler.

If the top candidate scores below the acceptance threshold and budget remains, the engine recommends an additional loop (e.g. "add control points", "capture more overlap") rather than silently degrading the output; the operator or an automated `Improve` task decides whether to continue.

## 4. Algorithm selection guidance

Rules of thumb (principled guidance, **not** a hard spec; the rule engine encodes these as weighted heuristics):

| Decision | Prefer | When |
|---|---|---|
| SfM engine | **VGGT** (AI prior) | scene has poor texture, repetitive patterns, motion blur, or sparse overlap — where classical feature matching fails |
| SfM engine | **COLMAP** (default, ADR-004) | textured, well-overlapped, "typical" photogrammetry scenes; when maximal geometric rigor and auditability are required |
| Feature matching | **LightGlue** | high-resolution images, many matches wanted, compute budget allows; as an augmenter of COLMAP's matcher |
| Feature matching | **SIFT** | speed-critical, low-res, or when match count must be bounded |
| Pose/alignment | **GNSS integration + ICP** | GNSS fix quality high (RTK); anchor to a survey frame |
| Pose/alignment | **LiDAR / SLAM (KISS-ICP, LidarOdometry + LoopClosure)** | LiDAR available and cameras poorly constrained (indoor corridors, tunnels, textureless) |
| Depth densification | **LiDAR-driven dense** | LiDAR observations dense; use cameras for color/texture only |
| Densification | **DenseStereo (image-only)** | camera-only capture; texture sufficient |
| Surface | **Mesh (SurfaceReconstruction)** | CAD/BIM, measurement, orthophoto, survey deliverable |
| Surface | **Gaussian (GaussianGeneration)** | view-synthesis, photoreal visualization, real-time rendering |
| Output | **Mesh for final deliverables; Gaussians as a view-synthesis layer** | both requested — run Gaussian after SfM/Dense, share poses |

The engine may combine: e.g. VGGT priors validated into COLMAP bundle adjustment (through the ADR-006 gate), or KISS-ICP trajectory fused into a COLMAP-free reconstruction.

## 5. Governance

- **AI never decides geometry directly** (ADR-006). The engine selects recipes and parameters only; it cannot inject poses, points, surfaces, or gaussians into the Scene. AI-derived geometry enters exclusively as **validated priors** on observations through the ADR-006 validation gate.
- **Human override always available:** any user may pin a specific recipe, edit parameters, or bypass the engine's recommendation at any stage. Overrides are recorded as decisions with provenance, and the pipeline replays deterministically.
- **Decisions are logged with provenance**: characteristics hash, candidate set, ranking inputs, chosen recipe, rationale, confidence, and who/what made the call (user override vs. engine).

## 6. Interfaces

Stable interfaces; **mock implementations ship in M0** (ADR-031). Production implementations (real extractors, learned predictors) replace the mocks behind the same surface.

| Interface | Responsibility |
|---|---|
| `StrategyProvider` | top-level: `Strategy = ranked Recipe[] + rationale + confidence` from (SceneCharacteristics, UserGoal) |
| `SceneCharacteristicExtractor` | runs in Validate; measures characteristics from a scene version (mock in M0, ADR-027) |
| `RecipeSelector` | candidate recipe generation from characteristics + goal |
| `QualityPredictor` | predicted quality/cost per recipe from Quality Engine priors (ADR-030, ADR-029) |

All interfaces are deterministic and side-effect free over (scene version + characteristics), matching the read-only Scene Query API style (ADR-035). Shape follows the Query API convention (`scene-model.md` §6):

```cpp
Characteristics extract(SceneVersionRef version);          // SceneCharacteristicExtractor
Recipe[] candidates(Characteristics, UserGoal);            // RecipeSelector
Score predictedQuality(Recipe, Characteristics, UserGoal); // QualityPredictor
Strategy recommend(Characteristics, UserGoal);             // StrategyProvider
```

## 7. Evolution

- **v1: transparent rule-based policy.** The guidance table (§4) is encoded as an auditable rule set with weights and explicit rationale output. Behavior is fully explainable; this ships first so users can trust the layer.
- **v2+: learned ranking.** QualityPredictor may learn from benchmark + production telemetry. **A policy change ships only when benchmark evidence confirms improvement** over the current policy on the standard benchmark suite (ADR-029) — no improvement, no change. Learned components remain behind the same stable interfaces, and their outputs are still logged with provenance.
- Recipes are immutable (ADR-026): policy changes select among existing recipes or author new ones; they never mutate a published recipe.
- The benchmark suite (ADR-029) is the **sole gate**: any change to policy weights, ranking models, or guidance heuristics must show a statistically significant gain on held-out benchmark scenes and no regressions beyond tolerance on prior wins. Telemetry may propose changes; only benchmarks ship them.

## 8. M0 scope

M0 delivers (ADR-031): the **data model** (`SceneCharacteristics`, `UserGoal`, `Strategy`, `Recipe` schema), the **four interfaces** (§6), and a **mock policy** (a fixed, documented rule set over the guidance table) wired into the mock pipeline of the SDK/CLI. No learned ranking, no telemetry ingestion, and no production extractors before their respective milestones; the schemas are fixed now and do not change.
