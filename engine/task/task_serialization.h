#pragma once

// JSON (de)serialization of the Task model. The scheduler persists each
// TaskInstance as spec_json in the `tasks` table (0003_scheduler.sql); the
// worker protocol carries the configuration as JSON as well. The format is
// canonical (stable key order) so that identical tasks produce identical
// bytes where it matters.

#include <string>

#include "engine/task/task_graph.h"
#include "engine/task/task_instance.h"

namespace spatial::engine {

std::string TaskToJson(const TaskInstance& task);
TaskInstance TaskFromJson(const std::string& json);

// Whole-graph (de)serialization for the CLI (`spatial run --dag`, RFC-0003
// AC-7). The document carries the job id, the declared external inputs (the
// type-match rule, task-model §2), and one entry per task with its edges:
//   { "job_id": ..., "external_inputs": [...],
//     "tasks": [ { "task": { ...TaskToJson... }, "dependencies": [...] } ] }
std::string TaskGraphToJson(const TaskGraph& graph,
                            const std::vector<ArtifactRef>& external_inputs);
// Restores the graph; `external_inputs` is the declared external input set.
TaskGraph TaskGraphFromJson(const std::string& json,
                            std::vector<ArtifactRef>& external_inputs);

std::string TaskStatusToString(TaskStatus status);
TaskStatus TaskStatusFromString(const std::string& name);

std::string CancellationPolicyToString(CancellationPolicy policy);
CancellationPolicy CancellationPolicyFromString(const std::string& name);

std::string CachePolicyToString(CachePolicy policy);
CachePolicy CachePolicyFromString(const std::string& name);

}  // namespace spatial::engine
