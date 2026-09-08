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
      "bundle_adjuster", "enabled_stages",
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

// bundle_adjuster stage keys (P3-impl-8c P9/P16). Algorithm settings ONLY:
// calibration still travels in input_refs, never here (RFC-0009 §6).
const std::unordered_set<std::string>& BundleAdjusterKeys() {
  static const std::unordered_set<std::string> keys = {
      "loss_function", "loss_scale_px", "max_num_iterations",
      "refine_focal_length", "refine_principal_point", "refine_extra_params",
      "refine_extrinsics", "refine_intrinsics",
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
  return name == "feature_extractor" || name == "matcher" || name == "mapper" ||
         name == "bundle_adjuster";
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
    case ColmapStage::kBundleAdjuster:
      return "bundle_adjuster";
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
  if (name == ColmapStageName(ColmapStage::kBundleAdjuster)) {
    return ColmapStage::kBundleAdjuster;
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

  // The engine's PipelineCompiler wraps each stage's effective configuration
  // in a fixed envelope {pipeline_id, pipeline_version, stage, config}
  // (pipeline_compiler.cpp); the adapter unwraps the algorithm settings inside
  // `config` and validates those. A document without the envelope (direct
  // adapter calls, the worker's task body) validates as-is.
  if (doc.is_object() && doc.size() == 4 && doc.contains("pipeline_id") &&
      doc.contains("pipeline_version") && doc.contains("stage") &&
      doc.contains("config") && doc["config"].is_object()) {
    doc = doc["config"];
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

    if (doc.contains("bundle_adjuster")) {
      const json& section = doc["bundle_adjuster"];
      if (!section.is_object()) {
        throw Violation("'bundle_adjuster' must be an object");
      }
      RejectUnknownKeys(section, "bundle_adjuster", BundleAdjusterKeys());
      if (section.contains("loss_function")) {
        const std::string loss = section["loss_function"].get<std::string>();
        if (loss != "SoftL1" && loss != "Trivial" && loss != "Cauchy") {
          throw Violation(
              "'bundle_adjuster.loss_function' must be one of "
              "{SoftL1, Trivial, Cauchy}");
        }
        config.bundle_adjuster.loss_function = loss;
      }
      if (section.contains("loss_scale_px")) {
        const double scale = section["loss_scale_px"].get<double>();
        if (scale < 0.0) {
          throw Violation(
              "'bundle_adjuster.loss_scale_px' must be >= 0 (0 = auto, the "
              "D5 threshold of the v3 observation set); a negative robust "
              "loss scale is invalid (P8)");
        }
        config.bundle_adjuster.loss_scale_px = scale;
      }
      if (section.contains("max_num_iterations")) {
        const int iters = section["max_num_iterations"].get<int>();
        if (iters < 1) {
          throw Violation("'bundle_adjuster.max_num_iterations' must be >= 1");
        }
        config.bundle_adjuster.max_num_iterations = iters;
      }
      if (section.contains("refine_focal_length")) {
        config.bundle_adjuster.refine_focal_length =
            section["refine_focal_length"].get<bool>();
      }
      if (section.contains("refine_principal_point")) {
        config.bundle_adjuster.refine_principal_point =
            section["refine_principal_point"].get<bool>();
      }
      if (section.contains("refine_extra_params")) {
        config.bundle_adjuster.refine_extra_params =
            section["refine_extra_params"].get<bool>();
      }
      if (section.contains("refine_extrinsics")) {
        config.bundle_adjuster.refine_extrinsics =
            section["refine_extrinsics"].get<bool>();
      }
      if (section.contains("refine_intrinsics")) {
        config.bundle_adjuster.refine_intrinsics =
            section["refine_intrinsics"].get<bool>();
      }
      // FIXED-INTRINSICS invariant (D3/D-8c-3, P5): every intrinsics-refine
      // toggle is pinned OFF. An ON toggle is rejected, never silently pinned
      // down or forwarded.
      if (config.bundle_adjuster.refine_focal_length ||
          config.bundle_adjuster.refine_principal_point ||
          config.bundle_adjuster.refine_extra_params ||
          config.bundle_adjuster.refine_intrinsics) {
        throw Violation(
            "bundle_adjuster intrinsics refinement (refine_focal_length / "
            "refine_principal_point / refine_extra_params / "
            "refine_intrinsics) is out of scope for 8c: 8c keeps intrinsics "
            "FIXED (D3), so every intrinsics-refine toggle must be false "
            "(P5/D-8c-3)");
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

    // P9/D6 (8c): a bundle_adjuster run REQUIRES a pinned non-empty seed — a
    // bundle_adjuster plan without one is rejected here, at configuration
    // validation (the seam also fails closed defensively). The requirement
    // keys on the ENABLED stage (the config round-trips the full
    // bundle_adjuster section even for plans that don't run it, so section
    // presence alone would break the non-BA config round-trip).
    const bool runs_bundle_adjuster =
        std::find(config.enabled_stages.begin(), config.enabled_stages.end(),
                  "bundle_adjuster") != config.enabled_stages.end();
    if (runs_bundle_adjuster && config.seed.empty()) {
      throw Violation(
          "a bundle_adjuster plan requires a non-empty 'seed': 8c is "
          "deterministic only with a pinned random seed (D6/P9)");
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
  doc["bundle_adjuster"] = {
      {"loss_function", bundle_adjuster.loss_function},
      {"loss_scale_px", bundle_adjuster.loss_scale_px},
      {"max_num_iterations", bundle_adjuster.max_num_iterations},
      {"refine_focal_length", bundle_adjuster.refine_focal_length},
      {"refine_principal_point", bundle_adjuster.refine_principal_point},
      {"refine_extra_params", bundle_adjuster.refine_extra_params},
      {"refine_extrinsics", bundle_adjuster.refine_extrinsics},
      {"refine_intrinsics", bundle_adjuster.refine_intrinsics},
  };
  doc["enabled_stages"] = enabled_stages;
  return doc.dump();
}

std::vector<std::string> ColmapConfig::Plan() const {
  const std::vector<std::string> order = {
      ColmapStageName(ColmapStage::kFeatureExtractor),
      ColmapStageName(ColmapStage::kMatcher),
      ColmapStageName(ColmapStage::kMapper),
      ColmapStageName(ColmapStage::kBundleAdjuster),
  };
  if (enabled_stages.empty()) {
    // Pre-8c default plan stays the frozen three-stage chain; bundle_adjuster
    // runs only when explicitly requested (8c / P3-impl-8c P16).
    return {order[0], order[1], order[2]};
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
    case ColmapStage::kBundleAdjuster: {
      // Fixed-intrinsics pins are always 0 (D3/D-8c-3). The robust-loss SCALE
      // is data-derived (the D5 threshold of the v3 observation set, P8) and
      // therefore NOT part of this pure config transform: the seam appends
      // --BundleAdjustment.robust_loss_scale after computing it.
      const char* loss = "SOFT_L1";
      if (bundle_adjuster.loss_function == "Trivial") loss = "TRIVIAL";
      if (bundle_adjuster.loss_function == "Cauchy") loss = "CAUCHY";
      return {
          "--BundleAdjustment.max_num_iterations",
          std::to_string(bundle_adjuster.max_num_iterations),
          "--BundleAdjustment.robust_loss_function", loss,
          "--BundleAdjustment.refine_focal_length", "0",
          "--BundleAdjustment.refine_principal_point", "0",
          "--BundleAdjustment.refine_extra_params", "0",
          "--BundleAdjustment.refine_extrinsics",
          bundle_adjuster.refine_extrinsics ? "1" : "0",
          "--BundleAdjustment.refine_intrinsics", "0",
      };
    }
  }
  return {};
}

}  // namespace spatial::adapters::colmap
