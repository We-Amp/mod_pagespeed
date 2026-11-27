// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Image Dimension Reader Implementation

#include "lib/image/image_dimensions.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "lib/base/message_handler.h"
#include "lib/image/image_analysis.h"
#include "lib/image/image_util.h"

namespace pagespeed {
namespace {

// The codecs report errors through PS_LOGGED_STATUS / PS_LOG_*, which
// dereference the handler UNCONDITIONALLY (lib/base/message_handler.h), and
// the GIF reader additionally asserts it is non-null.  Passing nullptr here
// therefore turned every codec error path -- a bad IHDR CRC, an invalid bit
// depth or color type, a zero width, a truncated PNG or JPEG -- into a
// SIGSEGV, and a perfectly valid GIF into a SIGABRT in assert-enabled builds
// (the GIF reader's non-null assert compiles out under NDEBUG; the malformed-
// input SIGSEGV class was live in all build modes).  We only want the
// dimensions, so discard the messages rather than suppress the handler.
MessageHandler& DimensionHandler() {
  static NullMessageHandler handler;
  return handler;
}

}  // namespace

ImageDimensions ReadImageDimensions(std::span<const uint8_t> data) {
  ImageDimensions result;
  if (data.empty()) return result;

  // Detect image format from magic bytes.
  std::string_view buf(reinterpret_cast<const char*>(data.data()), data.size());
  net_instaweb::ImageType image_type = net_instaweb::ComputeImageType(buf);
  if (image_type == net_instaweb::IMAGE_UNKNOWN) return result;

  // Map net_instaweb::ImageType to image_compression::ImageFormat.
  image_compression::ImageFormat format;
  switch (image_type) {
    case net_instaweb::IMAGE_JPEG:
      format = image_compression::IMAGE_JPEG;
      break;
    case net_instaweb::IMAGE_PNG:
      format = image_compression::IMAGE_PNG;
      break;
    case net_instaweb::IMAGE_GIF:
      format = image_compression::IMAGE_GIF;
      break;
    case net_instaweb::IMAGE_WEBP:
    case net_instaweb::IMAGE_WEBP_LOSSLESS_OR_ALPHA:
    case net_instaweb::IMAGE_WEBP_ANIMATED:
      format = image_compression::IMAGE_WEBP;
      break;
    default:
      return result;
  }

  int width = 0;
  int height = 0;

  // AnalyzeImage reads only headers for width/height — passing
  // nullptr for all other output parameters avoids unnecessary work.
  bool ok = image_compression::AnalyzeImage(
      format, data.data(), data.size(), &width, &height,
      /*is_progressive=*/nullptr, /*is_animated=*/nullptr,
      /*has_transparency=*/nullptr, /*is_photo=*/nullptr,
      /*quality=*/nullptr, /*reader=*/nullptr,
      /*handler=*/&DimensionHandler());

  if (ok && width > 0 && height > 0) {
    result.width = width;
    result.height = height;
    result.valid = true;
  }

  return result;
}

}  // namespace pagespeed
