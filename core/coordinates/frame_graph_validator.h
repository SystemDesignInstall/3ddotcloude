#pragma once

// Validation facade over FrameGraph invariants (RFC-0002 §7.3): exactly one
// root, no cycles, no dangling parents. Returns a structured result instead
// of throwing, for use at import and edit boundaries where a report is
// friendlier than an exception.

#include <string>

#include "core/coordinates/frame_graph.h"
#include "core/errors/project_error.h"

namespace spatial::core {

struct FrameGraphValidation {
  bool valid = true;
  std::string reason;             // empty when valid
  ErrorCode code = ErrorCode::kInternal;  // stable code of the first violation
};

class FrameGraphValidator {
 public:
  FrameGraphValidation Validate(const FrameGraph& graph) const noexcept;
};

}  // namespace spatial::core
