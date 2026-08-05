#pragma once

// Typed resource model (RFC-0003 §5.8, engine.md §3). The scheduler is the
// single allocator (process-model §5): workers advertise a ResourceProfile and
// the scheduler checks that a task's ResourceSpec fits before dispatch.
// Capabilities follow the frozen worker-capabilities.schema.json taxonomy
// (ADR-011/034): selection is by capability, never by task name.

#include <cstdint>
#include <string>
#include <vector>

namespace spatial::engine {

// Typed requirements of one task. All sizes are in bytes (ADR-007: typed).
struct ResourceSpec {
  int cores = 1;
  std::int64_t ram_bytes = 0;
  int gpus = 0;
  std::int64_t gpu_mem_bytes = 0;
  std::int64_t temp_disk_bytes = 0;

  bool operator==(const ResourceSpec&) const = default;
};

// Advertised capacity of one worker plus the capabilities it implements.
struct ResourceProfile {
  std::string name;
  std::vector<std::string> capabilities;  // worker-capabilities.schema.json
  ResourceSpec capacity;
  int max_concurrency = 1;
};

// True when `spec` fits entirely within `capacity`.
bool Fits(const ResourceSpec& spec, const ResourceSpec& capacity) noexcept;

// True when `worker_capabilities` contains every capability in `required`.
bool HasCapabilities(const std::vector<std::string>& worker_capabilities,
                     const std::vector<std::string>& required);

}  // namespace spatial::engine
