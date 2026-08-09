#pragma once

// CaptureSession — groups everything captured in one pass by one device
// (RFC-0002 §6.3, image-import.md §7). Every Observation and Frame must
// reference a CaptureSession (PPS-0001 §5.2). Session identity is instance
// identity (random UUIDv4), not content identity.

#include <cstdint>
#include <string>

#include "core/coordinates/timestamp.h"
#include "core/utils/uuid.h"

namespace spatial::core {

struct CaptureSession {
  Uuid session_id{};             // random UUIDv4, assigned at batch creation
  Uuid project_id{};             // owning project
  std::string name;              // batch name, e.g. "2026-08-09 scan"
  std::string source_uri;        // original location (portable URI, ADR-008)
  TimestampNs started_at{0};
  TimestampNs ended_at{0};       // may be nil while open
  std::string status = "open";   // open | closed | discarded
  std::string provenance_json;   // ImportManifest provenance (RFC-0002 §6.7)
};

}  // namespace spatial::core
