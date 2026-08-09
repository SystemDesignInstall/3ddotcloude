#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "importers/images/image_format.h"

namespace spatial::importers {
namespace {

const std::vector<std::uint8_t> kJpeg = {
    0xFF, 0xD8,                                   // SOI
    0xFF, 0xE0, 0x00, 0x10,                       // APP0
    0x4A, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x00, 0x00, 0x48, 0x00, 0x00,
    0x00, 0x00,                                   // JFIF segment body
    0xFF, 0xC0, 0x00, 0x11,                       // SOF0
    0x08, 0x00, 0x02, 0x00, 0x04, 0x03, 0x01, 0x22, 0x00, 0x02, 0x11, 0x01,
    0x03, 0x11, 0x01,                             // 8-bit, 2x4, 3 components
    0xFF, 0xD9,                                   // EOI
};

const std::vector<std::uint8_t> kPng = {
    0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n',  // signature
    0x00, 0x00, 0x00, 0x0D,                       // IHDR length 13
    'I', 'H', 'D', 'R',                           // IHDR
    0x00, 0x00, 0x00, 0x03,                       // width 3
    0x00, 0x00, 0x00, 0x02,                       // height 2
    0x08, 0x02, 0x00, 0x00, 0x00,                 // 8-bit RGB, no interlace
    0x00, 0x00, 0x00, 0x00,                       // crc (ignored)
};

const std::vector<std::uint8_t> kTiff = {
    'I', 'I', 0x2A, 0x00, 0x08, 0x00, 0x00, 0x00,  // II*, IFD0 at 8
    0x02, 0x00,                                    // 2 entries
    0x00, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0xE0, 0x01, 0x00, 0x00,  // width 480
    0x01, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x2C, 0x01, 0x00, 0x00,  // height 300
    0x00, 0x00, 0x00, 0x00,                       // next IFD
};

const std::vector<std::uint8_t> kTiffBigEndian = {
    'M', 'M', 0x00, 0x2A, 0x00, 0x00, 0x00, 0x08,  // MM*0, IFD0 at 8
    0x00, 0x02,                                    // 2 entries
    0x01, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00,  // width 1
    0x01, 0x01, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00,  // height 1
    0x00, 0x00, 0x00, 0x00,
};

const std::vector<std::uint8_t> kExr = {
    0x76, 0x2F, 0x31, 0x01, 0x00, 0x00, 0x00, 0x00,  // magic + version 1
    'd', 'a', 't', 'a', 'W', 'i', 'n', 'd', 'o', 'w', 0x00,  // "dataWindow"
    'b', 'o', 'x', '2', 'i', 0x00,                          // "box2i"
    0x10, 0x00, 0x00, 0x00,                                  // size 16
    0x00, 0x00, 0x00, 0x00,                                  // xMin
    0x00, 0x00, 0x00, 0x00,                                  // yMin
    0x7F, 0x07, 0x00, 0x00,                                  // xMax 1919
    0x37, 0x04, 0x00, 0x00,                                  // yMax 1079
    'c', 'h', 'a', 'n', 'n', 'e', 'l', 's', 0x00,            // "channels"
    'c', 'h', 'l', 'i', 's', 't', 0x00,                      // "chlist"
    0x13, 0x00, 0x00, 0x00,                                  // size 19
    'R', 0x00,                                              // channel "R"
    0x01, 0x00, 0x00, 0x00,                                  // pixelType half
    0x00, 0x00, 0x00, 0x00,                                  // pLinear+reserved
    0x01, 0x00, 0x00, 0x00,                                  // xSampling
    0x01, 0x00, 0x00, 0x00,                                  // ySampling
    0x00,                                                   // chlist terminator
    0x00,                                                   // end of header
};

TEST(ImageFormatTest, DetectMagicBytes) {
  EXPECT_EQ(DetectImageFormat(kJpeg.data(), kJpeg.size()),
            ImageFormat::kJpeg);
  EXPECT_EQ(DetectImageFormat(kPng.data(), kPng.size()), ImageFormat::kPng);
  EXPECT_EQ(DetectImageFormat(kTiff.data(), kTiff.size()), ImageFormat::kTiff);
  EXPECT_EQ(DetectImageFormat(kTiffBigEndian.data(), kTiffBigEndian.size()),
            ImageFormat::kTiff);
  EXPECT_EQ(DetectImageFormat(kExr.data(), kExr.size()), ImageFormat::kExr);
}

TEST(ImageFormatTest, DetectUnknownAndEmpty) {
  const std::vector<std::uint8_t> garbage = {0x01, 0x02, 0x03, 0x04};
  EXPECT_EQ(DetectImageFormat(garbage.data(), garbage.size()),
            ImageFormat::kUnknown);
  EXPECT_EQ(DetectImageFormat(kJpeg.data(), 0), ImageFormat::kUnknown);
  EXPECT_EQ(DetectImageFormat(kJpeg.data(), 2), ImageFormat::kUnknown);
  EXPECT_EQ(DetectImageFormat(nullptr, 0), ImageFormat::kUnknown);
}

TEST(ImageFormatTest, ParseJpegHeader) {
  const auto info = ParseImageHeader(kJpeg.data(), kJpeg.size());
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ(info->format, ImageFormat::kJpeg);
  EXPECT_EQ(info->width, 4);
  EXPECT_EQ(info->height, 2);
  EXPECT_EQ(info->pixel_format, "rgb8");
  EXPECT_EQ(info->mime_type, "image/jpeg");
}

TEST(ImageFormatTest, ParsePngHeader) {
  const auto info = ParseImageHeader(kPng.data(), kPng.size());
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ(info->format, ImageFormat::kPng);
  EXPECT_EQ(info->width, 3);
  EXPECT_EQ(info->height, 2);
  EXPECT_EQ(info->pixel_format, "rgb8");
  EXPECT_EQ(info->mime_type, "image/png");
}

TEST(ImageFormatTest, ParseTiffHeader) {
  const auto info = ParseImageHeader(kTiff.data(), kTiff.size());
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ(info->format, ImageFormat::kTiff);
  EXPECT_EQ(info->width, 480);
  EXPECT_EQ(info->height, 300);
  EXPECT_EQ(info->pixel_format, "gray8");
  EXPECT_EQ(info->mime_type, "image/tiff");
}

TEST(ImageFormatTest, ParseTiffBigEndianHeader) {
  const auto info = ParseImageHeader(kTiffBigEndian.data(),
                                     kTiffBigEndian.size());
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ(info->format, ImageFormat::kTiff);
  EXPECT_EQ(info->width, 1);
  EXPECT_EQ(info->height, 1);
}

TEST(ImageFormatTest, ParseExrHeader) {
  const auto info = ParseImageHeader(kExr.data(), kExr.size());
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ(info->format, ImageFormat::kExr);
  EXPECT_EQ(info->width, 1920);
  EXPECT_EQ(info->height, 1080);
  EXPECT_EQ(info->pixel_format, "float16");
  EXPECT_EQ(info->mime_type, "image/x-exr");
}

TEST(ImageFormatTest, ParseTruncatedReturnsNullopt) {
  // Valid magic, incomplete header.
  EXPECT_FALSE(
      ParseImageHeader(kPng.data(), 8).has_value());
  EXPECT_FALSE(ParseImageHeader(kExr.data(), 8).has_value());
  EXPECT_FALSE(ParseImageHeader(kTiff.data(), 8).has_value());
  EXPECT_FALSE(ParseImageHeader(kJpeg.data(), 4).has_value());
}

TEST(ImageFormatTest, DetectCr2AsRaw) {
  const std::vector<std::uint8_t> cr2 = {
      'I', 'I', 0x2A, 0x00, 0x10, 0x00, 0x00, 0x00,  // II*, IFD0 at 16
      'C', 'R', 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,  // CR2 magic + version
      0x02, 0x00,                                    // 2 entries
      0x00, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0xE0, 0x01, 0x00, 0x00,  // width 480
      0x01, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x2C, 0x01, 0x00, 0x00,  // height 300
      0x00, 0x00, 0x00, 0x00,                        // next IFD
  };
  EXPECT_EQ(DetectImageFormat(cr2.data(), cr2.size()), ImageFormat::kRaw);
  const auto info = ParseImageHeader(cr2.data(), cr2.size());
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ(info->format, ImageFormat::kRaw);
  EXPECT_EQ(info->mime_type, "image/x-canon-cr2");
  EXPECT_EQ(info->width, 480);
}

TEST(ImageFormatTest, DetectDngAsRaw) {
  const std::vector<std::uint8_t> dng = {
      'I', 'I', 0x2A, 0x00, 0x08, 0x00, 0x00, 0x00,  // II*, IFD0 at 8
      0x03, 0x00,                                    // 3 entries
      0x00, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0xE0, 0x01, 0x00, 0x00,  // width 480
      0x01, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x2C, 0x01, 0x00, 0x00,  // height 300
      0x12, 0xC6, 0x01, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,  // DNGVersion
      0x00, 0x00, 0x00, 0x00,
  };
  EXPECT_EQ(DetectImageFormat(dng.data(), dng.size()), ImageFormat::kRaw);
  const auto info = ParseImageHeader(dng.data(), dng.size());
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ(info->format, ImageFormat::kRaw);
  EXPECT_EQ(info->mime_type, "image/x-adobe-dng");
  EXPECT_EQ(info->width, 480);
}

TEST(ImageFormatTest, DetectNefAsRaw) {
  const std::vector<std::uint8_t> nef = {
      'I', 'I', 0x2A, 0x00, 0x08, 0x00, 0x00, 0x00,  // II*, IFD0 at 8
      0x03, 0x00,                                    // 3 entries
      0x00, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0xE0, 0x01, 0x00, 0x00,  // width 480
      0x01, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x2C, 0x01, 0x00, 0x00,  // height 300
      0x0F, 0x01, 0x02, 0x00, 0x06, 0x00, 0x00, 0x00, 0x32, 0x00, 0x00, 0x00,  // Make->0x32
      0x00, 0x00, 0x00, 0x00,                        // next IFD
      'N', 'I', 'K', 'O', 'N', 0x00,                // "NIKON" at 0x32
  };
  EXPECT_EQ(DetectImageFormat(nef.data(), nef.size()), ImageFormat::kRaw);
  const auto info = ParseImageHeader(nef.data(), nef.size());
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ(info->format, ImageFormat::kRaw);
  EXPECT_EQ(info->mime_type, "image/x-nikon-nef");
}

TEST(ImageFormatTest, ToStringCoverage) {
  EXPECT_EQ(ToString(ImageFormat::kJpeg), "jpeg");
  EXPECT_EQ(ToString(ImageFormat::kPng), "png");
  EXPECT_EQ(ToString(ImageFormat::kTiff), "tiff");
  EXPECT_EQ(ToString(ImageFormat::kExr), "exr");
  EXPECT_EQ(ToString(ImageFormat::kRaw), "raw");
  EXPECT_EQ(ToString(ImageFormat::kUnknown), "unknown");
}

}  // namespace
}  // namespace spatial::importers
