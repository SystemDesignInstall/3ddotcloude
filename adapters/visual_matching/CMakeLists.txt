# P3-impl-7a: classical visual matcher adapter (Q1/Q3). Pure, deterministic,
# std-only implementation of the backend-independent LoopClosureFeatureMatcher
# contract. It links only core (for the canonical feature matcher contract),
# never the engine aggregate — proving the seam is self-contained (RFC-0008 §5).

add_library(spatial_visual_matching_adapter STATIC
  visual_matcher_adapter.cpp
)

target_include_directories(spatial_visual_matching_adapter PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

target_link_libraries(spatial_visual_matching_adapter PUBLIC
  spatial_core
)

set_target_properties(spatial_visual_matching_adapter PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
)
