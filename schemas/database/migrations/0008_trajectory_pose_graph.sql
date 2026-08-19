-- P3-impl-1: Canonical Trajectory / Pose Graph / Loop Closure domain types.
-- Creates tables for trajectories, pose_graphs, loop_closures (candidates
-- and accepted closures), and optimization_results.
-- Applied by the migration runner (core/storage/metadata_db.cpp) which owns
-- the transaction; BEGIN/COMMIT are stripped at build time.
--
-- Normative decisions: D-TRJ-01 through D-TRJ-10, D-PG-01 through D-PG-07,
-- D-LC-01 through D-LC-09, D-OPT-01 through D-OPT-06.

-- Drop the legacy trajectories table from migration 0001 (unused stub with
-- incompatible columns: artifact_ref, confidence_json, properties_json).
DROP TABLE IF EXISTS trajectories;

-- Trajectory metadata (D-TRJ-01): CAS artifact with metadata row.
CREATE TABLE trajectories (
  trajectory_id      BLOB PRIMARY KEY,
  scene_id           BLOB NOT NULL,
  session_id         BLOB NOT NULL,
  kind               TEXT,           -- "odometry" | "slam" | "sfm" | "survey" (D-TRJ-03)
  coordinate_frame   TEXT,           -- e.g. "trajectory_0" (D-TRJ-04)
  status             TEXT,           -- "building" | "optimized" | "superseded" (D-TRJ-05)
  node_count         INTEGER,
  total_distance_m   REAL,
  total_duration_ns  INTEGER,
  created_at_ns      INTEGER,
  document_json      TEXT            -- full Trajectory as JSON (CAS payload content)
);

CREATE INDEX idx_trajectories_scene_session ON trajectories (scene_id, session_id, created_at_ns);
CREATE INDEX idx_trajectories_scene_status  ON trajectories (scene_id, status);

-- Pose Graph metadata (D-PG-01): separate CAS artifact.
CREATE TABLE pose_graphs (
  graph_id                   BLOB PRIMARY KEY,
  trajectory_id              BLOB NOT NULL,
  scene_id                   BLOB NOT NULL,
  status                     TEXT,     -- "building" | "ready" | "optimizing" | "optimized" | "failed" (D-PG-02)
  node_count                 INTEGER,
  edge_count                 INTEGER,
  odometry_edge_count        INTEGER,
  loop_closure_edge_count    INTEGER,
  prior_edge_count           INTEGER,
  created_at_ns              INTEGER,
  document_json              TEXT      -- full PoseGraph as JSON (CAS payload content)
);

CREATE INDEX idx_pose_graphs_trajectory ON pose_graphs (trajectory_id, created_at_ns);
CREATE INDEX idx_pose_graphs_scene      ON pose_graphs (scene_id, created_at_ns);

-- Loop Closure Candidates (D-LC-02, D-LC-03): persisted for audit trail.
CREATE TABLE loop_closure_candidates (
  candidate_id         BLOB PRIMARY KEY,
  trajectory_id        BLOB NOT NULL,
  source_frame_id      BLOB NOT NULL,
  target_frame_id      BLOB NOT NULL,
  feature_match_score  REAL,
  matcher              TEXT,
  created_at_ns        INTEGER
);

CREATE INDEX idx_lcc_trajectory ON loop_closure_candidates (trajectory_id, created_at_ns);
CREATE INDEX idx_lcc_source_target ON loop_closure_candidates (source_frame_id, target_frame_id);

-- Loop Closures (D-LC-05): accepted and rejected, both persisted for audit.
CREATE TABLE loop_closures (
  closure_id            BLOB PRIMARY KEY,
  trajectory_id         BLOB NOT NULL,
  candidate_id          BLOB,
  source_frame_id       BLOB NOT NULL,
  target_frame_id       BLOB NOT NULL,
  status                TEXT,         -- "accepted" | "rejected" (D-LC-05)
  inlier_ratio          REAL,
  inlier_count          INTEGER,
  confidence            REAL,
  temporal_separation_ns INTEGER,
  spatial_separation_m  REAL,
  created_at_ns         INTEGER
);

CREATE INDEX idx_lc_trajectory    ON loop_closures (trajectory_id, created_at_ns);
CREATE INDEX idx_lc_source_target ON loop_closures (source_frame_id, target_frame_id);
CREATE INDEX idx_lc_status        ON loop_closures (trajectory_id, status);

-- Optimization Results (D-OPT-01): clean input/output contract.
CREATE TABLE optimization_results (
  result_id          BLOB PRIMARY KEY,
  graph_id           BLOB NOT NULL,
  trajectory_id      BLOB NOT NULL,
  status             TEXT,           -- "converged" | "failed" | "diverged" (D-OPT-02)
  iterations         INTEGER,
  initial_error      REAL,
  final_error        REAL,
  error_reduction    REAL,
  created_at_ns      INTEGER,
  document_json      TEXT            -- full OptimizationResult as JSON (CAS payload content)
);

CREATE INDEX idx_opt_results_trajectory ON optimization_results (trajectory_id, created_at_ns);
CREATE INDEX idx_opt_results_graph      ON optimization_results (graph_id, created_at_ns);
