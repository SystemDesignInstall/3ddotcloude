# Benchmark & Validation Framework Specification

- **Status:** draft (P0)
- **References:** ADR-029, ADR-030, ADR-016, `docs/specifications/reconstruction-pipeline.md`, `docs/specifications/adaptive-engine.md`
- **Protected surface:** `benchmarks/**`, `core/bench/**`

## 1. Purpose

The Benchmark & Validation Framework is the platform's evidence base. It exists to:

1. **Prove** reconstruction accuracy, completeness, and performance against the two reference commercial competitors — RealityCapture and Metashape — on public, fixed, immutable datasets.
2. **Gate** every algorithm and policy change: no pipeline change, no adaptive-engine policy change, and no geometry provider replacement merges unless the regression benchmark passes on the designated gate datasets.
3. Produce reproducible, comparable, auditable results that double as marketing evidence.

Design principles:

- **Reproducibility over convenience.** A benchmark result that cannot be reproduced byte-for-byte from the recorded environment and inputs is not a result (ADR-029).
- **Determinism by default.** Runs are seeded and executed such that identical `BenchmarkRun` inputs yield identical metrics (see §4). Non-deterministic backends are pinned and reported.
- **Comparison is measured, not claimed.** Competitor results are produced by running the competitor on the same machine in the same harness, under the same metric definitions — never taken from external publications.
- **Everything is gated.** The gate set (defined in `benchmarks/gates.yml`) is the CI merge condition for all algorithm, recipe, and adaptive-engine changes (ADR-030).

## 2. Dataset Registry

Datasets are registered in `benchmarks/DATASET_LICENSES.yml`, which records for each dataset its license, redistribution rights, download URL, and verified SHA-256 checksums of every file used. A dataset is usable only after its checksums verify. Payloads are downloaded into the shared CAS artifact store (SHA-256 content addressing, ADR-010), never into the benchmark workspace.

| Dataset | Subsets | Purpose | License | Ground-truth type | Metrics to report |
|---|---|---|---|---|---|
| ETH3D | Multi-view; benchmark splits: high-res, low-res, indoor, outdoor | Generic multi-view reconstruction and camera calibration | CC BY-NC-SA 4.0 (research-only) | Laser scans; SLAM scans (aligned) | accuracy %, completeness %, F-score, reprojection error |
| Tanks and Temples | intermediate, advanced | Large-scale indoor/outdoor reconstruction | Non-commercial research terms | Multi-camera photogrammetry (intermediate), LiDAR (advanced) | F-score (precision/recall), completeness % |
| DTU | dense MVS | Dense multi-view stereo, camera poses | CC BY-NC-SA 4.0 | Structured-light reference scans | accuracy mm, completeness mm, overall score |
| KITTI | odometry / LiDAR | Visual-LiDAR odometry and mapping | CC BY-NC-SA 3.0 | Registered Velodyne + GPS/IMU poses | ATE, RPE |
| TUM RGB-D | RGB-D SLAM | Indoor SLAM, handheld RGB-D | CC BY 3.0 (per sequence) | Motion-capture camera trajectories | ATE, RPE |
| Replica | synthetic indoor | Metric evaluation of indoor reconstruction | Research-only (not redistributable) | Ground-truth mesh, semantics, camera poses | mesh accuracy, completeness, F-score |
| ScanNet | indoor RGB-D | Indoor scene reconstruction and pose | ScanNet Terms of Use (research-only) | BundleFusion-registered scans, withheld online eval | registration error, mesh accuracy %, ATE |
| MegaDepth | internet photos | Sparse/dense from unstructured internet imagery | CC BY-NC-SA 4.0 (images keep Flickr terms) | SfM-derived reference (relative only, no metric scale) | reprojection error, coverage, coarse geometry |
| BlendedMVS | synthetic MVS | Dense MVS with ground-truth depth and poses | Dataset bundle research-only; textures CC0 | Blender-rendered depth maps and camera poses | depth error, accuracy %, completeness %, F-score |

Rules:

- **Subset completeness.** A run must state the exact subset and sequence; aggregate scores are only reported across a fully enumerated subset.
- **No cherry-picking.** Sequences may not be dropped to improve a score; any exclusion is a schema violation.
- **Test/validation discipline.** Tuning recipes on the benchmark split contaminates the evidence; tuning uses the validation split, evidence uses the benchmark split.
- **License compliance.** Non-commercial and research-only datasets (ETH3D, Tanks and Temples, ScanNet, Replica) are used for internal validation and marketing evidence only, never redistributed with the product.

## 3. Metrics Taxonomy

Each metric is computed by a named, schema-registered implementation (`core/bench/metrics/`), referenced by `metric_id`, never by free text.

- **Mean/median distance to GT** (`DistanceMeters`): per-vertex distance from reconstructed geometry to the nearest ground-truth surface; reported as mean and median.
- **Accuracy %**: percentage of reconstructed vertices whose distance to GT is below the threshold (`eth3d` threshold, 2 cm at metric scale).
- **Completeness %**: percentage of ground-truth surface that is covered by reconstruction within the threshold.
- **F-score**: harmonic mean of precision (accuracy %) and recall (completeness %); the primary geometry quality metric.
- **Reprojection error**: mean Euclidean error in pixels of projected 3D points versus detected image observations after optimization.
- **ATE** (Absolute Trajectory Error): RMSE of camera trajectory over all poses after best-fit similarity alignment of the estimated trajectory to GT.
- **RPE** (Relative Pose Error): drift between consecutive pose pairs, reported as translation RMSE and rotation error (deg).
- **Coverage**: fraction of the GT surface with at least one reconstructing vertex (alias of completeness for sparse output).
- **Runtime**: wall-clock time for the full recipe on the recorded environment.
- **Peak RAM / VRAM**: maximum resident set / GPU memory during the run.
- **Throughput**: million points produced per second (MPts/s), measured from raw input to dense geometry.
- **Texture quality (PSNR/SSIM)**: measured on the rendered textured mesh versus GT imagery where texture GT exists (DTU, Replica).

## 4. Harness

A run is defined by

```
BenchmarkRun = { dataset, recipe, environment, metrics[] }
```

- **dataset**: registry entry + subset + sequence list (see §2).
- **recipe**: named pipeline configuration (feature, match, BA, dense, mesh, texture, adaptive-engine policy). Recipes live in `benchmarks/recipes/`; the resolved config is captured by `config_hash` (SHA-256 of the canonicalized JSON).
- **environment**: fully recorded, including OS/kernel, CPU model, RAM, GPU model + driver, SDK versions, engine binary `git_commit`, competitor binaries and versions.
- **metrics[]**: the metric_id, value, unit, threshold, and comparator (e.g. `eth3d_benchmark`, `tnn_benchmark`) for each reported metric.

Determinism: the engine runs with a recorded seed; the harness verifies the result artifacts are byte-identical to a prior control run (or records the observed drift when a backend is inherently non-deterministic).

### 4.1 JSON report

The harness writes a single JSON report per run (`benchmarks/reports/<run_id>.json`), versioned by `schema_version`:

```json
{
  "schema_version": 1,
  "run_id": "uuid",
  "product": { "name": "spatial-platform", "version": "0.1.0", "git_commit": "sha256" },
  "environment": {
    "os": "windows 11 23h2", "cpu": "amd 7950x", "ram_gb": 64,
    "gpu": [ { "model": "rtx 4090", "vram_mb": 24576, "driver": "552.44" } ],
    "software": { "compiler": "...", "cuda": "12.4", "competitors": { "realitycapture": "1.3", "metashape": "2.1.2" } }
  },
  "dataset": { "name": "eth3d", "subset": "high_res", "sequences": ["courtyard", "delivery_area"], "artifact_hashes": ["sha256:..."] },
  "recipe": { "name": "default-mvs", "config_hash": "sha256:...", "adaptive_engine_policy": "balanced" },
  "metrics": [
    { "metric_id": "fs_score", "value": 0.892, "unit": "percent", "threshold_m": 0.02, "comparator": "eth3d_benchmark" }
  ],
  "artifacts": [ { "role": "result_mesh", "artifact_ref": "sha256:..." } ],
  "determinism": { "seed": 42, "control_run_matches": true },
  "provenance": { "worker_log_refs": ["sha256:..."], "started_at": "iso-8601", "duration_ns": 123456789 }
}
```

- Metrics are **schema-validated** (`schemas/json/benchmark-report.schema.json`, ADR-016 property-tested) before they can be persisted or compared.
- HTML/CSV export for human review is a later milestone; the JSON report is the source of truth.

## 5. Usage

- **Regression gate (CI).** The gate set runs on every algorithm/recipe/policy change; a regression of geometry F-score > 0.5 pp or runtime > 5% on any gate dataset blocks merge unless the author records an acknowledged, explained waiver (ADR-030).
- **Recipe tuning.** Recipe authors use the validation split to tune; the benchmark split is used only for final evidence.
- **Adaptive-engine validation.** Policy changes to the adaptive engine are validated against the full registry, with per-dataset cost/quality trade-off curves reported (`docs/specifications/adaptive-engine.md`).
- **Marketing evidence.** Reported numbers must resolve to published, hash-pinned reports; marketing inherits only the gated numbers.

## 6. M0 Scope

M0 implements (ADR-031):

- `DATASET_LICENSES.yml` registry with license/redistribution metadata and checksum verification of downloadable files;
- the metrics taxonomy schema and metric registry stubs;
- the harness scaffolding and the JSON report schema with validation and property tests;
- a **mock harness run** that returns a synthetic-but-schema-valid report end-to-end, so consumers (CI, reporting, dashboard) are built against the real contract.

Deferred to post-M0: executing real datasets, running competitors, the CI gate itself, HTML/CSV export.
