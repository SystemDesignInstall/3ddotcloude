#pragma once

// Canonical PoseGraph JSON serialization (P3-Production-E2E §3.3).
// Single source of truth for PoseGraph -> CAS document round-trip.
// Conforms to schemas/json/pose-graph.schema.json (schema_version 1).
// Deterministic field order and nlohmann number formatting (ADR-020).

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/errors/project_error.h"
#include "core/trajectory/pose_graph.h"

namespace spatial::core {

inline std::string PoseGraphToJson(const PoseGraph& graph,
                                   const std::vector<PoseGraphNode>& nodes,
                                   const std::vector<PoseGraphEdge>& edges) {
  nlohmann::json doc;
  doc["schema_version"] = 1;
  doc["graph_id"] = graph.graph_id;
  doc["trajectory_id"] = graph.trajectory_id;

  nlohmann::json jnodes = nlohmann::json::array();
  for (const auto& n : nodes) {
    jnodes.push_back({{"node_id", n.node_id},
                      {"frame_id", n.frame_id},
                      {"timestamp_ns", n.timestamp_ns}});
  }
  doc["nodes"] = std::move(jnodes);

  nlohmann::json jedges = nlohmann::json::array();
  for (const auto& e : edges) {
    nlohmann::json jedge;
    jedge["edge_id"] = e.edge_id;
    jedge["type"] = e.type;
    jedge["source_node_id"] = e.source_node_id;
    jedge["target_node_id"] = e.target_node_id;
    jedge["relative_position_xyz"] = {e.relative_position_xyz[0],
                                      e.relative_position_xyz[1],
                                      e.relative_position_xyz[2]};
    jedge["relative_rotation_xyzw"] = {e.relative_rotation_xyzw[0],
                                       e.relative_rotation_xyzw[1],
                                       e.relative_rotation_xyzw[2],
                                       e.relative_rotation_xyzw[3]};
    std::vector<double> info6(e.information_matrix_6x6.begin(),
                              e.information_matrix_6x6.end());
    jedge["information_matrix_6x6"] = std::move(info6);
    jedge["confidence"] = e.confidence;
    if (!e.source.empty()) jedge["source"] = e.source;
    if (!e.configuration_hash.empty())
      jedge["configuration_hash"] = e.configuration_hash;
    jedges.push_back(std::move(jedge));
  }
  doc["edges"] = std::move(jedges);

  return doc.dump();
}

}  // namespace spatial::core
