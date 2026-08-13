# Spatial Platform adapters (RFC-0007 §7 placeholder activated by RFC-0008 §5).
#
# Dependency direction: adapters -> interfaces -> engine headers (header-only
# protocol value types only); adapters -> core (ratified typed-error types,
# and later the CAS/artifact contracts). The engine never depends on this
# tree: it consumes only WorkerExecutor (ADR-034).

add_subdirectory(interfaces)
add_subdirectory(colmap)
