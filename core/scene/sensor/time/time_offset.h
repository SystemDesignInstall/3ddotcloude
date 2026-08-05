#pragma once

// Signed offset between two clock domains (RFC-0002 §7.5):
//   target_time = source_time + offset.
// Positive when the target clock leads the source clock.

#include <compare>

#include "core/coordinates/timestamp.h"

namespace spatial::core {

class TimeOffset {
 public:
  explicit constexpr TimeOffset(DurationNs offset) noexcept : offset_(offset) {}

  constexpr DurationNs value() const noexcept { return offset_; }

  constexpr TimestampNs Apply(TimestampNs source) const noexcept {
    return source + offset_;
  }

  constexpr TimeOffset operator-() const noexcept {
    return TimeOffset(-offset_);
  }

  friend constexpr TimeOffset operator+(TimeOffset a, TimeOffset b) noexcept {
    return TimeOffset(a.offset_ + b.offset_);
  }

  friend constexpr TimeOffset operator-(TimeOffset a, TimeOffset b) noexcept {
    return TimeOffset(a.offset_ - b.offset_);
  }

  friend constexpr bool operator==(TimeOffset a, TimeOffset b) noexcept {
    return a.offset_ == b.offset_;
  }

  friend constexpr bool operator!=(TimeOffset a, TimeOffset b) noexcept {
    return !(a == b);
  }

  friend constexpr auto operator<=>(TimeOffset a, TimeOffset b) noexcept {
    return a.offset_ <=> b.offset_;
  }

 private:
  DurationNs offset_;
};

}  // namespace spatial::core
