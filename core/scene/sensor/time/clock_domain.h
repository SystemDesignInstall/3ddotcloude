#pragma once

// Clock domains (RFC-0002 §7.5, docs/specifications/sensor-model.md §4).
// A TimestampNs value is meaningless without its declaring domain; every
// time-carrying record names the domain it ticks in.

#include <cstdint>

namespace spatial::core {

enum class ClockDomain : uint8_t {
  kSystem = 0,   // host system clock, local epoch
  kSensor = 1,   // an individual sensor's internal clock
  kGnss = 2,     // GNSS time (GPS/UTC-class reference)
  kPtp = 3,      // Precision Time Protocol network clock
  kExternal = 4, // vendor/cloud clock, no platform guarantees
};

inline const char* ClockDomainName(ClockDomain domain) noexcept {
  switch (domain) {
    case ClockDomain::kSystem:
      return "SYSTEM";
    case ClockDomain::kSensor:
      return "SENSOR";
    case ClockDomain::kGnss:
      return "GNSS";
    case ClockDomain::kPtp:
      return "PTP";
    case ClockDomain::kExternal:
      return "EXTERNAL";
    default:
      return "UNKNOWN";
  }
}

}  // namespace spatial::core
