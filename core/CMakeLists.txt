# Spatial Platform C++ kernel (M0/P1: Project Core + Artifact Store).
#
# Dependency direction enforced here (CONSTITUTION principle 5):
#   project -> storage -> artifacts -> errors/utils
# Raw Eigen stays inside core/geometry/ and adapters/ only.

# Generate migrations_generated.h from the ratified SQL migrations
# (single source of truth under schemas/database/). The runner owns the
# transaction (ADR-009), so the embedded scripts have BEGIN/COMMIT stripped.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../schemas/database/migrations/0001_init.sql" SPATIAL_MIGRATION_0001)
string(REPLACE "PRAGMA foreign_keys = ON;" "" SPATIAL_MIGRATION_0001 "${SPATIAL_MIGRATION_0001}")
string(REPLACE "BEGIN TRANSACTION;" "" SPATIAL_MIGRATION_0001 "${SPATIAL_MIGRATION_0001}")
string(REPLACE "COMMIT;" "" SPATIAL_MIGRATION_0001 "${SPATIAL_MIGRATION_0001}")
# schema_meta is provisioned by the runner (MetadataDb), so the migration
# script must not create it again nor record its own version.
string(REGEX REPLACE "CREATE TABLE schema_meta \\([^;]*\\);" "" SPATIAL_MIGRATION_0001 "${SPATIAL_MIGRATION_0001}")
string(REGEX REPLACE "INSERT INTO schema_meta \\(version\\) VALUES \\(1\\);" "" SPATIAL_MIGRATION_0001 "${SPATIAL_MIGRATION_0001}")
# 0003_scheduler.sql is written without BEGIN/COMMIT (the runner owns the
# transaction) and has no schema_meta handling.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../schemas/database/migrations/0003_scheduler.sql" SPATIAL_MIGRATION_0003)
string(REPLACE "PRAGMA foreign_keys = ON;" "" SPATIAL_MIGRATION_0003 "${SPATIAL_MIGRATION_0003}")
# 0004_execution_manifest.sql (RFC-0003 P1.4) is likewise written without
# BEGIN/COMMIT; the runner records schema version 4 in schema_meta.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../schemas/database/migrations/0004_execution_manifest.sql" SPATIAL_MIGRATION_0004)
string(REPLACE "PRAGMA foreign_keys = ON;" "" SPATIAL_MIGRATION_0004 "${SPATIAL_MIGRATION_0004}")
# 0005_import_rejections.sql (RFC-0006 §14, P2.1 review debt #8) is written
# without BEGIN/COMMIT; the runner records schema version 5 in schema_meta.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../schemas/database/migrations/0005_import_rejections.sql" SPATIAL_MIGRATION_0005)
string(REPLACE "PRAGMA foreign_keys = ON;" "" SPATIAL_MIGRATION_0005 "${SPATIAL_MIGRATION_0005}")
# 0006_sensor_calibration_validity.sql (RFC-0002 §3.1 + RFC-0006 §9) is
# likewise written without BEGIN/COMMIT; the runner records schema version 6.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../schemas/database/migrations/0006_sensor_calibration_validity.sql" SPATIAL_MIGRATION_0006)
string(REPLACE "PRAGMA foreign_keys = ON;" "" SPATIAL_MIGRATION_0006 "${SPATIAL_MIGRATION_0006}")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/storage/migrations_generated.h.in"
               "${CMAKE_CURRENT_BINARY_DIR}/storage/migrations_generated.h" @ONLY)

add_library(spatial_core STATIC
  errors/project_error.cpp
  utils/sha256.cpp
  utils/uuid.cpp
  utils/fs.cpp
  storage/metadata_db.cpp
  artifacts/artifact_manifest.cpp
  artifacts/artifact_store.cpp
  coordinates/frame_graph.cpp
  coordinates/frame_graph_validator.cpp
  project/project.cpp
  scene/query/scene_query.cpp
)

target_include_directories(spatial_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(spatial_core PUBLIC
  SQLite::SQLite3
  nlohmann_json::nlohmann_json
  Eigen3::Eigen
)

set_target_properties(spatial_core PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
)
