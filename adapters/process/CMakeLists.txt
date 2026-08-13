# Generic subprocess runner for the adapter layer (C1-S3; plan §9). A pure
# process-execution primitive (spawn / capture / deadline / cancellation).
# It is backend-agnostic — no COLMAP type or include — and lives OUTSIDE
# adapters/colmap/** because it is not COLMAP-specific. It links nothing but
# the standard library, so adapters can depend on it without ever linking the
# engine (RFC-0008 §5).

if(WIN32)
  set(SPATIAL_PROCESS_PLATFORM_SOURCES process_runner_win32.cpp)
else()
  set(SPATIAL_PROCESS_PLATFORM_SOURCES process_runner_posix.cpp)
endif()

add_library(spatial_process STATIC
  ${SPATIAL_PROCESS_PLATFORM_SOURCES}
)

target_include_directories(spatial_process PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

set_target_properties(spatial_process PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
)
