#pragma once

// COLMAP adapter configuration model (RFC-0008 §9; C1 plan §1.1
// colmap_config.h). Declarative per-stage algorithm settings (detector /
// descriptor, matcher thresholds, mapper options) that join the config_json
// surface of the feature_extraction / sparse_reconstruction /
// bundle_adjustment stages.
//
// The configuration is algorithm settings ONLY (RFC-0009 §6): a calibration
// value (fx / fy / cx / cy / distortion / ...) in the configuration surface is
// a contract violation rejected by validation, and spatial data always travels
// in TaskRequest.input_refs as CAS content hashes. This invariant is
// machine-checked here (test_config_rejection contract).
//
// This file is pure configuration marshaling. It never executes COLMAP, never
// builds the executable/subcommand argv (that is the CLI wrapper's concern in
// the worker increment), and carries no COLMAP-native type — COLMAP types stay
// below the adapter boundary (RFC-0008 §17).

#include <cstdint>
#include <string>
#include <vector>

namespace spatial::adapters::colmap {

// The ordered COLMAP stage vocabulary of the C1 plan (§3.2: one progress
// substage per CLI tool). Default plan = feature_extractor -> matcher ->
// mapper.
enum class ColmapStage : int {
  kFeatureExtractor = 0,
  kMatcher = 1,
  kMapper = 2,
};

// Canonical lowercase stage name (also the plan step and CLI subcommand).
const char* ColmapStageName(ColmapStage stage) noexcept;

// Feature extraction (feature_extraction capability): detector/descriptor
// settings (RFC-0008 §9). Algorithm settings only.
struct FeatureExtractorOptions {
  int max_image_size = 3200;
  int max_num_features = 8192;
  std::string detector = "SIFT";
  std::string descriptor = "SIFT";
  std::string sift_scale_space_octaves = "auto";
  std::string sift_domain_size_pooling = "auto";

  bool operator==(const FeatureExtractorOptions&) const = default;
};

// Matching (matcher): SIFT matching thresholds.
struct MatcherOptions {
  bool guided_matching = true;
  double max_ratio = 0.8;
  double max_distance = 0.7;
  bool cross_check = true;

  bool operator==(const MatcherOptions&) const = default;
};

// Mapping / bundle adjustment (sparse_reconstruction + bundle_adjustment).
struct MapperOptions {
  int min_num_matches = 15;
  bool ba_refine_principal_point = false;
  int ba_min_num_residuals_for_multithreading = 50000;

  bool operator==(const MapperOptions&) const = default;
};

// Effective configuration of one COLMAP task (RFC-0008 §9). The same
// settings that join config_json; Sha256Hex(ToJson()) is the ADR-020
// configuration hash (algorithm settings only — calibration is an input, not
// configuration, RFC-0009 §6).
struct ColmapConfig {
  // Determinism pins (RFC-0008 §8): equal config must yield equal outputs.
  int threads = -1;    // -1 = use all logical cores
  std::string seed;    // empty = platform default; set to pin COLMAP's RNG

  FeatureExtractorOptions feature_extractor;
  MatcherOptions matcher;
  MapperOptions mapper;

  // Ordered subset of the stage vocabulary to run. Empty = the default plan
  // {feature_extractor, matcher, mapper}.
  std::vector<std::string> enabled_stages;

  bool operator==(const ColmapConfig&) const = default;

  static ColmapConfig Default();

  // Parse the effective stage configuration. An empty / whitespace-only
  // document is the default configuration (a task with no config still gets a
  // deterministic default plan). Throws spatial::core::ValidationError
  // (ErrorCode::kValidationDomain, ADR-014) on:
  //   - malformed JSON or a wrong-typed value,
  //   - an unknown key or an unknown / duplicated stage name,
  //   - any calibration vocabulary in the configuration surface
  //     (fx / fy / cx / cy / distortion / intrinsics / extrinsics /
  //     calibration, anywhere in the document) — a contract violation
  //     (RFC-0009 §6).
  static ColmapConfig FromJson(const std::string& config_json);

  // Serialize back to the canonical config_json form (round-trip; used by
  // provenance and the ADR-020 configuration hash). Deterministic key order.
  std::string ToJson() const;

  // Ordered plan of enabled COLMAP stages, canonical dependency order
  // (C1 plan §3.2 substages).
  std::vector<std::string> Plan() const;

  // Marshal one stage's algorithm options to COLMAP CLI-style tokens
  // (e.g. {"--SiftExtraction.max_image_size", "3200"}). A pure data
  // transform: no executable, no subcommand, no workspace paths.
  std::vector<std::string> BuildStageArgs(ColmapStage stage) const;
};

}  // namespace spatial::adapters::colmap
