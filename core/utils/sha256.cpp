#include "core/utils/sha256.h"

#include <cstring>

namespace spatial::core {
namespace {

constexpr std::uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr std::uint32_t kInit[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372,
                                    0xa54ff53a, 0x510e527f, 0x9b05688c,
                                    0x1f83d9ab, 0x5be0cd19};

std::uint32_t RotR(std::uint32_t x, unsigned n) {
  return (x >> n) | (x << (32u - n));
}

}  // namespace

Sha256::Sha256() {
  std::memcpy(state_, kInit, sizeof(state_));
}

void Sha256::Transform(const std::uint8_t block[64]) {
  std::uint32_t w[64];
  for (unsigned i = 0; i < 16; ++i) {
    w[i] = (std::uint32_t{block[i * 4]} << 24) |
           (std::uint32_t{block[i * 4 + 1]} << 16) |
           (std::uint32_t{block[i * 4 + 2]} << 8) |
           std::uint32_t{block[i * 4 + 3]};
  }
  for (unsigned i = 16; i < 64; ++i) {
    const std::uint32_t s0 =
        RotR(w[i - 15], 7) ^ RotR(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const std::uint32_t s1 =
        RotR(w[i - 2], 17) ^ RotR(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (unsigned i = 0; i < 64; ++i) {
    const std::uint32_t s1 =
        RotR(e, 6) ^ RotR(e, 11) ^ RotR(e, 25);
    const std::uint32_t ch = (e & f) ^ ((~e) & g);
    const std::uint32_t temp1 = h + s1 + ch + kK[i] + w[i];
    const std::uint32_t s0 =
        RotR(a, 2) ^ RotR(a, 13) ^ RotR(a, 22);
    const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = s0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

Sha256& Sha256::Update(const void* data, std::size_t size) {
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
  return *this;
}

std::vector<std::uint8_t> Sha256::Final() {
  if (!finalized_) {
    finalized_ = true;
    const std::uint64_t bit_count = bit_count_;
    const std::uint8_t pad = 0x80;
    Update(&pad, 1);
    std::uint8_t zeros[64];
    std::memset(zeros, 0, sizeof(zeros));
    // Pad until the buffer length is 56 mod 64, then append the bit count.
    while (buffer_len_ != 56) {
      Update(zeros, 1);
    }
    std::uint8_t len_bytes[8];
    for (int i = 0; i < 8; ++i) {
      len_bytes[i] = static_cast<std::uint8_t>(bit_count >> (56 - i * 8));
    }
    Update(len_bytes, 8);
  }

  std::vector<std::uint8_t> out(kDigestBytes);
  for (unsigned i = 0; i < 8; ++i) {
    out[i * 4] = static_cast<std::uint8_t>(state_[i] >> 24);
    out[i * 4 + 1] = static_cast<std::uint8_t>(state_[i] >> 16);
    out[i * 4 + 2] = static_cast<std::uint8_t>(state_[i] >> 8);
    out[i * 4 + 3] = static_cast<std::uint8_t>(state_[i]);
  }
  return out;
}

std::string Sha256Hex(const void* data, std::size_t size) {
  Sha256 hasher;
  hasher.Update(data, size);
  const auto digest = hasher.Final();
  constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(Sha256::kDigestBytes * 2);
  for (const auto byte : digest) {
    out.push_back(kHex[byte >> 4]);
    out.push_back(kHex[byte & 0x0f]);
  }
  return out;
}

std::string Sha256Hex(std::string_view data) {
  return Sha256Hex(data.data(), data.size());
}

std::string Sha256Hex(const std::vector<std::uint8_t>& data) {
  return Sha256Hex(data.data(), data.size());
}

}  // namespace spatial::core
