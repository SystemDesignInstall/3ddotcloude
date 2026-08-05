#include "engine/resources/resource_spec.h"

#include <algorithm>

namespace spatial::engine {

bool Fits(const ResourceSpec& spec, const ResourceSpec& capacity) noexcept {
  return spec.cores <= capacity.cores && spec.ram_bytes <= capacity.ram_bytes &&
         spec.gpus <= capacity.gpus &&
         spec.gpu_mem_bytes <= capacity.gpu_mem_bytes &&
         spec.temp_disk_bytes <= capacity.temp_disk_bytes;
}

bool HasCapabilities(const std::vector<std::string>& worker_capabilities,
                     const std::vector<std::string>& required) {
  for (const auto& capability : required) {
    if (std::find(worker_capabilities.begin(), worker_capabilities.end(),
                  capability) == worker_capabilities.end()) {
      return false;
    }
  }
  return true;
}

}  // namespace spatial::engine
