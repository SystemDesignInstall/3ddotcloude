# Image importer family (RFC-0006 §6.8). Deterministic, header-only ingest;
# RAW stays an opaque byte payload (image-import.md §14).

add_library(spatial_importers STATIC
  image_format.cpp
)

target_include_directories(spatial_importers PUBLIC
  ${CMAKE_SOURCE_DIR}
)

target_link_libraries(spatial_importers PUBLIC
  spatial_core
)

set_target_properties(spatial_importers PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
)
