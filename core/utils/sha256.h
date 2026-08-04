#pragma once

// Self-contained SHA-256 (FIPS 180-4). Kept in core/utils because SHA-256 is
// not part of the M0 dependency allowlist (ADR-003); the CAS store (ADR-010)
// is the only consumer. Feed entire payloads or stream incrementally.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace spatial::core {

class Sha256 {
 public:
  Sha256();
  Sha256& Update(const void* data, std::size_t size);
  Sha256& Update(std::string_view data) {
    return Update(data.data(), data.size());
  }
  // Finalizes and returns the 32-byte digest.
  std::vector<std::uint8_t> Final();

  static constexpr std::size_t kDigestBytes = 32;

 private:
  void Transform(const std::uint8_t block[64]);

  std::uint32_t state_[8];
  std::uint64_t bit_count_ = 0;
  std::uint8_t buffer_[64];
  std::size_t buffer_len_ = 0;
  bool finalized_ = false;
};

// Hex-encoded SHA-256 of the given bytes (lowercase, 64 chars).
std::string Sha256Hex(const void* data, std::size_t size);
std::string Sha256Hex(std::string_view data);
std::string Sha256Hex(const std::vector<std::uint8_t>& data);

}  // namespace spatial::core
