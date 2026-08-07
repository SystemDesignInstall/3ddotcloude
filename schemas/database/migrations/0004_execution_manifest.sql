-- RFC-0003 P1.4: ExecutionManifest (pipeline-level execution document).
-- Applied by the migration runner (core/storage/metadata_db.cpp) which owns
-- the transaction; BEGIN/COMMIT are stripped at build time.
--
-- The manifest is the golden source for Resume / Audit / Reproducibility and
-- the QualityReport link (RFC-0005). It is owned by engine/pipeline; the
-- scheduler stays agnostic (it persists tasks/task_runs only, §5.12).

-- One row per pipeline run. manifest_id doubles as the scheduler graph
-- job_id so tasks and the manifest are linked by a single identity.
CREATE TABLE execution_manifests (
  manifest_id          BLOB PRIMARY KEY,        -- = graph job_id (Uuid)
  pipeline_id          TEXT NOT NULL,
  pipeline_version     TEXT NOT NULL,
  pipeline_hash        TEXT NOT NULL,           -- identity of the whole run
  config_hash          TEXT NOT NULL,
  git_commit           TEXT NOT NULL,
  status               TEXT NOT NULL DEFAULT 'running',
                         -- running|succeeded|failed|cancelled
  external_inputs_json TEXT NOT NULL DEFAULT '[]',
  quality_report_id    BLOB,                    -- RFC-0005: validate-stage artifact
  created_at_ns        INTEGER NOT NULL,
  finished_at_ns       INTEGER
);

CREATE INDEX idx_manifests_status ON execution_manifests (status);

CREATE TABLE execution_manifest_stages (
  manifest_id          BLOB NOT NULL REFERENCES execution_manifests (manifest_id),
  sequence             INTEGER NOT NULL,
  stage_id             TEXT NOT NULL,
  capability           TEXT NOT NULL,           -- worker-capabilities.schema.json
  implementation       TEXT NOT NULL,           -- inprocess | process
  task_hash            TEXT NOT NULL,           -- ADR-020 cache key of the stage task
  status               TEXT NOT NULL DEFAULT 'pending',
                         -- pending|running|succeeded|failed|cancelled|skipped
  cache_hit            INTEGER NOT NULL DEFAULT 0,
  task_id              BLOB,
  output_refs_json     TEXT NOT NULL DEFAULT '[]',
  started_at_ns        INTEGER,
  finished_at_ns       INTEGER,
  PRIMARY KEY (manifest_id, sequence)
);
