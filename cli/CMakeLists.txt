# spatial CLI (RFC-0003 P1.4, AC-7): a thin client of the Engine composition
# root. Built with the other binaries so both Debug and Release configurations
# produce it (ADR-022).

add_executable(spatial main.cpp)
target_link_libraries(spatial PRIVATE spatial_engine spatial_core spatial_importers)
target_include_directories(spatial PRIVATE ${CMAKE_SOURCE_DIR})
set_target_properties(spatial PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)
