#pragma once

// Image file format detection and header-only geometry (RFC-0006 §6.8,
// image-import.md §14). The importer is the only place that touches a source
// file; header parsing lives here with no external image library (PPS-0001
// §5.1). Width/height/pixel_format come from JPEG SOF, PNG IHDR, TIFF IFD, and
// OpenEXR attributes. RAW families (CR2/NEF/ARW/DNG, all TIFF containers) are
// detected and their IFD geometry is read header-only, but the payload stays
// an opaque byte copy - never decoded at import (image-import.md §14).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace spatial::importers {

enum class ImageFormat {
  kUnknown,
  kJpeg,
  kPng,
  kTiff,
  kExr,
  kRaw,  // opaque payload; geometry read header-only from the TIFF IFD
};

std::string ToString(ImageFormat format);

struct ImageHeaderInfo {
  ImageFormat format = ImageFormat::kUnknown;
  std::int64_t width = 0;
  std::int64_t height = 0;
  // rgb8 | gray8 | rgba16 | float16 | float32 | gray12 | ... : channel layout
  // + bits/sample from the container header, never from a decode.
  std::string pixel_format;
  // image/jpeg | image/png | image/tiff | image/x-exr | image/x-canon-cr2 |
  // image/x-nikon-nef | image/x-sony-arw | image/x-adobe-dng
  std::string mime_type;
};

// Detects the format from magic bytes; returns kUnknown for unrecognized data.
ImageFormat DetectImageFormat(const std::uint8_t* data, std::size_t size);

// Parses header-only geometry for JPEG/PNG/TIFF/EXR (RAW included, via TIFF
// IFD). Returns nullopt when the header is truncated, corrupt, or empty.
// Never decodes pixels.
std::optional<ImageHeaderInfo> ParseImageHeader(const std::uint8_t* data,
                                                std::size_t size);

}  // namespace spatial::importers
