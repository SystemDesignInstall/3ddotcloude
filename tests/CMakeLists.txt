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
  unit/test_cli_import.cpp
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
)

include(GoogleTest)
gtest_discover_tests(spatial_unit_tests)
