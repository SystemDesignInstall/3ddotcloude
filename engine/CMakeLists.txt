# Spatial Platform Processing Engine (RFC-0003, ADR-038).
#
# Dependency direction enforced here (CONSTITUTION §5, engine.md §2):
#   engine/workers -> engine/scheduler -> engine/task -> core/*
#   engine/cache   -> core/artifacts
#   engine/scheduler -> core/storage (persistence)
# The engine owns neither algorithms nor storage nor GPU management (ADR-038).

# Build identity: the engine git commit is embedded at configure time and is
# part of the ADR-020 task-cache key. Falls back to "unknown" outside a git
# checkout (never breaks the build).
find_program(SPATIAL_GIT_EXECUTABLE git)
execute_process(
  COMMAND "${SPATIAL_GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}/.."
          rev-parse HEAD
  RESULT_VARIABLE SPATIAL_GIT_RESULT
  OUTPUT_VARIABLE SPATIAL_ENGINE_GIT_COMMIT
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT SPATIAL_GIT_RESULT EQUAL 0 OR SPATIAL_ENGINE_GIT_COMMIT STREQUAL "")
  set(SPATIAL_ENGINE_GIT_COMMIT "unknown")
endif()
set(SPATIAL_ENGINE_VERSION "0.1.0")
configure_file(engine_build_info.h.in
               "${CMAKE_CURRENT_BINARY_DIR}/engine_build_info.h" @ONLY)

add_library(spatial_engine STATIC
  resources/resource_spec.cpp
  task/task_types.cpp
  task/task_graph.cpp
  task/task_serialization.cpp
  execution/execution_record.cpp
  scheduler/task_state_machine.cpp
  scheduler/scheduler_state_store.cpp
  scheduler/scheduler.cpp
  cache/task_cache.cpp
)

target_include_directories(spatial_engine PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(spatial_engine PUBLIC spatial_core)

set_target_properties(spatial_engine PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
)
