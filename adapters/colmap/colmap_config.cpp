#include "adapters/colmap/colmap_config.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/errors/project_error.h"

namespace spatial::adapters::colmap {

namespace {

using nlohmann::json;
using spatial::core::ErrorCode;
using spatial::core::ValidationError;

// Calibration vocabulary that must never appear in the configuration surface
// (RFC-0009 §6): calibration travels as a content hash in input_refs, never
// inside config_json. Checked recursively at every object key.
const std::unordered_set<std::string>& CalibrationKeys() {
  static const std::unordered_set<std::string> keys = {
      "fx", "fy", "cx", "cy",       "distortion",
      "intrinsics", "extrinsics",   "calibration",
  };
  return keys;
}

const std::unordered_set<std::string>& TopLevelKeys() {
  static const std::unordered_set<std::string> keys = {
      "threads", "seed", "feature_extractor", "matcher", "mapper",
      "enabled_stages",
  };
  return keys;
}

const std::unordered_set<std::string>& FeatureExtractorKeys() {
  static const std::unordered_set<std::string> keys = {
      "max_image_size", "max_num_features", "detector", "descriptor",
      "sift_scale_space_octaves", "sift_domain_size_pooling",
  };
  return keys;
}

const std::unordered_set<std::string>& MatcherKeys() {
  static const std::unordered_set<std::string> keys = {
      "guided_matching", "max_ratio", "max_distance", "cross_check",
  };
  return keys;
}

const std::unordered_set<std::string>& MapperKeys() {
  static const std::unordered_set<std::string> keys = {
      "min_num_matches", "ba_refine_principal_point",
      "ba_min_num_residuals_for_multithreading",
  };
  return keys;
}

ValidationError Violation(const std::string& message) {
  return ValidationError(ErrorCode::kValidationDomain, message, {},
                         /*recoverable=*/false,
                         "Fix the configuration document (config_json) so it "
                         "matches the keys and value types declared by the "
                         "COLMAP adapter configuration schema.");
}

ValidationError CalibrationViolation(const std::string& message) {
  return ValidationError(
      ErrorCode::kValidationDomain, message, {},
      /*recoverable=*/false,
      "Move spatial measurements into TaskRequest.input_refs as CAS content "
      "hashes (a CalibrationArtifact for calibration); config_json carries "
      "algorithm settings only (RFC-0009 §6).");
}

bool IsKnownStage(const std::string& name) {
  return name == "feature_extractor" || name == "matcher" || name == "mapper";
}

// Rejects any object key that belongs to the calibration vocabulary, anywhere
// in the document (RFC-0009 §6: "a calibration value in the configuration
// surface is a contract violation rejected by validation").
void RejectCalibrationVocabulary(const json& node) {
  if (node.is_object()) {
    for (auto it = node.begin(); it != node.end(); ++it) {
      if (CalibrationKeys().count(it.key()) != 0) {
        throw CalibrationViolation(
            "calibration value '" + it.key() +
            "' in the configuration surface is a contract "
            "violation (RFC-0009 §6)");
      }
      RejectCalibrationVocabulary(it.value());
    }
  } else if (node.is_array()) {
    for (const auto& item : node) {
      RejectCalibrationVocabulary(item);
    }
  }
}

void RejectUnknownKeys(const json& obj, const std::string& section,
                       const std::unordered_set<std::string>& allowed) {
  for (auto it = obj.begin(); it != obj.end(); ++it) {
    if (allowed.count(it.key()) == 0) {
      throw Violation("unknown key '" + it.key() + "' in section '" +
                      section + "'");
    }
  }
}

}  // namespace

const char* ColmapStageName(ColmapStage stage) noexcept {
  switch (stage) {
    case ColmapStage::kFeatureExtractor:
      return "feature_extractor";
    case ColmapStage::kMatcher:
      return "matcher";
    case ColmapStage::kMapper:
      return "mapper";
  }
  return "unknown";
}

std::optional<ColmapStage> ColmapStageFromName(const std::string& name) noexcept {
  if (name == ColmapStageName(ColmapStage::kFeatureExtractor)) {
    return ColmapStage::kFeatureExtractor;
  }
  if (name == ColmapStageName(ColmapStage::kMatcher)) {
    return ColmapStage::kMatcher;
  }
  if (name == ColmapStageName(ColmapStage::kMapper)) {
    return ColmapStage::kMapper;
  }
  return std::nullopt;
}

ColmapConfig ColmapConfig::Default() {
  ColmapConfig config;
  config.enabled_stages = {};
  return config;
}

ColmapConfig ColmapConfig::FromJson(const std::string& config_json) {
  // Empty / whitespace-only document = the default effective configuration.
  const bool blank =
      std::all_of(config_json.begin(), config_json.end(),
                  [](unsigned char c) {
                    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
                  });
  json doc = json::object();
  if (!blank) {
    doc = json::parse(config_json, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded()) {
      throw Violation("config_json is not valid JSON");
    }
  }

  try {
    RejectCalibrationVocabulary(doc);
    if (!doc.is_object()) {
      throw Violation("config_json must be a JSON object");
    }
    RejectUnknownKeys(doc, "config", TopLevelKeys());

    ColmapConfig config;

    if (doc.contains("threads")) {
      config.threads = doc["threads"].get<int>();
      if (config.threads == 0 || config.threads < -1) {
        throw Violation("'threads' must be -1 or >= 1");
      }
    }
    if (doc.contains("seed")) {
      config.seed = doc["seed"].get<std::string>();
    }

    if (doc.contains("feature_extractor")) {
      const json& section = doc["feature_extractor"];
      if (!section.is_object()) {
        throw Violation("'feature_extractor' must be an object");
      }
      RejectUnknownKeys(section, "feature_extractor", FeatureExtractorKeys());
      if (section.contains("max_image_size")) {
        config.feature_extractor.max_image_size =
            section["max_image_size"].get<int>();
      }
      if (section.contains("max_num_features")) {
        config.feature_extractor.max_num_features =
            section["max_num_features"].get<int>();
      }
      if (section.contains("detector")) {
        config.feature_extractor.detector =
            section["detector"].get<std::string>();
      }
      if (section.contains("descriptor")) {
        config.feature_extractor.descriptor =
            section["descriptor"].get<std::string>();
      }
      if (section.contains("sift_scale_space_octaves")) {
        config.feature_extractor.sift_scale_space_octaves =
            section["sift_scale_space_octaves"].get<std::string>();
      }
      if (section.contains("sift_domain_size_pooling")) {
        config.feature_extractor.sift_domain_size_pooling =
            section["sift_domain_size_pooling"].get<std::string>();
      }
    }

    if (doc.contains("matcher")) {
      const json& section = doc["matcher"];
      if (!section.is_object()) {
        throw Violation("'matcher' must be an object");
      }
      RejectUnknownKeys(section, "matcher", MatcherKeys());
      if (section.contains("guided_matching")) {
        config.matcher.guided_matching =
            section["guided_matching"].get<bool>();
      }
      if (section.contains("max_ratio")) {
        config.matcher.max_ratio = section["max_ratio"].get<double>();
      }
      if (section.contains("max_distance")) {
        config.matcher.max_distance = section["max_distance"].get<double>();
      }
      if (section.contains("cross_check")) {
        config.matcher.cross_check = section["cross_check"].get<bool>();
      }
    }

    if (doc.contains("mapper")) {
      const json& section = doc["mapper"];
      if (!section.is_object()) {
        throw Violation("'mapper' must be an object");
      }
      RejectUnknownKeys(section, "mapper", MapperKeys());
      if (section.contains("min_num_matches")) {
        config.mapper.min_num_matches =
            section["min_num_matches"].get<int>();
      }
      if (section.contains("ba_refine_principal_point")) {
        config.mapper.ba_refine_principal_point =
            section["ba_refine_principal_point"].get<bool>();
      }
      if (section.contains("ba_min_num_residuals_for_multithreading")) {
        config.mapper.ba_min_num_residuals_for_multithreading =
            section["ba_min_num_residuals_for_multithreading"].get<int>();
      }
    }

    if (doc.contains("enabled_stages")) {
      const json& stages = doc["enabled_stages"];
      if (!stages.is_array()) {
        throw Violation("'enabled_stages' must be an array of stage names");
      }
      std::unordered_set<std::string> seen;
      for (const auto& entry : stages) {
        if (!entry.is_string()) {
          throw Violation("'enabled_stages' entries must be strings");
        }
        const std::string name = entry.get<std::string>();
        if (!IsKnownStage(name)) {
          throw Violation("unknown stage '" + name + "' in 'enabled_stages'");
        }
        if (!seen.insert(name).second) {
          throw Violation("duplicate stage '" + name +
                          "' in 'enabled_stages'");
        }
        config.enabled_stages.push_back(name);
      }
    }

    return config;
  } catch (const nlohmann::json::exception& e) {
    throw Violation(std::string("invalid configuration value: ") + e.what());
  }
}

std::string ColmapConfig::ToJson() const {
  json doc;
  doc["threads"] = threads;
  doc["seed"] = seed;
  doc["feature_extractor"] = {
      {"max_image_size", feature_extractor.max_image_size},
      {"max_num_features", feature_extractor.max_num_features},
      {"detector", feature_extractor.detector},
      {"descriptor", feature_extractor.descriptor},
      {"sift_scale_space_octaves", feature_extractor.sift_scale_space_octaves},
      {"sift_domain_size_pooling", feature_extractor.sift_domain_size_pooling},
  };
  doc["matcher"] = {
      {"guided_matching", matcher.guided_matching},
      {"max_ratio", matcher.max_ratio},
      {"max_distance", matcher.max_distance},
      {"cross_check", matcher.cross_check},
  };
  doc["mapper"] = {
      {"min_num_matches", mapper.min_num_matches},
      {"ba_refine_principal_point", mapper.ba_refine_principal_point},
      {"ba_min_num_residuals_for_multithreading",
       mapper.ba_min_num_residuals_for_multithreading},
  };
  doc["enabled_stages"] = enabled_stages;
  return doc.dump();
}

std::vector<std::string> ColmapConfig::Plan() const {
  const std::vector<std::string> order = {
      ColmapStageName(ColmapStage::kFeatureExtractor),
      ColmapStageName(ColmapStage::kMatcher),
      ColmapStageName(ColmapStage::kMapper),
  };
  if (enabled_stages.empty()) {
    return order;
  }
  std::vector<std::string> plan;
  plan.reserve(enabled_stages.size());
  for (const std::string& stage : order) {
    if (std::find(enabled_stages.begin(), enabled_stages.end(), stage) !=
        enabled_stages.end()) {
      plan.push_back(stage);
    }
  }
  return plan;
}

std::vector<std::string> ColmapConfig::BuildStageArgs(ColmapStage stage) const {
  switch (stage) {
    case ColmapStage::kFeatureExtractor:
      return {
          "--SiftExtraction.max_image_size",
          std::to_string(feature_extractor.max_image_size),
          "--SiftExtraction.max_num_features",
          std::to_string(feature_extractor.max_num_features),
          "--SiftExtraction.detector", feature_extractor.detector,
          "--SiftExtraction.descriptor", feature_extractor.descriptor,
          "--SiftExtraction.scale_space_octaves",
          feature_extractor.sift_scale_space_octaves,
          "--SiftExtraction.domain_size_pooling",
          feature_extractor.sift_domain_size_pooling,
      };
    case ColmapStage::kMatcher:
      return {
          "--SiftMatching.guided_matching",
          matcher.guided_matching ? "1" : "0",
          "--SiftMatching.max_ratio", std::to_string(matcher.max_ratio),
          "--SiftMatching.max_distance", std::to_string(matcher.max_distance),
          "--SiftMatching.cross_check", matcher.cross_check ? "1" : "0",
      };
    case ColmapStage::kMapper:
      return {
          "--Mapper.min_num_matches", std::to_string(mapper.min_num_matches),
          "--Mapper.ba_refine_principal_point",
          mapper.ba_refine_principal_point ? "1" : "0",
          "--Mapper.ba_min_num_residuals_for_multithreading",
          std::to_string(mapper.ba_min_num_residuals_for_multithreading),
      };
  }
  return {};
}

}  // namespace spatial::adapters::colmap
