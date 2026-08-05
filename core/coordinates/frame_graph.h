#pragma once

// Directed acyclic frame graph (RFC-0002 §7.3). Invariants: exactly one root
// (nil parent), every parent edge targets an existing frame, no cycles.
// Transform(from, to) resolves the unique path between two frames.

#include <optional>
#include <unordered_map>
#include <vector>

#include "core/coordinates/coordinate_frame.h"
#include "core/geometry/se3.h"

namespace spatial::core {

struct FrameGraphTestAccess;  // test-only insertion hook (bypasses invariants)

class FrameGraph {
 public:
  friend struct FrameGraphTestAccess;

  // Throws CoordinateError(kCoordFrameExists) on a duplicate id and
  // CoordinateError(kCoordFrameNotFound) when parent does not exist.
  void AddFrame(CoordinateFrame frame);

  const CoordinateFrame* Find(FrameId id) const noexcept;
  bool HasFrame(FrameId id) const noexcept;
  std::size_t Size() const noexcept { return frames_.size(); }

  // Exactly one root is required; throws kCoordFrameNotFound when there is
  // none and kCoordFrameMultipleRoots when there are several.
  FrameId Root() const;

  const std::vector<FrameId>& Frames() const noexcept { return frame_order_; }

  // Returns M with p_from = M * p_to. Identity when from == to. Throws
  // kCoordFrameNotFound for unknown frames and kCoordFrameDisconnected when
  // the frames are not ancestor/descendant (different trees or cycles).
  geometry::SE3 Transform(FrameId from, FrameId to) const;

  // false when the graph contains a cycle or a dangling parent edge.
  bool ValidateAcyclic() const noexcept;

 private:
  // Invariant-bypassing insert for loading damaged/legacy graphs and for
  // testing the validator. ValidateAcyclic() must gate any use of a graph
  // built through this path.
  void InsertUnchecked(CoordinateFrame frame);

  std::unordered_map<FrameId, CoordinateFrame> frames_;
  std::vector<FrameId> frame_order_;
};

}  // namespace spatial::core
