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

}  // namespace
}  // namespace spatial::core
