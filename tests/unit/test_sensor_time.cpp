#include <gtest/gtest.h>

#include "core/coordinates/timestamp.h"
#include "core/scene/sensor/time/clock_domain.h"
#include "core/scene/sensor/time/latency_model.h"
#include "core/scene/sensor/time/sensor_clock.h"
#include "core/scene/sensor/time/time_offset.h"
#include "core/scene/sensor/time/time_sync_record.h"

namespace spatial::core {
namespace {

TEST(SensorTime, TimeOffsetAppliesSignedly) {
  const TimeOffset offset(17_ms);
  const TimestampNs source(1'000);
  EXPECT_EQ(offset.Apply(source), TimestampNs(1'000 + 17'000'000));
  EXPECT_EQ((-offset).Apply(source), TimestampNs(1'000 - 17'000'000));
  EXPECT_EQ(offset + offset, TimeOffset(34_ms));
}

TEST(SensorTime, ClockReadingRoundTrip) {
  const SensorClock clock{ClockDomain::kSensor, 12.0, 5_ms, "test"};
  const TimestampNs reading(9'999);
  const TimestampNs nominal = ToNominalTime(clock, reading);
  EXPECT_EQ(nominal, TimestampNs(9'999 - 5'000'000));
  EXPECT_EQ(nominal + clock.offset, reading);
}

TEST(SensorTime, SyncRecordResolvesOffset) {
  const TimeSyncRecord record{ClockDomain::kSystem, ClockDomain::kSensor,
                              TimestampNs(0), TimeOffset(10_ms), 2_ms};
  const TimestampNs source(100);
  EXPECT_EQ(Resolve(record, source), TimestampNs(100 + 10'000'000));

  // Uncertainty does not shift the resolved instant; it qualifies it.
  const TimeSyncRecord exact{record.source, record.target, record.measured_at,
                             record.offset, 0_ns};
  EXPECT_EQ(Resolve(exact, source), Resolve(record, source));
}

TEST(SensorTime, SyncRecordUncertaintyIsNonNegativeCarriedValue) {
  const TimeSyncRecord record{ClockDomain::kGnss, ClockDomain::kSensor,
                              TimestampNs(0), TimeOffset(1_ms), 500_us};
  EXPECT_GE(record.uncertainty.value(), 0);
  EXPECT_NE(record.uncertainty, 0_ns);
}

TEST(SensorTime, LatencyModelCorrectsEventTime) {
  const LatencyModel model{ClockDomain::kSensor, 20_ms, 1.5, "constant"};
  const TimestampNs sample(5'000);
  EXPECT_EQ(CorrectEventTime(model, sample), TimestampNs(5'000 - 20'000'000));
}

TEST(SensorTime, ClockDomainNames) {
  EXPECT_STREQ(ClockDomainName(ClockDomain::kSystem), "SYSTEM");
  EXPECT_STREQ(ClockDomainName(ClockDomain::kSensor), "SENSOR");
  EXPECT_STREQ(ClockDomainName(ClockDomain::kGnss), "GNSS");
  EXPECT_STREQ(ClockDomainName(ClockDomain::kPtp), "PTP");
  EXPECT_STREQ(ClockDomainName(ClockDomain::kExternal), "EXTERNAL");
}

}  // namespace
}  // namespace spatial::core
