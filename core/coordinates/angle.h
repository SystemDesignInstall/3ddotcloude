#pragma once

// Strong angle type (ADR-018, RFC-0002 §7). Angles are stored in radians;
// degrees exist only at adapter boundaries and are converted explicitly.

#include <compare>

namespace spatial::core {

class AngleRadians {
 public:
  explicit constexpr AngleRadians(double value) noexcept : value_(value) {}

  constexpr double value() const noexcept { return value_; }

  constexpr AngleRadians operator-() const noexcept {
    return AngleRadians(-value_);
  }

  constexpr AngleRadians& operator+=(AngleRadians other) noexcept {
    value_ += other.value_;
    return *this;
  }

  constexpr AngleRadians& operator-=(AngleRadians other) noexcept {
    value_ -= other.value_;
    return *this;
  }

  friend constexpr AngleRadians operator+(AngleRadians a,
                                          AngleRadians b) noexcept {
    return AngleRadians(a.value_ + b.value_);
  }

  friend constexpr AngleRadians operator-(AngleRadians a,
                                          AngleRadians b) noexcept {
    return AngleRadians(a.value_ - b.value_);
  }

  friend constexpr bool operator==(AngleRadians a, AngleRadians b) noexcept {
    return a.value_ == b.value_;
  }

  friend constexpr bool operator!=(AngleRadians a, AngleRadians b) noexcept {
    return !(a == b);
  }

  friend constexpr auto operator<=>(AngleRadians a, AngleRadians b) noexcept {
    return a.value_ <=> b.value_;
  }

 private:
  double value_;
};

inline constexpr AngleRadians operator""_rad(long double value) {
  return AngleRadians(static_cast<double>(value));
}

}  // namespace spatial::core
