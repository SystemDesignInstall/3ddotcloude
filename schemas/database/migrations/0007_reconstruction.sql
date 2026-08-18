-- P2.5 (D-CRM-15): Extend the existing reconstructions table (migration 0001)
-- with canonical reconstruction fields. The table already has:
--   reconstruction_id, scene_id, type, backend, input_artifact_hashes_json,
--   config_hash, status, quality_report_id
-- This migration adds coordinate_frame, created_at_ns, and document_json.
-- Applied by the migration runner (core/storage/metadata_db.cpp) which owns
-- the transaction; BEGIN/COMMIT are stripped at build time.

ALTER TABLE reconstructions ADD COLUMN coordinate_frame TEXT;
ALTER TABLE reconstructions ADD COLUMN created_at_ns INTEGER;
ALTER TABLE reconstructions ADD COLUMN document_json TEXT;

-- Backfill: existing rows get a default coordinate_frame and timestamp.
UPDATE reconstructions
   SET coordinate_frame = 'reconstruction_0'
 WHERE coordinate_frame IS NULL;

UPDATE reconstructions
   SET created_at_ns = 0
 WHERE created_at_ns IS NULL;

-- Indexes for P2.5 query patterns.
CREATE INDEX idx_reconstructions_scene     ON reconstructions (scene_id, created_at_ns);
CREATE INDEX idx_reconstructions_status    ON reconstructions (scene_id, status);
