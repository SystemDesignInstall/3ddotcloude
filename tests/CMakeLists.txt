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
  unit/test_task_types.cpp
  unit/test_task_graph.cpp
  unit/test_scheduler_state_store.cpp
  unit/test_task_cache.cpp
  unit/test_scheduler.cpp
)

add_executable(spatial_unit_tests ${SPATIAL_UNIT_TESTS})
target_link_libraries(spatial_unit_tests PRIVATE
  spatial_core
  spatial_engine
  GTest::gtest
  GTest::gtest_main
)
target_include_directories(spatial_unit_tests PRIVATE ${CMAKE_SOURCE_DIR})

include(GoogleTest)
gtest_discover_tests(spatial_unit_tests)
