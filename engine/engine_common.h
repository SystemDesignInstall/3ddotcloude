#pragma once

// Engine-wide common aliases. `Uuid` is core's identity primitive (16-byte
// RFC-4122) reused across the whole engine surface; keeping the alias in one
// header avoids scattering `using`-declarations through every engine TU.
// ArtifactRef and the task model live in engine/task/task_types.h.

#include "core/utils/uuid.h"

namespace spatial::engine {

using spatial::core::Uuid;

}  // namespace spatial::engine
