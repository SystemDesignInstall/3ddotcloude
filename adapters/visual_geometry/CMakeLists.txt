# P3-impl-7b: classical geometric verifier adapter (uncalibrated fundamental +
# RANSAC). Deterministic, std + Eigen only (no OpenCV / COLMAP / GTSAM).
# Links only core (for the canonical GeometricVerifier contract and typed-error
# types), never the engine aggregate — proving the seam is self-contained
# (RFC-0008 §5, P3-impl-7b §14).
# Eigen is permitted here: the root CMake ceiling places raw Eigen in
# core/geometry/ and adapters/ only.

add_library(spatial_visual_geometry_adapter STATIC
  fundamental_verifier.cpp
)

target_include_directories(spatial_visual_geometry_adapter PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

target_link_libraries(spatial_visual_geometry_adapter PUBLIC
  spatial_core
)

set_target_properties(spatial_visual_geometry_adapter PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
)
