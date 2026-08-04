#include "core/project/project.h"

#include <nlohmann/json.hpp>

#include "core/errors/project_error.h"
#include "core/utils/sha256.h"

namespace spatial::core {
namespace {

using json = nlohmann::json;

constexpr const char* kProjectJson = "project.json";
constexpr const char* kProjectDb = "project.db";
constexpr const char* kArtifactsDir = "artifacts";
constexpr const char* kCacheDir = "cache";
constexpr const char* kLogsDir = "logs";
constexpr const char* kTempDir = "temp";
constexpr const char* kLockFile = ".lock";

bool HasSpxExtension(const std::filesystem::path& p) {
  return p.extension() == ".spx";
}

}  // namespace

Project::Project(Project&&) noexcept = default;
Project& Project::operator=(Project&&) noexcept = default;

Project Project::Create(const std::filesystem::path& root,
                        const ProjectInfo& info) {
  if (!HasSpxExtension(root)) {
    throw StorageError(ErrorCode::kProjectInvalid,
                       "project root must carry the .spx extension: " +
                           root.string(),
                       {}, false,
                       "Name the project directory with a .spx extension.");
  }
  if (info.uuid == Uuid{}) {
    throw StorageError(ErrorCode::kProjectInvalid,
                       "project uuid must not be nil", {}, false,
                       "Supply a project uuid.");
  }
  if (fs::Exists(root)) {
    throw StorageError(ErrorCode::kProjectInvalid,
                       "project already exists: " + root.string(), {}, false,
                       "Choose a different project path.");
  }

  fs::CreateDirectories(root);
  fs::CreateDirectories(root / kArtifactsDir);
  fs::CreateDirectories(root / kCacheDir);
  fs::CreateDirectories(root / kLogsDir);
  fs::CreateDirectories(root / kTempDir);

  Project p;
  p.root_ = root;
  p.artifacts_root_ = root / kArtifactsDir;
  p.cache_root_ = root / kCacheDir;
  p.logs_root_ = root / kLogsDir;
  p.temp_root_ = root / kTempDir;
  p.db_path_ = root / kProjectDb;
  p.info_ = info;
  p.read_only_ = false;

  p.db_ = MetadataDb::Create(p.db_path_);
  p.artifacts_ = std::make_unique<ArtifactStore>(p.artifacts_root_, p.db_);
  p.WriteProjectJson();

  p.db_.InsertProject(info.uuid, info.name, info.schema_version,
                      json{{"app", info.created_by.app},
                           {"version", info.created_by.version},
                           {"git_commit", info.created_by.git_commit}}
                          .dump(),
                      fs::TimestampNsNow(), info.default_crs, info.root_frame,
                      json{{"read_only", info.read_only},
                           {"encrypted", info.encrypted}}
                          .dump(),
                      info.properties.dump());

  p.open_ = true;
  return p;
}

ProjectInfo Project::ReadProjectJson(const std::filesystem::path& root) {
  const auto path = root / kProjectJson;
  if (!fs::Exists(path)) {
    throw StorageError(ErrorCode::kProjectCorrupt,
                       "project.json not found: " + path.string(), {}, false,
                       "The project directory is corrupt or incomplete.");
  }
  json j = json::parse(fs::ReadText(path), nullptr, false);
  if (j.is_discarded() || !j.is_object()) {
    throw StorageError(ErrorCode::kProjectCorrupt,
                       "project.json is not valid JSON: " + path.string(), {},
                       false, "The project manifest is corrupt.");
  }
  ProjectInfo info;
  info.uuid = ParseUuid(j.at("uuid").get<std::string>());
  info.name = j.at("name").get<std::string>();
  info.schema_version = j.at("schema_version").get<std::int64_t>();
  if (j.contains("created_by")) {
    const auto& cb = j["created_by"];
    if (cb.contains("app")) info.created_by.app = cb["app"].get<std::string>();
    if (cb.contains("version")) {
      info.created_by.version = cb["version"].get<std::string>();
    }
    if (cb.contains("git_commit")) {
      info.created_by.git_commit = cb["git_commit"].get<std::string>();
    }
  }
  if (j.contains("created_at")) info.created_at = j["created_at"].get<std::string>();
  if (j.contains("default_crs")) {
    info.default_crs = j["default_crs"].get<std::string>();
  }
  if (j.contains("root_frame")) info.root_frame = j["root_frame"].get<std::string>();
  if (j.contains("flags")) {
    const auto& flags = j["flags"];
    if (flags.contains("read_only")) {
      info.read_only = flags["read_only"].get<bool>();
    }
    if (flags.contains("encrypted")) {
      info.encrypted = flags["encrypted"].get<bool>();
    }
  }
  if (j.contains("properties")) info.properties = j["properties"];
  return info;
}

void Project::WriteProjectJson() const {
  json j;
  j["uuid"] = FormatUuid(info_.uuid);
  j["name"] = info_.name;
  j["schema_version"] = info_.schema_version;
  j["created_by"]["app"] = info_.created_by.app;
  j["created_by"]["version"] = info_.created_by.version;
  j["created_by"]["git_commit"] = info_.created_by.git_commit;
  j["created_at"] =
      info_.created_at.empty() ? fs::Iso8601UtcNow() : info_.created_at;
  j["default_crs"] = info_.default_crs;
  j["root_frame"] = info_.root_frame;
  j["flags"]["read_only"] = info_.read_only;
  j["flags"]["encrypted"] = info_.encrypted;
  j["properties"] = info_.properties;
  fs::AtomicWrite(root_ / kProjectJson, j.dump(2));
}

Project Project::Open(const std::filesystem::path& root, bool read_only) {
  if (!fs::Exists(root)) {
    throw StorageError(ErrorCode::kProjectNotFound,
                       "project not found: " + root.string(), {}, false,
                       "Check the project path.");
  }
  ProjectInfo info = ReadProjectJson(root);

  Project p;
  p.root_ = root;
  p.artifacts_root_ = root / kArtifactsDir;
  p.cache_root_ = root / kCacheDir;
  p.logs_root_ = root / kLogsDir;
  p.temp_root_ = root / kTempDir;
  p.db_path_ = root / kProjectDb;
  p.info_ = info;
  p.read_only_ = read_only || info.read_only;

  for (const auto& dir : {kArtifactsDir, kCacheDir, kLogsDir, kTempDir}) {
    if (!fs::Exists(root / dir)) {
      throw StorageError(ErrorCode::kProjectCorrupt,
                         "project layout is incomplete (missing " +
                             std::string(dir) + "): " + root.string(),
                         {}, false,
                         "The project directory is corrupt or was not created "
                         "by this platform.");
    }
  }

  if (!p.read_only_) {
    p.lock_ = std::make_unique<FileLock>(root / kLockFile);
  }

  p.db_ = p.read_only_ ? MetadataDb::OpenReadOnly(p.db_path_)
                       : MetadataDb::Open(p.db_path_);
  p.artifacts_ = std::make_unique<ArtifactStore>(p.artifacts_root_, p.db_);
  p.open_ = true;
  return p;
}

void Project::Save() {
  if (read_only_) {
    throw StorageError(ErrorCode::kStorageReadOnly,
                       "cannot save a read-only project", {}, false,
                       "Open the project for writing to modify it.");
  }
  if (!open_) {
    throw StorageError(ErrorCode::kProjectNotFound,
                       "project is not open", {}, false,
                       "Open the project first.");
  }
  WriteProjectJson();
}

void Project::VerifyIntegrity() const {
  for (const auto& shard : fs::ListDirectories(artifacts_root_ / "cas")) {
    for (const auto& file : fs::ListFiles(shard)) {
      const std::string hash = file.filename().string();
      const auto bytes = fs::ReadFile(file);
      const std::string actual = Sha256Hex(bytes);
      if (actual != hash) {
        throw ArtifactError(
            ErrorCode::kArtifactHashMismatch,
            "integrity check failed for payload: " + hash, {},
            false, "The artifact is corrupt; run repair or re-import.");
      }
    }
  }
}

void Project::Close() {
  db_.Close();
  lock_.reset();
  artifacts_.reset();
  open_ = false;
}

}  // namespace spatial::core
