#pragma once

// Canonical Loop Closure domain types (P3).
// Backend-independent representation of loop closure detection and verification.
// Every loop closure backend produces/consumes these types through its adapter.
// The types carry no backend-specific fields.
//
// Normative decisions: D-LC-01 through D-LC-09 (P3-trajectory-pose-graph-loop-closure.md).

#include <cstdint>
#include <string>

#include "core/reconstruction/reconstruction.h"

namespace spatial::core {

// --- Loop Closure Candidate (D-LC-02, D-LC-03) ---
// A potential match between two frames, before geometric verification.
// Candidates are lightweight and may be numerous. Persisted for audit.

struct LoopClosureCandidate {
  std::string candidate_id;                // UUIDv4
  std::string trajectory_id;
  std::string source_frame_id;             // the newer frame
  std::string target_frame_id;             // the older frame (the "revisited" location)
  double feature_match_score = 0.0;        // raw matching score [0, infinity), backend-specific
  std::string matcher;                     // which matcher produced this (e.g. "bow", "netvlad")
  std::int64_t created_at_ns = 0;
  bool operator==(const LoopClosureCandidate&) const = default;
};

// --- Loop Closure (D-LC-05, D-LC-06) ---
// An accepted or rejected loop closure — a verified pose constraint.
// Both accepted and rejected closures are persisted for audit.

struct LoopClosure {
  std::string closure_id;                  // UUIDv4
  std::string trajectory_id;
  std::string candidate_id;                // back-reference to the candidate
  std::string source_frame_id;
  std::string target_frame_id;
  std::string status;                      // "accepted" | "rejected" (D-LC-05)
  double inlier_ratio = 0.0;               // fraction of geometrically consistent matches
  std::int64_t inlier_count = 0;           // absolute number of inliers
  double confidence = 0.0;                 // combined confidence [0, 1]
  std::int64_t temporal_separation_ns = 0; // |timestamp_source - timestamp_target| (D-LC-06)
  double spatial_separation_m = 0.0;       // Euclidean distance between original poses (D-LC-06)
  std::int64_t created_at_ns = 0;
  ReconstructionProvenance provenance;
  bool operator==(const LoopClosure&) const = default;
};

}  // namespace spatial::core
