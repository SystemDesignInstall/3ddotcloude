#include "core/utils/uuid.h"

#include <algorithm>
#include <cctype>
#include <random>
#include <stdexcept>

#include "core/errors/project_error.h"

namespace spatial::core {
namespace {

class UuidRng {
 public:
  UuidRng() : rng_(std::random_device{}()) {}

  void Fill(Uuid& out) {
    std::uniform_int_distribution<int> byte_dist(0, 255);
    for (auto& b : out) {
      b = static_cast<std::uint8_t>(byte_dist(rng_));
    }
  }

 private:
  std::mt19937 rng_;
};

UuidRng& RngInstance() {
  static UuidRng rng;
  return rng;
}

int HexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

Uuid GenerateUuid() {
  Uuid out{};
  RngInstance().Fill(out);
  out[6] = static_cast<std::uint8_t>((out[6] & 0x0f) | 0x40);  // version 4
  out[8] = static_cast<std::uint8_t>((out[8] & 0x3f) | 0x80);  // variant 10
  return out;
}

Uuid ParseUuid(const std::string& s) {
  Uuid out{};
  std::size_t hi = 0;
  int nibble = 0;
  bool seen_hyphen = false;
  std::size_t expect_hyphen_at = 8;

  for (std::size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '-') {
      if (i != expect_hyphen_at) {
        throw ValidationError(ErrorCode::kValidationDomain,
                              "malformed UUID: bad hyphen position");
      }
      seen_hyphen = true;
      if (expect_hyphen_at == 8) expect_hyphen_at = 13;
      else if (expect_hyphen_at == 13) expect_hyphen_at = 18;
      else if (expect_hyphen_at == 18) expect_hyphen_at = 23;
      continue;
    }
    const int v = HexValue(c);
    if (v < 0) {
      throw ValidationError(ErrorCode::kValidationDomain,
                            "malformed UUID: non-hex character");
    }
    if (hi >= 16) {
      throw ValidationError(ErrorCode::kValidationDomain,
                            "malformed UUID: too many characters");
    }
    if (nibble == 0) {
      out[hi] = static_cast<std::uint8_t>(v << 4);
      nibble = 1;
    } else {
      out[hi] = static_cast<std::uint8_t>(out[hi] | v);
      nibble = 0;
      ++hi;
    }
  }
  if (hi != 16 || nibble != 0 || !seen_hyphen) {
    throw ValidationError(ErrorCode::kValidationDomain,
                          "malformed UUID: length or structure");
  }
  return out;
}

std::string FormatUuid(const Uuid& uuid) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(36);
  for (std::size_t i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) out.push_back('-');
    out.push_back(kHex[uuid[i] >> 4]);
    out.push_back(kHex[uuid[i] & 0x0f]);
  }
  return out;
}

bool IsNil(const Uuid& uuid) {
  return std::all_of(uuid.begin(), uuid.end(),
                     [](std::uint8_t b) { return b == 0; });
}

}  // namespace spatial::core
