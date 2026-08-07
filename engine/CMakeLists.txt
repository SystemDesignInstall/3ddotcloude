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

# ---------------------------------------------------------------------------
# Worker IPC codegen (ADR-011/012, RFC-0003 §5.7). worker.proto and
# errors.proto are the frozen cross-language contract; generated C++ lives in
# the build tree and is never committed.
# ---------------------------------------------------------------------------
find_package(protobuf CONFIG REQUIRED)

set(SPATIAL_PROTOBUF_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../schemas/protobuf")
set(SPATIAL_GENERATED_PROTO_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${SPATIAL_GENERATED_PROTO_DIR}")

set(SPATIAL_PROTO_GENERATED
  "${SPATIAL_GENERATED_PROTO_DIR}/worker.pb.cc"
  "${SPATIAL_GENERATED_PROTO_DIR}/worker.pb.h"
  "${SPATIAL_GENERATED_PROTO_DIR}/worker_pb2.py"
  "${SPATIAL_GENERATED_PROTO_DIR}/errors.pb.cc"
  "${SPATIAL_GENERATED_PROTO_DIR}/errors.pb.h"
  "${SPATIAL_GENERATED_PROTO_DIR}/errors_pb2.py"
)

add_custom_command(
  OUTPUT ${SPATIAL_PROTO_GENERATED}
  COMMAND protobuf::protoc
  ARGS --proto_path=${SPATIAL_PROTOBUF_INCLUDE}
       --cpp_out=${SPATIAL_GENERATED_PROTO_DIR}
       --python_out=${SPATIAL_GENERATED_PROTO_DIR}
       worker.proto errors.proto
  DEPENDS
    ${SPATIAL_PROTOBUF_INCLUDE}/worker.proto
    ${SPATIAL_PROTOBUF_INCLUDE}/errors.proto
  VERBATIM
)

# Generated protobuf code is third-party output: it must never fail the build
# on compiler warnings (it is not ours to fix).
set_source_files_properties(
  "${SPATIAL_GENERATED_PROTO_DIR}/worker.pb.cc"
  "${SPATIAL_GENERATED_PROTO_DIR}/errors.pb.cc"
  PROPERTIES COMPILE_OPTIONS "/WX-")

# Runtime paths for the demo worker (used by tests and the ProcessExecutor).
# Paths are normalized to forward slashes so they are valid in string literals.
set(SPATIAL_DEMO_WORKER_SCRIPT
    "${CMAKE_CURRENT_SOURCE_DIR}/workers/python/demo_worker.py")
file(TO_CMAKE_PATH "${SPATIAL_DEMO_WORKER_SCRIPT}" SPATIAL_DEMO_WORKER_SCRIPT)
file(TO_CMAKE_PATH "${SPATIAL_GENERATED_PROTO_DIR}" SPATIAL_GENERATED_PROTO_DIR)
configure_file(workers/worker_paths.h.in
               "${CMAKE_CURRENT_BINARY_DIR}/worker_paths.h" @ONLY)

add_library(spatial_engine STATIC
  engine.cpp
  resources/resource_spec.cpp
  task/task_types.cpp
  task/task_graph.cpp
  task/task_serialization.cpp
  execution/execution_record.cpp
  scheduler/task_state_machine.cpp
  scheduler/scheduler_state_store.cpp
  scheduler/scheduler.cpp
  cache/task_cache.cpp
  pipeline/pipeline_registry.cpp
  pipeline/pipeline_compiler.cpp
  pipeline/execution_manifest.cpp
  pipeline/quality/quality_report.cpp
  pipeline/mock_photogrammetry.cpp
  workers/protocol_framing.cpp
  workers/in_process_executor.cpp
  workers/process_executor.cpp
  workers/mock_pipeline_runner.cpp
  ${SPATIAL_PROTO_GENERATED}
)

if(WIN32)
  target_sources(spatial_engine PRIVATE workers/child_process_win32.cpp)
else()
  target_sources(spatial_engine PRIVATE workers/child_process_posix.cpp)
endif()

target_include_directories(spatial_engine PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(spatial_engine PUBLIC spatial_core protobuf::libprotobuf)

set_target_properties(spatial_engine PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
)
