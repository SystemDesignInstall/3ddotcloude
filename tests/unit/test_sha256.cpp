#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "core/utils/sha256.h"

namespace spatial::core {
namespace {

// FIPS 180-4 test vectors.
TEST(Sha256, EmptyString) {
  EXPECT_EQ(Sha256Hex(""), "e3b0c44298fc1c149afbf4c8996fb924"
                           "27ae41e4649b934ca495991b7852b855");
}

TEST(Sha256, Abc) {
  EXPECT_EQ(Sha256Hex("abc"), "ba7816bf8f01cfea414140de5dae2223"
                              "b00361a396177a9cb410ff61f20015ad");
}

TEST(Sha256, Abcdbcde) {
  EXPECT_EQ(Sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkl"
                      "jklmklmnlmnomnopnopq"),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167"
            "f6ecedd419db06c1");
}

TEST(Sha256, OneMillionA) {
  const std::string a(1'000'000, 'a');
  EXPECT_EQ(Sha256Hex(a), "cdc76e5c9914fb9281a1c7e284d73e67"
                          "f1809a48a497200e046d39ccc7112cd0");
}

TEST(Sha256, KnownPhrase) {
  EXPECT_EQ(Sha256Hex("The quick brown fox jumps over the lazy dog"),
            "d7a8fbb307d7809469ca9abcb0082e4f"
            "8d5651e46d3cdb762d02d0bf37c9e592");
}

TEST(Sha256, ChunkedMatchesWhole) {
  const std::string input =
      "The quick brown fox jumps over the lazy dog 0123456789";
  std::vector<std::uint8_t> bytes(input.begin(), input.end());
  const std::string whole = Sha256Hex(bytes);

  // Non-overlapping chunks: 7 | 33 | rest.
  Sha256 hasher;
  hasher.Update(bytes.data(), 7);
  hasher.Update(bytes.data() + 7, 33);
  hasher.Update(bytes.data() + 40, bytes.size() - 40);
  const auto digest = hasher.Final();

  constexpr char kHex[] = "0123456789abcdef";
  std::string chunked;
  chunked.reserve(64);
  for (const auto byte : digest) {
    chunked.push_back(kHex[byte >> 4]);
    chunked.push_back(kHex[byte & 0x0f]);
  }
  EXPECT_EQ(whole, chunked);
  EXPECT_EQ(whole, Sha256Hex(input));
}

}  // namespace
}  // namespace spatial::core
