// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// In-memory PNG helpers for tests that feed the visual-regression path.
// Extracted from visual_regression_gate_test.cc so the critical-CSS validator
// tests can build the same synthetic screenshots without a second copy.

#ifndef TEST_TEST_UTIL_PNG_IMAGE_H_
#define TEST_TEST_UTIL_PNG_IMAGE_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "png.h"  // NOLINT

namespace pagespeed {
namespace test {

// Encode RGBA pixel data into a PNG in memory.  Returns {} on failure.
inline std::vector<uint8_t> EncodePng(const std::vector<uint8_t>& rgba,
                                      uint32_t width, uint32_t height) {
  std::vector<uint8_t> output;

  png_structp png =
      png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  if (!png) return {};

  png_infop info = png_create_info_struct(png);
  if (!info) {
    png_destroy_write_struct(&png, nullptr);
    return {};
  }

  if (setjmp(png_jmpbuf(png))) {
    png_destroy_write_struct(&png, &info);
    return {};
  }

  struct WriteState {
    std::vector<uint8_t>* vec;
  };
  WriteState state{&output};

  png_set_write_fn(
      png, &state,
      [](png_structp p, png_bytep data, png_size_t len) {
        auto* s = static_cast<WriteState*>(png_get_io_ptr(p));
        s->vec->insert(s->vec->end(), data, data + len);
      },
      [](png_structp) {});

  png_set_IHDR(png, info, width, height, 8, PNG_COLOR_TYPE_RGBA,
               PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
               PNG_FILTER_TYPE_DEFAULT);

  png_write_info(png, info);

  std::vector<png_bytep> row_ptrs(height);
  for (uint32_t y = 0; y < height; ++y) {
    row_ptrs[y] =
        const_cast<png_bytep>(rgba.data() + static_cast<size_t>(y) * width * 4);
  }

  png_write_image(png, row_ptrs.data());
  png_write_end(png, nullptr);
  png_destroy_write_struct(&png, &info);

  return output;
}

// Solid-color RGBA image.
inline std::vector<uint8_t> MakeSolidImage(uint32_t width, uint32_t height,
                                           uint8_t r, uint8_t g, uint8_t b,
                                           uint8_t a = 255) {
  std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
  for (size_t i = 0; i < pixels.size(); i += 4) {
    pixels[i] = r;
    pixels[i + 1] = g;
    pixels[i + 2] = b;
    pixels[i + 3] = a;
  }
  return pixels;
}

}  // namespace test
}  // namespace pagespeed

#endif  // TEST_TEST_UTIL_PNG_IMAGE_H_
