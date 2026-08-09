-- RFC-0006 §14 (P2.1 completion-review debt #8): persistent provenance for
-- rejected inputs. A rejected file is never imported: it creates no artifact,
-- no frame, and no observation (image-import.md §12). This table keeps the
-- rejection itself reproducible and queryable — original path, detected MIME,
-- importer identity, the stable IMPORT_* error code, and the timestamp.
-- Applied by the migration runner (core/storage/metadata_db.cpp) which owns
-- the transaction; BEGIN/COMMIT are stripped at build time.

CREATE TABLE import_rejections (
    rejection_id      BLOB PRIMARY KEY,
    project_id        BLOB NOT NULL REFERENCES projects(project_id),
    session_id        BLOB REFERENCES capture_sessions(session_id),
    sequence_index    INTEGER,
    source_path       TEXT NOT NULL,
    mime_type         TEXT,
    importer          TEXT NOT NULL,
    importer_version  TEXT NOT NULL,
    error_code        TEXT NOT NULL,
    diagnostic        TEXT,
    rejected_at_ns    INTEGER NOT NULL
);

CREATE INDEX idx_import_rejections_session ON import_rejections (session_id, rejected_at_ns);
CREATE INDEX idx_import_rejections_source  ON import_rejections (source_path);
