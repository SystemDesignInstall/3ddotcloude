#include "core/utils/uuid.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <random>
#include <stdexcept>
#include <vector>

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

// Self-contained SHA-1 (FIPS 180-4). Kept local to core/utils because SHA-1 is
// required only for RFC-4122 v5 UUID derivation; SHA-256 (CAS) is separate.
std::uint32_t RotateLeft(std::uint32_t x, unsigned n) {
  return (x << n) | (x >> (32u - n));
}

class Sha1 {
 public:
  Sha1() { std::memcpy(state_, kInit, sizeof(state_)); }

  void Update(const void* data, std::size_t size) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    bit_count_ += static_cast<std::uint64_t>(size) * 8;

    while (size > 0) {
      const std::size_t space = 64 - buffer_len_;
      const std::size_t take = size < space ? size : space;
      std::memcpy(buffer_ + buffer_len_, p, take);
      buffer_len_ += take;
      p += take;
      size -= take;
      if (buffer_len_ == 64) {
        Transform(buffer_);
        buffer_len_ = 0;
      }
    }
  }

  // Finalizes and returns the 20-byte digest.
  std::vector<std::uint8_t> Final() {
    if (!finalized_) {
      finalized_ = true;
      const std::uint64_t bit_count = bit_count_;
      const std::uint8_t pad = 0x80;
      Update(&pad, 1);
      std::uint8_t zeros[64];
      std::memset(zeros, 0, sizeof(zeros));
      while (buffer_len_ != 56) {
        Update(zeros, 1);
      }
      std::uint8_t len_bytes[8];
      for (int i = 0; i < 8; ++i) {
        len_bytes[i] = static_cast<std::uint8_t>(bit_count >> (56 - i * 8));
      }
      Update(len_bytes, 8);
    }

    std::vector<std::uint8_t> out(20);
    for (unsigned i = 0; i < 5; ++i) {
      out[i * 4] = static_cast<std::uint8_t>(state_[i] >> 24);
      out[i * 4 + 1] = static_cast<std::uint8_t>(state_[i] >> 16);
      out[i * 4 + 2] = static_cast<std::uint8_t>(state_[i] >> 8);
      out[i * 4 + 3] = static_cast<std::uint8_t>(state_[i]);
    }
    return out;
  }

 private:
  void Transform(const std::uint8_t block[64]) {
    std::uint32_t w[80];
    for (unsigned i = 0; i < 16; ++i) {
      w[i] = (std::uint32_t{block[i * 4]} << 24) |
             (std::uint32_t{block[i * 4 + 1]} << 16) |
             (std::uint32_t{block[i * 4 + 2]} << 8) |
             std::uint32_t{block[i * 4 + 3]};
    }
    for (unsigned i = 16; i < 80; ++i) {
      w[i] = RotateLeft(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];

    for (unsigned i = 0; i < 80; ++i) {
      std::uint32_t f;
      std::uint32_t k;
      if (i < 20) {
        f = (b & c) | ((~b) & d);
        k = 0x5a827999;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ed9eba1;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8f1bbcdc;
      } else {
        f = b ^ c ^ d;
        k = 0xca62c1d6;
      }
      const std::uint32_t temp =
          RotateLeft(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = RotateLeft(b, 30);
      b = a;
      a = temp;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
  }

  static constexpr std::uint32_t kInit[5] = {
      0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};

  std::uint32_t state_[5];
  std::uint64_t bit_count_ = 0;
  std::uint8_t buffer_[64];
  std::size_t buffer_len_ = 0;
  bool finalized_ = false;
};

}  // namespace

Uuid GenerateUuid() {
  Uuid out{};
  RngInstance().Fill(out);
  out[6] = static_cast<std::uint8_t>((out[6] & 0x0f) | 0x40);  // version 4
  out[8] = static_cast<std::uint8_t>((out[8] & 0x3f) | 0x80);  // variant 10
  return out;
}

Uuid GenerateUuidV5(const Uuid& namespace_uuid, const std::string& name) {
  Sha1 hasher;
  hasher.Update(namespace_uuid.data(), namespace_uuid.size());
  hasher.Update(name.data(), name.size());
  const std::vector<std::uint8_t> digest = hasher.Final();

  Uuid out{};
  std::memcpy(out.data(), digest.data(), out.size());
  out[6] = static_cast<std::uint8_t>((out[6] & 0x0f) | 0x50);  // version 5
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
