# P3-impl-6a: GTSAM optimizer adapter (isolated from core).
#
# This target is the ONLY place in the spatial-platform codebase that links
# GTSAM::GTSAM. The core library (spatial_core) and all other adapters must
# remain GTSAM-free. This is the boundary invariant validated by the
# P3-impl-6a verification report.

# Build identity for provenance (RFC-0008 §8).
find_program(SPATIAL_GTSAM_GIT_EXECUTABLE git)
execute_process(
  COMMAND "${SPATIAL_GTSAM_GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}/../.."
          rev-parse HEAD
  RESULT_VARIABLE SPATIAL_GTSAM_GIT_RESULT
  OUTPUT_VARIABLE SPATIAL_GTSAM_ADAPTER_GIT_COMMIT
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT SPATIAL_GTSAM_GIT_RESULT EQUAL 0 OR SPATIAL_GTSAM_ADAPTER_GIT_COMMIT STREQUAL "")
  set(SPATIAL_GTSAM_ADAPTER_GIT_COMMIT "unknown")
endif()
set(SPATIAL_GTSAM_ADAPTER_VERSION "0.1.0")
configure_file(gtsam_adapter_build_info.h.in
               "${CMAKE_CURRENT_BINARY_DIR}/gtsam_adapter_build_info.h"
               @ONLY)

add_library(spatial_gtsam_adapter STATIC
  gtsam_optimizer_adapter.cpp
)

target_include_directories(spatial_gtsam_adapter PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../..
  ${CMAKE_CURRENT_BINARY_DIR}
)

# The ONLY GTSAM link in the entire project. Core and all other adapters
# must never depend on this target.
target_link_libraries(spatial_gtsam_adapter PUBLIC
  spatial_core
  PRIVATE gtsam::gtsam
)

set_target_properties(spatial_gtsam_adapter PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
)

# GTSAM headers produce warnings in our /W4 /WX build; suppress them since
# they are third-party code not ours to fix (same pattern as protobuf and engine).
target_compile_options(spatial_gtsam_adapter PRIVATE /WX-)
