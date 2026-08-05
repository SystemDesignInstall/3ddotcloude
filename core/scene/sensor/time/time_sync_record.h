#pragma once

// One synchronization sample between two clock domains (RFC-0002 §7.5).
// measured_at (in the source domain) tells when the offset was observed;
// uncertainty is the half-width of the offset's confidence interval,
// expressed in the source domain's nanoseconds. Zero uncertainty means the
// sample is exact (e.g. a hardware-disciplined PTP gate).

#include "core/coordinates/timestamp.h"
#include "core/scene/sensor/time/clock_domain.h"
#include "core/scene/sensor/time/time_offset.h"

namespace spatial::core {

struct TimeSyncRecord {
  ClockDomain source = ClockDomain::kSystem;
  ClockDomain target = ClockDomain::kSensor;
  TimestampNs measured_at;  // observation instant, in source domain
  TimeOffset offset;        // target = source + offset
  DurationNs uncertainty;   // >= 0
};

// Applies the recorded offset to a source-domain instant, yielding the
// target-domain instant. Range validity (age vs. measured_at, drift over
// time) is policy for the caller and not encoded here.
inline TimestampNs Resolve(const TimeSyncRecord& record,
                           TimestampNs source_time) noexcept {
  return record.offset.Apply(source_time);
}

}  // namespace spatial::core
