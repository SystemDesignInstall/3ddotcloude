# Spatial Platform adapter interfaces (RFC-0007 §7, activated by RFC-0008 §5).
#
# Constitution-protected path (CONSTITUTION §2): changes require a ratified
# RFC. The seam depends only on std types plus the engine's header-only
# protocol value types (TaskRequest / ResourceProfile); it is an INTERFACE
# library (headers only) and never links the engine or core.

add_library(spatial_adapter_interfaces INTERFACE)
target_include_directories(spatial_adapter_interfaces INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/../..
)
target_compile_features(spatial_adapter_interfaces INTERFACE cxx_std_20)
