-- RFC-0003: Processing Engine scheduler state (metadata only, ADR-009/020).
-- Applied by the migration runner (core/storage/metadata_db.cpp) which owns
-- the transaction; BEGIN/COMMIT are stripped at build time.

-- 0001 (RFC-0002 data model) shipped an unused `tasks` placeholder with no
-- writers. RFC-0003 supersedes the execution model and fixes the engine table
-- layout (§5.12), so the placeholder is dropped and recreated here. No
-- consumer ever wrote to the old table; engine tables are fresh in 0003.
DROP TABLE IF EXISTS task_runs;
DROP TABLE IF EXISTS task_dependencies;
DROP TABLE IF EXISTS tasks;

CREATE TABLE tasks (
  task_id              BLOB PRIMARY KEY,               -- Uuid (16 bytes)
  job_id               BLOB NOT NULL,                  -- owning job / task graph
  task_type            TEXT NOT NULL,                  -- semantic type, ADR-011
  spec_json            TEXT NOT NULL,                  -- TaskInstance spec (JSON)
  config_hash          TEXT NOT NULL,                  -- effective config hash (ADR-020)
  cache_policy         TEXT NOT NULL DEFAULT 'cacheable',  -- cacheable | never
  deterministic        INTEGER NOT NULL DEFAULT 0,     -- byte-identical outputs
  cancellation_policy  TEXT NOT NULL DEFAULT 'cooperative',
  status               TEXT NOT NULL DEFAULT 'pending',    -- pending|running|succeeded|failed|cancelled|skipped
  retry_policy_json    TEXT NOT NULL,
  created_at_ns        INTEGER NOT NULL,
  updated_at_ns        INTEGER NOT NULL
);

CREATE INDEX idx_tasks_job ON tasks (job_id);
CREATE INDEX idx_tasks_status ON tasks (status);

CREATE TABLE task_runs (
  run_id               BLOB PRIMARY KEY,               -- ExecutionId
  task_id              BLOB NOT NULL REFERENCES tasks (task_id),
  attempt              INTEGER NOT NULL DEFAULT 1,
  worker_id            BLOB,
  started_at_ns        INTEGER,
  ended_at_ns          INTEGER,
  terminal_state       TEXT,
  error_json           TEXT,
  input_refs_json      TEXT NOT NULL DEFAULT '[]',     -- content hashes (CAS SHA-256)
  output_refs_json     TEXT NOT NULL DEFAULT '[]',
  environment_json     TEXT NOT NULL DEFAULT '{}',     -- engine version, git commit, protocol
  hardware_json        TEXT NOT NULL DEFAULT '{}'      -- cpu/ram/gpu, os/arch
);

CREATE INDEX idx_runs_task ON task_runs (task_id);

CREATE TABLE task_dependencies (
  task_id              BLOB NOT NULL REFERENCES tasks (task_id),
  dependency_id        BLOB NOT NULL REFERENCES tasks (task_id),
  PRIMARY KEY (task_id, dependency_id)
);

CREATE TABLE workers (
  worker_id            BLOB PRIMARY KEY,
  name                 TEXT NOT NULL,
  capabilities_json    TEXT NOT NULL,                  -- worker-capabilities.schema.json
  resource_profile_json TEXT NOT NULL,
  protocol_version     INTEGER NOT NULL,
  max_concurrency      INTEGER NOT NULL DEFAULT 1,
  last_heartbeat_ns    INTEGER,
  status               TEXT NOT NULL DEFAULT 'idle'
);

CREATE TABLE cache_entries (
  cache_key            TEXT PRIMARY KEY,               -- ADR-020 composite key
  artifact_id          BLOB NOT NULL,
  task_type            TEXT NOT NULL,
  producer_version     TEXT NOT NULL,
  git_commit           TEXT NOT NULL,
  config_hash          TEXT NOT NULL,
  created_at_ns        INTEGER NOT NULL,
  status               TEXT NOT NULL DEFAULT 'valid'
);
