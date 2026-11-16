/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "pagespeed/kernel/image/image_distortion.h"

#include <algorithm>
#include <cmath>
#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/image/image_util.h"
#include "pagespeed/kernel/image/png_optimizer.h"
#include "pagespeed/kernel/image/webp_optimizer.h"

namespace pagespeed {

namespace image_compression {

namespace {

// Folds one finished band of tiles (tile_rows scanlines) into the minimum.
void ScoreBand(const std::vector<double>& sums, size_t width, size_t tile,
               size_t tile_rows, double* worst_db, bool* scored) {
  const size_t min_pixels = (tile * tile) / 4;
  for (size_t tx = 0; tx * tile < width; ++tx) {
    const size_t tile_cols = std::min(tile, width - tx * tile);
    if (tile_cols * tile_rows < min_pixels) {
      continue;
    }
    const double mse =
        sums[tx] / (static_cast<double>(tile_cols) * tile_rows * 3.0);
    const double db = mse == 0.0 ? kIdenticalTilePsnrDb
                                 : 10.0 * std::log10(255.0 * 255.0 / mse);
    if (!*scored || db < *worst_db) {
      *worst_db = db;
      *scored = true;
    }
  }
}

}  // namespace

bool WorstTilePsnr(const PngReaderInterface& png_reader,
                   const GoogleString& png, const GoogleString& webp, int tile,
                   double* worst_db, MessageHandler* handler) {
  if (tile < 2 || worst_db == nullptr) {
    return false;
  }

  // Decode the source the way ImageConverter::ConvertPngToWebp does, so that
  // the pixels compared are the pixels the encoder was given.
  PngScanlineReader source(handler);
  source.set_transform(PNG_TRANSFORM_EXPAND | PNG_TRANSFORM_STRIP_16 |
                       PNG_TRANSFORM_GRAY_TO_RGB);
  source.set_require_opaque(true);
  if (setjmp(*source.GetJmpBuf())) {
    return false;
  }
  if (!source.InitializeRead(png_reader, png)) {
    return false;
  }

  WebpScanlineReader decoded(handler);
  if (!decoded.Initialize(webp.data(), webp.size())) {
    return false;
  }

  const size_t width = source.GetImageWidth();
  const size_t height = source.GetImageHeight();
  if (width == 0 || height == 0 || width != decoded.GetImageWidth() ||
      height != decoded.GetImageHeight() ||
      source.GetPixelFormat() != RGB_888 ||
      decoded.GetPixelFormat() != RGB_888) {
    return false;
  }

  const size_t tile_size = static_cast<size_t>(tile);
  std::vector<double> sums((width + tile_size - 1) / tile_size, 0.0);
  double worst = 0.0;
  bool scored = false;
  size_t rows_in_band = 0;
  for (size_t y = 0; y < height; ++y) {
    void* source_row = nullptr;
    void* decoded_row = nullptr;
    if (!source.ReadNextScanline(&source_row) ||
        !decoded.ReadNextScanline(&decoded_row)) {
      return false;
    }
    const uint8_t* a = static_cast<const uint8_t*>(source_row);
    const uint8_t* b = static_cast<const uint8_t*>(decoded_row);
    for (size_t tx = 0; tx * tile_size < width; ++tx) {
      const size_t x_end = std::min(width, (tx + 1) * tile_size);
      double row_sum = 0.0;
      for (size_t i = tx * tile_size * 3; i < x_end * 3; ++i) {
        const double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
        row_sum += d * d;
      }
      sums[tx] += row_sum;
    }
    if (++rows_in_band == tile_size || y + 1 == height) {
      ScoreBand(sums, width, tile_size, rows_in_band, &worst, &scored);
      std::fill(sums.begin(), sums.end(), 0.0);
      rows_in_band = 0;
    }
  }

  if (!scored) {
    return false;
  }
  *worst_db = worst;
  return true;
}

}  // namespace image_compression

}  // namespace pagespeed
