#include "importers/trajectory/trajectory_importer.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <locale>
#include <map>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/errors/project_error.h"
#include "core/scene/query/scene_query.h"
#include "core/trajectory/trajectory.h"
#include "core/utils/fs.h"
#include "core/utils/sha256.h"
#include "trajectory_import_build_info.h"

namespace spatial::importers {
namespace {

using core::ArtifactManifest;
using core::ErrorCode;
using core::FormatUuid;
using core::GenerateUuid;
using core::TrajectoryPoseNode;
using core::Uuid;
using core::ValidationError;
using json = nlohmann::json;

constexpr const char* kProducer = "trajectory-import.tum";

[[noreturn]] void Invalid(const std::string& message) {
  throw ValidationError(ErrorCode::kValidationDomain,
                        "trajectory import: " + message);
}

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

// Parse decimal seconds without first rounding through a floating-point
// value. Non-zero precision finer than a nanosecond is rejected, not rounded.
std::int64_t ParseTimestampNs(const std::string& text, std::size_t line) {
  const auto fail = [&]() -> void {
    Invalid("invalid or non-nanosecond timestamp at line " +
            std::to_string(line));
  };
  if (text.empty() || text.size() > 128) fail();
  std::size_t pos = 0;
  if (text[pos] == '+') ++pos;
  if (pos == text.size() || text[pos] == '-') fail();
  std::string digits;
  std::size_t fractional = 0;
  while (pos < text.size() && IsDigit(text[pos])) digits.push_back(text[pos++]);
  if (pos < text.size() && text[pos] == '.') {
    ++pos;
    while (pos < text.size() && IsDigit(text[pos])) {
      digits.push_back(text[pos++]);
      ++fractional;
    }
  }
  if (digits.empty()) fail();
  int exponent = 0;
  if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
    ++pos;
    bool negative = false;
    if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
      negative = text[pos] == '-';
      ++pos;
    }
    const auto start = pos;
    while (pos < text.size() && IsDigit(text[pos])) {
      if (exponent > 1000) fail();
      exponent = exponent * 10 + (text[pos++] - '0');
    }
    if (pos == start || exponent > 1000) fail();
    if (negative) exponent = -exponent;
  }
  if (pos != text.size()) fail();
  const auto first = digits.find_first_not_of('0');
  if (first == std::string::npos) return 0;
  digits.erase(0, first);
  const int power = 9 + exponent - static_cast<int>(fractional);
  if (power < 0) {
    const auto remove = static_cast<std::size_t>(-power);
    if (remove >= digits.size()) fail();
    for (std::size_t i = digits.size() - remove; i < digits.size(); ++i) {
      if (digits[i] != '0') fail();
    }
    digits.resize(digits.size() - remove);
  } else {
    if (power > 19 || digits.size() + static_cast<std::size_t>(power) > 19) {
      fail();
    }
    digits.append(static_cast<std::size_t>(power), '0');
  }
  std::int64_t value = 0;
  const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size()) fail();
  return value;
}

double ParseFinite(const std::string& text, std::size_t line) {
  double value = 0.0;
  const char* begin = text.data();
  const char* end = begin + text.size();
  if (begin != end && *begin == '+') ++begin;
  const auto parsed = std::from_chars(begin, end, value, std::chars_format::general);
  if (begin == end || parsed.ec != std::errc{} || parsed.ptr != end ||
      !std::isfinite(value)) {
    Invalid("non-finite or invalid pose value at line " + std::to_string(line));
  }
  return value;
}

std::vector<TrajectoryPoseNode> ParseTum(const std::vector<std::uint8_t>& source) {
  const std::string text(source.begin(), source.end());
  std::istringstream input(text);
  input.imbue(std::locale::classic());
  std::vector<TrajectoryPoseNode> nodes;
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    const auto start = line.find_first_not_of(" \t\r");
    if (start == std::string::npos || line[start] == '#') continue;
    std::istringstream fields(line);
    fields.imbue(std::locale::classic());
    std::array<std::string, 8> values;
    for (auto& value : values) {
      if (!(fields >> value)) Invalid("expected eight fields at line " + std::to_string(line_number));
    }
    std::string extra;
    if (fields >> extra) Invalid("unexpected extra field at line " + std::to_string(line_number));
    TrajectoryPoseNode node;
    node.timestamp_ns = ParseTimestampNs(values[0], line_number);
    if (!nodes.empty() && node.timestamp_ns <= nodes.back().timestamp_ns) {
      Invalid("timestamps must be strictly increasing at line " + std::to_string(line_number));
    }
    for (std::size_t i = 0; i < 3; ++i) node.position_xyz[i] = ParseFinite(values[i + 1], line_number);
    for (std::size_t i = 0; i < 4; ++i) node.rotation_xyzw[i] = ParseFinite(values[i + 4], line_number);
    const double norm = std::hypot(std::hypot(node.rotation_xyzw[0], node.rotation_xyzw[1]),
                                   std::hypot(node.rotation_xyzw[2], node.rotation_xyzw[3]));
    if (!std::isfinite(norm) || std::abs(norm - 1.0) > 1e-3) {
      Invalid("quaternion is not unit length at line " + std::to_string(line_number));
    }
    for (auto& value : node.rotation_xyzw) value /= norm;
    if (nodes.size() > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
      Invalid("too many poses");
    }
    node.sequence_index = static_cast<std::int64_t>(nodes.size());
    nodes.push_back(std::move(node));
  }
  if (nodes.empty()) Invalid("source has no poses");
  return nodes;
}

json NodesJson(const std::vector<TrajectoryPoseNode>& nodes) {
  json out = json::array();
  for (const auto& node : nodes) {
    out.push_back({{"frame_id", node.frame_id},
                   {"timestamp_ns", node.timestamp_ns},
                   {"sequence_index", node.sequence_index},
                   {"position_xyz", node.position_xyz},
                   {"rotation_xyzw", node.rotation_xyzw}});
  }
  return out;
}

double TotalDistance(const std::vector<TrajectoryPoseNode>& nodes) {
  double total = 0.0;
  for (std::size_t i = 1; i < nodes.size(); ++i) {
    const auto& a = nodes[i - 1].position_xyz;
    const auto& b = nodes[i].position_xyz;
    total += std::hypot(b[0] - a[0], b[1] - a[1], b[2] - a[2]);
    if (!std::isfinite(total)) Invalid("trajectory distance overflows");
  }
  return total;
}

ArtifactManifest MakeManifest(const std::string& type) {
  ArtifactManifest manifest;
  manifest.artifact_uuid = GenerateUuid();
  manifest.type = type;
  manifest.schema_version = 1;
  manifest.producer = {kProducer, kTrajectoryImportVersion, kTrajectoryImportGitCommit};
  manifest.creation_timestamp = core::fs::Iso8601UtcNow();
  manifest.validation_status = "unverified";
  return manifest;
}

}  // namespace

TrajectoryImportResult ImportTumCameraTrajectory(
    core::ArtifactStore& store, core::MetadataDb& db, const Uuid& project_id,
    const std::vector<std::uint8_t>& source, const TrajectoryImportOptions& options) {
  if (core::IsNil(options.session_id) || core::IsNil(options.sensor_id)) {
    Invalid("session and camera sensor are required");
  }
  if (options.coordinate_frame.empty() ||
      options.coordinate_frame.find_first_not_of(" \t\r\n") == std::string::npos ||
      std::any_of(options.coordinate_frame.begin(), options.coordinate_frame.end(),
                  [](char value) {
                    const auto c = static_cast<unsigned char>(value);
                    return c < 32 || c == 127;
                  })) {
    Invalid("a named coordinate frame is required");
  }
  if (options.source_unit != TrajectoryLengthUnit::kMeters ||
      options.clock_domain != TrajectoryClockDomain::kPlatform ||
      options.pose_frame != TrajectoryPoseFrame::kCamera) {
    Invalid("explicit meters, platform clock, and camera pose declarations are required");
  }
  const auto has_control = [](const std::string& value) {
    return std::any_of(value.begin(), value.end(),
                       [](char raw) {
                         const auto c = static_cast<unsigned char>(raw);
                         return c < 32 || c == 127;
                       });
  };
  if (has_control(options.source_producer) || has_control(options.source_version) ||
      (options.source_producer.empty() && !options.source_version.empty())) {
    Invalid("source producer and version are inconsistent");
  }

  core::SceneQuery query(db);
  const auto session = query.FindCaptureSession(options.session_id);
  if (!session || session->project_id != project_id) Invalid("session is not in this project");
  const auto scene = query.SessionScene(options.session_id);
  if (!scene || scene->project_id != project_id) Invalid("session has no scene in this project");
  const auto sensor = query.ResolveSensor(options.sensor_id);
  if (!sensor || sensor->project_id != project_id ||
      (sensor->type != "camera" && sensor->type != "rgbd")) {
    Invalid("sensor is not a camera in this project");
  }

  auto nodes = ParseTum(source);
  std::map<std::int64_t, std::vector<Uuid>> frames;
  for (const auto& frame : query.FramesBySession(options.session_id)) {
    if (frame.sensor_id == options.sensor_id && frame.scene_id == scene->scene_id) {
      frames[frame.timestamp_ns.value()].push_back(frame.frame_id);
    }
  }
  for (auto& node : nodes) {
    const auto found = frames.find(node.timestamp_ns);
    if (found == frames.end()) {
      Invalid("no frame matches timestamp " + std::to_string(node.timestamp_ns));
    }
    if (found->second.size() != 1) {
      Invalid("multiple frames match timestamp " + std::to_string(node.timestamp_ns));
    }
    node.frame_id = FormatUuid(found->second.front());
  }
  const double distance = TotalDistance(nodes);
  const auto duration = nodes.back().timestamp_ns - nodes.front().timestamp_ns;
  const json nodes_json = NodesJson(nodes);
  const std::string source_hash = core::Sha256Hex(source);
  json config = {{"format", "tum_camera_v1"},
                 {"project_id", FormatUuid(project_id)},
                 {"scene_id", FormatUuid(scene->scene_id)},
                 {"session_id", FormatUuid(options.session_id)},
                 {"sensor_id", FormatUuid(options.sensor_id)},
                 {"coordinate_frame", options.coordinate_frame},
                 {"source_unit", "meters"},
                 {"clock_domain", "platform"},
                 {"pose_frame", "camera"},
                 {"source_producer", options.source_producer},
                 {"source_version", options.source_version},
                 {"source_hash", source_hash},
                 {"producer_version", kTrajectoryImportVersion},
                 {"producer_commit", kTrajectoryImportGitCommit}};
  config["frame_ids"] = json::array();
  for (const auto& node : nodes) config["frame_ids"].push_back(node.frame_id);
  const std::string config_text = config.dump();
  const std::vector<std::uint8_t> config_bytes(config_text.begin(), config_text.end());
  const std::string config_hash = core::Sha256Hex(config_bytes);

  // Reuse existing bytes without replacing their artifact index. The index is
  // currently unique by content hash, so writing another instance could
  // otherwise invalidate references to the existing artifact.
  const auto source_index = db.FindArtifactByHash(source_hash);
  if (source_index || store.Has(source_hash)) {
    const auto existing = store.Get(source_hash);
    if (!existing || *existing != source) Invalid("existing source CAS payload is missing or corrupt");
  }
  const auto config_index = db.FindArtifactByHash(config_hash);
  if (config_index || store.Has(config_hash)) {
    const auto existing = store.Get(config_hash);
    if (!existing || *existing != config_bytes) Invalid("existing configuration CAS payload is missing or corrupt");
  }

  for (const auto& index : db.FindArtifactsByType("trajectory")) {
    if (index.config_hash != config_hash) continue;
    const auto manifest = store.ReadManifest(index.artifact_id);
    if (!manifest || manifest->producer.id != kProducer ||
        manifest->configuration_hash != config_hash ||
        manifest->input_artifact_hashes != std::vector<std::string>{source_hash, config_hash} ||
        manifest->coordinate_frame != options.coordinate_frame || manifest->unit != "meters" ||
        manifest->validation_status != "unverified") {
      Invalid("existing trajectory provenance is inconsistent");
    }
    const auto bytes = store.Get(index.content_hash);
    if (!bytes) Invalid("existing trajectory CAS payload is missing");
    const std::string document(bytes->begin(), bytes->end());
    const auto doc = json::parse(document, nullptr, false);
    if (!doc.is_object() || doc.size() != 3 || !doc.contains("schema_version") ||
        !doc.at("schema_version").is_number_integer() || doc.at("schema_version") != 1 ||
        !doc.contains("trajectory_id") || !doc.at("trajectory_id").is_string() ||
        !doc.contains("nodes") || doc.at("nodes") != nodes_json ||
        doc.contains("metric_basis")) {
      Invalid("existing trajectory payload is inconsistent");
    }
    const Uuid trajectory_id = core::ParseUuid(doc.at("trajectory_id").get<std::string>());
    const auto rows = db.FindTrajectoriesBySession(options.session_id);
    const auto row = std::find_if(rows.begin(), rows.end(), [&](const auto& x) {
      return x.trajectory_id == trajectory_id;
    });
    if (row == rows.end() || row->scene_id != scene->scene_id ||
        row->coordinate_frame != options.coordinate_frame || row->kind != "slam" ||
        row->node_count != static_cast<std::int64_t>(nodes.size()) ||
        row->document_json != document || !source_index || !config_index) {
      Invalid("existing trajectory database record is inconsistent");
    }
    return {trajectory_id, index.artifact_id, source_index->artifact_id,
            config_index->artifact_id, scene->scene_id, index.content_hash,
            source_hash, config_hash, row->node_count, row->total_distance_m, true};
  }

  const Uuid trajectory_id = GenerateUuid();
  const json document = {{"schema_version", 1},
                         {"trajectory_id", FormatUuid(trajectory_id)},
                         {"nodes", nodes_json}};
  const std::string serialized = document.dump();
  const std::vector<std::uint8_t> payload(serialized.begin(), serialized.end());
  core::TrajectoryRow row;
  row.trajectory_id = trajectory_id;
  row.scene_id = scene->scene_id;
  row.session_id = options.session_id;
  row.kind = "slam";
  row.coordinate_frame = options.coordinate_frame;
  row.status = "building";
  row.node_count = static_cast<std::int64_t>(nodes.size());
  row.total_distance_m = distance;
  row.total_duration_ns = duration;
  row.created_at_ns = core::fs::TimestampNsNow();
  row.document_json = serialized;

  auto raw_manifest = MakeManifest("trajectory_source");
  raw_manifest.unit = "none";
  raw_manifest.mime_type = "text/plain";
  auto config_manifest = MakeManifest("trajectory_import_configuration");
  config_manifest.unit = "none";
  config_manifest.mime_type = "application/json";
  config_manifest.input_artifact_hashes = {source_hash};
  auto trajectory_manifest = MakeManifest("trajectory");
  trajectory_manifest.coordinate_frame = options.coordinate_frame;
  trajectory_manifest.unit = "meters";
  trajectory_manifest.mime_type = "application/json";
  trajectory_manifest.configuration_hash = config_hash;
  trajectory_manifest.input_artifact_hashes = {source_hash, config_hash};

  core::MetadataDb::WriteTransaction transaction(db);
  const auto raw = store.Put(source, raw_manifest);
  const auto stored_config = store.Put(config_bytes, config_manifest);
  const auto stored = store.Put(payload, trajectory_manifest);
  db.AddTrajectory(row);
  transaction.Commit();
  return {trajectory_id, stored.artifact_uuid, raw.artifact_uuid,
          stored_config.artifact_uuid, scene->scene_id, stored.content_hash,
          raw.content_hash, stored_config.content_hash, row.node_count, distance, false};
}

}  // namespace spatial::importers
