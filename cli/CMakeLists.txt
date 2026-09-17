# spatial CLI (RFC-0003 P1.4, AC-7): a thin client of the Engine composition
# root. Built with the other binaries so both Debug and Release configurations
# produce it (ADR-022).

add_executable(spatial main.cpp)
target_link_libraries(spatial PRIVATE
  spatial_engine
  spatial_core
  spatial_importers
  spatial_visual_matching_adapter
  spatial_visual_geometry_adapter
)
target_include_directories(spatial PRIVATE ${CMAKE_SOURCE_DIR})
set_target_properties(spatial PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

# Step 12 (real providers): the CLI is the Engine composition root, so it
# injects the REAL GTSAM providers (GtsamTrajectoryOptimizer +
# GtsamBundleAdjustmentOptimizer) on top of the std-only matching/verification
# adapters. GTSAM's shared runtime deps (oneTBB, hwloc) must load whenever
# `spatial` is spawned as a child process (the CLI E2E and the pre-existing
# CLI import/feature-extraction tests), so copy them next to the binary instead
# of requiring a manual PATH step. BOOST_ALL_NO_LIB mirrors the GTSAM test
# targets (autolinking would pull boost_test_exec_monitor).
if(TARGET spatial_gtsam_adapter)
  target_link_libraries(spatial PRIVATE spatial_gtsam_adapter)
  target_compile_definitions(spatial PRIVATE BOOST_ALL_NO_LIB)
  add_custom_command(TARGET spatial POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:spatial> $<TARGET_FILE_DIR:spatial>
    COMMAND_EXPAND_LISTS
  )
endif()
