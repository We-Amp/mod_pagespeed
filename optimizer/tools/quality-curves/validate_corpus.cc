// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// mod_pagespeed — Corpus Validation Tool
//
// Loads images via the scanline API, rejects animated images,
// runs ContentAnalyzer, and outputs a CSV manifest.
//
// Usage: validate_corpus --input-dir DIR --output manifest.csv [--threads N]

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "lib/base/message_handler.h"
#include "lib/base/string_util.h"
#include "lib/image/content_analyzer.h"
#include "lib/image/image_frame_interface.h"
#include "lib/image/image_util.h"
#include "lib/image/jpeg_utils.h"
#include "lib/image/read_image.h"
#include "lib/image/scanline_utils.h"

namespace ic = pagespeed::image_compression;
namespace fs = std::filesystem;

namespace {

struct Args {
  std::string input_dir;
  std::string output;
  int threads = 4;
};

bool ParseArgs(int argc, char** argv, Args& args) {
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--input-dir") == 0 && i + 1 < argc) {
      args.input_dir = argv[++i];
    } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
      args.output = argv[++i];
    } else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
      args.threads = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--help") == 0) {
      fprintf(stderr,
              "Usage: validate_corpus --input-dir DIR --output manifest.csv "
              "[--threads N]\n");
      return false;
    }
  }
  if (args.input_dir.empty() || args.output.empty()) {
    fprintf(stderr,
            "Usage: validate_corpus --input-dir DIR --output manifest.csv "
            "[--threads N]\n");
    return false;
  }
  if (args.threads < 1) args.threads = 1;
  return true;
}

std::string ReadFile(const fs::path& path) {
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

// Map net_instaweb::ImageType to pagespeed::image_compression::ImageFormat.
ic::ImageFormat ImageTypeToFormat(net_instaweb::ImageType t) {
  switch (t) {
    case net_instaweb::IMAGE_JPEG:
      return ic::IMAGE_JPEG;
    case net_instaweb::IMAGE_PNG:
      return ic::IMAGE_PNG;
    case net_instaweb::IMAGE_GIF:
      return ic::IMAGE_GIF;
    case net_instaweb::IMAGE_WEBP:
    case net_instaweb::IMAGE_WEBP_LOSSLESS_OR_ALPHA:
    case net_instaweb::IMAGE_WEBP_ANIMATED:
      return ic::IMAGE_WEBP;
    default:
      return ic::IMAGE_UNKNOWN;
  }
}

// Source format code for CSV: 0=JPEG, 1=PNG, 2=GIF, 3=WebP.
int FormatCode(ic::ImageFormat fmt) {
  switch (fmt) {
    case ic::IMAGE_JPEG:
      return 0;
    case ic::IMAGE_PNG:
      return 1;
    case ic::IMAGE_GIF:
      return 2;
    case ic::IMAGE_WEBP:
      return 3;
    default:
      return -1;
  }
}

bool IsImageExtension(const fs::path& ext) {
  auto e = ext.string();
  for (auto& c : e) c = net_instaweb::LowerChar(c);
  return e == ".jpg" || e == ".jpeg" || e == ".png" || e == ".gif" ||
         e == ".webp";
}

struct ValidatedEntry {
  std::string path;
  size_t width = 0;
  size_t height = 0;
  int content_class = 0;
  bool has_alpha = false;
  int format_code = -1;
  int jpeg_quality = -1;
  bool accepted = false;
};

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, args)) return 1;

  // Phase 1: Collect all candidate paths.
  std::vector<fs::path> paths;
  for (const auto& entry : fs::recursive_directory_iterator(args.input_dir)) {
    if (!entry.is_regular_file()) continue;
    if (!IsImageExtension(entry.path().extension())) continue;
    paths.push_back(entry.path());
  }
  const int total = static_cast<int>(paths.size());
  fprintf(stderr, "Found %d candidate images, validating with %d threads...\n",
          total, args.threads);

  // Phase 2: Pre-allocate results, process in parallel.
  std::vector<ValidatedEntry> results(total);
  std::atomic<int> next_index{0};
  std::atomic<int> accepted{0};
  std::atomic<int> rejected{0};

  auto worker = [&]() {
    pagespeed::NullMessageHandler handler;
    for (;;) {
      int idx = next_index.fetch_add(1, std::memory_order_relaxed);
      if (idx >= total) break;

      const auto& path = paths[idx];
      auto& result = results[idx];
      result.path = path.string();

      std::string data = ReadFile(path);
      if (data.empty()) {
        rejected.fetch_add(1, std::memory_order_relaxed);
        continue;
      }

      auto image_type = net_instaweb::ComputeImageType(data);
      ic::ImageFormat fmt = ImageTypeToFormat(image_type);
      if (fmt == ic::IMAGE_UNKNOWN) {
        rejected.fetch_add(1, std::memory_order_relaxed);
        continue;
      }

      // Check for animated images via MultipleFrameReader.
      if (fmt == ic::IMAGE_GIF || fmt == ic::IMAGE_WEBP) {
        ic::ScanlineStatus status;
        std::unique_ptr<ic::MultipleFrameReader> frame_reader(
            ic::CreateImageFrameReader(fmt, data.data(), data.size(), &handler,
                                       &status));
        if (frame_reader) {
          ic::ImageSpec spec;
          if (frame_reader->GetImageSpec(&spec).Success() &&
              spec.num_frames > 1) {
            rejected.fetch_add(1, std::memory_order_relaxed);
            continue;
          }
        }
      }

      // Decode to pixels.
      void* pixels = nullptr;
      ic::PixelFormat pixel_format;
      size_t w = 0, h = 0, stride = 0;
      if (!ic::ReadImage(fmt, data.data(), data.size(), &pixels, &pixel_format,
                         &w, &h, &stride, &handler)) {
        rejected.fetch_add(1, std::memory_order_relaxed);
        continue;
      }

      int bpp = static_cast<int>(
          ic::GetNumChannelsFromPixelFormat(pixel_format, &handler));

      // Run content analysis.
      auto analysis = pagespeed::AnalyzeContent(
          reinterpret_cast<const uint8_t*>(pixels), w * h * bpp,
          static_cast<uint32_t>(w), static_cast<uint32_t>(h), bpp, &handler);

      // JPEG source quality.
      int jpeg_quality = -1;
      if (fmt == ic::IMAGE_JPEG) {
        jpeg_quality = ic::JpegUtils::GetImageQualityFromImage(
            data.data(), data.size(), &handler);
      }

      result.width = w;
      result.height = h;
      result.content_class = static_cast<int>(analysis.content_class);
      result.has_alpha = (pixel_format == ic::RGBA_8888);
      result.format_code = FormatCode(fmt);
      result.jpeg_quality = jpeg_quality;
      result.accepted = true;
      accepted.fetch_add(1, std::memory_order_relaxed);

      free(pixels);
    }
  };

  int num_threads = std::min(args.threads, total);
  std::vector<std::thread> threads;
  threads.reserve(num_threads);
  for (int i = 0; i < num_threads; ++i) {
    threads.emplace_back(worker);
  }
  for (auto& t : threads) {
    t.join();
  }

  // Phase 3: Write results sequentially for deterministic output.
  FILE* out = fopen(args.output.c_str(), "w");
  if (!out) {
    fprintf(stderr, "Cannot open output: %s\n", args.output.c_str());
    return 1;
  }
  fprintf(out,
          "path,width,height,content_class,has_alpha,source_format,"
          "source_jpeg_quality\n");

  for (const auto& r : results) {
    if (!r.accepted) continue;
    fprintf(out, "%s,%zu,%zu,%d,%d,%d,%d\n", r.path.c_str(), r.width, r.height,
            r.content_class, r.has_alpha ? 1 : 0, r.format_code,
            r.jpeg_quality);
  }

  fclose(out);
  fprintf(stderr, "Total: %d, Accepted: %d, Rejected: %d\n", total,
          accepted.load(), rejected.load());
  return 0;
}
