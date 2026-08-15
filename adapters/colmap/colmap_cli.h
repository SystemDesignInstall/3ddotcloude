#pragma once

// COLMAP CLI orchestration wrapper (C1 plan §1.1 colmap_cli.h; RFC-0008 §9).
// Thin, pure CLI code: builds argv, prepares the per-task workspace layout,
// and discovers produced outputs. No domain types and no COLMAP-native
// format reading (that is colmap_converter's concern, a later increment).
//
// The only boundary rule this file must never violate: argv references local
// workspace paths only — a CAS content hash is never passed as a path
// (RFC-0009 §6; worker-protocol §4 workspace isolation). Inputs arrive as
// hashes, are materialized into the workspace by the adapter, and the CLI
// sees the materialized files.

#include <filesystem>
#include <string>
#include <vector>

#include "adapters/colmap/colmap_config.h"

namespace spatial::adapters::colmap {

// The COLMAP CLI subcommand for each stage (plan §1.1: feature_extractor,
// matcher, mapper; the general matcher is exhaustive_matcher). Pure data.
std::string StageSubcommand(ColmapStage stage);

// Deterministic per-task workspace layout (plan §4):
//   <workspace>/{images/, features/, matches/, sparse/, logs/}
// plus an empty `database.db` (COLMAP's private SQLite file — it is the
// task's isolated workspace, never a host database handle, ADR-038).
// Idempotent.
void CreateWorkspace(const std::filesystem::path& workspace);

// Full argv for one stage, e.g.:
//   {<exe>, feature_extractor, --database_path <ws>/database.db,
//    --image_path <ws>/images, --SiftExtraction.*, --threads N,
//    --random_seed S}
// Path flags use only local workspace paths; stage algorithm options come
// from BuildStageArgs; threads/seed are the top-level determinism pins.
std::vector<std::string> BuildStageCommand(
    const std::string& executable, ColmapStage stage,
    const std::filesystem::path& workspace, const ColmapConfig& config);

// The mapper's model output directory: <ws>/sparse/0/ mirrors real COLMAP's
// sparse/<n>/ reconstruction directories (the mapper's --output_path is
// <ws>/sparse). C1-S4 emits the first reconstruction only.
std::filesystem::path SparseModelDir(
    const std::filesystem::path& workspace);

// Discovers a complete native COLMAP model in SparseModelDir (C1-S4): the
// three files cameras.bin / images.bin / points3D.bin in a deterministic
// order. Returns {} when no model directory exists; throws AdapterError when
// the directory is only partially written (fail-closed — never convert a
// half-written reconstruction).
std::vector<std::filesystem::path> DiscoverNativeModelFiles(
    const std::filesystem::path& workspace);

}  // namespace spatial::adapters::colmap
