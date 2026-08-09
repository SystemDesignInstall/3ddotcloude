#pragma once

// Deterministic identity derivation for scene entities (RFC-0006 §6.3,
// image-import.md §6). FrameID/ObservationID are UUIDv5 over a canonical name
// derived from content and capture context:
//
//   name := <prefix> "|" <sensor_id> "|" <timestamp_ns> "|" <content_hash>
//
// The same (sensor, capture time, content) tuple maps to the same IDs across
// re-imports, machines, and processes (PPS-0001 §5.4, §6.3). The platform
// namespace UUID is reserved here and is identical across runs/installations.

#include <cstdint>
#include <string>

#include "core/utils/uuid.h"

namespace spatial::core {

// Reserved platform namespace UUID for v5 identity. Fixed for the lifetime of
// the platform; never reused for another purpose.
inline const Uuid& PlatformIdentityNamespace() {
  static const Uuid kNamespace =
      ParseUuid("1a2b3c4d-5e6f-4a8b-9c0d-1e2f3a4b5c6d");
  return kNamespace;
}

// Canonical v5 name for an entity (prefix "frame" or "observation").
inline std::string EntityIdentityName(const std::string& prefix,
                                      const Uuid& sensor_id,
                                      std::int64_t timestamp_ns,
                                      const std::string& content_hash) {
  return prefix + "|" + FormatUuid(sensor_id) + "|" +
         std::to_string(timestamp_ns) + "|" + content_hash;
}

// FrameID = UUIDv5(ns, "frame|<sensor_id>|<timestamp_ns>|<content_hash>").
inline Uuid DeriveFrameId(const Uuid& sensor_id, std::int64_t timestamp_ns,
                          const std::string& content_hash) {
  return GenerateUuidV5(PlatformIdentityNamespace(),
                        EntityIdentityName("frame", sensor_id, timestamp_ns,
                                           content_hash));
}

// ObservationID = UUIDv5(ns, "observation|<sensor_id>|<timestamp_ns>|<content_hash>").
inline Uuid DeriveObservationId(const Uuid& sensor_id,
                                std::int64_t timestamp_ns,
                                const std::string& content_hash) {
  return GenerateUuidV5(PlatformIdentityNamespace(),
                        EntityIdentityName("observation", sensor_id,
                                           timestamp_ns, content_hash));
}

}  // namespace spatial::core
