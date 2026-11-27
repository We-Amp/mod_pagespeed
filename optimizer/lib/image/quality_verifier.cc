// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - SSIMULACRA2 Perceptual Quality Verification

#include "lib/image/quality_verifier.h"

#include <cstddef>
#include <cstring>
#include <vector>

#include "lib/base/message_handler.h"
#include "lib/extras/codestream_header.h"
#include "lib/extras/packed_image.h"
#include "tools/ssimulacra2.h"

namespace pagespeed {

namespace {

// Build a PackedPixelFile from raw sRGB uint8 pixels.
// Returns false on failure.
bool BuildPPF(const uint8_t* pixels, uint32_t width, uint32_t height, int bpp,
              jxl::extras::PackedPixelFile* ppf) {
  // Only RGB (3) and RGBA (4) are supported by SSIMULACRA2.
  // For grayscale (1), we expand to RGB.
  uint32_t num_channels = (bpp == 1) ? 3 : static_cast<uint32_t>(bpp);
  bool has_alpha = (bpp == 4);

  JxlPixelFormat format;
  format.num_channels = num_channels;
  format.data_type = JXL_TYPE_UINT8;
  format.endianness = JXL_NATIVE_ENDIAN;
  format.align = 0;

  auto image_or = jxl::extras::PackedImage::Create(width, height, format);
  if (!image_or.ok()) return false;
  auto image = std::move(image_or).value_();

  if (bpp == 1) {
    // Expand grayscale to RGB.
    auto* dst = static_cast<uint8_t*>(image.pixels());
    for (uint32_t y = 0; y < height; ++y) {
      for (uint32_t x = 0; x < width; ++x) {
        size_t src_idx = static_cast<size_t>(y) * width + x;
        uint8_t gray = pixels[src_idx];
        size_t offset = src_idx * 3;
        dst[offset + 0] = gray;
        dst[offset + 1] = gray;
        dst[offset + 2] = gray;
      }
    }
  } else {
    size_t total = static_cast<size_t>(width) * height * bpp;
    memcpy(image.pixels(), pixels, total);
  }

  // Set up basic info.
  memset(&ppf->info, 0, sizeof(ppf->info));
  ppf->info.xsize = width;
  ppf->info.ysize = height;
  ppf->info.bits_per_sample = 8;
  ppf->info.exponent_bits_per_sample = 0;
  ppf->info.num_color_channels = 3;
  ppf->info.num_extra_channels = has_alpha ? 1 : 0;
  ppf->info.alpha_bits = has_alpha ? 8 : 0;

  // sRGB color encoding (set manually since JxlColorEncodingSetToSRGB
  // may not be available as a free function).
  memset(&ppf->color_encoding, 0, sizeof(ppf->color_encoding));
  ppf->color_encoding.color_space = JXL_COLOR_SPACE_RGB;
  ppf->color_encoding.white_point = JXL_WHITE_POINT_D65;
  ppf->color_encoding.primaries = JXL_PRIMARIES_SRGB;
  ppf->color_encoding.transfer_function = JXL_TRANSFER_FUNCTION_SRGB;
  ppf->color_encoding.rendering_intent = JXL_RENDERING_INTENT_PERCEPTUAL;

  // Add the frame.
  ppf->frames.clear();
  jxl::extras::PackedFrame frame(std::move(image));
  ppf->frames.push_back(std::move(frame));

  return true;
}

}  // namespace

std::optional<float> ComputeSSIMULACRA2(const uint8_t* original,
                                        const uint8_t* encoded, uint32_t width,
                                        uint32_t height, int bpp,
                                        MessageHandler* handler) {
  if (original == nullptr || encoded == nullptr) {
    if (handler != nullptr) {
      handler->Warning("SSIMULACRA2: null pixel buffer");
    }
    return std::nullopt;
  }
  if (width == 0 || height == 0) {
    if (handler != nullptr) {
      handler->Warning("SSIMULACRA2: zero dimensions");
    }
    return std::nullopt;
  }
  if (bpp != 1 && bpp != 3 && bpp != 4) {
    if (handler != nullptr) {
      handler->Warning("SSIMULACRA2: unsupported bpp=%d", bpp);
    }
    return std::nullopt;
  }

  // SSIMULACRA2 requires at least 8x8 pixels.
  if (width < 8 || height < 8) {
    if (handler != nullptr) {
      handler->Info("SSIMULACRA2: image too small (%ux%u), skipping", width,
                    height);
    }
    return std::nullopt;
  }

  jxl::extras::PackedPixelFile orig_ppf, dist_ppf;

  if (!BuildPPF(original, width, height, bpp, &orig_ppf)) {
    if (handler != nullptr) {
      handler->Warning("SSIMULACRA2: failed to build original PPF");
    }
    return std::nullopt;
  }

  if (!BuildPPF(encoded, width, height, bpp, &dist_ppf)) {
    if (handler != nullptr) {
      handler->Warning("SSIMULACRA2: failed to build distorted PPF");
    }
    return std::nullopt;
  }

  // Call the global ::ComputeSSIMULACRA2 from jpegli tools.
  auto result = ::ComputeSSIMULACRA2(orig_ppf, dist_ppf);
  if (!result.ok()) {
    if (handler != nullptr) {
      handler->Warning("SSIMULACRA2: computation failed");
    }
    return std::nullopt;
  }

  return static_cast<float>(std::move(result).value_().Score());
}

}  // namespace pagespeed
