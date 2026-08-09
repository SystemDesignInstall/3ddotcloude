#include <gtest/gtest.h>

#include <set>
#include <string>

#include "core/errors/project_error.h"
#include "core/utils/uuid.h"

namespace spatial::core {
namespace {

TEST(Uuid, RoundTrip) {
  const auto uuid = GenerateUuid();
  const std::string s = FormatUuid(uuid);
  EXPECT_EQ(s.size(), 36u);
  EXPECT_EQ(ParseUuid(s), uuid);
}

TEST(Uuid, VersionAndVariant) {
  const auto uuid = GenerateUuid();
  EXPECT_EQ(uuid[6] >> 4, 4);
  EXPECT_EQ(uuid[8] >> 6, 2);
}

TEST(Uuid, Uniqueness) {
  std::set<std::string> seen;
  for (int i = 0; i < 1000; ++i) {
    seen.insert(FormatUuid(GenerateUuid()));
  }
  EXPECT_EQ(seen.size(), 1000u);
}

TEST(Uuid, FormatIsLowercaseCanonical) {
  const auto uuid = GenerateUuid();
  const std::string s = FormatUuid(uuid);
  for (const char c : s) {
    if (c == '-') continue;
    EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
  }
  EXPECT_EQ(s[8], '-');
  EXPECT_EQ(s[13], '-');
  EXPECT_EQ(s[18], '-');
  EXPECT_EQ(s[23], '-');
}

TEST(Uuid, ParseRejectsMalformed) {
  const std::string valid = FormatUuid(GenerateUuid());
  EXPECT_NO_THROW(ParseUuid(valid));
  EXPECT_THROW(ParseUuid(""), ValidationError);
  EXPECT_THROW(ParseUuid("not-a-uuid"), ValidationError);
  EXPECT_THROW(ParseUuid(valid.substr(0, 35)), ValidationError);
  EXPECT_THROW(ParseUuid(valid + "0"), ValidationError);
  // Invalid hex char.
  std::string bad = valid;
  bad[0] = 'g';
  EXPECT_THROW(ParseUuid(bad), ValidationError);
}

TEST(Uuid, NilDetection) {
  Uuid nil{};
  EXPECT_TRUE(IsNil(nil));
  EXPECT_FALSE(IsNil(GenerateUuid()));
}

TEST(UuidV5, Deterministic) {
  const Uuid ns = ParseUuid("6ba7b810-9dad-11d1-80b4-00c04fd430c8");  // DNS
  const Uuid a = GenerateUuidV5(ns, "www.example.com");
  const Uuid b = GenerateUuidV5(ns, "www.example.com");
  EXPECT_EQ(a, b);
}

TEST(UuidV5, VersionAndVariant) {
  const Uuid ns = ParseUuid("6ba7b810-9dad-11d1-80b4-00c04fd430c8");
  const auto uuid = GenerateUuidV5(ns, "www.example.com");
  EXPECT_EQ(uuid[6] >> 4, 5);
  EXPECT_EQ(uuid[8] >> 6, 2);
}

TEST(UuidV5, NameSensitive) {
  const Uuid ns = ParseUuid("6ba7b810-9dad-11d1-80b4-00c04fd430c8");
  EXPECT_NE(GenerateUuidV5(ns, "a"), GenerateUuidV5(ns, "b"));
}

TEST(UuidV5, NamespaceSensitive) {
  const Uuid ns_dns = ParseUuid("6ba7b810-9dad-11d1-80b4-00c04fd430c8");
  const Uuid ns_url = ParseUuid("6ba7b811-9dad-11d1-80b4-00c04fd430c8");
  EXPECT_NE(GenerateUuidV5(ns_dns, "www.example.com"),
            GenerateUuidV5(ns_url, "www.example.com"));
}

TEST(UuidV5, Rfc4122Vectors) {
  // RFC 4122 §4.3 reference vectors (DNS/URL namespaces).
  const Uuid ns_dns = ParseUuid("6ba7b810-9dad-11d1-80b4-00c04fd430c8");
  const Uuid ns_url = ParseUuid("6ba7b811-9dad-11d1-80b4-00c04fd430c8");
  EXPECT_EQ(FormatUuid(GenerateUuidV5(ns_dns, "www.example.com")),
            "2ed6657d-e927-568b-95e1-2665a8aea6a2");
  EXPECT_EQ(FormatUuid(GenerateUuidV5(ns_url, "www.example.com")),
            "b63cdfa4-3df9-568e-97ae-006c5b8fd652");
  EXPECT_EQ(FormatUuid(GenerateUuidV5(ns_url, "example.com")),
            "a5cf6e8e-4cfa-5f31-a804-6de6d1245e26");
}

TEST(UuidV5, CanonicalNameRoundTrip) {
  // The P2.1 canonical name (image-import.md §6) round-trips through ParseUuid.
  const Uuid ns = ParseUuid("6ba7b810-9dad-11d1-80b4-00c04fd430c8");
  const std::string name =
      "frame|camera0|1699999999123456789|"
      "3b78ce563f89a0ed9414f5aa28ad0d96d6795f9c63";
  const std::string s = FormatUuid(GenerateUuidV5(ns, name));
  EXPECT_EQ(ParseUuid(s), GenerateUuidV5(ns, name));
}

}  // namespace
}  // namespace spatial::core
