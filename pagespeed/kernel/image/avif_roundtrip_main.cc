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

// the design record Stream B/C build-validation harness. NOT a shipping target.
//
// 1. Encodes a representative RGB image to AVIF through the real read_image
//    factory + AvifFrameWriter, and times the aom still-image encode.
// 2. Decodes that AVIF back through AvifFrameReader and checks the pixels
//    round-trip (dimensions + a mean-abs-error bound), proving real bytes flow.
// 3. Encodes the same image to WebP (lossy, comparable quality) for a size
//    comparison -- the plan's blocking encode-speed / size GO-NO-GO gate.
//
// Usage: avif_roundtrip_main [width height] [quality] [speed] [max_threads]
//
// max_threads defaults to 1 to MATCH PRODUCTION. Nothing on the serving path
// sets AvifConfiguration::max_threads, so it keeps its default of 1 and every
// production still-image encode is single-threaded. A multi-threaded
// measurement here would understate real encode cost by the parallel speedup,
// so anything calibrated from this harness (e.g. the encode-cost estimate that
// backs the still-image admission guard in avif_optimizer.cc) must be read off
// a max_threads=1 run. The thread count is printed with every result so a
// number can never be quoted without its threading assumption attached.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <vector>

#include "pagespeed/kernel/base/google_message_handler.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/image/avif_optimizer.h"
#include "pagespeed/kernel/image/read_image.h"
#include "pagespeed/kernel/image/scanline_interface.h"
#include "pagespeed/kernel/image/scanline_status.h"
#include "pagespeed/kernel/image/webp_optimizer.h"

namespace {

using net_instaweb::GoogleMessageHandler;
using pagespeed::image_compression::AvifConfiguration;
using pagespeed::image_compression::CreateScanlineWriter;
using pagespeed::image_compression::IMAGE_AVIF;
using pagespeed::image_compression::IMAGE_WEBP;
using pagespeed::image_compression::RGB_888;
using pagespeed::image_compression::ScanlineReaderInterface;
using pagespeed::image_compression::ScanlineStatus;
using pagespeed::image_compression::ScanlineWriterInterface;
using pagespeed::image_compression::WebpConfiguration;

double NowMs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

// Build a deterministic photograph-like RGB image: a smooth two-axis gradient,
// a mid-frequency sinusoidal texture, and fine pseudo-random noise. This gives
// aom real, compressible-but-detailed content (unlike a flat test pattern that
// would understate encode time and overstate compression).
GoogleString MakePhotoRgb(int w, int h) {
  GoogleString out;
  out.resize(static_cast<size_t>(w) * h * 3);
  uint8_t* p = reinterpret_cast<uint8_t*>(&out[0]);
  uint32_t rng = 0x1234567u;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      rng = rng * 1664525u + 1013904223u;
      const double fx = static_cast<double>(x) / w;
      const double fy = static_cast<double>(y) / h;
      const double tex = 0.5 + 0.5 * std::sin(fx * 40.0) * std::cos(fy * 33.0);
      const int noise = static_cast<int>((rng >> 24) & 0x1f) - 16;
      int r = static_cast<int>(255 * (0.2 + 0.6 * fx) + 30 * tex) + noise;
      int g = static_cast<int>(255 * (0.3 + 0.5 * fy) + 25 * tex) + noise;
      int b = static_cast<int>(255 * (0.5 + 0.4 * (1 - fx * fy)) + 20 * tex) +
              noise;
      p[0] = static_cast<uint8_t>(r < 0 ? 0 : (r > 255 ? 255 : r));
      p[1] = static_cast<uint8_t>(g < 0 ? 0 : (g > 255 ? 255 : g));
      p[2] = static_cast<uint8_t>(b < 0 ? 0 : (b > 255 ? 255 : b));
      p += 3;
    }
  }
  return out;
}

// Encodes RGB_888 pixels to 'format' via the real scanline factory. Returns the
// encoded bytes (empty on failure) and sets *ms to the wall-clock encode time.
GoogleString Encode(pagespeed::image_compression::ImageFormat format,
                    const void* config, const GoogleString& rgb, int w, int h,
                    GoogleMessageHandler* handler, double* ms) {
  GoogleString encoded;
  ScanlineStatus status;
  std::unique_ptr<ScanlineWriterInterface> writer(CreateScanlineWriter(
      format, RGB_888, w, h, config, &encoded, handler, &status));
  if (writer == nullptr || !status.Success()) {
    fprintf(stderr, "encode: writer init failed: %s\n",
            status.ToString().c_str());
    return GoogleString();
  }
  const uint8_t* row = reinterpret_cast<const uint8_t*>(rgb.data());
  const size_t row_bytes = static_cast<size_t>(w) * 3;
  const double t0 = NowMs();
  for (int y = 0; y < h; ++y) {
    if (!writer->WriteNextScanline(row)) {
      fprintf(stderr, "encode: WriteNextScanline failed at row %d\n", y);
      return GoogleString();
    }
    row += row_bytes;
  }
  if (!writer->FinalizeWrite()) {
    fprintf(stderr, "encode: FinalizeWrite failed\n");
    return GoogleString();
  }
  *ms = NowMs() - t0;
  return encoded;
}

}  // namespace

int main(int argc, char** argv) {
  int w = 1280;
  int h = 854;
  int quality = 55;
  int speed = 6;
  // Default 1 == the production default (AvifConfiguration::max_threads); see
  // the file header. Do not raise this to "make the benchmark look good".
  int max_threads = 1;
  if (argc >= 3) {
    w = atoi(argv[1]);
    h = atoi(argv[2]);
  }
  if (argc >= 4) {
    quality = atoi(argv[3]);
  }
  if (argc >= 5) {
    speed = atoi(argv[4]);
  }
  if (argc >= 6) {
    max_threads = atoi(argv[5]);
  }
  if (max_threads < 1) {
    max_threads = 1;
  }

  GoogleMessageHandler handler;
  const GoogleString rgb = MakePhotoRgb(w, h);
  const double megapixels = (static_cast<double>(w) * h) / 1.0e6;
  printf("== AVIF round-trip + encode-speed gate ==\n");
  printf(
      "source: %dx%d RGB (%.2f Mpx), avif quality=%d speed=%d "
      "max_threads=%d%s\n",
      w, h, megapixels, quality, speed, max_threads,
      max_threads == 1 ? " (production default)"
                       : " (NOT production: "
                         "production encodes with 1 "
                         "thread)");

  // ---- AVIF encode (timed) ----
  AvifConfiguration avif_config;
  avif_config.lossless = 0;
  avif_config.quality = quality;
  avif_config.speed = speed;
  avif_config.max_threads = max_threads;
  double avif_ms = 0;
  const GoogleString avif =
      Encode(IMAGE_AVIF, &avif_config, rgb, w, h, &handler, &avif_ms);
  if (avif.empty()) {
    fprintf(stderr, "FAIL: AVIF encode produced no bytes\n");
    return 2;
  }
  // The thread count is repeated on the result line itself: this is the line
  // that gets quoted, and a ms/Mpx figure without its thread count is a
  // number that will be misread.
  printf(
      "AVIF encode: %.1f ms -> %zu bytes (%.0f ms/Mpx @ speed=%d "
      "max_threads=%d)\n",
      avif_ms, avif.size(), avif_ms / megapixels, speed, max_threads);

  // ---- AVIF decode / round-trip check ----
  ScanlineStatus status;
  std::unique_ptr<ScanlineReaderInterface> reader(CreateScanlineReader(
      IMAGE_AVIF, avif.data(), avif.size(), &handler, &status));
  if (reader == nullptr || !status.Success()) {
    fprintf(stderr, "FAIL: AVIF decode init: %s\n", status.ToString().c_str());
    return 3;
  }
  if (static_cast<int>(reader->GetImageWidth()) != w ||
      static_cast<int>(reader->GetImageHeight()) != h) {
    fprintf(stderr, "FAIL: decoded dims %zux%zu != %dx%d\n",
            reader->GetImageWidth(), reader->GetImageHeight(), w, h);
    return 3;
  }
  // Compare decoded pixels to the source (lossy => allow a mean-abs-error bound).
  double sum_abs = 0;
  size_t count = 0;
  const uint8_t* src = reinterpret_cast<const uint8_t*>(rgb.data());
  int y = 0;
  while (reader->HasMoreScanLines()) {
    void* scan = nullptr;
    if (!reader->ReadNextScanline(&scan)) {
      fprintf(stderr, "FAIL: ReadNextScanline at row %d\n", y);
      return 3;
    }
    const uint8_t* d = reinterpret_cast<const uint8_t*>(scan);
    const uint8_t* s = src + static_cast<size_t>(y) * w * 3;
    for (int i = 0; i < w * 3; ++i) {
      sum_abs += std::abs(static_cast<int>(d[i]) - static_cast<int>(s[i]));
      ++count;
    }
    ++y;
  }
  const double mae = count ? sum_abs / count : 999;
  printf("AVIF decode: %d rows, mean-abs-error=%.2f/255\n", y, mae);
  if (y != h) {
    fprintf(stderr, "FAIL: decoded %d rows != %d\n", y, h);
    return 3;
  }
  if (mae > 12.0) {
    fprintf(stderr, "FAIL: round-trip MAE %.2f too high (garbled pixels)\n",
            mae);
    return 3;
  }

  // ---- WebP encode for size comparison ----
  WebpConfiguration webp_config;
  webp_config.lossless = 0;
  webp_config.quality = 75;  // libwebp default-ish lossy quality.
  webp_config.method = 4;
  double webp_ms = 0;
  const GoogleString webp =
      Encode(IMAGE_WEBP, &webp_config, rgb, w, h, &handler, &webp_ms);
  if (webp.empty()) {
    fprintf(stderr, "WARN: WebP encode failed; size comparison skipped\n");
  } else {
    printf("WebP encode: %.1f ms -> %zu bytes (q75)\n", webp_ms, webp.size());
    printf("SIZE: avif=%zu webp=%zu  avif/webp=%.2fx\n", avif.size(),
           webp.size(),
           static_cast<double>(avif.size()) / static_cast<double>(webp.size()));
  }

  printf("ROUND-TRIP OK: AVIF encode+decode succeeded with real pixels.\n");
  return 0;
}
