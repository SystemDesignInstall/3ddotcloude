#pragma once

// Strong distance type (ADR-018, RFC-0002 §7). The platform stores distances
// in metres only; the type guards unit confusion at the compile level.

#include <compare>

namespace spatial::core {

class DistanceMeters {
 public:
  explicit constexpr DistanceMeters(double value) noexcept : value_(value) {}

  constexpr double value() const noexcept { return value_; }

  constexpr DistanceMeters operator-() const noexcept {
    return DistanceMeters(-value_);
  }

  constexpr DistanceMeters& operator+=(DistanceMeters other) noexcept {
    value_ += other.value_;
    return *this;
  }

  constexpr DistanceMeters& operator-=(DistanceMeters other) noexcept {
    value_ -= other.value_;
    return *this;
  }

  constexpr DistanceMeters& operator*=(double scalar) noexcept {
    value_ *= scalar;
    return *this;
  }

  friend constexpr DistanceMeters operator+(DistanceMeters a,
                                            DistanceMeters b) noexcept {
    return DistanceMeters(a.value_ + b.value_);
  }

  friend constexpr DistanceMeters operator-(DistanceMeters a,
                                            DistanceMeters b) noexcept {
    return DistanceMeters(a.value_ - b.value_);
  }

  friend constexpr DistanceMeters operator*(DistanceMeters a,
                                            double scalar) noexcept {
    return DistanceMeters(a.value_ * scalar);
  }

  friend constexpr DistanceMeters operator*(double scalar,
                                            DistanceMeters a) noexcept {
    return a * scalar;
  }

  friend constexpr bool operator==(DistanceMeters a, DistanceMeters b) noexcept {
    return a.value_ == b.value_;
  }

  friend constexpr bool operator!=(DistanceMeters a, DistanceMeters b) noexcept {
    return !(a == b);
  }

  friend constexpr auto operator<=>(DistanceMeters a,
                                    DistanceMeters b) noexcept {
    return a.value_ <=> b.value_;
  }

 private:
  double value_;
};

inline constexpr DistanceMeters operator""_m(long double value) {
  return DistanceMeters(static_cast<double>(value));
}

}  // namespace spatial::core
