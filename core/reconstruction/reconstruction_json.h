#pragma once

// Canonical Reconstruction JSON serialization (P3-impl-8c; schemas/json/
// reconstruction.schema.json, schema_version 2). The single source of truth
// for the Reconstruction -> document_json round-trip used by the engine
// orchestration stage (v3 row -> v3 Reconstruction -> BA seam -> v4 row).
//
// The serializer is byte-compatible with the former adapter-side duplicate
// (colmap_converter.cpp ReconstructionToJson, removed during the production
// hardening milestone): identical field order and
// canonical nlohmann number formatting (equal values -> equal JSON, ADR-020).
// The one additive field is provenance.backend_specific_json, which the
// schema declares optional (reconstruction.schema.json:336-339) and which is
// now emitted when the provenance carries it (8c bundle-adjustment stats) —
// an empty backend_specific_json is omitted exactly as before, so existing
// payloads are byte-identical.
//
// ReconstructionFromJson is the strict inverse: it parses every field the
// serializer emits, leaves unknown keys alone (forward compatibility), and
// FAILS CLOSED (spatial::core::ValidationError) on malformed JSON or a
// wrong-typed value — a corrupt document never yields a silently partial
// Reconstruction.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/errors/project_error.h"
#include "core/reconstruction/reconstruction.h"

namespace spatial::core {

// Serializes a canonical Reconstruction document (schema_version 2). Deterministic
// field order and number formatting (ADR-020); equal values produce identical bytes.
inline std::string ReconstructionToJson(const Reconstruction& rec) {
  nlohmann::json doc;

  doc["schema_version"] = 2;
  doc["reconstruction_id"] = rec.reconstruction_id;
  doc["scene_id"] = rec.scene_id;

  nlohmann::json sids = nlohmann::json::array();
  for (const auto& sid : rec.session_ids) sids.push_back(sid);
  doc["session_ids"] = std::move(sids);

  doc["coordinate_frame"] = rec.coordinate_frame;
  if (!rec.status.empty()) doc["status"] = rec.status;
  doc["created_at_ns"] = rec.created_at_ns;

  // Provenance.
  nlohmann::json prov;
  nlohmann::json backend;
  backend["name"] = rec.provenance.backend.name;
  backend["version"] = rec.provenance.backend.version;
  backend["adapter_version"] = rec.provenance.backend.adapter_version;
  prov["backend"] = std::move(backend);
  prov["configuration_hash"] = rec.provenance.configuration_hash;
  nlohmann::json iah = nlohmann::json::array();
  for (const auto& h : rec.provenance.input_artifact_hashes) iah.push_back(h);
  prov["input_artifact_hashes"] = std::move(iah);
  prov["engine_version"] = rec.provenance.engine_version;
  prov["engine_commit"] = rec.provenance.engine_commit;
  prov["git_commit"] = rec.provenance.git_commit;
  prov["started_at_ns"] = rec.provenance.started_at_ns;
  prov["finished_at_ns"] = rec.provenance.finished_at_ns;
  prov["duration_ns"] = rec.provenance.duration_ns;
  if (!rec.provenance.backend_specific_json.empty()) {
    prov["backend_specific_json"] = rec.provenance.backend_specific_json;
  }
  doc["provenance"] = std::move(prov);

  // Cameras.
  nlohmann::json cameras = nlohmann::json::array();
  for (const ReconCamera& c : rec.cameras) {
    nlohmann::json entry;
    entry["camera_id"] = c.camera_id;
    entry["width"] = c.width;
    entry["height"] = c.height;
    entry["intrinsic_model"] = c.intrinsic_model;
    entry["fx"] = c.fx;
    entry["fy"] = c.fy;
    entry["cx"] = c.cx;
    entry["cy"] = c.cy;
    entry["distortion_model"] = c.distortion_model;
    entry["distortion_coefficients"] = c.distortion_coefficients;
    if (!c.calibration_ref.empty()) entry["calibration_ref"] = c.calibration_ref;
    cameras.push_back(std::move(entry));
  }
  doc["cameras"] = std::move(cameras);

  // Images.
  nlohmann::json images = nlohmann::json::array();
  for (const ReconImage& img : rec.images) {
    nlohmann::json entry;
    entry["image_id"] = img.image_id;
    entry["camera_id"] = img.camera_id;
    if (!img.frame_id.empty()) entry["frame_id"] = img.frame_id;
    entry["name"] = img.name;
    nlohmann::json pose;
    pose["rotation_xyzw"] = {img.pose.rotation_xyzw[0],
                             img.pose.rotation_xyzw[1],
                             img.pose.rotation_xyzw[2],
                             img.pose.rotation_xyzw[3]};
    pose["translation_xyz"] = {img.pose.translation_xyz[0],
                               img.pose.translation_xyz[1],
                               img.pose.translation_xyz[2]};
    entry["pose"] = std::move(pose);
    entry["detected"] = img.detected;
    images.push_back(std::move(entry));
  }
  doc["images"] = std::move(images);

  // Points3D.
  nlohmann::json points = nlohmann::json::array();
  for (const ReconPoint3D& pt : rec.points3D) {
    nlohmann::json entry;
    entry["point3d_id"] = pt.point3d_id;
    entry["xyz"] = {pt.xyz[0], pt.xyz[1], pt.xyz[2]};
    entry["color"] = {pt.color[0], pt.color[1], pt.color[2]};
    entry["error"] = pt.error;
    nlohmann::json track = nlohmann::json::array();
    for (const ReconPoint3D::TrackElement& te : pt.track) {
      track.push_back({{"image_id", te.image_id},
                       {"point2d_idx", te.point2d_idx}});
    }
    entry["track"] = std::move(track);
    points.push_back(std::move(entry));
  }
  doc["points3D"] = std::move(points);

  return doc.dump();
}

namespace {

inline ValidationError ReconstructionJsonViolation(const std::string& detail) {
  return ValidationError(ErrorCode::kValidationDomain,
                         std::string("reconstruction document: ") + detail, {},
                         /*recoverable=*/false,
                         "The reconstruction document in the persistence layer "
                         "is malformed or uses an unsupported value type; "
                         "regenerate the artifact.");
}

template <typename T>
T JsonGetField(const nlohmann::json& obj, const char* key) {
  const auto it = obj.find(key);
  if (it == obj.end()) {
    throw ReconstructionJsonViolation(std::string("missing field '") + key +
                                      "'");
  }
  try {
    return it->get<T>();
  } catch (const nlohmann::json::exception&) {
    throw ReconstructionJsonViolation(std::string("field '") + key +
                                      "' has an invalid value type");
  }
}

template <typename T>
std::optional<T> JsonOptionalField(const nlohmann::json& obj,
                                   const char* key) {
  const auto it = obj.find(key);
  if (it == obj.end()) return std::nullopt;
  try {
    return it->get<T>();
  } catch (const nlohmann::json::exception&) {
    throw ReconstructionJsonViolation(std::string("field '") + key +
                                      "' has an invalid value type");
  }
}

// Helper for optional double-vector fields (distortion_coefficients).
inline std::vector<double> JsonDoubleVector(const nlohmann::json& obj,
                                            const char* key,
                                            bool optional) {
  const auto it = obj.find(key);
  if (it == obj.end()) {
    if (optional) return {};
    throw ReconstructionJsonViolation(std::string("missing field '") + key +
                                      "'");
  }
  if (!it->is_array()) {
    throw ReconstructionJsonViolation(std::string("field '") + key +
                                      "' must be an array");
  }
  std::vector<double> values;
  values.reserve(it->size());
  for (const auto& item : *it) {
    if (!item.is_number()) {
      throw ReconstructionJsonViolation(std::string("field '") + key +
                                        "' contains a non-number");
    }
    values.push_back(item.get<double>());
  }
  return values;
}

inline std::array<double, 4> JsonVec4(const nlohmann::json& obj,
                                      const char* key) {
  const std::vector<double> v = JsonDoubleVector(obj, key, /*optional=*/false);
  if (v.size() != 4) {
    throw ReconstructionJsonViolation(std::string("field '") + key +
                                      "' must have exactly 4 elements");
  }
  return {v[0], v[1], v[2], v[3]};
}

inline std::array<double, 3> JsonVec3(const nlohmann::json& obj,
                                      const char* key) {
  const std::vector<double> v = JsonDoubleVector(obj, key, /*optional=*/false);
  if (v.size() != 3) {
    throw ReconstructionJsonViolation(std::string("field '") + key +
                                      "' must have exactly 3 elements");
  }
  return {v[0], v[1], v[2]};
}

inline std::array<std::uint8_t, 3> JsonColor(const nlohmann::json& obj,
                                             const char* key) {
  const std::vector<double> v = JsonDoubleVector(obj, key, /*optional=*/false);
  if (v.size() != 3) {
    throw ReconstructionJsonViolation(std::string("field '") + key +
                                      "' must have exactly 3 elements");
  }
  return {static_cast<std::uint8_t>(v[0]), static_cast<std::uint8_t>(v[1]),
          static_cast<std::uint8_t>(v[2])};
}

}  // namespace

// Parses a canonical Reconstruction document (schema_version 2) back into the
// in-memory type — the strict inverse of ReconstructionToJson. FAILS CLOSED with
// a typed ValidationError on malformed JSON, missing required fields, or
// wrong-typed values. Unknown keys are ignored (forward compatibility).
inline Reconstruction ReconstructionFromJson(const std::string& json_text) {
  nlohmann::json doc =
      nlohmann::json::parse(json_text, nullptr, /*allow_exceptions=*/false);
  if (doc.is_discarded() || !doc.is_object()) {
    throw ReconstructionJsonViolation(
        "document is not valid JSON object text");
  }

  Reconstruction rec;
  rec.reconstruction_id = JsonGetField<std::string>(doc, "reconstruction_id");
  rec.scene_id = JsonGetField<std::string>(doc, "scene_id");

  const auto sids = JsonGetField<nlohmann::json>(doc, "session_ids");
  if (!sids.is_array()) {
    throw ReconstructionJsonViolation("field 'session_ids' must be an array");
  }
  for (const auto& sid : sids) {
    if (!sid.is_string()) {
      throw ReconstructionJsonViolation(
          "field 'session_ids' contains a non-string");
    }
    rec.session_ids.push_back(sid.get<std::string>());
  }

  rec.coordinate_frame = JsonGetField<std::string>(doc, "coordinate_frame");
  if (auto s = JsonOptionalField<std::string>(doc, "status")) {
    rec.status = *s;
  }
  rec.created_at_ns = JsonGetField<std::int64_t>(doc, "created_at_ns");

  // Provenance.
  const nlohmann::json& prov = JsonGetField<nlohmann::json>(doc, "provenance");
  if (!prov.is_object()) {
    throw ReconstructionJsonViolation("field 'provenance' must be an object");
  }
  const nlohmann::json& backend = JsonGetField<nlohmann::json>(prov, "backend");
  if (!backend.is_object()) {
    throw ReconstructionJsonViolation("provenance 'backend' must be an object");
  }
  rec.provenance.backend.name = JsonGetField<std::string>(backend, "name");
  rec.provenance.backend.version = JsonGetField<std::string>(backend, "version");
  rec.provenance.backend.adapter_version =
      JsonGetField<std::string>(backend, "adapter_version");
  rec.provenance.configuration_hash =
      JsonGetField<std::string>(prov, "configuration_hash");
  const nlohmann::json& iah =
      JsonGetField<nlohmann::json>(prov, "input_artifact_hashes");
  if (!iah.is_array()) {
    throw ReconstructionJsonViolation(
        "provenance 'input_artifact_hashes' must be an array");
  }
  for (const auto& h : iah) {
    if (!h.is_string()) {
      throw ReconstructionJsonViolation(
          "provenance 'input_artifact_hashes' contains a non-string");
    }
    rec.provenance.input_artifact_hashes.push_back(h.get<std::string>());
  }
  rec.provenance.engine_version =
      JsonGetField<std::string>(prov, "engine_version");
  rec.provenance.engine_commit = JsonGetField<std::string>(prov, "engine_commit");
  rec.provenance.git_commit = JsonGetField<std::string>(prov, "git_commit");
  rec.provenance.started_at_ns = JsonGetField<std::int64_t>(prov, "started_at_ns");
  rec.provenance.finished_at_ns =
      JsonGetField<std::int64_t>(prov, "finished_at_ns");
  rec.provenance.duration_ns = JsonGetField<std::int64_t>(prov, "duration_ns");
  if (auto bsj = JsonOptionalField<std::string>(prov, "backend_specific_json")) {
    rec.provenance.backend_specific_json = *bsj;
  }

  // Cameras.
  const nlohmann::json& cameras = JsonGetField<nlohmann::json>(doc, "cameras");
  if (!cameras.is_array()) {
    throw ReconstructionJsonViolation("field 'cameras' must be an array");
  }
  for (const auto& entry : cameras) {
    if (!entry.is_object()) {
      throw ReconstructionJsonViolation("camera entry must be an object");
    }
    ReconCamera cam;
    cam.camera_id = JsonGetField<std::uint32_t>(entry, "camera_id");
    cam.width = JsonGetField<std::int64_t>(entry, "width");
    cam.height = JsonGetField<std::int64_t>(entry, "height");
    cam.intrinsic_model = JsonGetField<std::string>(entry, "intrinsic_model");
    cam.fx = JsonGetField<double>(entry, "fx");
    cam.fy = JsonGetField<double>(entry, "fy");
    cam.cx = JsonGetField<double>(entry, "cx");
    cam.cy = JsonGetField<double>(entry, "cy");
    cam.distortion_model = JsonGetField<std::string>(entry, "distortion_model");
    cam.distortion_coefficients =
        JsonDoubleVector(entry, "distortion_coefficients", /*optional=*/true);
    if (auto ref = JsonOptionalField<std::string>(entry, "calibration_ref")) {
      cam.calibration_ref = *ref;
    }
    rec.cameras.push_back(std::move(cam));
  }

  // Images.
  const nlohmann::json& images = JsonGetField<nlohmann::json>(doc, "images");
  if (!images.is_array()) {
    throw ReconstructionJsonViolation("field 'images' must be an array");
  }
  for (const auto& entry : images) {
    if (!entry.is_object()) {
      throw ReconstructionJsonViolation("image entry must be an object");
    }
    ReconImage img;
    img.image_id = JsonGetField<std::uint32_t>(entry, "image_id");
    img.camera_id = JsonGetField<std::uint32_t>(entry, "camera_id");
    if (auto fid = JsonOptionalField<std::string>(entry, "frame_id")) {
      img.frame_id = *fid;
    }
    img.name = JsonGetField<std::string>(entry, "name");
    const nlohmann::json& pose = JsonGetField<nlohmann::json>(entry, "pose");
    if (!pose.is_object()) {
      throw ReconstructionJsonViolation("image 'pose' must be an object");
    }
    img.pose.rotation_xyzw = JsonVec4(pose, "rotation_xyzw");
    img.pose.translation_xyz = JsonVec3(pose, "translation_xyz");
    img.detected = JsonGetField<bool>(entry, "detected");
    rec.images.push_back(std::move(img));
  }

  // Points3D.
  const nlohmann::json& points = JsonGetField<nlohmann::json>(doc, "points3D");
  if (!points.is_array()) {
    throw ReconstructionJsonViolation("field 'points3D' must be an array");
  }
  for (const auto& entry : points) {
    if (!entry.is_object()) {
      throw ReconstructionJsonViolation("point3D entry must be an object");
    }
    ReconPoint3D pt;
    pt.point3d_id = JsonGetField<std::uint64_t>(entry, "point3d_id");
    pt.xyz = JsonVec3(entry, "xyz");
    pt.color = JsonColor(entry, "color");
    pt.error = JsonGetField<double>(entry, "error");
    const nlohmann::json& track = JsonGetField<nlohmann::json>(entry, "track");
    if (!track.is_array()) {
      throw ReconstructionJsonViolation("point3D 'track' must be an array");
    }
    for (const auto& te : track) {
      if (!te.is_object()) {
        throw ReconstructionJsonViolation("track element must be an object");
      }
      ReconPoint3D::TrackElement element;
      element.image_id = JsonGetField<std::uint32_t>(te, "image_id");
      element.point2d_idx = JsonGetField<std::int32_t>(te, "point2d_idx");
      pt.track.push_back(element);
    }
    rec.points3D.push_back(std::move(pt));
  }

  return rec;
}

}  // namespace spatial::core