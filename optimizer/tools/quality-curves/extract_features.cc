// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// mod_pagespeed — Feature Extraction Tool
//
// Reads a manifest CSV (from validate_corpus), decodes each image,
// calls ExtractImageFeatures(), and outputs features.csv.
//
// Usage: extract_features --manifest manifest.csv --output features.csv
//                         [--threads N]

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "lib/base/message_handler.h"
#include "lib/image/content_analyzer.h"
#include "lib/image/image_util.h"
#include "lib/image/jpeg_utils.h"
#include "lib/image/quality_features.h"
#include "lib/image/read_image.h"
#include "lib/image/scanline_utils.h"

namespace ic = pagespeed::image_compression;

namespace {

struct Args {
  std::string manifest;
  std::string output;
  int threads = 4;
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
    } else if (strcmp(argv[i], "--help") == 0) {
      fprintf(stderr,
              "Usage: extract_features --manifest manifest.csv "
              "--output features.csv [--threads N]\n");
      return false;
    }
  }
  if (args.manifest.empty() || args.output.empty()) {
    fprintf(stderr,
            "Usage: extract_features --manifest manifest.csv "
            "--output features.csv [--threads N]\n");
    return false;
  }
  return true;
}

std::string ReadFile(const std::string& path) {
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

// Parse a CSV line. Simple: no quoting, no embedded commas.
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
  int source_format_code;
};

struct FeatureResult {
  std::string path;
  pagespeed::ImageFeatures features;
  bool valid = false;
};

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, args)) return 1;

  std::string manifest_data = ReadFile(args.manifest);
  if (manifest_data.empty()) {
    fprintf(stderr, "Cannot read manifest: %s\n", args.manifest.c_str());
    return 1;
  }

  // Parse manifest into entries vector.
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
    int source_format_code = atoi(fields[5].c_str());
    if (CodeToFormat(source_format_code) == ic::IMAGE_UNKNOWN) continue;
    entries.push_back({fields[0], source_format_code});
  }

  int total = static_cast<int>(entries.size());
  fprintf(stderr, "Loaded %d images from manifest, using %d threads\n", total,
          args.threads);

  // Pre-allocate results array indexed by entry position for deterministic
  // output order regardless of thread scheduling.
  std::vector<FeatureResult> results(total);

  std::atomic<int> next_index{0};
  std::atomic<int> completed{0};
  std::atomic<int> errors{0};

  auto worker = [&]() {
    pagespeed::NullMessageHandler handler;
    while (true) {
      int idx = next_index.fetch_add(1);
      if (idx >= total) break;
      const auto& entry = entries[idx];

      ic::ImageFormat fmt = CodeToFormat(entry.source_format_code);

      std::string data = ReadFile(entry.path);
      if (data.empty()) {
        fprintf(stderr, "Cannot read: %s\n", entry.path.c_str());
        errors.fetch_add(1);
        continue;
      }

      // Decode to pixels.
      void* pixels = nullptr;
      ic::PixelFormat pixel_format;
      size_t w = 0, h = 0, stride = 0;
      if (!ic::ReadImage(fmt, data.data(), data.size(), &pixels, &pixel_format,
                         &w, &h, &stride, &handler)) {
        fprintf(stderr, "Decode failed: %s\n", entry.path.c_str());
        errors.fetch_add(1);
        continue;
      }

      int bpp = static_cast<int>(
          ic::GetNumChannelsFromPixelFormat(pixel_format, &handler));

      // Run content analysis.
      auto analysis = pagespeed::AnalyzeContent(
          reinterpret_cast<const uint8_t*>(pixels), w * h * bpp,
          static_cast<uint32_t>(w), static_cast<uint32_t>(h), bpp, &handler);

      // Extract source JPEG quality from DQT tables.
      int source_quality = -1;
      if (fmt == ic::IMAGE_JPEG) {
        source_quality =
            pagespeed::image_compression::JpegUtils::GetImageQualityFromImage(
                data.data(), data.size(), &handler);
      }

      // Extract features.
      auto features = pagespeed::ExtractImageFeatures(
          analysis, reinterpret_cast<const uint8_t*>(pixels),
          static_cast<uint32_t>(w), static_cast<uint32_t>(h), bpp,
          static_cast<uint8_t>(entry.source_format_code), source_quality);

      free(pixels);

      // Store result at the entry's index (no lock needed, each index unique).
      results[idx] = {entry.path, features, true};

      int done = completed.fetch_add(1) + 1;
      if (done % 100 == 0 || done == total) {
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

  // Write results in deterministic order.
  FILE* out = fopen(args.output.c_str(), "w");
  if (!out) {
    fprintf(stderr, "\nCannot open output: %s\n", args.output.c_str());
    return 1;
  }
  fprintf(out,
          "path,edge_density,noise_level,unique_color_ratio,photo_metric,"
          "color_entropy,spatial_freq_low,spatial_freq_high,"
          "mean_luminance,luminance_variance,"
          "width,height,content_class,source_format,has_alpha,is_grayscale,"
          "source_quality\n");

  int written = 0;
  for (const auto& r : results) {
    if (!r.valid) continue;
    fprintf(out,
            "%s,%.6f,%.6f,%.6f,%.6f,%.6f,%.1f,%.1f,%.2f,%.2f,"
            "%u,%u,%u,%u,%d,%d,%d\n",
            r.path.c_str(), r.features.edge_density, r.features.noise_level,
            r.features.unique_color_ratio, r.features.photo_metric,
            r.features.color_entropy, r.features.spatial_freq_low,
            r.features.spatial_freq_high, r.features.mean_luminance,
            r.features.luminance_variance, r.features.width, r.features.height,
            r.features.content_class, r.features.source_format,
            r.features.has_alpha ? 1 : 0, r.features.is_grayscale ? 1 : 0,
            r.features.source_quality);
    ++written;
  }

  fclose(out);
  fprintf(stderr, "\nProcessed: %d, Errors: %d, Written: %d\n",
          completed.load(), errors.load(), written);
  return 0;
}
