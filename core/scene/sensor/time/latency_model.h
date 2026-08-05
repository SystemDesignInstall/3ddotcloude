#pragma once

// Latency from a physical event to its timestamped sample (RFC-0002 §7.5,
// docs/specifications/sensor-model.md §4). fixed_latency is the constant
// pipeline delay; jitter is the observed spread (one-sigma or peak, per
// method) in nanoseconds. method is free-form so empirical models can be
// attached without schema changes.

#include <string>

#include "core/coordinates/timestamp.h"
#include "core/scene/sensor/time/clock_domain.h"

namespace spatial::core {

struct LatencyModel {
  ClockDomain domain = ClockDomain::kSensor;
  DurationNs fixed_latency;
  double jitter = 0.0;   // ns-equivalent, >= 0
  std::string method;    // e.g. "constant", "gaussian", "empirical"
};

// Best-effort estimate of the physical event time behind a sample that was
// stamped after passing through the modeled pipeline.
inline TimestampNs CorrectEventTime(const LatencyModel& model,
                                    TimestampNs sample_time) noexcept {
  return sample_time - model.fixed_latency;
}

}  // namespace spatial::core
