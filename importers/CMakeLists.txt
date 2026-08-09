# Importers (RFC-0006). The importer is the only place that touches a source
# file (image-import.md §2); header parsing lives here with no external image
# library crossing the boundary (PPS-0001 §5.1).

# Build identity for the image importer's producer block (image-import.md §8).
# The git commit is embedded at configure time; falls back to "unknown"
# outside a git checkout (never breaks the build).
find_program(SPATIAL_IMPORT_GIT_EXECUTABLE git)
execute_process(
  COMMAND "${SPATIAL_IMPORT_GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}/.."
          rev-parse HEAD
  RESULT_VARIABLE SPATIAL_IMPORT_GIT_RESULT
  OUTPUT_VARIABLE SPATIAL_IMPORT_GIT_COMMIT
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT SPATIAL_IMPORT_GIT_RESULT EQUAL 0 OR SPATIAL_IMPORT_GIT_COMMIT STREQUAL "")
  set(SPATIAL_IMPORT_GIT_COMMIT "unknown")
endif()
set(SPATIAL_IMPORT_VERSION "0.1.0")
configure_file(images/image_import_build_info.h.in
               "${CMAKE_CURRENT_BINARY_DIR}/images/image_import_build_info.h"
               @ONLY)

add_subdirectory(images)
