# Unit test suite (ADR-016). Kernel tests use GTest/gmock only.

find_package(GTest CONFIG REQUIRED)

set(SPATIAL_UNIT_TESTS
  unit/test_sha256.cpp
  unit/test_uuid.cpp
  unit/test_errors.cpp
  unit/test_metadata_db.cpp
  unit/test_artifact_store.cpp
  unit/test_project.cpp
  unit/test_coordinates.cpp
  unit/test_geometry.cpp
  unit/test_frame_graph.cpp
  unit/test_sensor_time.cpp
  unit/test_scene.cpp
  unit/test_scene_query.cpp
  unit/test_task_types.cpp
  unit/test_task_graph.cpp
  unit/test_scheduler_state_store.cpp
  unit/test_task_cache.cpp
  unit/test_scheduler.cpp
  unit/test_protocol_framing.cpp
  unit/test_in_process_executor.cpp
  unit/test_process_executor.cpp
  unit/test_engine_e2e.cpp
  unit/test_pipeline.cpp
  unit/test_mock_pipeline_e2e.cpp
  unit/test_quality_report.cpp
  unit/test_image_format.cpp
  unit/test_image_importer.cpp
  unit/test_feature_artifact.cpp
  unit/test_feature_extraction_runner.cpp
  unit/test_calibration_artifact.cpp
  unit/test_reconstruction.cpp
  unit/test_reconstruction_schema.cpp
  unit/test_trajectory.cpp
  unit/test_p3_schema.cpp
  unit/test_cli_import.cpp
  unit/test_cli_feature_extraction.cpp
)

# A tiny frame-echo helper the framing tests drive as a child process; it
# reads framed protobuf messages from stdin and writes them back (worker-
# protocol §1 framing, exercised over real pipes).
add_executable(spatial_frame_echo frame_echo_main.cpp)
target_link_libraries(spatial_frame_echo PRIVATE spatial_engine)
target_include_directories(spatial_frame_echo PRIVATE ${CMAKE_SOURCE_DIR})

add_executable(spatial_unit_tests ${SPATIAL_UNIT_TESTS})
target_link_libraries(spatial_unit_tests PRIVATE
  spatial_core
  spatial_engine
  spatial_importers
  GTest::gtest
  GTest::gtest_main
)
# The framing tests spawn spatial_frame_echo; make it a real build dependency
# so both Debug and Release configurations produce it.
add_dependencies(spatial_unit_tests spatial_frame_echo)
# The CLI import tests spawn the spatial binary itself.
add_dependencies(spatial_unit_tests spatial)
target_include_directories(spatial_unit_tests PRIVATE ${CMAKE_SOURCE_DIR})

# Runtime paths for the demo-worker integration tests (worker_paths.h).
target_compile_definitions(spatial_unit_tests PRIVATE
  SPATIAL_FRAME_ECHO_EXECUTABLE="$<TARGET_FILE:spatial_frame_echo>"
  SPATIAL_CLI_EXECUTABLE="$<TARGET_FILE:spatial>"
  SPATIAL_QUALITY_SCHEMA_JSON="${CMAKE_SOURCE_DIR}/schemas/json/quality-report.schema.json"
  SPATIAL_IMAGE_SCHEMA_JSON="${CMAKE_SOURCE_DIR}/schemas/json/image.schema.json"
  SPATIAL_FEATURE_SCHEMA_JSON="${CMAKE_SOURCE_DIR}/schemas/json/feature.schema.json"
  SPATIAL_CALIBRATION_SCHEMA_JSON="${CMAKE_SOURCE_DIR}/schemas/json/calibration.schema.json"
  SPATIAL_RECONSTRUCTION_SCHEMA_JSON="${CMAKE_SOURCE_DIR}/schemas/json/reconstruction.schema.json"
  SPATIAL_TRAJECTORY_SCHEMA_JSON="${CMAKE_SOURCE_DIR}/schemas/json/trajectory.schema.json"
  SPATIAL_POSEGRAPH_SCHEMA_JSON="${CMAKE_SOURCE_DIR}/schemas/json/pose-graph.schema.json"
  SPATIAL_LOOPCLOSURE_SCHEMA_JSON="${CMAKE_SOURCE_DIR}/schemas/json/loop-closure.schema.json"
  SPATIAL_OPTIMIZATION_RESULT_SCHEMA_JSON="${CMAKE_SOURCE_DIR}/schemas/json/optimization-result.schema.json"
)

include(GoogleTest)
gtest_discover_tests(spatial_unit_tests)

# Adapter seam tests (C1-S2, RFC-0008 §5). The adapter tests link only the
# adapter (which brings core's typed-error types) — never the engine aggregate
# — proving the seam is self-contained behind adapters/interfaces/**.
# spatial_colmap_probe_shim stands in for a runnable COLMAP installation in
# the doctor test (ValidateEnvironment present case).
add_executable(spatial_colmap_probe_shim unit/colmap_probe_shim_main.cpp)

add_executable(spatial_adapter_tests
  unit/test_colmap_adapter.cpp
  unit/test_colmap_converter.cpp
  unit/test_colmap_execution.cpp
  unit/test_trajectory_adapter.cpp
)
target_link_libraries(spatial_adapter_tests PRIVATE
  spatial_colmap_adapter
  GTest::gtest
  GTest::gtest_main
)
add_dependencies(spatial_adapter_tests spatial_colmap_probe_shim)
target_include_directories(spatial_adapter_tests PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_definitions(spatial_adapter_tests PRIVATE
  SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE="$<TARGET_FILE:spatial_colmap_probe_shim>"
  SPATIAL_RECONSTRUCTION_SCHEMA_JSON="${CMAKE_SOURCE_DIR}/schemas/json/reconstruction.schema.json"
  SPATIAL_TRAJECTORY_SCHEMA_JSON="${CMAKE_SOURCE_DIR}/schemas/json/trajectory.schema.json"
)
gtest_discover_tests(spatial_adapter_tests)

# Generic adapter-layer subprocess runner tests (C1-S3). Link only the
# std-only spatial_process target + the probe shim (a real child process);
# never the engine.
add_executable(spatial_process_tests unit/test_process_runner.cpp)
target_link_libraries(spatial_process_tests PRIVATE
  spatial_process
  GTest::gtest
  GTest::gtest_main
)
add_dependencies(spatial_process_tests spatial_colmap_probe_shim)
target_include_directories(spatial_process_tests PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_definitions(spatial_process_tests PRIVATE
  SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE="$<TARGET_FILE:spatial_colmap_probe_shim>"
)
gtest_discover_tests(spatial_process_tests)

# COLMAP worker protocol + pipeline E2E tests (C1-S4). Drive the real
# spatial_colmap_worker binary through the engine's ProcessExecutor and the
# registered sparse_reconstruction pipeline (images-only).
add_executable(spatial_colmap_worker_tests
  unit/test_colmap_worker.cpp
  unit/test_colmap_e2e.cpp
)
target_link_libraries(spatial_colmap_worker_tests PRIVATE
  spatial_engine
  spatial_core
  GTest::gtest
  GTest::gtest_main
)
add_dependencies(spatial_colmap_worker_tests
  spatial_colmap_worker
  spatial_colmap_probe_shim
)
target_include_directories(spatial_colmap_worker_tests PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_definitions(spatial_colmap_worker_tests PRIVATE
  SPATIAL_COLMAP_WORKER_EXECUTABLE="$<TARGET_FILE:spatial_colmap_worker>"
  SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE="$<TARGET_FILE:spatial_colmap_probe_shim>"
)
gtest_discover_tests(spatial_colmap_worker_tests)

# P3-impl-7a: Visual loop-closure candidate generation tests. Link core +
# engine (for WriteFeatureArtifactPayload / DetectLoopClosureCandidates) and the
# classical visual matcher adapter, which proves the candidate path from real
# FeatureArtifacts to persisted LoopClosureCandidates + a loop-closure CAS
# payload. Unconditioned (no GTSAM); the adapter is core-only (RFC-0008 §5).
add_executable(spatial_loop_closure_tests
  unit/test_loop_closure_detection.cpp
)
target_link_libraries(spatial_loop_closure_tests PRIVATE
  spatial_core
  spatial_engine
  spatial_visual_matching_adapter
  GTest::gtest
  GTest::gtest_main
)
target_include_directories(spatial_loop_closure_tests PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_definitions(spatial_loop_closure_tests PRIVATE
  SPATIAL_LOOPCLOSURE_SCHEMA_JSON="${CMAKE_SOURCE_DIR}/schemas/json/loop-closure.schema.json"
)
gtest_discover_tests(spatial_loop_closure_tests)

# P3-impl-7b: Geometric loop-closure verification tests. Link core + engine (for
# VerifyLoopClosureGeometry / WriteFeatureArtifactPayload), the classical visual
# matcher adapter (correspondence seam), and the classical geometric verifier
# adapter (F + RANSAC). Proves candidate -> geometric verification -> canonical
# accepted/rejected LoopClosure, incl. the two mandatory golden tests and the
# false-positive / degenerate / determinism / boundary / provenance cases.
add_executable(spatial_loop_closure_verification_tests
  unit/test_loop_closure_verification.cpp
)
target_link_libraries(spatial_loop_closure_verification_tests PRIVATE
  spatial_core
  spatial_engine
  spatial_visual_matching_adapter
  spatial_visual_geometry_adapter
  GTest::gtest
  GTest::gtest_main
)
target_include_directories(spatial_loop_closure_verification_tests PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_definitions(spatial_loop_closure_verification_tests PRIVATE
  SPATIAL_LOOPCLOSURE_SCHEMA_JSON="${CMAKE_SOURCE_DIR}/schemas/json/loop-closure.schema.json"
)
gtest_discover_tests(spatial_loop_closure_verification_tests)

# P3-impl-6a: GTSAM adapter tests — gated, never part of core test suite.
if(SPATIAL_HAS_GTSAM)
  add_executable(spatial_gtsam_tests
    unit/test_gtsam_adapter.cpp
  )
  target_link_libraries(spatial_gtsam_tests PRIVATE
    spatial_gtsam_adapter
    GTest::gtest
    # Intentionally omit gtest_main: GTSAM's transitive boost_test_exec_monitor
    # defines its own main() which conflicts. We provide main() in the test
    # source along with the test_main stub the boost lib requires.
  )
  # GTSAM's transitive Boost dependency auto-links boost_test_exec_monitor,
  # which expects a test_main() symbol. Suppress Boost auto-linking since we
  # use GTest, not Boost.Test.
  target_compile_definitions(spatial_gtsam_tests PRIVATE
    BOOST_ALL_NO_LIB
    SPATIAL_OPTIMIZATION_RESULT_SCHEMA_JSON="${CMAKE_SOURCE_DIR}/schemas/json/optimization-result.schema.json"
    SPATIAL_POSE_GRAPH_SCHEMA_JSON="${CMAKE_SOURCE_DIR}/schemas/json/pose-graph.schema.json"
    SPATIAL_LOOP_CLOSURE_SCHEMA_JSON="${CMAKE_SOURCE_DIR}/schemas/json/loop-closure.schema.json"
  )
  # GTest discovery cannot be used because GTSAM's transitive boost_test_exec_monitor
  # intercepts --gtest_list_tests. Use manual test registration instead.
  add_test(NAME spatial_gtsam_tests COMMAND spatial_gtsam_tests)

  # GTSAM links as a static library, but its transitive runtime deps (TBB /
  # oneTBB and hwloc) are shared libraries that must be on PATH for the test
  # executable to load. Conan's CMakeDeps generator records their bin dirs as
  # <pkg>_BIN_DIRS_<CONFIG> variables (exactly one config suffix is populated
  # per preset). Propagate those dirs into the test's runtime PATH so plain
  # `ctest --test-dir build/<preset>` works without any manual PATH step or
  # committing third-party DLLs. See P3-impl-6b finding #1.
  set(SPATIAL_GTSAM_RUNTIME_BIN_DIRS "")
  foreach(_pkg IN ITEMS onetbb hwloc)
    foreach(_cfg IN ITEMS DEBUG RELEASE)
      if(DEFINED ${_pkg}_BIN_DIRS_${_cfg} AND NOT "${${_pkg}_BIN_DIRS_${_cfg}}" STREQUAL "")
        list(APPEND SPATIAL_GTSAM_RUNTIME_BIN_DIRS "${${_pkg}_BIN_DIRS_${_cfg}}")
      endif()
    endforeach()
  endforeach()
  list(REMOVE_DUPLICATES SPATIAL_GTSAM_RUNTIME_BIN_DIRS)
  if(SPATIAL_GTSAM_RUNTIME_BIN_DIRS)
    # Each ENVIRONMENT_MODIFICATION entry is "VAR=op:VALUE" (singular VALUE,
    # ; separates entries). Prepend each runtime bin dir to PATH in order.
    set(SPATIAL_GTSAM_ENV_LIST "")
    foreach(_dir IN LISTS SPATIAL_GTSAM_RUNTIME_BIN_DIRS)
      list(APPEND SPATIAL_GTSAM_ENV_LIST "PATH=path_list_prepend:${_dir}")
    endforeach()
    string(JOIN ";" SPATIAL_GTSAM_ENV_STR ${SPATIAL_GTSAM_ENV_LIST})
    set_tests_properties(spatial_gtsam_tests PROPERTIES
      ENVIRONMENT_MODIFICATION "${SPATIAL_GTSAM_ENV_STR}"
    )
  endif()
endif()
