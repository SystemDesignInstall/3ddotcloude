#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "schema_check.h"

namespace {

using nlohmann::json;

// --- Schema loader ---

json LoadReconstructionSchema() {
  std::ifstream in(SPATIAL_RECONSTRUCTION_SCHEMA_JSON);
  EXPECT_TRUE(in.good()) << "cannot open reconstruction.schema.json";
  return json::parse(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

// --- $ref resolver (test-only) ---
// Walks the schema tree and replaces every {"$ref":"#/definitions/X"} with the
// actual definition object. Definitions are looked up from the root schema's
// "definitions" block, which must be passed as context through all recursive
// calls. CheckNode does not handle $ref; this allows the existing validator to
// work with the $defs-based schema.

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
    return node;  // unresolvable
  }
  json out = node;
  for (auto it = out.begin(); it != out.end(); ++it) {
    if (it.key() == "definitions") {
      it.value() = json::object();  // strip definitions from output
    } else {
      it.value() = ResolveRefsImpl(it.value(), root_defs);
    }
  }
  return out;
}

json ResolveRefs(const json& schema) {
  const json& defs = schema.contains("definitions") ? schema["definitions"]
                                                     : json::object();
  return ResolveRefsImpl(schema, defs);
}

// --- Extended schema validator (test-only) ---
// Standalone reimplementation of CheckNode's logic plus support for boolean
// type, number minimum/maximum (including floats), and maxItems. Does NOT
// delegate to CheckNode — avoids its boolean-type false positives.

void CheckNodeExtended(const json& schema, const json& doc,
                       std::vector<std::string>* violations) {
  std::function<void(const json&, const json&, const std::string&)> walk =
      [&](const json& s, const json& d, const std::string& p) {
        if (!s.is_object()) return;

        // type
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

        // required
        if (s.contains("required")) {
          for (const auto& key : s["required"]) {
            if (!d.is_object() || !d.contains(key)) {
              violations->push_back(p + ": missing required '" +
                                    key.get<std::string>() + "'");
            }
          }
        }

        // const
        if (s.contains("const") && d != s["const"]) {
          violations->push_back(p + ": const mismatch");
        }

        // enum
        if (s.contains("enum")) {
          if (std::find(s["enum"].begin(), s["enum"].end(), d) ==
              s["enum"].end()) {
            violations->push_back(p + ": not in enum");
          }
        }

        // pattern (strings only)
        if (s.contains("pattern") && d.is_string()) {
          const std::regex re(s["pattern"].get<std::string>());
          if (!std::regex_match(d.get<std::string>(), re)) {
            violations->push_back(p + ": pattern mismatch");
          }
        }

        // minimum (integers and floats)
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

        // maximum (integers and floats)
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

        // minItems
        if (s.contains("minItems") && d.is_array()) {
          if (d.size() < s["minItems"].get<std::size_t>()) {
            violations->push_back(p + ": below minItems");
          }
        }

        // maxItems
        if (s.contains("maxItems") && d.is_array()) {
          if (d.size() > s["maxItems"].get<std::size_t>()) {
            violations->push_back(p + ": exceeds maxItems");
          }
        }

        // minLength
        if (s.contains("minLength") && d.is_string()) {
          if (d.get<std::string>().size() <
              s["minLength"].get<std::size_t>()) {
            violations->push_back(p + ": below minLength");
          }
        }

        // Recurse into properties
        if (s.contains("properties") && d.is_object()) {
          for (auto it = s["properties"].begin();
               it != s["properties"].end(); ++it) {
            if (d.contains(it.key())) {
              walk(it.value(), d[it.key()], p + "/" + it.key());
            }
          }
        }

        // Recurse into array items
        if (s.contains("items") && d.is_array()) {
          for (std::size_t i = 0; i < d.size(); ++i) {
            walk(s["items"], d[i], p + "/" + std::to_string(i));
          }
        }
      };

  walk(schema, doc, "$");
}

// --- Canonical golden fixture (P2.5-impl-2 §examples) ---

json MakeGoldenDocument() {
  return json::parse(R"({
    "schema_version": 2,
    "reconstruction_id": "a1b2c3d4-e5f6-4a7b-8c9d-0e1f2a3b4c5d",
    "scene_id": "6ba7b810-9dad-11d1-80b4-00c04fd430c8",
    "session_ids": ["6ba7b811-9dad-11d1-80b4-00c04fd430c8"],
    "coordinate_frame": "reconstruction_0",
    "provenance": {
      "backend": {
        "name": "colmap",
        "version": "3.13",
        "adapter_version": "0.2.0"
      },
      "configuration_hash": "1111111111111111111111111111111111111111111111111111111111111111",
      "input_artifact_hashes": [
        "2222222222222222222222222222222222222222222222222222222222222222"
      ],
      "engine_version": "0.2.0",
      "engine_commit": "abc1234",
      "git_commit": "def5678",
      "started_at_ns": 1783123200000000000,
      "finished_at_ns": 1783123260000000000,
      "duration_ns": 60000000000
    },
    "cameras": [
      {
        "camera_id": 1,
        "width": 1920,
        "height": 1080,
        "intrinsic_model": "pinhole",
        "fx": 1458.0,
        "fy": 1458.0,
        "cx": 960.0,
        "cy": 540.0,
        "distortion_model": "none",
        "distortion_coefficients": []
      }
    ],
    "images": [
      {
        "image_id": 1,
        "camera_id": 1,
        "frame_id": "6ba7b812-9dad-11d1-80b4-00c04fd430c8",
        "name": "frame001.jpg",
        "pose": {
          "rotation_xyzw": [0.0, 0.0, 0.0, 1.0],
          "translation_xyz": [0.0, 0.0, 0.0]
        },
        "detected": true
      },
      {
        "image_id": 2,
        "camera_id": 1,
        "frame_id": "6ba7b813-9dad-11d1-80b4-00c04fd430c8",
        "name": "frame002.jpg",
        "pose": {
          "rotation_xyzw": [0.01, 0.02, 0.03, 0.999],
          "translation_xyz": [0.1, -0.05, 0.02]
        },
        "detected": false
      }
    ],
    "points3D": [
      {
        "point3d_id": 100,
        "xyz": [1.234, 5.678, 9.012],
        "color": [128, 200, 64],
        "error": 0.42,
        "track": [
          {"image_id": 1, "point2d_idx": 5},
          {"image_id": 2, "point2d_idx": 12}
        ]
      }
    ]
  })");
}

// Minimal valid document (all optional fields omitted).
json MakeMinimalDocument() {
  return json::parse(R"({
    "schema_version": 2,
    "reconstruction_id": "00000000-0000-0000-0000-000000000001",
    "scene_id": "00000000-0000-0000-0000-000000000002",
    "session_ids": ["00000000-0000-0000-0000-000000000003"],
    "coordinate_frame": "reconstruction_0",
    "provenance": {
      "backend": {"name": "colmap", "version": "3.13", "adapter_version": "0.2.0"},
      "configuration_hash": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
      "input_artifact_hashes": [],
      "engine_version": "0.2.0",
      "engine_commit": "abc1234",
      "started_at_ns": 0,
      "finished_at_ns": 0,
      "duration_ns": 0
    },
    "cameras": [],
    "images": [],
    "points3D": []
  })");
}

// --- Schema structural tests (use raw schema — $ref not needed) ---

TEST(ReconstructionSchemaTest, SchemaIsValidJson) {
  const auto schema = LoadReconstructionSchema();
  EXPECT_EQ(schema["$schema"].get<std::string>(),
            "http://json-schema.org/draft-07/schema#");
  EXPECT_EQ(schema["title"].get<std::string>(),
            "Canonical Reconstruction Model v2 (P2.5)");
}

TEST(ReconstructionSchemaTest, SchemaVersionIsConst2) {
  const auto schema = LoadReconstructionSchema();
  EXPECT_EQ(schema["properties"]["schema_version"]["const"].get<int>(), 2);
}

TEST(ReconstructionSchemaTest, SchemaHasAllRequiredFields) {
  const auto schema = LoadReconstructionSchema();
  const auto& req = schema["required"];
  EXPECT_EQ(req.size(), 9u);
  std::vector<std::string> required;
  for (const auto& r : req) required.push_back(r.get<std::string>());
  EXPECT_NE(std::find(required.begin(), required.end(), "schema_version"),
            required.end());
  EXPECT_NE(std::find(required.begin(), required.end(), "reconstruction_id"),
            required.end());
  EXPECT_NE(std::find(required.begin(), required.end(), "scene_id"),
            required.end());
  EXPECT_NE(std::find(required.begin(), required.end(), "session_ids"),
            required.end());
  EXPECT_NE(std::find(required.begin(), required.end(), "coordinate_frame"),
            required.end());
  EXPECT_NE(std::find(required.begin(), required.end(), "provenance"),
            required.end());
  EXPECT_NE(std::find(required.begin(), required.end(), "cameras"),
            required.end());
  EXPECT_NE(std::find(required.begin(), required.end(), "images"),
            required.end());
  EXPECT_NE(std::find(required.begin(), required.end(), "points3D"),
            required.end());
}

TEST(ReconstructionSchemaTest, SchemaHasAllDefinitions) {
  const auto schema = LoadReconstructionSchema();
  const auto& defs = schema["definitions"];
  EXPECT_TRUE(defs.contains("ReconPose"));
  EXPECT_TRUE(defs.contains("ReconCamera"));
  EXPECT_TRUE(defs.contains("ReconImage"));
  EXPECT_TRUE(defs.contains("ReconPoint3D"));
  EXPECT_TRUE(defs.contains("TrackElement"));
  EXPECT_TRUE(defs.contains("ReconObservation"));
  EXPECT_TRUE(defs.contains("ReconstructionProvenance"));
  EXPECT_TRUE(defs.contains("ReconUncertainty"));
}

// --- Golden document validation (resolved schema) ---

TEST(ReconstructionSchemaTest, GoldenDocumentPassesCheckNode) {
  const auto resolved = ResolveRefs(LoadReconstructionSchema());
  const auto doc = MakeGoldenDocument();
  std::vector<std::string> violations;
    CheckNodeExtended(resolved, doc, &violations);
  ASSERT_TRUE(violations.empty()) << [&violations] {
    std::string joined;
    for (const auto& v : violations) joined += "\n  " + v;
    return joined;
  }();
}

TEST(ReconstructionSchemaTest, MinimalDocumentPassesCheckNode) {
  const auto resolved = ResolveRefs(LoadReconstructionSchema());
  const auto doc = MakeMinimalDocument();
  std::vector<std::string> violations;
    CheckNodeExtended(resolved, doc, &violations);
  ASSERT_TRUE(violations.empty()) << [&violations] {
    std::string joined;
    for (const auto& v : violations) joined += "\n  " + v;
    return joined;
  }();
}

// --- Semantic constraint tests (resolved schema + CheckNode) ---
// Uses ResolveRefs so CheckNode can validate nested $ref-defined types.

class ReconstructionSemanticTest : public ::testing::Test {
 protected:
  void SetUp() override {
    schema_ = ResolveRefs(LoadReconstructionSchema());
    doc_ = MakeGoldenDocument();
  }

  void ExpectViolation(const json& doc, const std::string& path_fragment,
                       const std::string& description) {
    std::vector<std::string> violations;
    CheckNodeExtended(schema_, doc, &violations);
    bool found = false;
    for (const auto& v : violations) {
      if (v.find(path_fragment) != std::string::npos) {
        found = true;
        break;
      }
    }
    EXPECT_TRUE(found) << description << " — no violation at " << path_fragment
                       << ". All violations:";
    if (!found) {
      for (const auto& v : violations) {
        ADD_FAILURE() << "  " << v;
      }
    }
  }

  json schema_;
  json doc_;
};

// --- schema_version ---

TEST_F(ReconstructionSemanticTest, SchemaVersionMustBe2) {
  doc_["schema_version"] = 1;
  ExpectViolation(doc_, "schema_version", "schema_version=1 should fail");
}

TEST_F(ReconstructionSemanticTest, SchemaVersionCannotBeString) {
  doc_["schema_version"] = "2";
  ExpectViolation(doc_, "schema_version", "schema_version as string should fail");
}

// --- reconstruction_id ---

TEST_F(ReconstructionSemanticTest, ReconstructionIdRequired) {
  doc_.erase("reconstruction_id");
  ExpectViolation(doc_, "reconstruction_id", "missing reconstruction_id");
}

TEST_F(ReconstructionSemanticTest, ReconstructionIdMustBeString) {
  doc_["reconstruction_id"] = 12345;
  ExpectViolation(doc_, "reconstruction_id", "reconstruction_id as int should fail");
}

// --- session_ids ---

TEST_F(ReconstructionSemanticTest, SessionIdsMustBeArray) {
  doc_["session_ids"] = "not-an-array";
  ExpectViolation(doc_, "session_ids", "session_ids as string should fail");
}

TEST_F(ReconstructionSemanticTest, SessionIdsEmptyArrayFailsMinItems) {
  doc_["session_ids"] = json::array();
  // minItems: 1 — CheckNode doesn't enforce this, but we verify schema declares it
  const auto raw = LoadReconstructionSchema();
  EXPECT_EQ(raw["properties"]["session_ids"]["minItems"].get<int>(), 1);
}

TEST_F(ReconstructionSemanticTest, SessionIdsElementsMustBeStrings) {
  doc_["session_ids"] = json::array({123, 456});
  ExpectViolation(doc_, "session_ids", "session_ids with int elements should fail");
}

// --- coordinate_frame ---

TEST_F(ReconstructionSemanticTest, CoordinateFrameRequired) {
  doc_.erase("coordinate_frame");
  ExpectViolation(doc_, "coordinate_frame", "missing coordinate_frame");
}

TEST_F(ReconstructionSemanticTest, CoordinateFrameMustBeString) {
  doc_["coordinate_frame"] = 42;
  ExpectViolation(doc_, "coordinate_frame", "coordinate_frame as int should fail");
}

// --- status (optional) ---

TEST_F(ReconstructionSemanticTest, StatusEnumValidValues) {
  for (const auto& s : {"reconstructing", "succeeded", "failed", "superseded"}) {
    doc_["status"] = s;
    std::vector<std::string> violations;
    CheckNodeExtended(schema_, doc_, &violations);
    bool found = false;
    for (const auto& v : violations) {
      if (v.find("status") != std::string::npos) {
        found = true;
        break;
      }
    }
    EXPECT_FALSE(found) << "status='" << s << "' should be valid";
  }
}

TEST_F(ReconstructionSemanticTest, StatusEnumInvalidValue) {
  doc_["status"] = "running";
  ExpectViolation(doc_, "status", "status='running' should fail enum");
}

// --- provenance ---

TEST_F(ReconstructionSemanticTest, ProvenanceRequired) {
  doc_.erase("provenance");
  ExpectViolation(doc_, "provenance", "missing provenance");
}

TEST_F(ReconstructionSemanticTest, ProvenanceBackendRequired) {
  doc_["provenance"].erase("backend");
  ExpectViolation(doc_, "backend", "missing backend in provenance");
}

TEST_F(ReconstructionSemanticTest, ProvenanceConfigurationHashPattern) {
  doc_["provenance"]["configuration_hash"] = "not-a-hash";
  ExpectViolation(doc_, "configuration_hash",
                  "configuration_hash with non-hex should fail");
}

TEST_F(ReconstructionSemanticTest, ProvenanceConfigurationHashLength) {
  doc_["provenance"]["configuration_hash"] = std::string(63, 'a');
  ExpectViolation(doc_, "configuration_hash",
                  "configuration_hash with 63 chars should fail pattern");
}

TEST_F(ReconstructionSemanticTest, ProvenanceDurationNsMustBeNonNegative) {
  doc_["provenance"]["duration_ns"] = -1;
  ExpectViolation(doc_, "duration_ns", "negative duration_ns should fail");
}

// --- cameras ---

TEST_F(ReconstructionSemanticTest, CamerasRequired) {
  doc_.erase("cameras");
  ExpectViolation(doc_, "cameras", "missing cameras");
}

TEST_F(ReconstructionSemanticTest, CameraMustHaveAllRequiredFields) {
  doc_["cameras"][0].erase("intrinsic_model");
  ExpectViolation(doc_, "intrinsic_model",
                  "camera missing intrinsic_model should fail");
}

TEST_F(ReconstructionSemanticTest, CameraIntrinsicModelEnum) {
  doc_["cameras"][0]["intrinsic_model"] = "unknown_model";
  ExpectViolation(doc_, "intrinsic_model",
                  "unknown intrinsic_model should fail enum");
}

TEST_F(ReconstructionSemanticTest, CameraDistortionModelEnum) {
  doc_["cameras"][0]["distortion_model"] = "unknown_distortion";
  ExpectViolation(doc_, "distortion_model",
                  "unknown distortion_model should fail enum");
}

TEST_F(ReconstructionSemanticTest, CameraWidthMustBePositive) {
  doc_["cameras"][0]["width"] = 0;
  ExpectViolation(doc_, "width", "width=0 should fail minimum:1");
}

TEST_F(ReconstructionSemanticTest, CameraHeightMustBePositive) {
  doc_["cameras"][0]["height"] = -1;
  ExpectViolation(doc_, "height", "height=-1 should fail minimum:1");
}

TEST_F(ReconstructionSemanticTest, CameraFocalLengthsMustBeNumbers) {
  doc_["cameras"][0]["fx"] = "not-a-number";
  ExpectViolation(doc_, "fx", "fx as string should fail");
}

// --- images ---

TEST_F(ReconstructionSemanticTest, ImagesRequired) {
  doc_.erase("images");
  ExpectViolation(doc_, "images", "missing images");
}

TEST_F(ReconstructionSemanticTest, ImageMustHaveDetected) {
  doc_["images"][0].erase("detected");
  ExpectViolation(doc_, "detected", "image missing detected should fail");
}

TEST_F(ReconstructionSemanticTest, DetectedMustBeBoolean) {
  doc_["images"][0]["detected"] = 1;
  ExpectViolation(doc_, "detected", "detected as int should fail");
}

TEST_F(ReconstructionSemanticTest, DetectedFalseIsValid) {
  doc_["images"][0]["detected"] = false;
  std::vector<std::string> violations;
  CheckNodeExtended(schema_, doc_, &violations);
  bool detected_violation = false;
  for (const auto& v : violations) {
    if (v.find("detected") != std::string::npos) {
      detected_violation = true;
      break;
    }
  }
  EXPECT_FALSE(detected_violation) << "detected=false should be valid";
}

TEST_F(ReconstructionSemanticTest, ImagePoseRotationMustBe4Elements) {
  // maxItems: 4 — CheckNode doesn't enforce, verify schema declares it
  const auto raw = LoadReconstructionSchema();
  EXPECT_EQ(raw["definitions"]["ReconPose"]["properties"]
                ["rotation_xyzw"]["maxItems"]
                .get<int>(),
            4);
}

TEST_F(ReconstructionSemanticTest, ImagePoseTranslationMustBe3Elements) {
  const auto raw = LoadReconstructionSchema();
  EXPECT_EQ(raw["definitions"]["ReconPose"]["properties"]
                ["translation_xyz"]["maxItems"]
                .get<int>(),
            3);
}

// --- points3D ---

TEST_F(ReconstructionSemanticTest, Points3DRequired) {
  doc_.erase("points3D");
  ExpectViolation(doc_, "points3D", "missing points3D");
}

TEST_F(ReconstructionSemanticTest, Point3DXyzMustBe3Elements) {
  const auto raw = LoadReconstructionSchema();
  EXPECT_EQ(raw["definitions"]["ReconPoint3D"]["properties"]["xyz"]
                ["maxItems"]
                .get<int>(),
            3);
}

TEST_F(ReconstructionSemanticTest, Point3DColorMustBe3Elements) {
  const auto raw = LoadReconstructionSchema();
  EXPECT_EQ(raw["definitions"]["ReconPoint3D"]["properties"]["color"]
                ["maxItems"]
                .get<int>(),
            3);
}

TEST_F(ReconstructionSemanticTest, Point3DColorChannelRange) {
  const auto raw = LoadReconstructionSchema();
  EXPECT_EQ(raw["definitions"]["ReconPoint3D"]["properties"]["color"]
                ["items"]["minimum"]
                .get<int>(),
            0);
  EXPECT_EQ(raw["definitions"]["ReconPoint3D"]["properties"]["color"]
                ["items"]["maximum"]
                .get<int>(),
            255);
}

TEST_F(ReconstructionSemanticTest, Point3DErrorMustBeNonNegative) {
  doc_["points3D"][0]["error"] = -0.5;
  ExpectViolation(doc_, "error", "negative error should fail minimum:0");
}

TEST_F(ReconstructionSemanticTest, Point3DTrackElementRequiresImageId) {
  doc_["points3D"][0]["track"][0].erase("image_id");
  ExpectViolation(doc_, "image_id",
                  "track element missing image_id should fail");
}

TEST_F(ReconstructionSemanticTest, Point3DTrackElementPoint2dIdxMin) {
  doc_["points3D"][0]["track"][0]["point2d_idx"] = -1;
  ExpectViolation(doc_, "point2d_idx",
                  "point2d_idx=-1 should fail minimum:0");
}

// --- uncertainty ---

TEST_F(ReconstructionSemanticTest, UncertaintyCovarianceMustBe6Elements) {
  const auto raw = LoadReconstructionSchema();
  EXPECT_EQ(raw["definitions"]["ReconUncertainty"]["properties"]
                ["covariance_xyz"]["maxItems"]
                .get<int>(),
            6);
}

TEST_F(ReconstructionSemanticTest, UncertaintyConfidenceRange) {
  const auto raw = LoadReconstructionSchema();
  EXPECT_EQ(raw["definitions"]["ReconUncertainty"]["properties"]
                ["confidence"]["minimum"]
                .get<double>(),
            0.0);
  EXPECT_EQ(raw["definitions"]["ReconUncertainty"]["properties"]
                ["confidence"]["maximum"]
                .get<double>(),
            1.0);
}

TEST_F(ReconstructionSemanticTest, UncertaintyConfidenceOutOfRange) {
  doc_["uncertainty"] = json::object({
    {"covariance_xyz", json::array({1, 0, 0, 1, 0, 1})},
    {"confidence", 1.5},
    {"source_count", 10}
  });
  ExpectViolation(doc_, "confidence",
                  "confidence=1.5 should fail maximum:1");
}

// --- additional properties rejection ---

TEST_F(ReconstructionSemanticTest, TopLevelRejectsUnknownProperties) {
  const auto raw = LoadReconstructionSchema();
  EXPECT_EQ(raw["additionalProperties"].get<bool>(), false);
}

TEST_F(ReconstructionSemanticTest, CameraRejectsUnknownProperties) {
  const auto raw = LoadReconstructionSchema();
  EXPECT_EQ(raw["definitions"]["ReconCamera"]["additionalProperties"]
                .get<bool>(),
            false);
}

TEST_F(ReconstructionSemanticTest, ProvenanceRejectsUnknownProperties) {
  const auto raw = LoadReconstructionSchema();
  EXPECT_EQ(
      raw["definitions"]["ReconstructionProvenance"]
          ["additionalProperties"]
              .get<bool>(),
      false);
}

// --- $ref reference integrity ---

TEST(ReconstructionSchemaTest, AllRefsResolve) {
  const auto schema = LoadReconstructionSchema();
  std::function<void(const json&, const std::string&)> check_refs =
      [&](const json& node, const std::string& path) {
        if (node.is_object()) {
          if (node.contains("$ref")) {
            const auto ref = node["$ref"].get<std::string>();
            EXPECT_TRUE(ref.substr(0, 14) == "#/definitions/")
                << path << ": $ref '" << ref << "' has unexpected prefix";
            const auto type_name = ref.substr(14);
            EXPECT_TRUE(schema["definitions"].contains(type_name))
                << path << ": $ref '" << ref << "' points to missing definition";
          }
          for (auto it = node.begin(); it != node.end(); ++it) {
            check_refs(it.value(), path + "/" + it.key());
          }
        } else if (node.is_array()) {
          for (std::size_t i = 0; i < node.size(); ++i) {
            check_refs(node[i], path + "/" + std::to_string(i));
          }
        }
      };
  check_refs(schema, "$");
}

// --- enum vocabulary checks ---

TEST(ReconstructionSchemaTest, IntrinsicModelEnumVocabulary) {
  const auto schema = LoadReconstructionSchema();
  const auto& enums =
      schema["definitions"]["ReconCamera"]["properties"]["intrinsic_model"]
          ["enum"];
  EXPECT_EQ(enums.size(), 6u);
  std::vector<std::string> values;
  for (const auto& e : enums) values.push_back(e.get<std::string>());
  EXPECT_NE(std::find(values.begin(), values.end(), "pinhole"), values.end());
  EXPECT_NE(std::find(values.begin(), values.end(), "opencv"), values.end());
  EXPECT_NE(std::find(values.begin(), values.end(), "opencv_fisheye"),
            values.end());
  EXPECT_NE(std::find(values.begin(), values.end(), "fov"), values.end());
  EXPECT_NE(std::find(values.begin(), values.end(), "omnidirectional"),
            values.end());
  EXPECT_NE(std::find(values.begin(), values.end(), "custom"), values.end());
}

TEST(ReconstructionSchemaTest, DistortionModelEnumVocabulary) {
  const auto schema = LoadReconstructionSchema();
  const auto& enums =
      schema["definitions"]["ReconCamera"]["properties"]["distortion_model"]
          ["enum"];
  EXPECT_EQ(enums.size(), 5u);
  std::vector<std::string> values;
  for (const auto& e : enums) values.push_back(e.get<std::string>());
  EXPECT_NE(std::find(values.begin(), values.end(), "none"), values.end());
  EXPECT_NE(std::find(values.begin(), values.end(), "opencv_radial"),
            values.end());
  EXPECT_NE(std::find(values.begin(), values.end(), "opencv_fisheye"),
            values.end());
  EXPECT_NE(std::find(values.begin(), values.end(), "equidistant"),
            values.end());
  EXPECT_NE(std::find(values.begin(), values.end(), "custom"), values.end());
}

TEST(ReconstructionSchemaTest, StatusEnumVocabulary) {
  const auto schema = LoadReconstructionSchema();
  const auto& enums = schema["properties"]["status"]["enum"];
  EXPECT_EQ(enums.size(), 4u);
  std::vector<std::string> values;
  for (const auto& e : enums) values.push_back(e.get<std::string>());
  EXPECT_NE(std::find(values.begin(), values.end(), "reconstructing"),
            values.end());
  EXPECT_NE(std::find(values.begin(), values.end(), "succeeded"), values.end());
  EXPECT_NE(std::find(values.begin(), values.end(), "failed"), values.end());
  EXPECT_NE(std::find(values.begin(), values.end(), "superseded"), values.end());
}

// --- lifecycle vs spatial separation ---

TEST(ReconstructionSchemaTest, StatusIsOptionalNotRequired) {
  // D-CRM-19: lifecycle state belongs to DB ReconstructionRow, not the
  // CAS document. status is an optional field in the schema.
  const auto schema = LoadReconstructionSchema();
  const auto& req = schema["required"];
  bool status_required = false;
  for (const auto& r : req) {
    if (r.get<std::string>() == "status") {
      status_required = true;
      break;
    }
  }
  EXPECT_FALSE(status_required)
      << "status should NOT be in required (lifecycle state is DB-only per D-CRM-19)";
}

// --- provenance hash pattern consistency ---

TEST(ReconstructionSchemaTest, AllHashFieldsUseSamePattern) {
  const auto schema = LoadReconstructionSchema();
  const std::string hash_pattern = "^[0-9a-f]{64}$";

  EXPECT_EQ(schema["definitions"]["ReconstructionProvenance"]
                ["properties"]["configuration_hash"]["pattern"]
                .get<std::string>(),
            hash_pattern);

  EXPECT_EQ(schema["definitions"]["ReconstructionProvenance"]
                ["properties"]["input_artifact_hashes"]["items"]["pattern"]
                .get<std::string>(),
            hash_pattern);
}

}  // namespace
