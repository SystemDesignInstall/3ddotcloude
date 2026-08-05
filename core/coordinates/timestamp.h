#pragma once

// Strong time types (ADR-018, RFC-0002 §7). An instant (TimestampNs) is
// nanoseconds since a domain-declared epoch; the epoch is carried by the
// surrounding ClockDomain, never baked into the value. Durations are signed.

#include <compare>
#include <cstdint>

namespace spatial::core {

class DurationNs {
 public:
  explicit constexpr DurationNs(int64_t value) noexcept : value_(value) {}

  constexpr int64_t value() const noexcept { return value_; }

  constexpr DurationNs operator-() const noexcept { return DurationNs(-value_); }

  constexpr DurationNs& operator+=(DurationNs other) noexcept {
    value_ += other.value_;
    return *this;
  }

  constexpr DurationNs& operator-=(DurationNs other) noexcept {
    value_ -= other.value_;
    return *this;
  }

  friend constexpr DurationNs operator+(DurationNs a, DurationNs b) noexcept {
    return DurationNs(a.value_ + b.value_);
  }

  friend constexpr DurationNs operator-(DurationNs a, DurationNs b) noexcept {
    return DurationNs(a.value_ - b.value_);
  }

  friend constexpr DurationNs operator*(DurationNs a, int64_t scalar) noexcept {
    return DurationNs(a.value_ * scalar);
  }

  friend constexpr DurationNs operator*(int64_t scalar, DurationNs a) noexcept {
    return a * scalar;
  }

  friend constexpr bool operator==(DurationNs a, DurationNs b) noexcept {
    return a.value_ == b.value_;
  }

  friend constexpr bool operator!=(DurationNs a, DurationNs b) noexcept {
    return !(a == b);
  }

  friend constexpr auto operator<=>(DurationNs a, DurationNs b) noexcept {
    return a.value_ <=> b.value_;
  }

 private:
  int64_t value_;
};

class TimestampNs {
 public:
  explicit constexpr TimestampNs(int64_t value) noexcept : value_(value) {}

  constexpr int64_t value() const noexcept { return value_; }

  constexpr TimestampNs& operator+=(DurationNs delta) noexcept {
    value_ += delta.value();
    return *this;
  }

  constexpr TimestampNs& operator-=(DurationNs delta) noexcept {
    value_ -= delta.value();
    return *this;
  }

  friend constexpr TimestampNs operator+(TimestampNs t, DurationNs d) noexcept {
    return TimestampNs(t.value_ + d.value());
  }

  friend constexpr TimestampNs operator+(DurationNs d, TimestampNs t) noexcept {
    return t + d;
  }

  friend constexpr TimestampNs operator-(TimestampNs t, DurationNs d) noexcept {
    return TimestampNs(t.value_ - d.value());
  }

  friend constexpr DurationNs operator-(TimestampNs a, TimestampNs b) noexcept {
    return DurationNs(a.value_ - b.value_);
  }

  friend constexpr bool operator==(TimestampNs a, TimestampNs b) noexcept {
    return a.value_ == b.value_;
  }

  friend constexpr bool operator!=(TimestampNs a, TimestampNs b) noexcept {
    return !(a == b);
  }

  friend constexpr auto operator<=>(TimestampNs a, TimestampNs b) noexcept {
    return a.value_ <=> b.value_;
  }

 private:
  int64_t value_;
};

inline constexpr DurationNs operator""_ns(unsigned long long value) {
  return DurationNs(static_cast<int64_t>(value));
}

inline constexpr DurationNs operator""_us(unsigned long long value) {
  return DurationNs(static_cast<int64_t>(value) * 1000);
}

inline constexpr DurationNs operator""_ms(unsigned long long value) {
  return DurationNs(static_cast<int64_t>(value) * 1000 * 1000);
}

inline constexpr DurationNs operator""_s(unsigned long long value) {
  return DurationNs(static_cast<int64_t>(value) * 1000 * 1000 * 1000);
}

}  // namespace spatial::core
