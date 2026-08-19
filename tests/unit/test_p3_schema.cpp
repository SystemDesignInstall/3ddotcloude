#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <functional>
#include <iterator>
#include <regex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

using nlohmann::json;

// --- Schema loaders ---

json LoadSchema(const char* path) {
  std::ifstream in(path);
  EXPECT_TRUE(in.good()) << "cannot open schema: " << path;
  return json::parse(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

json LoadTrajectorySchema() {
  return LoadSchema(SPATIAL_TRAJECTORY_SCHEMA_JSON);
}
json LoadPoseGraphSchema() {
  return LoadSchema(SPATIAL_POSEGRAPH_SCHEMA_JSON);
}
json LoadLoopClosureSchema() {
  return LoadSchema(SPATIAL_LOOPCLOSURE_SCHEMA_JSON);
}

// --- $ref resolver (test-only, from test_reconstruction_schema.cpp) ---

json ResolveRefsImpl(const json& node, const json& root_defs) {
  if (!node.is_object()) return node;
  if (node.contains("$ref")) {
    const auto ref = node["$ref"].get<std::string>();
    if (ref.rfind("#/definitions/", 0) == 0) {
      const auto name = ref.substr(14);
      if (root_defs.contains(name)) {
        return ResolveRefsImpl(root_defs[name], root_defs);
      }
    }
    return node;
  }
  json out = node;
  for (auto it = out.begin(); it != out.end(); ++it) {
    if (it.key() == "definitions") {
      it.value() = json::object();
    } else {
      it.value() = ResolveRefsImpl(it.value(), root_defs);
    }
  }
  return out;
}

json ResolveRefs(const json& schema) {
  const json& defs =
      schema.contains("definitions") ? schema["definitions"] : json::object();
  return ResolveRefsImpl(schema, defs);
}

// --- Extended schema validator (test-only, from test_reconstruction_schema.cpp) ---

void CheckNodeExtended(const json& schema, const json& doc,
                       std::vector<std::string>* violations) {
  std::function<void(const json&, const json&, const std::string&)> walk =
      [&](const json& s, const json& d, const std::string& p) {
        if (!s.is_object()) return;

        if (s.contains("type")) {
          const std::string t = s["type"];
          bool ok = (t == "string" && d.is_string()) ||
                    (t == "number" && d.is_number()) ||
                    (t == "integer" && d.is_number_integer()) ||
                    (t == "object" && d.is_object()) ||
                    (t == "array" && d.is_array()) ||
                    (t == "boolean" && d.is_boolean());
          if (!ok) {
            violations->push_back(p + ": expected " + t + ", got " +
                                  d.type_name());
          }
        }

        if (s.contains("required")) {
          for (const auto& key : s["required"]) {
            if (!d.is_object() || !d.contains(key)) {
              violations->push_back(p + ": missing required '" +
                                    key.get<std::string>() + "'");
            }
          }
        }

        if (s.contains("const") && d != s["const"]) {
          violations->push_back(p + ": const mismatch");
        }

        if (s.contains("enum")) {
          if (std::find(s["enum"].begin(), s["enum"].end(), d) ==
              s["enum"].end()) {
            violations->push_back(p + ": not in enum");
          }
        }

        if (s.contains("pattern") && d.is_string()) {
          const std::regex re(s["pattern"].get<std::string>());
          if (!std::regex_match(d.get<std::string>(), re)) {
            violations->push_back(p + ": pattern mismatch");
          }
        }

        if (s.contains("minimum") && d.is_number()) {
          if (d.is_number_integer()) {
            if (d.get<std::int64_t>() < s["minimum"].get<std::int64_t>()) {
              violations->push_back(p + ": below minimum");
            }
          } else {
            if (d.get<double>() < s["minimum"].get<double>()) {
              violations->push_back(p + ": below minimum");
            }
          }
        }

        if (s.contains("maximum") && d.is_number()) {
          if (d.is_number_integer()) {
            if (d.get<std::int64_t>() > s["maximum"].get<std::int64_t>()) {
              violations->push_back(p + ": above maximum");
            }
          } else {
            if (d.get<double>() > s["maximum"].get<double>()) {
              violations->push_back(p + ": above maximum");
            }
          }
        }

        if (s.contains("minItems") && d.is_array()) {
          if (d.size() < s["minItems"].get<std::size_t>()) {
            violations->push_back(p + ": below minItems");
          }
        }

        if (s.contains("maxItems") && d.is_array()) {
          if (d.size() > s["maxItems"].get<std::size_t>()) {
            violations->push_back(p + ": exceeds maxItems");
          }
        }

        if (s.contains("minLength") && d.is_string()) {
          if (d.get<std::string>().size() <
              s["minLength"].get<std::size_t>()) {
            violations->push_back(p + ": below minLength");
          }
        }

        if (s.contains("properties") && d.is_object()) {
          for (auto it = s["properties"].begin();
               it != s["properties"].end(); ++it) {
            if (d.contains(it.key())) {
              walk(it.value(), d[it.key()], p + "/" + it.key());
            }
          }
          if (s.contains("additionalProperties") &&
              s["additionalProperties"] == false) {
            for (auto it = d.begin(); it != d.end(); ++it) {
              if (!s["properties"].contains(it.key())) {
                violations->push_back(p + ": unexpected property '" +
                                      it.key() + "'");
              }
            }
          }
        }

        if (s.contains("items") && d.is_array()) {
          for (std::size_t i = 0; i < d.size(); ++i) {
            walk(s["items"], d[i], p + "/" + std::to_string(i));
          }
        }
      };

  walk(schema, doc, "$");
}

std::vector<std::string> Validate(const json& schema, const json& doc) {
  std::vector<std::string> violations;
  CheckNodeExtended(schema, doc, &violations);
  return violations;
}

std::string JoinString(const std::vector<std::string>& v) {
  std::string out;
  for (std::size_t i = 0; i < v.size(); ++i) {
    if (i > 0) out += "; ";
    out += v[i];
  }
  return out;
}

// --- Golden documents ---

json MakeTrajectoryGolden() {
  return json::parse(R"({
    "schema_version": 1,
    "trajectory_id": "a1b2c3d4-e5f6-4a7b-8c9d-0e1f2a3b4c5d",
    "nodes": [
      {
        "frame_id": "6ba7b812-9dad-11d1-80b4-00c04fd430c8",
        "timestamp_ns": 1783123200000000000,
        "sequence_index": 0,
        "position_xyz": [1.0, 2.0, 3.0],
        "rotation_xyzw": [0.0, 0.0, 0.0, 1.0],
        "covariance_position": [0.01, 0.0, 0.0, 0.01, 0.0, 0.01],
        "covariance_rotation": [0.001, 0.0, 0.0, 0.001, 0.0, 0.001]
      },
      {
        "frame_id": "6ba7b813-9dad-11d1-80b4-00c04fd430c8",
        "timestamp_ns": 1783123201000000000,
        "sequence_index": 1,
        "position_xyz": [1.1, 2.05, 3.02],
        "rotation_xyzw": [0.01, 0.02, 0.03, 0.999]
      }
    ]
  })");
}

json MakeTrajectoryMinimal() {
  return json::parse(R"({
    "schema_version": 1,
    "trajectory_id": "a1b2c3d4-e5f6-4a7b-8c9d-0e1f2a3b4c5d",
    "nodes": []
  })");
}

json MakePoseGraphGolden() {
  return json::parse(R"({
    "schema_version": 1,
    "graph_id": "b2c3d4e5-f6a7-4b8c-9d0e-1f2a3b4c5d6e",
    "trajectory_id": "a1b2c3d4-e5f6-4a7b-8c9d-0e1f2a3b4c5d",
    "nodes": [
      {
        "node_id": 0,
        "frame_id": "6ba7b812-9dad-11d1-80b4-00c04fd430c8",
        "timestamp_ns": 1783123200000000000
      },
      {
        "node_id": 1,
        "frame_id": "6ba7b813-9dad-11d1-80b4-00c04fd430c8",
        "timestamp_ns": 1783123201000000000
      }
    ],
    "edges": [
      {
        "edge_id": 0,
        "type": "odometry",
        "source_node_id": 0,
        "target_node_id": 1,
        "relative_position_xyz": [0.1, 0.05, 0.02],
        "relative_rotation_xyzw": [0.0, 0.0, 0.0, 1.0],
        "information_matrix_6x6": [1000, 0, 0, 0, 1000, 0, 0, 0, 1000, 100, 0, 0, 0, 100, 0, 0, 0, 100, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
        "confidence": 0.95,
        "source": "visual_odometry",
        "configuration_hash": "1111111111111111111111111111111111111111111111111111111111111111"
      },
      {
        "edge_id": 1,
        "type": "loop_closure",
        "source_node_id": 1,
        "target_node_id": 0,
        "relative_position_xyz": [-0.1, -0.05, -0.02],
        "relative_rotation_xyzw": [0.0, 0.0, 0.0, 1.0],
        "information_matrix_6x6": [500, 0, 0, 0, 500, 0, 0, 0, 500, 50, 0, 0, 0, 50, 0, 0, 0, 50, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
        "confidence": 0.85,
        "source": "bow_matcher",
        "configuration_hash": "2222222222222222222222222222222222222222222222222222222222222222"
      }
    ]
  })");
}

json MakePoseGraphMinimal() {
  return json::parse(R"({
    "schema_version": 1,
    "graph_id": "b2c3d4e5-f6a7-4b8c-9d0e-1f2a3b4c5d6e",
    "trajectory_id": "a1b2c3d4-e5f6-4a7b-8c9d-0e1f2a3b4c5d",
    "nodes": [],
    "edges": []
  })");
}

json MakeLoopClosureGolden() {
  return json::parse(R"({
    "schema_version": 1,
    "candidates": [
      {
        "candidate_id": "c3d4e5f6-a7b8-4c9d-0e1f-2a3b4c5d6e7f",
        "trajectory_id": "a1b2c3d4-e5f6-4a7b-8c9d-0e1f2a3b4c5d",
        "source_frame_id": "6ba7b812-9dad-11d1-80b4-00c04fd430c8",
        "target_frame_id": "6ba7b813-9dad-11d1-80b4-00c04fd430c8",
        "feature_match_score": 0.85,
        "matcher": "bow",
        "created_at_ns": 1783123200000000000
      }
    ],
    "closures": [
      {
        "closure_id": "d4e5f6a7-b8c9-4d0e-1f2a-3b4c5d6e7f80",
        "trajectory_id": "a1b2c3d4-e5f6-4a7b-8c9d-0e1f2a3b4c5d",
        "candidate_id": "c3d4e5f6-a7b8-4c9d-0e1f-2a3b4c5d6e7f",
        "source_frame_id": "6ba7b812-9dad-11d1-80b4-00c04fd430c8",
        "target_frame_id": "6ba7b813-9dad-11d1-80b4-00c04fd430c8",
        "status": "accepted",
        "inlier_ratio": 0.92,
        "inlier_count": 150,
        "confidence": 0.95,
        "temporal_separation_ns": 5000000000,
        "spatial_separation_m": 25.3,
        "created_at_ns": 1783123201000000000
      },
      {
        "closure_id": "e5f6a7b8-c9d0-4e1f-2a3b-4c5d6e7f8091",
        "trajectory_id": "a1b2c3d4-e5f6-4a7b-8c9d-0e1f2a3b4c5d",
        "source_frame_id": "6ba7b812-9dad-11d1-80b4-00c04fd430c8",
        "target_frame_id": "6ba7b813-9dad-11d1-80b4-00c04fd430c8",
        "status": "rejected",
        "inlier_ratio": 0.30,
        "inlier_count": 20,
        "confidence": 0.25,
        "temporal_separation_ns": 500000000,
        "spatial_separation_m": 2.1,
        "created_at_ns": 1783123202000000000
      }
    ]
  })");
}

json MakeLoopClosureMinimal() {
  return json::parse(R"({
    "schema_version": 1,
    "candidates": [],
    "closures": []
  })");
}

// ============================================================
// TRAJECTORY SCHEMA TESTS
// ============================================================

class TrajectorySchemaTest : public ::testing::Test {
 protected:
  void SetUp() override {
    schema_ = ResolveRefs(LoadTrajectorySchema());
    doc_ = MakeTrajectoryGolden();
  }
  json schema_;
  json doc_;
};

TEST_F(TrajectorySchemaTest, SchemaIsValidJson) {
  EXPECT_TRUE(schema_.contains("$schema"));
  EXPECT_TRUE(schema_.contains("title"));
  EXPECT_TRUE(schema_.contains("type"));
}

TEST_F(TrajectorySchemaTest, SchemaVersionIsConst1) {
  EXPECT_TRUE(schema_.contains("properties"));
  EXPECT_EQ(schema_["properties"]["schema_version"]["const"], 1);
}

TEST_F(TrajectorySchemaTest, RequiredFieldsExist) {
  EXPECT_TRUE(schema_.contains("required"));
  const auto& req = schema_["required"];
  EXPECT_TRUE(std::find(req.begin(), req.end(), "schema_version") != req.end());
  EXPECT_TRUE(std::find(req.begin(), req.end(), "trajectory_id") != req.end());
  EXPECT_TRUE(std::find(req.begin(), req.end(), "nodes") != req.end());
}

TEST_F(TrajectorySchemaTest, DefinitionsExist) {
  auto raw = LoadTrajectorySchema();
  EXPECT_TRUE(raw.contains("definitions"));
  EXPECT_TRUE(raw["definitions"].contains("PoseNode"));
}

TEST_F(TrajectorySchemaTest, GoldenDocumentPasses) {
  auto v = Validate(schema_, doc_);
  EXPECT_TRUE(v.empty()) << "violations: " << JoinString(v);
}

TEST_F(TrajectorySchemaTest, MinimalDocumentPasses) {
  auto v = Validate(schema_, MakeTrajectoryMinimal());
  EXPECT_TRUE(v.empty()) << "violations: " << JoinString(v);
}

TEST_F(TrajectorySchemaTest, NoOptionalFields) {
  json doc = json::parse(R"({
    "schema_version": 1,
    "trajectory_id": "a1b2c3d4-e5f6-4a7b-8c9d-0e1f2a3b4c5d",
    "nodes": [
      {
        "frame_id": "6ba7b812-9dad-11d1-80b4-00c04fd430c8",
        "timestamp_ns": 1783123200000000000,
        "sequence_index": 0,
        "position_xyz": [1.0, 2.0, 3.0],
        "rotation_xyzw": [0.0, 0.0, 0.0, 1.0]
      }
    ]
  })");
  auto v = Validate(schema_, doc);
  EXPECT_TRUE(v.empty()) << "violations: " << JoinString(v);
}

TEST_F(TrajectorySchemaTest, NonTrivialQuaternionPasses) {
  json doc = MakeTrajectoryGolden();
  doc["nodes"][0]["rotation_xyzw"] = {0.3827, 0.0, 0.0, 0.9239};
  auto v = Validate(schema_, doc);
  EXPECT_TRUE(v.empty()) << "violations: " << JoinString(v);
}

TEST_F(TrajectorySchemaTest, NegativeSchemaVersionFails) {
  doc_["schema_version"] = 2;
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

TEST_F(TrajectorySchemaTest, MissingTrajectoryIdFails) {
  doc_.erase("trajectory_id");
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

TEST_F(TrajectorySchemaTest, MissingNodesFails) {
  doc_.erase("nodes");
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

TEST_F(TrajectorySchemaTest, MissingNodeFrameIdFails) {
  doc_["nodes"][0].erase("frame_id");
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

TEST_F(TrajectorySchemaTest, MissingNodeTimestampFails) {
  doc_["nodes"][0].erase("timestamp_ns");
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

TEST_F(TrajectorySchemaTest, MissingNodePositionFails) {
  doc_["nodes"][0].erase("position_xyz");
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

TEST_F(TrajectorySchemaTest, MissingNodeRotationFails) {
  doc_["nodes"][0].erase("rotation_xyzw");
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

TEST_F(TrajectorySchemaTest, WrongPositionLengthFails) {
  doc_["nodes"][0]["position_xyz"] = {1.0, 2.0};
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

TEST_F(TrajectorySchemaTest, WrongRotationLengthFails) {
  doc_["nodes"][0]["rotation_xyzw"] = {0.0, 0.0, 0.0};
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

TEST_F(TrajectorySchemaTest, WrongCovarianceLengthFails) {
  doc_["nodes"][0]["covariance_position"] = {0.01, 0.0, 0.0, 0.01};
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

TEST_F(TrajectorySchemaTest, UnknownPropertyFails) {
  doc_["backend_specific"] = "not allowed";
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

TEST_F(TrajectorySchemaTest, UnknownNodePropertyFails) {
  doc_["nodes"][0]["custom_field"] = "not allowed";
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

TEST_F(TrajectorySchemaTest, WrongNodeIdTypeFails) {
  doc_["nodes"][0]["node_id"] = 0;
  doc_["nodes"][0].erase("sequence_index");
  // sequence_index is required, missing should fail
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

TEST_F(TrajectorySchemaTest, NegativeSequenceIndexFails) {
  doc_["nodes"][0]["sequence_index"] = -1;
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

// ============================================================
// POSE GRAPH SCHEMA TESTS
// ============================================================

class PoseGraphSchemaTest : public ::testing::Test {
 protected:
  void SetUp() override {
    schema_ = ResolveRefs(LoadPoseGraphSchema());
    doc_ = MakePoseGraphGolden();
  }
  json schema_;
  json doc_;
};

TEST_F(PoseGraphSchemaTest, SchemaIsValidJson) {
  EXPECT_TRUE(schema_.contains("$schema"));
  EXPECT_TRUE(schema_.contains("title"));
}

TEST_F(PoseGraphSchemaTest, SchemaVersionIsConst1) {
  EXPECT_EQ(schema_["properties"]["schema_version"]["const"], 1);
}

TEST_F(PoseGraphSchemaTest, RequiredFieldsExist) {
  const auto& req = schema_["required"];
  EXPECT_TRUE(std::find(req.begin(), req.end(), "schema_version") != req.end());
  EXPECT_TRUE(std::find(req.begin(), req.end(), "graph_id") != req.end());
  EXPECT_TRUE(std::find(req.begin(), req.end(), "trajectory_id") != req.end());
  EXPECT_TRUE(std::find(req.begin(), req.end(), "nodes") != req.end());
  EXPECT_TRUE(std::find(req.begin(), req.end(), "edges") != req.end());
}

TEST_F(PoseGraphSchemaTest, DefinitionsExist) {
  auto raw = LoadPoseGraphSchema();
  EXPECT_TRUE(raw["definitions"].contains("GraphNode"));
  EXPECT_TRUE(raw["definitions"].contains("GraphEdge"));
}

TEST_F(PoseGraphSchemaTest, GoldenDocumentPasses) {
  auto v = Validate(schema_, doc_);
  EXPECT_TRUE(v.empty()) << "violations: " << JoinString(v);
}

TEST_F(PoseGraphSchemaTest, MinimalDocumentPasses) {
  auto v = Validate(schema_, MakePoseGraphMinimal());
  EXPECT_TRUE(v.empty()) << "violations: " << JoinString(v);
}

TEST_F(PoseGraphSchemaTest, NonTrivialInformationMatrixPasses) {
  json doc = MakePoseGraphGolden();
  // Identity information matrix
  doc["edges"][0]["information_matrix_6x6"] = {
      1.0, 0, 0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 0, 0, 1.0, 0, 0, 0,
      0, 0, 0, 1.0, 0, 0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 0, 0, 1.0};
  auto v = Validate(schema_, doc);
  EXPECT_TRUE(v.empty()) << "violations: " << JoinString(v);
}

TEST_F(PoseGraphSchemaTest, NegativeSchemaVersionFails) {
  doc_["schema_version"] = 2;
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

TEST_F(PoseGraphSchemaTest, MissingGraphIdFails) {
  doc_.erase("graph_id");
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

TEST_F(PoseGraphSchemaTest, MissingTrajectoryIdFails) {
  doc_.erase("trajectory_id");
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

TEST_F(PoseGraphSchemaTest, MissingNodesFails) {
  doc_.erase("nodes");
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

TEST_F(PoseGraphSchemaTest, MissingEdgesFails) {
  doc_.erase("edges");
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

TEST_F(PoseGraphSchemaTest, InvalidEdgeTypeFails) {
  doc_["edges"][0]["type"] = "unknown_type";
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

TEST_F(PoseGraphSchemaTest, EdgeTypeOdometryPasses) {
  doc_["edges"][0]["type"] = "odometry";
  auto v = Validate(schema_, doc_);
  EXPECT_TRUE(v.empty());
}

TEST_F(PoseGraphSchemaTest, EdgeTypeLoopClosurePasses) {
  doc_["edges"][0]["type"] = "loop_closure";
  auto v = Validate(schema_, doc_);
  EXPECT_TRUE(v.empty());
}

TEST_F(PoseGraphSchemaTest, EdgeTypePriorPasses) {
  doc_["edges"][0]["type"] = "prior";
  auto v = Validate(schema_, doc_);
  EXPECT_TRUE(v.empty());
}

TEST_F(PoseGraphSchemaTest, EdgeTypeGpsPasses) {
  doc_["edges"][0]["type"] = "gps";
  auto v = Validate(schema_, doc_);
  EXPECT_TRUE(v.empty());
}

TEST_F(PoseGraphSchemaTest, EdgeTypeImuPreintegrationPasses) {
  doc_["edges"][0]["type"] = "imu_preintegration";
  auto v = Validate(schema_, doc_);
  EXPECT_TRUE(v.empty());
}

TEST_F(PoseGraphSchemaTest, EdgeTypeLidarOdometryPasses) {
  doc_["edges"][0]["type"] = "lidar_odometry";
  auto v = Validate(schema_, doc_);
  EXPECT_TRUE(v.empty());
}

TEST_F(PoseGraphSchemaTest, InformationMatrixWrongLengthFails) {
  doc_["edges"][0]["information_matrix_6x6"] = {1.0, 0, 0};
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

TEST_F(PoseGraphSchemaTest, ConfidenceOutOfRangeFails) {
  doc_["edges"][0]["confidence"] = 1.5;
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

TEST_F(PoseGraphSchemaTest, ConfidenceNegativeFails) {
  doc_["edges"][0]["confidence"] = -0.1;
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

TEST_F(PoseGraphSchemaTest, MissingEdgeTypeFails) {
  doc_["edges"][0].erase("type");
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

TEST_F(PoseGraphSchemaTest, MissingEdgeSourceNodeIdFails) {
  doc_["edges"][0].erase("source_node_id");
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

TEST_F(PoseGraphSchemaTest, MissingInformationMatrixFails) {
  doc_["edges"][0].erase("information_matrix_6x6");
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

TEST_F(PoseGraphSchemaTest, UnknownPropertyFails) {
  doc_["gtsam_specific"] = "not allowed";
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

TEST_F(PoseGraphSchemaTest, UnknownEdgePropertyFails) {
  doc_["edges"][0]["gtsam_factor_type"] = "not allowed";
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

TEST_F(PoseGraphSchemaTest, NegativeNodeIdFails) {
  doc_["nodes"][0]["node_id"] = -1;
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

TEST_F(PoseGraphSchemaTest, NegativeEdgeIdFails) {
  doc_["edges"][0]["edge_id"] = -1;
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

TEST_F(PoseGraphSchemaTest, InvalidConfigurationHashLengthFails) {
  doc_["edges"][0]["configuration_hash"] = "abc";
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

TEST_F(PoseGraphSchemaTest, ValidConfigurationHashPasses) {
  doc_["edges"][0]["configuration_hash"] =
      "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
  auto v = Validate(schema_, doc_);
  EXPECT_TRUE(v.empty());
}

TEST_F(PoseGraphSchemaTest, ConfigurationHashUppercaseFails) {
  doc_["edges"][0]["configuration_hash"] =
      "ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789";
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

// ============================================================
// LOOP CLOSURE SCHEMA TESTS
// ============================================================

class LoopClosureSchemaTest : public ::testing::Test {
 protected:
  void SetUp() override {
    schema_ = ResolveRefs(LoadLoopClosureSchema());
    doc_ = MakeLoopClosureGolden();
  }
  json schema_;
  json doc_;
};

TEST_F(LoopClosureSchemaTest, SchemaIsValidJson) {
  EXPECT_TRUE(schema_.contains("$schema"));
  EXPECT_TRUE(schema_.contains("title"));
}

TEST_F(LoopClosureSchemaTest, SchemaVersionIsConst1) {
  EXPECT_EQ(schema_["properties"]["schema_version"]["const"], 1);
}

TEST_F(LoopClosureSchemaTest, RequiredFieldsExist) {
  const auto& req = schema_["required"];
  EXPECT_TRUE(std::find(req.begin(), req.end(), "schema_version") != req.end());
  EXPECT_TRUE(std::find(req.begin(), req.end(), "candidates") != req.end());
  EXPECT_TRUE(std::find(req.begin(), req.end(), "closures") != req.end());
}

TEST_F(LoopClosureSchemaTest, DefinitionsExist) {
  auto raw = LoadLoopClosureSchema();
  EXPECT_TRUE(raw["definitions"].contains("LoopClosureCandidate"));
  EXPECT_TRUE(raw["definitions"].contains("LoopClosure"));
}

TEST_F(LoopClosureSchemaTest, GoldenDocumentPasses) {
  auto v = Validate(schema_, doc_);
  EXPECT_TRUE(v.empty()) << "violations: " << JoinString(v);
}

TEST_F(LoopClosureSchemaTest, MinimalDocumentPasses) {
  auto v = Validate(schema_, MakeLoopClosureMinimal());
  EXPECT_TRUE(v.empty()) << "violations: " << JoinString(v);
}

TEST_F(LoopClosureSchemaTest, AcceptedClosurePasses) {
  json doc = MakeLoopClosureMinimal();
  doc["closures"].push_back(json::parse(R"({
    "closure_id": "d4e5f6a7-b8c9-4d0e-1f2a-3b4c5d6e7f80",
    "trajectory_id": "a1b2c3d4-e5f6-4a7b-8c9d-0e1f2a3b4c5d",
    "source_frame_id": "6ba7b812-9dad-11d1-80b4-00c04fd430c8",
    "target_frame_id": "6ba7b813-9dad-11d1-80b4-00c04fd430c8",
    "status": "accepted",
    "inlier_ratio": 0.92,
    "inlier_count": 150,
    "confidence": 0.95,
    "temporal_separation_ns": 5000000000,
    "spatial_separation_m": 25.3,
    "created_at_ns": 1783123201000000000
  })"));
  auto v = Validate(schema_, doc);
  EXPECT_TRUE(v.empty()) << "violations: " << JoinString(v);
}

TEST_F(LoopClosureSchemaTest, RejectedClosurePasses) {
  json doc = MakeLoopClosureMinimal();
  doc["closures"].push_back(json::parse(R"({
    "closure_id": "d4e5f6a7-b8c9-4d0e-1f2a-3b4c5d6e7f80",
    "trajectory_id": "a1b2c3d4-e5f6-4a7b-8c9d-0e1f2a3b4c5d",
    "source_frame_id": "6ba7b812-9dad-11d1-80b4-00c04fd430c8",
    "target_frame_id": "6ba7b813-9dad-11d1-80b4-00c04fd430c8",
    "status": "rejected",
    "inlier_ratio": 0.10,
    "inlier_count": 5,
    "confidence": 0.15,
    "temporal_separation_ns": 100000000,
    "spatial_separation_m": 500.0,
    "created_at_ns": 1783123202000000000
  })"));
  auto v = Validate(schema_, doc);
  EXPECT_TRUE(v.empty()) << "violations: " << JoinString(v);
}

TEST_F(LoopClosureSchemaTest, CandidateWithOptionalCandidateIdPasses) {
  json doc = MakeLoopClosureMinimal();
  doc["candidates"].push_back(json::parse(R"({
    "candidate_id": "c3d4e5f6-a7b8-4c9d-0e1f-2a3b4c5d6e7f",
    "trajectory_id": "a1b2c3d4-e5f6-4a7b-8c9d-0e1f2a3b4c5d",
    "source_frame_id": "6ba7b812-9dad-11d1-80b4-00c04fd430c8",
    "target_frame_id": "6ba7b813-9dad-11d1-80b4-00c04fd430c8",
    "feature_match_score": 0.85,
    "matcher": "netvlad",
    "created_at_ns": 1783123200000000000
  })"));
  auto v = Validate(schema_, doc);
  EXPECT_TRUE(v.empty()) << "violations: " << JoinString(v);
}

TEST_F(LoopClosureSchemaTest, NegativeSchemaVersionFails) {
  doc_["schema_version"] = 2;
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

TEST_F(LoopClosureSchemaTest, InvalidStatusFails) {
  doc_["closures"][0]["status"] = "pending";
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

TEST_F(LoopClosureSchemaTest, InlierRatioOutOfRangeFails) {
  doc_["closures"][0]["inlier_ratio"] = 1.5;
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

TEST_F(LoopClosureSchemaTest, InlierRatioNegativeFails) {
  doc_["closures"][0]["inlier_ratio"] = -0.1;
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

TEST_F(LoopClosureSchemaTest, ConfidenceOutOfRangeFails) {
  doc_["closures"][0]["confidence"] = 2.0;
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

TEST_F(LoopClosureSchemaTest, NegativeInlierCountFails) {
  doc_["closures"][0]["inlier_count"] = -1;
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

TEST_F(LoopClosureSchemaTest, NegativeTemporalSeparationFails) {
  doc_["closures"][0]["temporal_separation_ns"] = -1;
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

TEST_F(LoopClosureSchemaTest, NegativeSpatialSeparationFails) {
  doc_["closures"][0]["spatial_separation_m"] = -1.0;
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

TEST_F(LoopClosureSchemaTest, NegativeFeatureMatchScoreFails) {
  doc_["candidates"][0]["feature_match_score"] = -1.0;
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

TEST_F(LoopClosureSchemaTest, MissingCandidateIdFails) {
  doc_["candidates"][0].erase("candidate_id");
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

TEST_F(LoopClosureSchemaTest, MissingClosureStatusFails) {
  doc_["closures"][0].erase("status");
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

TEST_F(LoopClosureSchemaTest, EmptyMatcherFails) {
  doc_["candidates"][0]["matcher"] = "";
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

TEST_F(LoopClosureSchemaTest, UnknownCandidatePropertyFails) {
  doc_["candidates"][0]["bow_database_ref"] = "not allowed";
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

TEST_F(LoopClosureSchemaTest, UnknownClosurePropertyFails) {
  doc_["closures"][0]["gtsam_edge_id"] = "not allowed";
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

TEST_F(LoopClosureSchemaTest, UnknownRootPropertyFails) {
  doc_["backend_specific"] = "not allowed";
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

TEST_F(LoopClosureSchemaTest, MissingCandidateTrajectoryIdFails) {
  doc_["candidates"][0].erase("trajectory_id");
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

TEST_F(LoopClosureSchemaTest, MissingClosureTrajectoryIdFails) {
  doc_["closures"][0].erase("trajectory_id");
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

TEST_F(LoopClosureSchemaTest, MissingClosureSourceFrameFails) {
  doc_["closures"][0].erase("source_frame_id");
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

TEST_F(LoopClosureSchemaTest, MissingClosureTargetFrameFails) {
  doc_["closures"][0].erase("target_frame_id");
  auto v = Validate(schema_, doc_);
  EXPECT_FALSE(v.empty());
}

// ============================================================
// CROSS-SCHEMA CONSISTENCY TESTS
// ============================================================

TEST(P3SchemaCrossTest, QuaternionConventionConsistent) {
  // Both trajectory and pose-graph schemas enforce scalar-last quaternion
  auto traj_schema = LoadTrajectorySchema();
  auto pg_schema = LoadPoseGraphSchema();

  // Trajectory PoseNode has rotation_xyzw
  EXPECT_TRUE(traj_schema["definitions"]["PoseNode"]["properties"].contains(
      "rotation_xyzw"));
  // Pose Graph GraphEdge has relative_rotation_xyzw
  EXPECT_TRUE(pg_schema["definitions"]["GraphEdge"]["properties"].contains(
      "relative_rotation_xyzw"));
}

TEST(P3SchemaCrossTest, SchemaVersionsAllConst1) {
  auto traj = LoadTrajectorySchema();
  auto pg = LoadPoseGraphSchema();
  auto lc = LoadLoopClosureSchema();

  EXPECT_EQ(traj["properties"]["schema_version"]["const"], 1);
  EXPECT_EQ(pg["properties"]["schema_version"]["const"], 1);
  EXPECT_EQ(lc["properties"]["schema_version"]["const"], 1);
}

TEST(P3SchemaCrossTest, AllSchemasUseStrictProperties) {
  auto traj = LoadTrajectorySchema();
  auto pg = LoadPoseGraphSchema();
  auto lc = LoadLoopClosureSchema();

  EXPECT_EQ(traj["additionalProperties"], false);
  EXPECT_EQ(pg["additionalProperties"], false);
  EXPECT_EQ(lc["additionalProperties"], false);
}

TEST(P3SchemaCrossTest, AllSchemasHaveDraft07) {
  auto traj = LoadTrajectorySchema();
  auto pg = LoadPoseGraphSchema();
  auto lc = LoadLoopClosureSchema();

  EXPECT_EQ(traj["$schema"], "http://json-schema.org/draft-07/schema#");
  EXPECT_EQ(pg["$schema"], "http://json-schema.org/draft-07/schema#");
  EXPECT_EQ(lc["$schema"], "http://json-schema.org/draft-07/schema#");
}

TEST(P3SchemaCrossTest, FrameIdReferencesConsistent) {
  auto traj = LoadTrajectorySchema();
  auto pg = LoadPoseGraphSchema();
  auto lc = LoadLoopClosureSchema();

  // All use "string" + "uuid" for frame_id references
  EXPECT_EQ(traj["definitions"]["PoseNode"]["properties"]["frame_id"]["type"],
            "string");
  EXPECT_EQ(pg["definitions"]["GraphNode"]["properties"]["frame_id"]["type"],
            "string");
  // Loop closure uses source_frame_id / target_frame_id
  EXPECT_EQ(
      lc["definitions"]["LoopClosureCandidate"]["properties"]
          ["source_frame_id"]["type"],
      "string");
}

TEST(P3SchemaCrossTest, ConfidenceRangeConsistent) {
  auto pg = LoadPoseGraphSchema();
  auto lc = LoadLoopClosureSchema();

  // Both use [0, 1] for confidence
  EXPECT_EQ(pg["definitions"]["GraphEdge"]["properties"]["confidence"]
              ["minimum"],
            0);
  EXPECT_EQ(pg["definitions"]["GraphEdge"]["properties"]["confidence"]
              ["maximum"],
            1);
  EXPECT_EQ(lc["definitions"]["LoopClosure"]["properties"]["confidence"]
              ["minimum"],
            0);
  EXPECT_EQ(lc["definitions"]["LoopClosure"]["properties"]["confidence"]
              ["maximum"],
            1);
}

}  // namespace
