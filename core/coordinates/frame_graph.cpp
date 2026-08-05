#include "core/coordinates/frame_graph.h"

#include <unordered_set>

#include "core/errors/project_error.h"

namespace spatial::core {

void FrameGraph::AddFrame(CoordinateFrame frame) {
  if (frames_.contains(frame.id)) {
    throw CoordinateError(ErrorCode::kCoordFrameExists,
                          "frame id already registered",
                          {{"frame", frame.id.ToString()}});
  }
  if (!frame.parent.IsNil() && !frames_.contains(frame.parent)) {
    throw CoordinateError(ErrorCode::kCoordFrameNotFound,
                          "parent frame does not exist",
                          {{"frame", frame.id.ToString()},
                           {"parent", frame.parent.ToString()}});
  }
  frames_.emplace(frame.id, frame);
  frame_order_.push_back(frame.id);
}

const CoordinateFrame* FrameGraph::Find(FrameId id) const noexcept {
  const auto it = frames_.find(id);
  return it == frames_.end() ? nullptr : &it->second;
}

void FrameGraph::InsertUnchecked(CoordinateFrame frame) {
  frames_.emplace(frame.id, std::move(frame));
  frame_order_.push_back(frame.id);
}

bool FrameGraph::HasFrame(FrameId id) const noexcept {
  return frames_.contains(id);
}

FrameId FrameGraph::Root() const {
  FrameId root;
  bool found = false;
  for (const auto& [id, frame] : frames_) {
    if (frame.parent.IsNil()) {
      if (found) {
        throw CoordinateError(ErrorCode::kCoordFrameMultipleRoots,
                              "frame graph has more than one root",
                              {{"first_root", root.ToString()},
                               {"second_root", id.ToString()}});
      }
      root = id;
      found = true;
    }
  }
  if (!found) {
    throw CoordinateError(ErrorCode::kCoordFrameNotFound,
                          "frame graph has no root");
  }
  return root;
}

geometry::SE3 FrameGraph::Transform(FrameId from, FrameId to) const {
  if (from == to) {
    return geometry::SE3::Identity();
  }
  if (!HasFrame(from)) {
    throw CoordinateError(ErrorCode::kCoordFrameNotFound,
                          "from frame does not exist",
                          {{"frame", from.ToString()}});
  }
  if (!HasFrame(to)) {
    throw CoordinateError(ErrorCode::kCoordFrameNotFound,
                          "to frame does not exist",
                          {{"frame", to.ToString()}});
  }

  // Case 1: from is an ancestor of to. Climb from `to` up to `from` and
  // compose the parent_from_child edges in bottom-up order.
  {
    std::vector<geometry::SE3> edges;
    std::unordered_set<FrameId> visited;
    FrameId current = to;
    bool reached = false;
    while (!reached) {
      const CoordinateFrame* frame = Find(current);
      if (frame == nullptr || frame->parent.IsNil()) {
        break;
      }
      if (!visited.insert(current).second) {
        throw CoordinateError(ErrorCode::kCoordFrameCycle,
                              "cycle detected while resolving transform",
                              {{"frame", current.ToString()}});
      }
      edges.push_back(frame->parent_from_child.AsSe3());
      current = frame->parent;
      reached = (current == from);
    }
    if (reached) {
      geometry::SE3 result = geometry::SE3::Identity();
      for (auto it = edges.rbegin(); it != edges.rend(); ++it) {
        result = result * *it;
      }
      return result;
    }
  }

  // Case 2: to is an ancestor of from. Climb from `from` up to `to` and
  // invert the composed chain.
  {
    std::vector<geometry::SE3> edges;
    std::unordered_set<FrameId> visited;
    FrameId current = from;
    bool reached = false;
    while (!reached) {
      const CoordinateFrame* frame = Find(current);
      if (frame == nullptr || frame->parent.IsNil()) {
        break;
      }
      if (!visited.insert(current).second) {
        throw CoordinateError(ErrorCode::kCoordFrameCycle,
                              "cycle detected while resolving transform",
                              {{"frame", current.ToString()}});
      }
      edges.push_back(frame->parent_from_child.AsSe3());
      current = frame->parent;
      reached = (current == to);
    }
    if (reached) {
      geometry::SE3 result = geometry::SE3::Identity();
      for (auto it = edges.rbegin(); it != edges.rend(); ++it) {
        result = result * *it;
      }
      return result.Inverse();
    }
  }

  throw CoordinateError(ErrorCode::kCoordFrameDisconnected,
                        "frames are not connected by an ancestor chain",
                        {{"from", from.ToString()}, {"to", to.ToString()}});
}

bool FrameGraph::ValidateAcyclic() const noexcept {
  // Colour-marked traversal following parent edges from every unvisited node.
  enum class Mark { kWhite, kGray, kBlack };
  std::unordered_map<FrameId, Mark> color;
  color.reserve(frames_.size());
  for (const auto& [id, frame] : frames_) {
    color.emplace(id, Mark::kWhite);
  }

  for (const auto& [start, frame] : frames_) {
    if (color[start] == Mark::kBlack) {
      continue;
    }
    std::vector<FrameId> path;
    FrameId current = start;
    bool done = false;
    while (!done) {
      const auto color_it = color.find(current);
      if (color_it == color.end()) {
        return false;  // dangling parent edge
      }
      const Mark mark = color_it->second;
      if (mark == Mark::kBlack) {
        done = true;
      } else if (mark == Mark::kGray) {
        return false;  // cycle
      } else {
        color[current] = Mark::kGray;
        path.push_back(current);
        const CoordinateFrame* f = Find(current);
        if (f->parent.IsNil()) {
          done = true;  // reached a root
        } else {
          current = f->parent;
        }
      }
    }
    for (const FrameId id : path) {
      color[id] = Mark::kBlack;
    }
  }
  return true;
}

}  // namespace spatial::core
