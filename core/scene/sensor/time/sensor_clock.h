#pragma once

// A clock as seen at a sensor (RFC-0002 §7.5): which domain it ticks in,
// its frequency error relative to the nominal domain rate (ppm), and its
// absolute offset from the domain epoch. All values are engineering
// parameters, not trust statements; use TimeSyncRecord for observed sync.

#include <cstdint>
#include <string>

#include "core/coordinates/timestamp.h"
#include "core/scene/sensor/time/clock_domain.h"

namespace spatial::core {

struct SensorClock {
  ClockDomain domain = ClockDomain::kSensor;
  double drift_ppm = 0.0;            // frequency error, parts per million
  DurationNs offset = DurationNs(0); // clock_reading = nominal + offset
  std::string model;                 // free-form, e.g. "GNSS-disciplined OCXO"
};

// Maps a raw sensor clock reading into the domain's nominal time scale.
inline TimestampNs ToNominalTime(const SensorClock& clock,
                                 TimestampNs reading) noexcept {
  return reading - clock.offset;
}

}  // namespace spatial::core
