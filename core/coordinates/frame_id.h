#pragma once

// Strong identity for a coordinate frame (RFC-0002 §7.3). Backed by a UUID v4;
// a nil FrameId denotes "no frame" (e.g. a root node's parent).

#include <cstddef>
#include <cstdint>
#include <string>

#include "core/utils/uuid.h"

namespace spatial::core {

class FrameId {
 public:
  FrameId() = default;
  explicit FrameId(Uuid value) : value_(value) {}

  static FrameId Generate() { return FrameId(GenerateUuid()); }
  static FrameId FromString(const std::string& s) { return FrameId(ParseUuid(s)); }
  static FrameId Nil() { return FrameId(Uuid{}); }

  const Uuid& value() const noexcept { return value_; }
  bool IsNil() const noexcept { return ::spatial::core::IsNil(value_); }
  std::string ToString() const { return FormatUuid(value_); }

  friend bool operator==(FrameId a, FrameId b) noexcept {
    return a.value_ == b.value_;
  }

  friend bool operator!=(FrameId a, FrameId b) noexcept { return !(a == b); }

  friend bool operator<(FrameId a, FrameId b) noexcept {
    return a.value_ < b.value_;
  }

 private:
  Uuid value_;
};

}  // namespace spatial::core

namespace std {

template <>
struct hash<spatial::core::FrameId> {
  size_t operator()(spatial::core::FrameId id) const noexcept {
    size_t h = 1469598103934665603ull;
    for (const uint8_t b : id.value()) {
      h ^= b;
      h *= 1099511628211ull;
    }
    return h;
  }
};

}  // namespace std
