#pragma once

// Coordinate authority of a datum/epoch tie (RFC-0002 §7.3). Declares how a
// reference was established and how much it is trusted, so consumers can rank
// RTK fixes against surveyed control without reading narrative text.

#include <cstdint>
#include <string>

namespace spatial::core {

enum class AuthorityType : uint8_t {
  kLocal = 0,     // project-local arbitrary origin
  kSurvey = 1,    // total-station / manual surveyed control
  kRtk = 2,       // real-time kinematic GNSS fix
  kPpp = 3,       // precise point positioning
  kGnss = 4,      // absolute GNSS positioning (non-RTK)
  kImported = 5,  // taken verbatim from an external project / file
};

inline const char* AuthorityTypeName(AuthorityType type) noexcept {
  switch (type) {
    case AuthorityType::kLocal:
      return "LOCAL";
    case AuthorityType::kSurvey:
      return "SURVEY";
    case AuthorityType::kRtk:
      return "RTK";
    case AuthorityType::kPpp:
      return "PPP";
    case AuthorityType::kGnss:
      return "GNSS";
    case AuthorityType::kImported:
      return "IMPORTED";
    default:
      return "UNKNOWN";
  }
}

// Confidence is in [0, 1]: 1.0 for surveyed control, lower for RTK float
// fixes, and so on. The numeric accuracy is carried separately as a distance.
struct CoordinateAuthority {
  AuthorityType type = AuthorityType::kLocal;
  std::string provider;   // e.g. "EMLID_REACH_RS2", "NTRIP:CHIP", "TOTAL_STATION"
  double confidence = 1.0;
  std::string reference;  // e.g. "IGS14", "EPSG:4326", survey mark id
};

}  // namespace spatial::core
