#include "core/coordinates/frame_graph_validator.h"

namespace spatial::core {

FrameGraphValidation FrameGraphValidator::Validate(
    const FrameGraph& graph) const noexcept {
  if (!graph.ValidateAcyclic()) {
    return FrameGraphValidation{false, "frame graph contains a cycle or a "
                                       "dangling parent edge",
                                ErrorCode::kCoordFrameCycle};
  }
  if (graph.Size() == 0) {
    return FrameGraphValidation{false, "frame graph is empty",
                                ErrorCode::kCoordFrameNotFound};
  }
  try {
    graph.Root();
  } catch (const CoordinateError& e) {
    return FrameGraphValidation{false, e.message(), e.code()};
  }
  return FrameGraphValidation{};
}

}  // namespace spatial::core
