-- Spatial Platform: first migration.
-- Applies schema version 1 (identical to HEAD schema.sql).
--
-- Applied atomically by the Project Core migration runner (P1).

PRAGMA foreign_keys = ON;
BEGIN TRANSACTION;

CREATE TABLE schema_meta (
    version    INTEGER PRIMARY KEY,
    applied_at TEXT    NOT NULL DEFAULT (datetime('now'))
);

CREATE TABLE projects (
    project_id      BLOB    PRIMARY KEY,
    name            TEXT    NOT NULL,
    schema_version  INTEGER NOT NULL,
    created_by_json TEXT    NOT NULL,
    created_at_ns   INTEGER NOT NULL,
    default_crs     TEXT,
    root_frame      TEXT,
    flags_json      TEXT,
    properties_json TEXT
);

CREATE TABLE capture_sessions (
    session_id      BLOB PRIMARY KEY,
    project_id      BLOB NOT NULL REFERENCES projects(project_id),
    name            TEXT,
    started_at_ns   INTEGER,
    ended_at_ns     INTEGER,
    source_uri      TEXT,
    status          TEXT,
    provenance_json TEXT
);

CREATE TABLE sensors (
    sensor_id      BLOB PRIMARY KEY,
    project_id     BLOB NOT NULL REFERENCES projects(project_id),
    type           TEXT NOT NULL,
    manufacturer   TEXT,
    model          TEXT,
    serial_number  TEXT,
    time_domain    TEXT,
    calibration_id BLOB,
    rig_id         BLOB,
    source_json    TEXT,
    status         TEXT
);

CREATE TABLE sensor_rigs (
    rig_id              BLOB PRIMARY KEY,
    project_id          BLOB NOT NULL REFERENCES projects(project_id),
    name                TEXT,
    config_json         TEXT,
    calibration_version INTEGER
);

CREATE TABLE rig_links (
    rig_id               BLOB NOT NULL REFERENCES sensor_rigs(rig_id),
    sensor_id            BLOB NOT NULL REFERENCES sensors(sensor_id),
    rig_from_sensor_json TEXT NOT NULL,
    time_offset_ns       INTEGER,
    PRIMARY KEY (rig_id, sensor_id)
);

CREATE TABLE calibrations (
    calibration_id      BLOB PRIMARY KEY,
    sensor_id           BLOB NOT NULL REFERENCES sensors(sensor_id),
    version             INTEGER NOT NULL,
    calibration_time_ns INTEGER,
    source              TEXT,
    intrinsics_json     TEXT,
    distortion_json     TEXT,
    extrinsics_json     TEXT,
    uncertainty_json    TEXT
);

CREATE TABLE coordinate_frames (
    frame_id              TEXT PRIMARY KEY,
    scene_id              BLOB,
    parent_frame          TEXT,
    type                  TEXT,
    static_transform_json TEXT,
    calibration_version   INTEGER,
    source                TEXT
);

CREATE TABLE scenes (
    scene_id           BLOB PRIMARY KEY,
    project_id         BLOB NOT NULL REFERENCES projects(project_id),
    schema_version     INTEGER NOT NULL,
    name               TEXT,
    current_version_id BLOB,
    origin_frame       TEXT,
    crs                TEXT,
    status             TEXT,
    properties_json    TEXT
);

CREATE TABLE scene_versions (
    version_id        BLOB PRIMARY KEY,
    scene_id          BLOB NOT NULL REFERENCES scenes(scene_id),
    parent_version_id BLOB,
    stage             TEXT NOT NULL,
    created_by_json   TEXT,
    created_at_ns     INTEGER,
    status            TEXT
);

CREATE TABLE frames (
    frame_id        BLOB PRIMARY KEY,
    scene_id        BLOB NOT NULL REFERENCES scenes(scene_id),
    session_id      BLOB,
    timestamp_ns    INTEGER NOT NULL,
    sequence_index  INTEGER,
    sensor_id       BLOB REFERENCES sensors(sensor_id),
    pose_ref        BLOB,
    properties_json TEXT
);

CREATE TABLE poses (
    pose_id         BLOB PRIMARY KEY,
    frame_id        BLOB NOT NULL REFERENCES frames(frame_id),
    transform_json  TEXT NOT NULL,
    covariance_json TEXT,
    source_json     TEXT,
    status          TEXT
);

CREATE TABLE trajectories (
    trajectory_id   BLOB PRIMARY KEY,
    session_id      BLOB,
    kind            TEXT,
    artifact_ref    TEXT,
    confidence_json TEXT,
    properties_json TEXT
);

CREATE TABLE observations (
    observation_id  BLOB PRIMARY KEY,
    scene_id        BLOB NOT NULL REFERENCES scenes(scene_id),
    sensor_id       BLOB NOT NULL REFERENCES sensors(sensor_id),
    frame_id        BLOB REFERENCES frames(frame_id),
    session_id      BLOB,
    timestamp_ns    INTEGER NOT NULL,
    type            TEXT NOT NULL,
    artifact_ref    TEXT,
    source_json     TEXT,
    properties_json TEXT
);

CREATE TABLE observation_payloads (
    observation_id BLOB PRIMARY KEY REFERENCES observations(observation_id),
    width          INTEGER,
    height         INTEGER,
    pixel_format   TEXT,
    depth_min      REAL,
    depth_max      REAL,
    range_unit     TEXT,
    fix_type       TEXT,
    satellites     INTEGER,
    rtk_status     TEXT,
    projection     TEXT
);

CREATE TABLE geometry_elements (
    element_id      BLOB PRIMARY KEY,
    scene_id        BLOB NOT NULL REFERENCES scenes(scene_id),
    kind            TEXT NOT NULL,
    frame           TEXT NOT NULL,
    bounds_json     TEXT,
    lod             INTEGER,
    parent_id       BLOB,
    provenance_json TEXT NOT NULL,
    artifact_ref    TEXT NOT NULL,
    uncertainty_ref TEXT
);

CREATE TABLE control_points (
    control_point_id BLOB PRIMARY KEY,
    name             TEXT,
    crs              TEXT,
    coordinates_json TEXT,
    covariance_json  TEXT,
    constraint_type  TEXT,
    status           TEXT,
    source           TEXT
);

CREATE TABLE control_point_observations (
    control_point_id BLOB NOT NULL REFERENCES control_points(control_point_id),
    observation_id   BLOB NOT NULL REFERENCES observations(observation_id),
    PRIMARY KEY (control_point_id, observation_id)
);

CREATE TABLE feature_sets (
    feature_set_id  BLOB PRIMARY KEY,
    frame_id        BLOB,
    detector        TEXT,
    descriptor_type TEXT,
    count           INTEGER,
    artifact_ref    TEXT
);

CREATE TABLE match_sets (
    match_set_id  BLOB PRIMARY KEY,
    feature_set_a BLOB,
    feature_set_b BLOB,
    matcher       TEXT,
    count         INTEGER,
    artifact_ref  TEXT
);

CREATE TABLE reconstructions (
    reconstruction_id          BLOB PRIMARY KEY,
    scene_id                   BLOB NOT NULL REFERENCES scenes(scene_id),
    type                       TEXT,
    backend                    TEXT,
    input_artifact_hashes_json TEXT,
    config_hash                TEXT,
    status                     TEXT,
    quality_report_id          BLOB
);

CREATE TABLE surface_models (
    surface_model_id BLOB PRIMARY KEY,
    reconstruction_id BLOB REFERENCES reconstructions(reconstruction_id),
    geometry_type    TEXT,
    bounds_json      TEXT,
    stats_json       TEXT,
    artifact_ref     TEXT
);

CREATE TABLE texture_sets (
    texture_set_id    BLOB PRIMARY KEY,
    surface_model_id  BLOB REFERENCES surface_models(surface_model_id),
    uv_layout         TEXT,
    material_json     TEXT,
    artifact_refs_json TEXT
);

CREATE TABLE gaussian_models (
    gaussian_model_id BLOB PRIMARY KEY,
    reconstruction_id BLOB REFERENCES reconstructions(reconstruction_id),
    splat_count       INTEGER,
    params_json       TEXT,
    origin_frame      TEXT,
    artifact_ref      TEXT,
    asset_tag         TEXT
);

CREATE TABLE quality_reports (
    report_id    BLOB PRIMARY KEY,
    scene_id     BLOB,
    scope        TEXT,
    metrics_json TEXT,
    artifact_ref TEXT
);

CREATE TABLE processing_jobs (
    job_id           BLOB PRIMARY KEY,
    project_id       BLOB NOT NULL REFERENCES projects(project_id),
    pipeline_id      TEXT,
    recipe_ref_json  TEXT,
    config_hash      TEXT,
    dag_id           TEXT,
    status           TEXT,
    progress         REAL,
    error_json       TEXT,
    timings_json     TEXT
);

CREATE TABLE tasks (
    task_id           BLOB PRIMARY KEY,
    job_id            BLOB NOT NULL REFERENCES processing_jobs(job_id),
    task_type         TEXT NOT NULL,
    inputs_json       TEXT,
    outputs_json      TEXT,
    deps_json         TEXT,
    resources_json    TEXT,
    retry_policy_json TEXT,
    cancel_policy_json TEXT,
    cache_policy_json TEXT,
    deterministic     INTEGER,
    status            TEXT,
    attempts          INTEGER,
    worker_id         TEXT,
    cache_key         TEXT,
    started_at_ns     INTEGER,
    completed_at_ns   INTEGER
);

CREATE TABLE artifacts (
    artifact_id      BLOB PRIMARY KEY,
    content_hash     TEXT NOT NULL UNIQUE,
    type             TEXT NOT NULL,
    schema_version   INTEGER,
    producer_json    TEXT NOT NULL,
    config_hash      TEXT,
    created_at_ns    INTEGER,
    coordinate_frame TEXT,
    unit             TEXT,
    file_size        INTEGER,
    mime_type        TEXT,
    validation_status TEXT
);

CREATE TABLE artifact_dependencies (
    input_hash  TEXT NOT NULL,
    output_hash TEXT NOT NULL,
    role        TEXT,
    PRIMARY KEY (input_hash, output_hash)
);

CREATE TABLE scheduler_state (
    key        TEXT PRIMARY KEY,
    value_json TEXT NOT NULL
);

CREATE TABLE log_records (
    record_id    INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp_ns INTEGER,
    severity     TEXT,
    component    TEXT,
    project_id   BLOB,
    job_id       BLOB,
    task_id      BLOB,
    worker_id    TEXT,
    event        TEXT,
    message      TEXT,
    context_json TEXT,
    error_code   TEXT,
    stack        TEXT
);

CREATE INDEX idx_observations_scene_ts ON observations(scene_id, timestamp_ns);
CREATE INDEX idx_frames_scene_ts      ON frames(scene_id, timestamp_ns);
CREATE INDEX idx_geometry_scene_kind  ON geometry_elements(scene_id, kind);
CREATE INDEX idx_tasks_job            ON tasks(job_id);
CREATE INDEX idx_artifacts_hash       ON artifacts(content_hash);

INSERT INTO schema_meta (version) VALUES (1);
COMMIT;
