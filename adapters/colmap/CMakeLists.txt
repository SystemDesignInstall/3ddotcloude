# COLMAP adapter (RFC-0008 §5; C1 plan §1.1 adapters/colmap/**).
#
# The COLMAP backend is LAUNCHED, never linked: this target contains no COLMAP
# build/link dependency and no COLMAP header (THIRD_PARTY.yml keeps COLMAP
# status `planned` until the worker proves the chain end-to-end, RFC-0008 §16).
# The adapter links core only for the ratified typed-error types (ADR-014);
# it never links the engine — it consumes the engine's header-only protocol
# value types (TaskRequest / ResourceProfile) and the engine never depends on
# this target.

# Build identity: the git commit is part of artifact provenance (RFC-0008 §8,
# producer block). Falls back to "unknown" outside a git checkout.
find_program(SPATIAL_COLMAP_GIT_EXECUTABLE git)
execute_process(
  COMMAND "${SPATIAL_COLMAP_GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}/../.."
          rev-parse HEAD
  RESULT_VARIABLE SPATIAL_COLMAP_GIT_RESULT
  OUTPUT_VARIABLE SPATIAL_COLMAP_ADAPTER_GIT_COMMIT
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT SPATIAL_COLMAP_GIT_RESULT EQUAL 0 OR SPATIAL_COLMAP_ADAPTER_GIT_COMMIT STREQUAL "")
  set(SPATIAL_COLMAP_ADAPTER_GIT_COMMIT "unknown")
endif()
set(SPATIAL_COLMAP_ADAPTER_VERSION "0.1.0")
configure_file(colmap_adapter_build_info.h.in
               "${CMAKE_CURRENT_BINARY_DIR}/colmap_adapter_build_info.h"
               @ONLY)

add_library(spatial_colmap_adapter STATIC
  colmap_adapter.cpp
  colmap_config.cpp
)

target_include_directories(spatial_colmap_adapter PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../..
  ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(spatial_colmap_adapter PUBLIC
  spatial_adapter_interfaces
  spatial_core
  nlohmann_json::nlohmann_json
)

set_target_properties(spatial_colmap_adapter PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
)
