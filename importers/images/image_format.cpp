#include "importers/images/image_format.h"

#include <cstring>
#include <string>

namespace spatial::importers {
namespace {

constexpr std::size_t kPngMagicLen = 8;
constexpr std::size_t kExrMagicLen = 4;
constexpr std::size_t kMaxHeaderSize = 1u << 20;  // 1 MiB scan bound

// ---------------------------------------------------------------------------
// Bounds-checked big/little-endian readers (headers only).
// ---------------------------------------------------------------------------

std::optional<std::uint16_t> ReadU16(const std::uint8_t* data,
                                     std::size_t size, std::size_t off,
                                     bool little) {
  if (off > size || size - off < 2) return std::nullopt;
  const std::uint16_t a = data[off];
  const std::uint16_t b = data[off + 1];
  return little ? static_cast<std::uint16_t>((a) | (b << 8))
                : static_cast<std::uint16_t>((b) | (a << 8));
}

std::optional<std::uint32_t> ReadU32(const std::uint8_t* data,
                                     std::size_t size, std::size_t off,
                                     bool little) {
  if (off > size || size - off < 4) return std::nullopt;
  std::uint32_t v = 0;
  if (little) {
    v = static_cast<std::uint32_t>(data[off]) |
        (static_cast<std::uint32_t>(data[off + 1]) << 8) |
        (static_cast<std::uint32_t>(data[off + 2]) << 16) |
        (static_cast<std::uint32_t>(data[off + 3]) << 24);
  } else {
    v = static_cast<std::uint32_t>(data[off + 3]) |
        (static_cast<std::uint32_t>(data[off + 2]) << 8) |
        (static_cast<std::uint32_t>(data[off + 1]) << 16) |
        (static_cast<std::uint32_t>(data[off]) << 24);
  }
  return v;
}

// ---------------------------------------------------------------------------
// Format detection (magic bytes only).
// ---------------------------------------------------------------------------

bool IsJpeg(const std::uint8_t* data, std::size_t size) {
  return size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF;
}

bool IsPng(const std::uint8_t* data, std::size_t size) {
  static const std::uint8_t kMagic[kPngMagicLen] = {
      0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  return size >= kPngMagicLen &&
         std::memcmp(data, kMagic, kPngMagicLen) == 0;
}

bool IsTiff(const std::uint8_t* data, std::size_t size) {
  if (size < 8) return false;
  const bool little = data[0] == 'I' && data[1] == 'I';
  const bool big = data[0] == 'M' && data[1] == 'M';
  if (!little && !big) return false;
  const auto magic = ReadU16(data, size, 2, little);
  return magic.has_value() && *magic == 42;
}

bool IsExr(const std::uint8_t* data, std::size_t size) {
  return size >= kExrMagicLen && data[0] == 0x76 && data[1] == 0x2F &&
         data[2] == 0x31 && data[3] == 0x01;
}

// ---------------------------------------------------------------------------
// TIFF IFD (shared by plain TIFF and the TIFF-container RAW families).
// ---------------------------------------------------------------------------

struct TiffIfdEntry {
  std::uint16_t tag = 0;
  std::uint16_t type = 0;
  std::uint32_t count = 0;
  // Where the entry payload lives (inline in the 4-byte value field, or at a
  // file offset), and its size in bytes.
  std::optional<std::uint32_t> value_off;
  std::size_t value_size = 0;
};

constexpr std::size_t kTagImageWidth = 0x0100;
constexpr std::size_t kTagImageLength = 0x0101;
constexpr std::size_t kTagBitsPerSample = 0x0102;
constexpr std::size_t kTagMake = 0x010F;
constexpr std::size_t kTagSamplesPerPixel = 0x0115;
constexpr std::size_t kTagDngVersion = 0xC612;

constexpr std::size_t kTypeShort = 3;
constexpr std::size_t kTypeLong = 4;

bool TiffLittleEndian(const std::uint8_t* data) {
  return data[0] == 'I' && data[1] == 'I';
}

// Iterates IFD0 entries, invoking on_entry(tag, entry). Returns false on
// malformed IFD layout.
template <typename Fn>
bool ForEachTiffEntry(const std::uint8_t* data, std::size_t size,
                      std::uint32_t ifd_off, bool little, Fn&& on_entry) {
  const auto count = ReadU16(data, size, ifd_off, little);
  if (!count.has_value()) return false;
  for (std::uint32_t i = 0; i < *count; ++i) {
    const std::size_t base = static_cast<std::size_t>(ifd_off) + 2 +
                             static_cast<std::size_t>(i) * 12u;
    if (base + 12 > size) return false;
    const auto tag = ReadU16(data, size, base, little);
    const auto type = ReadU16(data, size, base + 2, little);
    const auto cnt = ReadU32(data, size, base + 4, little);
    if (!tag.has_value() || !type.has_value() || !cnt.has_value()) return false;
    const std::size_t type_size =
        *type == 1 ? 1u : *type == 2 ? 1u : *type == 3 ? 2u : *type == 4 ? 4u : 0u;
    if (type_size == 0) return false;  // unsupported field type: skip entry
    const std::size_t total = static_cast<std::size_t>(*cnt) * type_size;
    TiffIfdEntry e;
    e.tag = *tag;
    e.type = *type;
    e.count = *cnt;
    if (total <= 4) {
      // Value stored inline in the 4-byte field.
      e.value_off = static_cast<std::uint32_t>(base + 8);
      e.value_size = total;
    } else {
      const auto off = ReadU32(data, size, base + 8, little);
      if (!off.has_value()) return false;
      e.value_off = *off;
      e.value_size = total;
    }
    if (!on_entry(e)) return false;
  }
  return true;
}

// Reads the numeric value of a SHORT/LONG tag. Only the first element is
// returned for arrays (BitsPerSample etc. — homogeneous arrays in practice).
std::optional<std::uint64_t> ReadTiffTagValue(const TiffIfdEntry& e,
                                              const std::uint8_t* data,
                                              std::size_t size, bool little) {
  if (!e.value_off.has_value()) return std::nullopt;
  const std::size_t off = *e.value_off;
  if (e.type == kTypeShort) {
    const auto v = ReadU16(data, size, off, little);
    return v.has_value() ? std::optional<std::uint64_t>(*v) : std::nullopt;
  }
  if (e.type == kTypeLong) {
    const auto v = ReadU32(data, size, off, little);
    return v.has_value() ? std::optional<std::uint64_t>(*v) : std::nullopt;
  }
  return std::nullopt;
}

// Reads an ASCII tag (Make). Returns empty string when absent/unreadable.
std::string ReadTiffAscii(const TiffIfdEntry& e, const std::uint8_t* data,
                          std::size_t size) {
  if (e.type != 2 || !e.value_off.has_value()) return {};
  const std::size_t off = *e.value_off;
  if (off >= size) return {};
  const std::size_t n = std::min(e.value_size, size - off);
  if (n == 0) return {};
  std::string s(reinterpret_cast<const char*>(data + off), n);
  // Trim trailing NUL(s).
  while (!s.empty() && s.back() == '\0') s.pop_back();
  return s;
}

// Classifies a TIFF-container file: DNG (DNGVersion tag), CR2 (magic marker),
// NEF/ARW (vendor Make), else plain TIFF.
ImageFormat ClassifyTiffContainer(const std::uint8_t* data, std::size_t size) {
  const bool little = TiffLittleEndian(data);
  const auto ifd_off = ReadU32(data, size, 4, little);
  if (!ifd_off.has_value()) return ImageFormat::kTiff;

  // CR2 magic: bytes 8-9 == "CR".
  if (size >= 10 && data[8] == 'C' && data[9] == 'R') {
    return ImageFormat::kRaw;
  }

  std::string make;
  bool has_dng = false;
  ForEachTiffEntry(data, size, *ifd_off, little,
                   [&](const TiffIfdEntry& e) {
                     if (e.tag == kTagDngVersion) has_dng = true;
                     if (e.tag == kTagMake) make = ReadTiffAscii(e, data, size);
                     return true;
                   });
  if (has_dng) return ImageFormat::kRaw;  // DNG
  if (make.rfind("NIKON", 0) == 0 || make.rfind("SONY", 0) == 0 ||
      make.rfind("Canon", 0) == 0) {
    return ImageFormat::kRaw;
  }
  return ImageFormat::kTiff;
}

// ---------------------------------------------------------------------------
// Per-format header parsers.
// ---------------------------------------------------------------------------

std::optional<ImageHeaderInfo> ParseJpeg(const std::uint8_t* data,
                                         std::size_t size) {
  // Skip APPn/COM/other segments until a SOF marker (C0-CF minus C4/D8/D9/DA).
  std::size_t off = 2;  // past SOI
  std::optional<ImageHeaderInfo> out;
  while (off + 4 <= size) {
    if (data[off] != 0xFF) return std::nullopt;  // marker byte lost -> corrupt
    std::uint8_t marker = 0;
    while (off < size && data[off] == 0xFF) {
      ++off;
    }
    if (off >= size) return std::nullopt;
    marker = data[off];
    if (marker == 0x00) return std::nullopt;  // stuffed 0xFF not expected here
    ++off;
    if (marker == 0xD8 || marker == 0xD9 || marker == 0xDA ||
        (marker >= 0xD0 && marker <= 0xD7)) {
      continue;  // SOI/EOI/SOS/RST: no length to skip
    }
    const auto len = ReadU16(data, size, off, false);  // JPEG is big-endian
    if (!len.has_value() || *len < 2) return std::nullopt;
    const bool is_sof =
        (marker >= 0xC0 && marker <= 0xCF) && marker != 0xC4 && marker != 0xC8;
    if (is_sof) {
      if (off + 8 > size) return std::nullopt;
      // SOF payload: length(2) precision(1) height(2) width(2) components(1).
      const std::uint8_t precision = data[off + 2];
      const std::uint16_t height = static_cast<std::uint16_t>(
          (static_cast<std::uint16_t>(data[off + 3]) << 8) | data[off + 4]);
      const std::uint16_t width = static_cast<std::uint16_t>(
          (static_cast<std::uint16_t>(data[off + 5]) << 8) | data[off + 6]);
      const std::uint8_t components = data[off + 7];
      ImageHeaderInfo info;
      info.format = ImageFormat::kJpeg;
      info.width = width;
      info.height = height;
      info.mime_type = "image/jpeg";
      switch (components) {
        case 1:
          info.pixel_format = "gray" + std::to_string(precision);
          break;
        case 3:
          info.pixel_format = "rgb" + std::to_string(precision);
          break;
        case 4:
          info.pixel_format = "rgba" + std::to_string(precision);
          break;
        default:
          info.pixel_format = "rgb" + std::to_string(precision);
          break;
      }
      out = info;
      return out;
    }
    off += *len;  // skip this segment (len includes the 2 length bytes)
  }
  return out;  // no SOF found
}

std::optional<ImageHeaderInfo> ParsePng(const std::uint8_t* data,
                                        std::size_t size) {
  // Signature(8) + chunk header(len+type=8) + IHDR data(13) = 29.
  if (size < 29) return std::nullopt;
  // First chunk header at offset 8: length(4) type(4).
  const std::uint32_t len = static_cast<std::uint32_t>(
      (static_cast<std::uint32_t>(data[8]) << 24) |
      (static_cast<std::uint32_t>(data[9]) << 16) |
      (static_cast<std::uint32_t>(data[10]) << 8) | data[11]);
  const bool is_ihdr = data[12] == 'I' && data[13] == 'H' && data[14] == 'D' &&
                       data[15] == 'R';
  if (!is_ihdr || len < 13) return std::nullopt;
  // IHDR data at offset 16.
  if (size - 16 < 13) return std::nullopt;
  const std::uint32_t width = static_cast<std::uint32_t>(
      (static_cast<std::uint32_t>(data[16]) << 24) |
      (static_cast<std::uint32_t>(data[17]) << 16) |
      (static_cast<std::uint32_t>(data[18]) << 8) | data[19]);
  const std::uint32_t height = static_cast<std::uint32_t>(
      (static_cast<std::uint32_t>(data[20]) << 24) |
      (static_cast<std::uint32_t>(data[21]) << 16) |
      (static_cast<std::uint32_t>(data[22]) << 8) | data[23]);
  const std::uint8_t bit_depth = data[24];
  const std::uint8_t color_type = data[25];

  ImageHeaderInfo info;
  info.format = ImageFormat::kPng;
  info.width = width;
  info.height = height;
  info.mime_type = "image/png";
  switch (color_type) {
    case 0:
      info.pixel_format = "gray" + std::to_string(bit_depth);
      break;
    case 2:
      info.pixel_format = "rgb" + std::to_string(bit_depth);
      break;
    case 3:
      info.pixel_format = "palette" + std::to_string(bit_depth);
      break;
    case 4:
      info.pixel_format = "graya" + std::to_string(bit_depth);
      break;
    case 6:
      info.pixel_format = "rgba" + std::to_string(bit_depth);
      break;
    default:
      info.pixel_format = "unknown";
      break;
  }
  return info;
}

std::optional<ImageHeaderInfo> ParseTiff(const std::uint8_t* data,
                                         std::size_t size) {
  const bool little = TiffLittleEndian(data);
  const auto ifd_off = ReadU32(data, size, 4, little);
  if (!ifd_off.has_value()) return std::nullopt;

  std::uint64_t width = 0, height = 0, bits = 8, samples = 1;
  std::string make;
  bool have_width = false, have_height = false, has_dng = false;
  const bool is_cr2 = size >= 10 && data[8] == 'C' && data[9] == 'R';
  ForEachTiffEntry(data, size, *ifd_off, little, [&](const TiffIfdEntry& e) {
    if (e.tag == kTagImageWidth || e.tag == kTagImageLength) {
      const auto v = ReadTiffTagValue(e, data, size, little);
      if (v.has_value()) {
        if (e.tag == kTagImageWidth) {
          width = *v;
          have_width = true;
        } else {
          height = *v;
          have_height = true;
        }
      }
    } else if (e.tag == kTagBitsPerSample) {
      const auto v = ReadTiffTagValue(e, data, size, little);
      if (v.has_value()) bits = *v;
    } else if (e.tag == kTagSamplesPerPixel) {
      const auto v = ReadTiffTagValue(e, data, size, little);
      if (v.has_value()) samples = *v;
    } else if (e.tag == kTagMake) {
      make = ReadTiffAscii(e, data, size);
    } else if (e.tag == kTagDngVersion) {
      has_dng = true;
    }
    return true;
  });
  if (!have_width || !have_height || width == 0 || height == 0) {
    return std::nullopt;
  }

  ImageHeaderInfo info;
  info.width = static_cast<std::int64_t>(width);
  info.height = static_cast<std::int64_t>(height);
  if (is_cr2) {
    info.format = ImageFormat::kRaw;
    info.mime_type = "image/x-canon-cr2";
  } else if (has_dng) {
    info.format = ImageFormat::kRaw;
    info.mime_type = "image/x-adobe-dng";
  } else if (make.rfind("NIKON", 0) == 0) {
    info.format = ImageFormat::kRaw;
    info.mime_type = "image/x-nikon-nef";
  } else if (make.rfind("SONY", 0) == 0) {
    info.format = ImageFormat::kRaw;
    info.mime_type = "image/x-sony-arw";
  } else if (make.rfind("Canon", 0) == 0) {
    info.format = ImageFormat::kRaw;
    info.mime_type = "image/x-canon-cr2";
  } else {
    info.format = ImageFormat::kTiff;
    info.mime_type = "image/tiff";
  }
  switch (samples) {
    case 1:
      info.pixel_format = "gray" + std::to_string(bits);
      break;
    case 2:
      info.pixel_format = "graya" + std::to_string(bits);
      break;
    case 3:
      info.pixel_format = "rgb" + std::to_string(bits);
      break;
    case 4:
      info.pixel_format = "rgba" + std::to_string(bits);
      break;
    default:
      info.pixel_format = "rgb" + std::to_string(bits);
      break;
  }
  return info;
}

// EXR header: attribute sequence name\0 type\0 size(int32) data[size...],
// terminated by an empty name (single zero byte). We read dataWindow (box2i:
// xMin yMin xMax yMax as little-endian int32) and channels (chlist) for the
// sample type of the first channel.
std::optional<ImageHeaderInfo> ParseExr(const std::uint8_t* data,
                                        std::size_t size) {
  if (size < 8) return std::nullopt;
  std::size_t off = 8;  // past magic + version
  std::int64_t width = 0, height = 0;
  std::int64_t x_min = 0, y_min = 0;
  bool have_window = false;
  std::string sample_type;

  // Reads a NUL-terminated string at *off, advancing past it. Returns false
  // when the terminator is missing.
  auto read_cstring = [&](std::string& out) -> bool {
    std::size_t end = off;
    while (end < size && data[end] != '\0') ++end;
    if (end >= size) return false;
    out.assign(reinterpret_cast<const char*>(data + off), end - off);
    off = end + 1;
    return true;
  };

  while (off < size) {
    std::string name;
    if (!read_cstring(name)) return std::nullopt;
    if (name.empty()) break;  // end of header
    std::string type;
    if (!read_cstring(type)) return std::nullopt;
    const auto attr_size = ReadU32(data, size, off, true);
    if (!attr_size.has_value()) return std::nullopt;
    off += 4;
    if (off > size || *attr_size > size - off) return std::nullopt;
    const std::size_t data_off = off;

    if (name == "dataWindow" && type == "box2i" && *attr_size >= 16) {
      const auto x1 = ReadU32(data, size, data_off, true);
      const auto y1 = ReadU32(data, size, data_off + 4, true);
      const auto x2 = ReadU32(data, size, data_off + 8, true);
      const auto y2 = ReadU32(data, size, data_off + 12, true);
      if (x1.has_value() && y1.has_value() && x2.has_value() && y2.has_value()) {
        // box2i stores int32; negative offsets are unusual but legal. Re-read
        // as signed by reinterpreting the 32-bit value.
        x_min = static_cast<std::int32_t>(*x1);
        y_min = static_cast<std::int32_t>(*y1);
        const std::int32_t x_max = static_cast<std::int32_t>(*x2);
        const std::int32_t y_max = static_cast<std::int32_t>(*y2);
        width = static_cast<std::int64_t>(x_max) - x_min + 1;
        height = static_cast<std::int64_t>(y_max) - y_min + 1;
        have_window = true;
      }
    } else if (name == "channels" && type == "chlist") {
      // First channel record: name\0 pixelType(int32) pLinear(uint8)
      // reserved(3) xSampling(int32) ySampling(int32).
      std::size_t p = data_off;
      const std::size_t end = data_off + *attr_size;
      while (p < end) {
        std::size_t cname_end = p;
        while (cname_end < end && data[cname_end] != '\0') ++cname_end;
        if (cname_end >= end) break;
        if (cname_end == p) break;  // empty channel name ends the list
        p = cname_end + 1;
        const auto pt = ReadU32(data, end, p, true);
        if (pt.has_value()) {
          switch (*pt) {
            case 0:
              sample_type = "uint32";
              break;
            case 1:
              sample_type = "float16";
              break;
            case 2:
              sample_type = "float32";
              break;
            default:
              sample_type = "unknown";
              break;
          }
        }
        break;  // only the first channel's sample type is needed
      }
    }

    off = data_off + *attr_size;
  }

  if (!have_window || width <= 0 || height <= 0) return std::nullopt;

  ImageHeaderInfo info;
  info.format = ImageFormat::kExr;
  info.width = width;
  info.height = height;
  info.mime_type = "image/x-exr";
  info.pixel_format = sample_type.empty() ? "unknown" : sample_type;
  return info;
}

}  // namespace

ImageFormat DetectImageFormat(const std::uint8_t* data, std::size_t size) {
  if (data == nullptr || size == 0) return ImageFormat::kUnknown;
  if (IsJpeg(data, size)) return ImageFormat::kJpeg;
  if (IsPng(data, size)) return ImageFormat::kPng;
  if (IsExr(data, size)) return ImageFormat::kExr;
  if (IsTiff(data, size)) return ClassifyTiffContainer(data, size);
  return ImageFormat::kUnknown;
}

std::optional<ImageHeaderInfo> ParseImageHeader(const std::uint8_t* data,
                                                std::size_t size) {
  if (data == nullptr || size == 0) return std::nullopt;
  if (size > kMaxHeaderSize) size = kMaxHeaderSize;
  const ImageFormat fmt = DetectImageFormat(data, size);
  switch (fmt) {
    case ImageFormat::kJpeg:
      return ParseJpeg(data, size);
    case ImageFormat::kPng:
      return ParsePng(data, size);
    case ImageFormat::kTiff:
    case ImageFormat::kRaw:
      return ParseTiff(data, size);
    case ImageFormat::kExr:
      return ParseExr(data, size);
    default:
      return std::nullopt;
  }
}

std::string ToString(ImageFormat format) {
  switch (format) {
    case ImageFormat::kUnknown:
      return "unknown";
    case ImageFormat::kJpeg:
      return "jpeg";
    case ImageFormat::kPng:
      return "png";
    case ImageFormat::kTiff:
      return "tiff";
    case ImageFormat::kExr:
      return "exr";
    case ImageFormat::kRaw:
      return "raw";
  }
  return "unknown";
}

}  // namespace spatial::importers
