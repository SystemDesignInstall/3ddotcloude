#pragma once

// Canonical OptimizationResult JSON serialization (P3-Production-E2E §3.3).
// Single source of truth for OptimizationResult -> CAS document round-trip.
// Conforms to schemas/json/optimization-result.schema.json (schema_version 1).
// Deterministic field order and nlohmann number formatting (ADR-020).

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/trajectory/optimization.h"

namespace spatial::core {

inline std::string OptimizationResultToJson(
    const OptimizationResult& result,
    const std::vector<OptimizedPoseNode>& optimized_nodes) {
  nlohmann::json doc;
  doc["schema_version"] = 1;
  doc["result_id"] = result.result_id;
  doc["graph_id"] = result.graph_id;
  doc["trajectory_id"] = result.trajectory_id;
  doc["status"] = result.status;
  doc["iterations"] = result.iterations;
  doc["initial_error"] = result.initial_error;
  doc["final_error"] = result.final_error;
  doc["error_reduction"] = result.error_reduction;
  doc["created_at_ns"] = result.created_at_ns;

  nlohmann::json prov;
  nlohmann::json opt_info;
  opt_info["name"] = result.provenance.optimizer.name;
  opt_info["version"] = result.provenance.optimizer.version;
  prov["optimizer"] = std::move(opt_info);
  prov["configuration_hash"] = result.provenance.configuration_hash;
  nlohmann::json iah = nlohmann::json::array();
  for (const auto& h : result.provenance.input_artifact_hashes)
    iah.push_back(h);
  prov["input_artifact_hashes"] = std::move(iah);
  prov["adapter_version"] = result.provenance.adapter_version;
  if (!result.provenance.git_commit.empty())
    prov["git_commit"] = result.provenance.git_commit;
  if (!result.provenance.backend_specific_json.empty())
    prov["backend_specific_json"] = result.provenance.backend_specific_json;
  doc["provenance"] = std::move(prov);

  nlohmann::json jnodes = nlohmann::json::array();
  for (const auto& n : optimized_nodes) {
    nlohmann::json jn;
    jn["frame_id"] = n.frame_id;
    jn["timestamp_ns"] = n.timestamp_ns;
    jn["sequence_index"] = n.sequence_index;
    jn["position_xyz"] = {n.position_xyz[0], n.position_xyz[1],
                          n.position_xyz[2]};
    jn["rotation_xyzw"] = {n.rotation_xyzw[0], n.rotation_xyzw[1],
                           n.rotation_xyzw[2], n.rotation_xyzw[3]};
    {
      std::vector<double> covp(n.covariance_position.begin(),
                               n.covariance_position.end());
      jn["covariance_position"] = std::move(covp);
    }
    {
      std::vector<double> covr(n.covariance_rotation.begin(),
                               n.covariance_rotation.end());
      jn["covariance_rotation"] = std::move(covr);
    }
    jnodes.push_back(std::move(jn));
  }
  doc["nodes"] = std::move(jnodes);

  return doc.dump();
}

}  // namespace spatial::core
