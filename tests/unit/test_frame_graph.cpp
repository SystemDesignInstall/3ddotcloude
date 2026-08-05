#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <utility>

#include <Eigen/Geometry>

#include "core/coordinates/coordinate_frame.h"
#include "core/coordinates/frame_graph.h"
#include "core/coordinates/frame_graph_validator.h"
#include "core/coordinates/frame_id.h"
#include "core/errors/project_error.h"

namespace spatial::core {

struct FrameGraphTestAccess {
  static void InsertUnchecked(FrameGraph& graph, CoordinateFrame frame) {
    graph.InsertUnchecked(std::move(frame));
  }
};

namespace {

constexpr double kTol = 1e-9;

CoordinateFrame MakeFrame(const std::string& name, FrameId parent,
                          geometry::SE3 parent_from_child) {
  CoordinateFrame frame;
  frame.id = FrameId::Generate();
  frame.name = name;
  frame.parent = parent;
  frame.parent_from_child = geometry::RigidTransform(parent_from_child);
  return frame;
}

TEST(FrameGraph, AcyclicChainValidates) {
  FrameGraph graph;
  const FrameId world = FrameId::Generate();
  CoordinateFrame w = MakeFrame("world", FrameId::Nil(), geometry::SE3::Identity());
  w.id = world;
  const FrameId rig = FrameId::Generate();
  CoordinateFrame r = MakeFrame("rig", world, geometry::SE3::Identity());
  r.id = rig;
  const FrameId sensor = FrameId::Generate();
  CoordinateFrame s = MakeFrame("sensor", rig, geometry::SE3::Identity());
  s.id = sensor;

  graph.AddFrame(w);
  graph.AddFrame(r);
  graph.AddFrame(s);

  EXPECT_EQ(graph.Root(), world);
  EXPECT_TRUE(graph.ValidateAcyclic());
  EXPECT_EQ(graph.Size(), 3u);
  EXPECT_TRUE(graph.HasFrame(sensor));
  EXPECT_NE(graph.Find(sensor), nullptr);

  const FrameGraphValidator validator;
  const FrameGraphValidation result = validator.Validate(graph);
  EXPECT_TRUE(result.valid) << result.reason;
}

TEST(FrameGraph, TransformChainEqualsDirectComposition) {
  const geometry::SE3 world_from_rig(geometry::Quaternion::FromAxisAngle(
                                         Eigen::Vector3d(0, 0, 1), 0.3),
                                     Eigen::Vector3d(1.0, 2.0, 3.0));
  const geometry::SE3 rig_from_sensor(geometry::Quaternion::Identity(),
                                      Eigen::Vector3d(0.1, 0.2, 0.3));

  FrameGraph graph;
  const FrameId world = FrameId::Generate();
  CoordinateFrame w = MakeFrame("world", FrameId::Nil(), geometry::SE3::Identity());
  w.id = world;
  const FrameId rig = FrameId::Generate();
  CoordinateFrame r = MakeFrame("rig", world, world_from_rig);
  r.id = rig;
  const FrameId sensor = FrameId::Generate();
  CoordinateFrame s = MakeFrame("sensor", rig, rig_from_sensor);
  s.id = sensor;
  graph.AddFrame(w);
  graph.AddFrame(r);
  graph.AddFrame(s);

  const geometry::SE3 direct = world_from_rig * rig_from_sensor;
  const geometry::SE3 via_graph = graph.Transform(world, sensor);
  const Eigen::Vector3d p(0.0, 0.0, 0.0);
  EXPECT_TRUE((direct.TransformPoint(p) - via_graph.TransformPoint(p)).norm() < kTol);

  const geometry::SE3 reverse = graph.Transform(sensor, world);
  const Eigen::Vector3d back = reverse.TransformPoint(via_graph.TransformPoint(p));
  EXPECT_TRUE((back - p).norm() < kTol);
}

TEST(FrameGraph, IdentityForSameFrame) {
  FrameGraph graph;
  const FrameId world = FrameId::Generate();
  CoordinateFrame w = MakeFrame("world", FrameId::Nil(), geometry::SE3::Identity());
  w.id = world;
  graph.AddFrame(w);
  const Eigen::Vector3d p(3.0, 4.0, 5.0);
  EXPECT_TRUE(
      (graph.Transform(world, world).TransformPoint(p) - p).norm() < kTol);
}

TEST(FrameGraph, CycleIsRejected) {
  FrameGraph graph;
  const FrameId a = FrameId::Generate();
  const FrameId b = FrameId::Generate();
  const FrameId leaf = FrameId::Generate();
  CoordinateFrame fa = MakeFrame("a", b, geometry::SE3::Identity());
  fa.id = a;
  CoordinateFrame fb = MakeFrame("b", a, geometry::SE3::Identity());
  fb.id = b;
  CoordinateFrame fl = MakeFrame("leaf", a, geometry::SE3::Identity());
  fl.id = leaf;
  FrameGraphTestAccess::InsertUnchecked(graph, std::move(fa));
  FrameGraphTestAccess::InsertUnchecked(graph, std::move(fb));
  FrameGraphTestAccess::InsertUnchecked(graph, std::move(fl));

  EXPECT_FALSE(graph.ValidateAcyclic());

  const FrameGraphValidator validator;
  const FrameGraphValidation result = validator.Validate(graph);
  EXPECT_FALSE(result.valid);
  EXPECT_EQ(result.code, ErrorCode::kCoordFrameCycle);

  // A cycle reachable from the queried pair must be detected, not looped.
  try {
    graph.Transform(leaf, b);
    FAIL() << "expected CoordinateError";
  } catch (const CoordinateError& e) {
    EXPECT_EQ(e.code(), ErrorCode::kCoordFrameCycle);
  }
}

TEST(FrameGraph, MultipleRootsRejected) {
  FrameGraph graph;
  const FrameId a = FrameId::Generate();
  const FrameId b = FrameId::Generate();
  CoordinateFrame fa = MakeFrame("a", FrameId::Nil(), geometry::SE3::Identity());
  fa.id = a;
  CoordinateFrame fb = MakeFrame("b", FrameId::Nil(), geometry::SE3::Identity());
  fb.id = b;
  graph.AddFrame(fa);
  graph.AddFrame(fb);

  try {
    graph.Root();
    FAIL() << "expected CoordinateError";
  } catch (const CoordinateError& e) {
    EXPECT_EQ(e.code(), ErrorCode::kCoordFrameMultipleRoots);
  }
}

TEST(FrameGraph, DuplicateFrameRejected) {
  FrameGraph graph;
  CoordinateFrame frame = MakeFrame("root", FrameId::Nil(), geometry::SE3::Identity());
  graph.AddFrame(frame);
  EXPECT_THROW(graph.AddFrame(frame), CoordinateError);
  try {
    graph.AddFrame(frame);
  } catch (const CoordinateError& e) {
    EXPECT_EQ(e.code(), ErrorCode::kCoordFrameExists);
  }
}

TEST(FrameGraph, DanglingParentRejected) {
  FrameGraph graph;
  CoordinateFrame frame =
      MakeFrame("orphan", FrameId::Generate(), geometry::SE3::Identity());
  try {
    graph.AddFrame(frame);
    FAIL() << "expected CoordinateError";
  } catch (const CoordinateError& e) {
    EXPECT_EQ(e.code(), ErrorCode::kCoordFrameNotFound);
  }
}

TEST(FrameGraph, DisconnectedFramesRejected) {
  FrameGraph graph;
  const FrameId world1 = FrameId::Generate();
  const FrameId world2 = FrameId::Generate();
  const FrameId leaf1 = FrameId::Generate();
  const FrameId leaf2 = FrameId::Generate();

  CoordinateFrame w1 = MakeFrame("w1", FrameId::Nil(), geometry::SE3::Identity());
  w1.id = world1;
  CoordinateFrame l1 = MakeFrame("l1", world1, geometry::SE3::Identity());
  l1.id = leaf1;
  CoordinateFrame w2 = MakeFrame("w2", FrameId::Nil(), geometry::SE3::Identity());
  w2.id = world2;
  CoordinateFrame l2 = MakeFrame("l2", world2, geometry::SE3::Identity());
  l2.id = leaf2;
  graph.AddFrame(w1);
  graph.AddFrame(l1);
  graph.AddFrame(w2);
  graph.AddFrame(l2);

  EXPECT_TRUE(graph.ValidateAcyclic());

  const FrameGraphValidator validator;
  const FrameGraphValidation validation = validator.Validate(graph);
  EXPECT_FALSE(validation.valid);
  EXPECT_EQ(validation.code, ErrorCode::kCoordFrameMultipleRoots);

  try {
    graph.Transform(leaf1, leaf2);
    FAIL() << "expected CoordinateError";
  } catch (const CoordinateError& e) {
    EXPECT_EQ(e.code(), ErrorCode::kCoordFrameDisconnected);
  }
}

TEST(FrameGraph, UnknownFrameRejected) {
  FrameGraph graph;
  CoordinateFrame root = MakeFrame("root", FrameId::Nil(), geometry::SE3::Identity());
  graph.AddFrame(root);
  EXPECT_THROW(graph.Transform(FrameId::Generate(), root.id), CoordinateError);
  try {
    graph.Transform(root.id, FrameId::Generate());
  } catch (const CoordinateError& e) {
    EXPECT_EQ(e.code(), ErrorCode::kCoordFrameNotFound);
  }
}

TEST(FrameGraphValidator, EmptyGraphInvalid) {
  FrameGraph graph;
  const FrameGraphValidator validator;
  const FrameGraphValidation result = validator.Validate(graph);
  EXPECT_FALSE(result.valid);
  EXPECT_EQ(result.code, ErrorCode::kCoordFrameNotFound);
}

}  // namespace
}  // namespace spatial::core
