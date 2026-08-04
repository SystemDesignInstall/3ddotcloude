cmake_minimum_required(VERSION 3.28)

project(spatial_platform
  VERSION 0.1.0
  DESCRIPTION "Spatial Platform: scene-centric 3D reconstruction platform"
  LANGUAGES CXX
)

# ---------------------------------------------------------------------------
# Options (ADR-022: preset-driven configuration only).
# ---------------------------------------------------------------------------
option(SPATIAL_ENABLE_TESTS "Build the unit/integration test suites" ON)
option(SPATIAL_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  message(FATAL_ERROR "Configure via a CMake preset only (ADR-022). "
                      "Use: cmake --preset default|release")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT MSVC)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic -Wconversion")
endif()
if(SPATIAL_WARNINGS_AS_ERRORS)
  if(MSVC)
    add_compile_options(/W4 /WX)
  else()
    add_compile_options(-Werror)
  endif()
endif()

if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_compile_definitions(SPATIAL_NDEBUG)
endif()

# ---------------------------------------------------------------------------
# Dependencies (Conan 2, ADR-003). CMakeDeps targets, no ad hoc find_package.
# ---------------------------------------------------------------------------
find_package(Eigen3 CONFIG REQUIRED)
find_package(SQLite3 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_subdirectory(core)

if(SPATIAL_ENABLE_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
