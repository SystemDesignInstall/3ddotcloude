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
#include <optional>
#include <string>
#include <vector>

namespace spatial::adapters::colmap {

// The ordered COLMAP stage vocabulary of the C1 plan (§3.2: one progress
// substage per CLI tool). Default plan = feature_extractor -> matcher ->
// mapper. bundle_adjuster (P3-impl-8c) is a 4th vocabulary stage reached only
// when explicitly listed in enabled_stages (the pre-8c default plan is
// unchanged: the frozen three-stage chain).
enum class ColmapStage : int {
  kFeatureExtractor = 0,
  kMatcher = 1,
  kMapper = 2,
  kBundleAdjuster = 3,
};

// Canonical lowercase stage name (also the plan step and CLI subcommand).
const char* ColmapStageName(ColmapStage stage) noexcept;

// Reverse of ColmapStageName; nullopt for a name outside the stage
// vocabulary. Used by the executor to validate plan steps.
std::optional<ColmapStage> ColmapStageFromName(const std::string& name) noexcept;

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

// bundle_adjuster stage options (P3-impl-8c, P9/P16). FIXED-INTRINSICS policy
// (D3/D-8c-3): every intrinsics-refine toggle is pinned OFF here and rejected
// by FromJson validation if a caller ever sets one ON; `refine_extrinsics` is
// the only pose/geometry DOF and defaults to true. `loss_scale_px == 0` means
// "auto": the seam computes the D5 threshold (max(3*median, 2 px)) from the
// v3 observation set and pins that as the robust-loss scale (P8/D-8c-4); a
// negative value fails closed.
struct BundleAdjusterOptions {
  std::string loss_function = "SoftL1";  // {SoftL1, Trivial, Cauchy}
  double loss_scale_px = 0.0;            // 0 = auto (D5 threshold); < 0 rejected
  int max_num_iterations = 100;          // >= 1
  bool refine_focal_length = false;      // pinned OFF (D3)
  bool refine_principal_point = false;   // pinned OFF (D3)
  bool refine_extra_params = false;      // pinned OFF (D3)
  bool refine_extrinsics = true;
  bool refine_intrinsics = false;        // pinned OFF (D3)

  bool operator==(const BundleAdjusterOptions&) const = default;
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
  BundleAdjusterOptions bundle_adjuster;

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
