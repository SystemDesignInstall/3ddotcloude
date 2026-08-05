#include <gtest/gtest.h>

#include <type_traits>

#include "core/coordinates/angle.h"
#include "core/coordinates/distance.h"
#include "core/coordinates/timestamp.h"

namespace spatial::core {
namespace {

TEST(Coordinates, DistanceIsStronglyTyped) {
  static_assert(!std::is_convertible_v<double, DistanceMeters>);
  static_assert(!std::is_convertible_v<DistanceMeters, double>);
  static_assert(std::is_same_v<decltype(DistanceMeters(1.0) + DistanceMeters(2.0)),
                               DistanceMeters>);
  static_assert(std::is_same_v<decltype(DistanceMeters(2.0) * 3.0),
                               DistanceMeters>);
}

TEST(Coordinates, DistanceArithmetic) {
  const DistanceMeters a = 2.0_m;
  const DistanceMeters b = 3.0_m;
  EXPECT_DOUBLE_EQ((a + b).value(), 5.0);
  EXPECT_DOUBLE_EQ((b - a).value(), 1.0);
  EXPECT_DOUBLE_EQ((-a).value(), -2.0);
  EXPECT_DOUBLE_EQ((a * 4.0).value(), 8.0);
  EXPECT_DOUBLE_EQ((4.0 * a).value(), 8.0);
  EXPECT_TRUE(3.0_m == 3.0_m);
  EXPECT_TRUE(2.0_m < 3.0_m);
  EXPECT_TRUE(3.0_m > 2.0_m);
  EXPECT_TRUE(2.0_m <= 2.0_m);
}

TEST(Coordinates, AngleArithmetic) {
  const AngleRadians pi = 3.141592653589793_rad;
  const AngleRadians half = 1.5707963267948966_rad;
  EXPECT_DOUBLE_EQ((half + half).value(), pi.value());
  EXPECT_DOUBLE_EQ((pi - half).value(), half.value());
  EXPECT_DOUBLE_EQ((-half).value(), -half.value());
  static_assert(!std::is_convertible_v<double, AngleRadians>);
}

TEST(Coordinates, TimestampDifferenceIsDuration) {
  static_assert(std::is_same_v<decltype(TimestampNs(5) - TimestampNs(2)),
                               DurationNs>);
  static_assert(std::is_same_v<decltype(TimestampNs(5) + 1_ns), TimestampNs>);
}

TEST(Coordinates, TimestampArithmetic) {
  const TimestampNs t0(1'000);
  const DurationNs delta = 5_ms;
  EXPECT_EQ(t0 + delta, TimestampNs(1'000 + 5'000'000));
  EXPECT_EQ(t0 - 2_us, TimestampNs(1'000 - 2'000));
  EXPECT_EQ(TimestampNs(7) - TimestampNs(3), 4_ns);
  EXPECT_EQ(TimestampNs(10) + 3_ns, TimestampNs(13));
  EXPECT_EQ(3_ns + TimestampNs(10), TimestampNs(13));
  EXPECT_TRUE(TimestampNs(1) < TimestampNs(2));
  EXPECT_TRUE(2_ns > 1_ns);
}

TEST(Coordinates, DurationLiterals) {
  EXPECT_EQ(1_s, 1000_ms);
  EXPECT_EQ(1_ms, 1000_us);
  EXPECT_EQ(1_us, 1000_ns);
  EXPECT_EQ((-2_ms).value(), -2'000'000);
}

TEST(Coordinates, RoundTripThroughOffset) {
  const TimestampNs original(123'456'789);
  const DurationNs offset = 17_ms;
  const TimestampNs shifted = original + offset;
  EXPECT_EQ(shifted - original, offset);
  EXPECT_EQ(original - shifted, -offset);
}

}  // namespace
}  // namespace spatial::core
