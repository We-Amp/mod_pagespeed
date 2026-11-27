// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// mod_pagespeed — Quality Sweep Tool
//
// For each (image, format, quality): encode → decode → ComputeSSIMULACRA2().
// Outputs sweep_results.csv for training quality prediction models.
//
// Usage: quality_sweep --manifest manifest.csv --output sweep_results.csv
//                      [--threads N] [--formats jpeg,webp,avif]

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "avif/avif.h"
#include "lib/base/message_handler.h"
#include "lib/image/image_util.h"
#include "lib/image/jpeg_optimizer.h"
#include "lib/image/quality_verifier.h"
#include "lib/image/read_image.h"
#include "lib/image/scanline_interface.h"
#include "lib/image/scanline_utils.h"
#include "lib/image/webp_optimizer.h"

namespace ic = pagespeed::image_compression;

namespace {

struct Args {
  std::string manifest;
  std::string output;
  int threads = 4;
  bool do_jpeg = true;
  bool do_webp = true;
  bool do_avif = false;
};

bool ParseArgs(int argc, char** argv, Args& args) {
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--manifest") == 0 && i + 1 < argc) {
      args.manifest = argv[++i];
    } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
      args.output = argv[++i];
    } else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
      args.threads = atoi(argv[++i]);
      if (args.threads < 1) args.threads = 1;
    } else if (strcmp(argv[i], "--formats") == 0 && i + 1 < argc) {
      std::string fmts = argv[++i];
      args.do_jpeg = fmts.find("jpeg") != std::string::npos;
      args.do_webp = fmts.find("webp") != std::string::npos;
      args.do_avif = fmts.find("avif") != std::string::npos;
    } else if (strcmp(argv[i], "--help") == 0) {
      fprintf(stderr,
              "Usage: quality_sweep --manifest manifest.csv "
              "--output sweep.csv [--threads N] [--formats jpeg,webp,avif]\n");
      return false;
    }
  }
  if (args.manifest.empty() || args.output.empty()) {
    fprintf(stderr,
            "Usage: quality_sweep --manifest manifest.csv "
            "--output sweep.csv [--threads N] [--formats jpeg,webp,avif]\n");
    return false;
  }
  return true;
}

std::string ReadFileContents(const std::string& path) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return {};
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  if (sz <= 0) {
    fclose(f);
    return {};
  }
  fseek(f, 0, SEEK_SET);
  std::string buf(static_cast<size_t>(sz), '\0');
  size_t nread = fread(buf.data(), 1, buf.size(), f);
  fclose(f);
  buf.resize(nread);
  return buf;
}

std::vector<std::string> SplitCsv(std::string_view line) {
  std::vector<std::string> fields;
  size_t pos = 0;
  while (pos <= line.size()) {
    auto next = line.find(',', pos);
    if (next == std::string_view::npos) {
      fields.emplace_back(line.substr(pos));
      break;
    }
    fields.emplace_back(line.substr(pos, next - pos));
    pos = next + 1;
  }
  return fields;
}

ic::ImageFormat CodeToFormat(int code) {
  switch (code) {
    case 0:
      return ic::IMAGE_JPEG;
    case 1:
      return ic::IMAGE_PNG;
    case 2:
      return ic::IMAGE_GIF;
    case 3:
      return ic::IMAGE_WEBP;
    default:
      return ic::IMAGE_UNKNOWN;
  }
}

struct ManifestEntry {
  std::string path;
  int source_format;
};

// Quality step ranges per format. JPEG: 1-100, WebP: 0-100.
// Adaptive: steep region gets fine steps, flat regions get coarse steps.
struct QualityRange {
  int start;
  int end;
  int step;
};

std::vector<int> GetJpegQualities() {
  std::vector<int> q;
  // Coarse: 1-29 step 5
  for (int i = 1; i < 30; i += 5) q.push_back(i);
  // Fine: 30-90 step 2
  for (int i = 30; i <= 90; i += 2) q.push_back(i);
  // Coarse: 91-100 step 3
  for (int i = 93; i <= 100; i += 3) q.push_back(i);
  return q;
}

std::vector<int> GetWebpQualities() {
  std::vector<int> q;
  // Coarse: 0-29 step 5
  for (int i = 0; i < 30; i += 5) q.push_back(i);
  // Fine: 30-90 step 2
  for (int i = 30; i <= 90; i += 2) q.push_back(i);
  // Coarse: 91-100 step 3
  for (int i = 93; i <= 100; i += 3) q.push_back(i);
  return q;
}

std::vector<int> GetAvifQualities() {
  std::vector<int> q;
  // Coarse: 0-29 step 5
  for (int i = 0; i < 30; i += 5) q.push_back(i);
  // Fine: 30-70 step 2
  for (int i = 30; i <= 70; i += 2) q.push_back(i);
  // Coarse: 71-100 step 5
  for (int i = 75; i <= 100; i += 5) q.push_back(i);
  return q;
}

struct SweepResult {
  std::string path;
  std::string format;
  int quality;
  float ssimulacra2;
  size_t encoded_size;
};

// Encode pixels to JPEG at given quality, return encoded data.
std::string EncodeJpeg(const uint8_t* pixels, size_t w, size_t h,
                       ic::PixelFormat pixel_format, int quality,
                       pagespeed::MessageHandler* handler) {
  ic::JpegCompressionOptions opts;
  opts.lossy = true;
  opts.lossy_options.quality = quality;
  opts.progressive = (w * h > 10000);

  std::string output;
  std::unique_ptr<ic::ScanlineWriterInterface> writer(ic::CreateScanlineWriter(
      ic::IMAGE_JPEG, pixel_format, w, h, &opts, &output, handler));
  if (!writer) return {};

  int bpp = static_cast<int>(
      ic::GetNumChannelsFromPixelFormat(pixel_format, handler));
  for (size_t y = 0; y < h; ++y) {
    const void* scanline = pixels + y * w * bpp;
    if (!writer->WriteNextScanline(scanline)) return {};
  }
  if (!writer->FinalizeWrite()) return {};
  return output;
}

// Encode pixels to lossy WebP at given quality, return encoded data.
std::string EncodeWebp(const uint8_t* pixels, size_t w, size_t h,
                       ic::PixelFormat pixel_format, int quality,
                       pagespeed::MessageHandler* handler) {
  ic::WebpConfiguration opts;
  opts.lossless = 0;
  opts.quality = static_cast<float>(quality);
  opts.method = 4;

  std::string output;
  std::unique_ptr<ic::ScanlineWriterInterface> writer(ic::CreateScanlineWriter(
      ic::IMAGE_WEBP, pixel_format, w, h, &opts, &output, handler));
  if (!writer) return {};

  int bpp = static_cast<int>(
      ic::GetNumChannelsFromPixelFormat(pixel_format, handler));
  for (size_t y = 0; y < h; ++y) {
    const void* scanline = pixels + y * w * bpp;
    if (!writer->WriteNextScanline(scanline)) return {};
  }
  if (!writer->FinalizeWrite()) return {};
  return output;
}

// Encode pixels to AVIF at given quality, return encoded data.
// Follows the pattern from image_transcoder.cc:EncodeAvifFromPixels.
std::string EncodeAvif(const uint8_t* pixels, size_t w, size_t h,
                       ic::PixelFormat pixel_format, int quality,
                       pagespeed::MessageHandler* /*handler*/) {
  // Skip GRAY_8 — complex YUV400 handling not needed for sweep.
  if (pixel_format == ic::GRAY_8) return {};

  bool has_alpha = (pixel_format == ic::RGBA_8888);
  int bpp = has_alpha ? 4 : 3;

  avifPixelFormat yuv_format = AVIF_PIXEL_FORMAT_YUV420;
  avifImage* image = avifImageCreate(static_cast<uint32_t>(w),
                                     static_cast<uint32_t>(h), 8, yuv_format);
  if (!image) return {};

  avifRGBImage rgb;
  avifRGBImageSetDefaults(&rgb, image);
  rgb.depth = 8;
  rgb.format = has_alpha ? AVIF_RGB_FORMAT_RGBA : AVIF_RGB_FORMAT_RGB;
  rgb.pixels = const_cast<uint8_t*>(pixels);
  rgb.rowBytes = static_cast<uint32_t>(w * bpp);

  if (avifImageRGBToYUV(image, &rgb) != AVIF_RESULT_OK) {
    avifImageDestroy(image);
    return {};
  }

  avifEncoder* encoder = avifEncoderCreate();
  if (!encoder) {
    avifImageDestroy(image);
    return {};
  }

  encoder->quality = quality;
  encoder->qualityAlpha = quality;
  encoder->speed = 6;  // Balanced speed for sweep (matches transcoder default).

  avifRWData output = AVIF_DATA_EMPTY;
  avifResult result = avifEncoderWrite(encoder, image, &output);
  avifEncoderDestroy(encoder);
  avifImageDestroy(image);

  if (result != AVIF_RESULT_OK) {
    avifRWDataFree(&output);
    return {};
  }

  std::string encoded(reinterpret_cast<const char*>(output.data), output.size);
  avifRWDataFree(&output);
  return encoded;
}

// Decode AVIF encoded data back to raw pixels for SSIMULACRA2 comparison.
// Returns malloc'd pixel buffer (caller must free), sets width/height/bpp.
// Returns nullptr on failure.
uint8_t* DecodeAvif(const std::string& encoded, size_t* out_w, size_t* out_h,
                    int* out_bpp) {
  avifDecoder* decoder = avifDecoderCreate();
  if (!decoder) return nullptr;

  avifResult result = avifDecoderSetIOMemory(
      decoder, reinterpret_cast<const uint8_t*>(encoded.data()),
      encoded.size());
  if (result != AVIF_RESULT_OK) {
    avifDecoderDestroy(decoder);
    return nullptr;
  }

  result = avifDecoderParse(decoder);
  if (result != AVIF_RESULT_OK) {
    avifDecoderDestroy(decoder);
    return nullptr;
  }

  result = avifDecoderNextImage(decoder);
  if (result != AVIF_RESULT_OK) {
    avifDecoderDestroy(decoder);
    return nullptr;
  }

  avifRGBImage rgb;
  avifRGBImageSetDefaults(&rgb, decoder->image);
  rgb.depth = 8;
  rgb.format = (decoder->image->alphaPlane != nullptr) ? AVIF_RGB_FORMAT_RGBA
                                                       : AVIF_RGB_FORMAT_RGB;

  int bpp = (rgb.format == AVIF_RGB_FORMAT_RGBA) ? 4 : 3;
  size_t w = decoder->image->width;
  size_t h = decoder->image->height;
  size_t buf_size = w * h * bpp;

  auto* buf = static_cast<uint8_t*>(malloc(buf_size));
  if (!buf) {
    avifDecoderDestroy(decoder);
    return nullptr;
  }

  rgb.pixels = buf;
  rgb.rowBytes = static_cast<uint32_t>(w * bpp);

  result = avifImageYUVToRGB(decoder->image, &rgb);
  avifDecoderDestroy(decoder);

  if (result != AVIF_RESULT_OK) {
    free(buf);
    return nullptr;
  }

  *out_w = w;
  *out_h = h;
  *out_bpp = bpp;
  return buf;
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, args)) return 1;

  std::string manifest_data = ReadFileContents(args.manifest);
  if (manifest_data.empty()) {
    fprintf(stderr, "Cannot read manifest: %s\n", args.manifest.c_str());
    return 1;
  }

  // Parse manifest.
  std::vector<ManifestEntry> entries;
  std::string_view view(manifest_data);
  size_t line_start = 0;
  bool header_skipped = false;
  while (line_start < view.size()) {
    auto line_end = view.find('\n', line_start);
    if (line_end == std::string_view::npos) line_end = view.size();
    auto line = view.substr(line_start, line_end - line_start);
    line_start = line_end + 1;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.empty()) continue;
    if (!header_skipped) {
      header_skipped = true;
      continue;
    }
    auto fields = SplitCsv(line);
    if (fields.size() < 6) continue;
    entries.push_back({fields[0], atoi(fields[5].c_str())});
  }

  fprintf(stderr, "Loaded %zu images from manifest\n", entries.size());

  auto jpeg_qualities = GetJpegQualities();
  auto webp_qualities = GetWebpQualities();
  auto avif_qualities = GetAvifQualities();

  // Thread-safe output.
  std::mutex output_mutex;
  FILE* out = fopen(args.output.c_str(), "w");
  if (!out) {
    fprintf(stderr, "Cannot open output: %s\n", args.output.c_str());
    return 1;
  }
  fprintf(out, "path,format,quality,ssimulacra2,encoded_size\n");

  std::atomic<int> next_index{0};
  std::atomic<int> completed{0};
  int total = static_cast<int>(entries.size());

  auto worker = [&]() {
    pagespeed::NullMessageHandler handler;
    while (true) {
      int idx = next_index.fetch_add(1);
      if (idx >= total) break;
      const auto& entry = entries[idx];

      std::string data = ReadFileContents(entry.path);
      if (data.empty()) continue;

      ic::ImageFormat fmt = CodeToFormat(entry.source_format);
      if (fmt == ic::IMAGE_UNKNOWN) continue;

      // Decode original.
      void* raw_pixels = nullptr;
      ic::PixelFormat pixel_format;
      size_t w = 0, h = 0, stride = 0;
      if (!ic::ReadImage(fmt, data.data(), data.size(), &raw_pixels,
                         &pixel_format, &w, &h, &stride, &handler)) {
        continue;
      }

      auto* pixels = reinterpret_cast<const uint8_t*>(raw_pixels);
      int bpp = static_cast<int>(
          ic::GetNumChannelsFromPixelFormat(pixel_format, &handler));

      // Sweep per format.
      std::vector<SweepResult> results;

      auto sweep_format = [&](const std::string& fmt_name,
                              const std::vector<int>& qualities,
                              auto encode_fn) {
        for (int q : qualities) {
          std::string encoded =
              encode_fn(pixels, w, h, pixel_format, q, &handler);
          if (encoded.empty()) continue;

          // Decode the encoded result.
          ic::ImageFormat decode_fmt =
              (fmt_name == "jpeg") ? ic::IMAGE_JPEG : ic::IMAGE_WEBP;
          void* dec_pixels = nullptr;
          ic::PixelFormat dec_pf;
          size_t dec_w = 0, dec_h = 0, dec_stride = 0;
          if (!ic::ReadImage(decode_fmt, encoded.data(), encoded.size(),
                             &dec_pixels, &dec_pf, &dec_w, &dec_h, &dec_stride,
                             &handler)) {
            continue;
          }

          int dec_bpp = static_cast<int>(
              ic::GetNumChannelsFromPixelFormat(dec_pf, &handler));

          // Both original and decoded must have same dimensions for SSIMULACRA2.
          if (dec_w != w || dec_h != h) {
            free(dec_pixels);
            continue;
          }

          // Use the minimum bpp for comparison.
          int cmp_bpp = std::min(bpp, dec_bpp);
          std::optional<float> score = pagespeed::ComputeSSIMULACRA2(
              pixels, reinterpret_cast<const uint8_t*>(dec_pixels),
              static_cast<uint32_t>(w), static_cast<uint32_t>(h), cmp_bpp,
              &handler);
          free(dec_pixels);
          if (!score.has_value()) continue;

          results.push_back({entry.path, fmt_name, q, *score, encoded.size()});
        }
      };

      if (args.do_jpeg) {
        sweep_format("jpeg", jpeg_qualities, EncodeJpeg);
      }
      if (args.do_webp) {
        sweep_format("webp", webp_qualities, EncodeWebp);
      }

      // AVIF sweep: separate path because ReadImage doesn't support AVIF decode.
      // Uses EncodeAvif + DecodeAvif (libavif) directly.
      if (args.do_avif && pixel_format != ic::GRAY_8) {
        for (int q : avif_qualities) {
          std::string encoded =
              EncodeAvif(pixels, w, h, pixel_format, q, &handler);
          if (encoded.empty()) continue;

          size_t dec_w = 0, dec_h = 0;
          int dec_bpp = 0;
          uint8_t* dec_pixels = DecodeAvif(encoded, &dec_w, &dec_h, &dec_bpp);
          if (!dec_pixels) continue;

          if (dec_w != w || dec_h != h) {
            free(dec_pixels);
            continue;
          }

          int cmp_bpp = std::min(bpp, dec_bpp);
          std::optional<float> score = pagespeed::ComputeSSIMULACRA2(
              pixels, dec_pixels, static_cast<uint32_t>(w),
              static_cast<uint32_t>(h), cmp_bpp, &handler);
          free(dec_pixels);
          if (!score.has_value()) continue;

          results.push_back({entry.path, "avif", q, *score, encoded.size()});
        }
      }

      free(raw_pixels);

      // Write results.
      if (!results.empty()) {
        std::lock_guard<std::mutex> lock(output_mutex);
        for (const auto& r : results) {
          fprintf(out, "%s,%s,%d,%.4f,%zu\n", r.path.c_str(), r.format.c_str(),
                  r.quality, r.ssimulacra2, r.encoded_size);
        }
      }

      int done = completed.fetch_add(1) + 1;
      if (done % 10 == 0) {
        fprintf(stderr, "\rProgress: %d/%d", done, total);
      }
    }
  };

  std::vector<std::thread> threads;
  for (int i = 0; i < args.threads; ++i) {
    threads.emplace_back(worker);
  }
  for (auto& t : threads) {
    t.join();
  }

  fclose(out);
  fprintf(stderr, "\nDone. Processed %d images.\n", completed.load());
  return 0;
}
