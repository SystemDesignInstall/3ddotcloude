-- RFC-0002 §3.1 (sensor-model.md §3.1) + RFC-0006 §9 (P2.2):
-- calibration validity intervals. Resolution is half-open
-- [valid_from_ns, valid_to_ns); valid_to_ns IS NULL means open-ended
-- (valid for every timestamp >= valid_from_ns). The scalar
-- sensors.calibration_id pointer is a maintained "latest" convenience only
-- and MUST NOT participate in historical resolution.
-- Applied by the migration runner (core/storage/metadata_db.cpp) which owns
-- the transaction; BEGIN/COMMIT are stripped at build time.

ALTER TABLE calibrations ADD COLUMN valid_from_ns INTEGER;
ALTER TABLE calibrations ADD COLUMN valid_to_ns INTEGER;

-- Backfill: pre-existing calibrations are treated as valid from their
-- calibration_time_ns with no upper bound, so every legacy row resolves.
UPDATE calibrations
   SET valid_from_ns = calibration_time_ns
 WHERE valid_from_ns IS NULL;

-- Interval lookups: (sensor, valid_from, valid_to) covers
-- "calibration valid at T" as a range scan.
CREATE INDEX idx_calibrations_sensor_validity
    ON calibrations (sensor_id, valid_from_ns, valid_to_ns);
