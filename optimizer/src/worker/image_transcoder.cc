// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Image Transcoder Implementation

#include "src/worker/image_transcoder.h"

#include <algorithm>
#include <cinttypes>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "avif/avif.h"
#include "lib/base/message_handler.h"
#include "lib/image/content_analyzer.h"
#include "lib/image/exif_orientation.h"
#include "lib/image/image_converter.h"
#include "lib/image/image_resizer.h"
#include "lib/image/image_util.h"
#include "lib/image/jpeg_optimizer.h"
#include "lib/image/jpeg_utils.h"
#include "lib/image/png_optimizer.h"
#include "lib/image/quality_features.h"
#include "lib/image/quality_predictor.h"
#include "lib/image/quality_verifier.h"
#include "lib/image/read_image.h"
#include "lib/image/webp_optimizer.h"

namespace pagespeed {

using image_compression::CreateScanlineReader;
using image_compression::CreateScanlineWriter;
using image_compression::IMAGE_GIF;
using image_compression::IMAGE_JPEG;
using image_compression::IMAGE_PNG;
using image_compression::IMAGE_WEBP;
using image_compression::ImageConverter;
using image_compression::ImageFormat;
using image_compression::JpegCompressionOptions;
using image_compression::OptimizeJpeg;
using image_compression::OptimizeJpegWithOptions;
using image_compression::PngOptimizer;
using image_compression::PngReader;
using image_compression::WebpConfiguration;
using net_instaweb::ComputeImageType;
using net_instaweb::ExtractPngC2paChunks;
using net_instaweb::ImageHasC2paManifest;
using net_instaweb::ImageHasJumbfC2pa;
using net_instaweb::ImageHasXmpC2pa;
using net_instaweb::ScanlineReaderInterface;
using net_instaweb::ScanlineStatus;
using net_instaweb::ScanlineWriterInterface;

ImageTranscoder::ImageTranscoder(ConfigAccessor config_accessor,
                                 MessageHandler* handler)
    : config_accessor_(std::move(config_accessor)), handler_(handler) {}

ImageTranscoder::ImageTranscoder(const ImageTranscoderConfig& config,
                                 MessageHandler* handler)
    : config_accessor_([config] { return config; }), handler_(handler) {}

namespace {

// True when a still WebP carries a LOSSLESS (VP8L) image bitstream.
//
// ComputeImageType cannot answer this: it reports
// IMAGE_WEBP_LOSSLESS_OR_ALPHA for the lossless form AND for a lossy image
// that merely has an alpha channel (VP8X + ALPH + VP8), and those two want
// opposite treatment -- the lossy-with-alpha one is an ordinary re-encode
// candidate, the lossless one is not (#1375).  So walk the RIFF
// chunk list and look for the bitstream chunk itself.  Animated WebP never
// reaches here (it classifies as its own type and no path accepts it), so the
// walk does not need to descend into ANMF frames.
bool WebpIsLossless(std::string_view data) {
  if (data.size() < 16 || data.compare(0, 4, "RIFF") != 0 ||
      data.compare(8, 4, "WEBP") != 0) {
    return false;
  }
  size_t pos = 12;
  while (pos + 8 <= data.size()) {
    std::string_view fourcc = data.substr(pos, 4);
    uint32_t chunk_size =
        static_cast<uint8_t>(data[pos + 4]) |
        (static_cast<uint32_t>(static_cast<uint8_t>(data[pos + 5])) << 8) |
        (static_cast<uint32_t>(static_cast<uint8_t>(data[pos + 6])) << 16) |
        (static_cast<uint32_t>(static_cast<uint8_t>(data[pos + 7])) << 24);
    if (fourcc == "VP8L") return true;
    if (fourcc == "VP8 ") return false;
    // Chunks are 2-byte aligned; guard the advance against overflow so a
    // declared size near UINT32_MAX cannot wrap the cursor backwards.
    uint64_t next =
        static_cast<uint64_t>(pos) + 8 + chunk_size + (chunk_size & 1);
    if (next <= pos || next > data.size()) return false;
    pos = static_cast<size_t>(next);
  }
  return false;
}

}  // namespace

TranscodeResult ImageTranscoder::Transcode(std::string_view input_data,
                                           const CapabilityMask& target_mask) {
  if (input_data.empty()) {
    return {false, {}, {}, "Empty input data"};
  }

  // Snapshot config once so the entire operation uses consistent settings,
  // even if hot-reload changes config between the conversion attempt and
  // the optimize-original fallback path.
  auto cfg = config();

  // Detect input image type from magic bytes.
  auto image_type = ComputeImageType(input_data);

  // Map net_instaweb::ImageType to image_compression::ImageFormat.
  ImageFormat src_format = image_compression::IMAGE_UNKNOWN;
  switch (image_type) {
    case net_instaweb::IMAGE_JPEG:
      src_format = IMAGE_JPEG;
      break;
    case net_instaweb::IMAGE_PNG:
      src_format = IMAGE_PNG;
      break;
    case net_instaweb::IMAGE_GIF:
      src_format = IMAGE_GIF;
      break;
    case net_instaweb::IMAGE_WEBP:
    case net_instaweb::IMAGE_WEBP_LOSSLESS_OR_ALPHA:
      src_format = IMAGE_WEBP;
      break;
    default:
      return {false, {}, {}, "Unsupported image format"};
  }

  // Determine target format from capability mask.
  auto target_format = target_mask.image_format();

  // C2PA preserve fallback. The JPEG->JPEG codec carry (OptimizeJpeg)
  // preserves the APP11/JUMBF manifest, but AVIF/WebP/PNG outputs and the XMP/APP1
  // form do not survive transcode. When a manifest is present and the chosen output
  // cannot carry it, serve the ORIGINAL bytes unchanged (skip-not-strip) rather than
  // silently dropping provenance. Detect-only; never decode/validate/re-emit (D3).
  const bool c2pa_jumbf = cfg.preserve_c2pa && ImageHasJumbfC2pa(input_data);
  const bool c2pa_xmp = cfg.preserve_c2pa && ImageHasXmpC2pa(input_data);
  if (c2pa_jumbf || c2pa_xmp) {
    // Level A carry-through (opt-in via cfg.c2pa_carry, PNG only). A
    // manifest-bearing PNG that stays in PNG form (kOriginal) is recompressed and
    // its caBX/iTXt chunks are spliced back (OptimizePngWithC2paCarry), instead of
    // skip-not-strip; the JPEG->JPEG codec already carries APP11/JUMBF on its own,
    // and AVIF/WebP outputs still cannot carry it. Every non-carry case skips.
    const bool png_carry =
        cfg.c2pa_carry &&
        target_format == CapabilityMask::ImageFormat::kOriginal &&
        src_format == IMAGE_PNG;
    if (png_carry) {
      return OptimizePngWithC2paCarry(input_data);
    }
    const bool carryable =
        target_format == CapabilityMask::ImageFormat::kOriginal &&
        src_format == IMAGE_JPEG && c2pa_jumbf && !c2pa_xmp;
    if (!carryable) {
      return {true, std::string(input_data),
              image_compression::ImageFormatToMimeTypeString(src_format),
              "C2PA passthrough (skip-not-strip)"};
    }
    // Carryable: fall through; the kOriginal JPEG path runs OptimizeJpeg, which
    // carries the APP11/JUMBF manifest through the recompress.
  }

  switch (target_format) {
    case CapabilityMask::ImageFormat::kWebP:
      // Convert to WebP if source is not already WebP.
      if (src_format == IMAGE_GIF) {
        auto result = ConvertGifToWebp(input_data);
        if (result.success) {
          return result;
        }
        // Fall through to return original GIF on failure.
        if (handler_ != nullptr) {
          handler_->Warning(
              "GIF→WebP conversion failed (%s), falling back to original",
              result.error_message.c_str());
        }
      } else if (src_format != IMAGE_WEBP) {
        auto result = ConvertToWebp(input_data);
        if (result.success) {
          return result;
        }
        // Fall through to optimize original on conversion failure.
        if (handler_ != nullptr) {
          handler_->Warning(
              "WebP conversion failed (%s), falling back to "
              "optimizing original",
              result.error_message.c_str());
        }
      }
      break;

    case CapabilityMask::ImageFormat::kAvif: {
      if (src_format != IMAGE_WEBP) {
        auto result = ConvertToAvif(input_data);
        if (result.success) {
          return result;
        }
        if (handler_ != nullptr) {
          handler_->Warning(
              "AVIF conversion failed (%s), falling back to "
              "optimizing original",
              result.error_message.c_str());
        }
      }
      break;
    }

    case CapabilityMask::ImageFormat::kSvg:
      // SVG is not a transcoding target — optimize original.
      if (handler_ != nullptr) {
        handler_->Info(
            "SVG format not a transcoding target, optimizing original");
      }
      break;

    case CapabilityMask::ImageFormat::kOriginal:
    default:
      break;
  }

  // Optimize in the original format.
  switch (src_format) {
    case IMAGE_JPEG:
      return OptimizeJpeg(input_data, cfg);
    case IMAGE_PNG:
      return OptimizePng(input_data);
    case IMAGE_GIF:
      // GIF can't be optimized in-place; return as-is.
      return {true, std::string(input_data), "image/gif", {}};
    case IMAGE_WEBP:
      // The same rule JPEG and PNG get above: re-encode and keep the result
      // only if it is strictly smaller. WebP inputs used to be handed back
      // untouched on the grounds that they were "already optimized by the
      // encoder", which does not hold for arbitrary lossy WebP origins -- an
      // origin encoded at a higher quality than this pipeline targets
      // re-encodes materially smaller, and the acceptance rule already
      // protects the cases where the origin really is optimal (#1375).
      // OptimizeWebp itself exempts LOSSLESS origins; see there.
      return OptimizeWebp(input_data, cfg);
    default:
      return {false, {}, {}, "Cannot optimize unknown format"};
  }
}

TranscodeResult ImageTranscoder::OptimizeJpeg(
    std::string_view input_data, const ImageTranscoderConfig& cfg) {
  std::string src(input_data);
  std::string output;

  JpegCompressionOptions options;
  options.lossy = cfg.lossy_jpeg;
  options.progressive = cfg.progressive_jpeg;
  // Preserve C2PA / Content Credentials provenance through this JPEG->JPEG
  // (kOriginal) recompress. Only this same-format path can
  // carry the APP11/JUMBF manifest; the AVIF/WebP transcode and the
  // encode-from-pixels paths carry pixels only and still strip C2PA in v1
  // (Track B, demand-gated).
  options.preserve_c2pa = cfg.preserve_c2pa;
  if (cfg.lossy_jpeg) {
    options.lossy_options.quality = cfg.jpeg_quality;
  }

  if (OptimizeJpegWithOptions(src, &output, options, handler_)) {
    // Only use optimized if it's actually smaller.
    if (output.size() < input_data.size()) {
      return {true, std::move(output), "image/jpeg", {}};
    }
    // Original is already well-optimized — no savings.
    return {false, {}, {}, "JPEG optimization produced no size savings"};
  }

  return {false, {}, {}, "JPEG optimization failed"};
}

TranscodeResult ImageTranscoder::OptimizePng(std::string_view input_data) {
  std::string src(input_data);
  std::string output;

  // The PNG re-encode drops ancillary chunks, including the eXIf chunk that
  // makes a rotated image display upright, and this path does not bake the
  // orientation into the pixels. Keep serving the original bytes, which still
  // carry the tag.
  if (image_compression::ReadPngExifOrientation(input_data.data(),
                                                input_data.size()) != 1) {
    return {false, {}, {}, "PNG has EXIF orientation; kept verbatim"};
  }

  PngReader reader(handler_);
  if (PngOptimizer::OptimizePng(reader, src, &output, handler_)) {
    if (output.size() < input_data.size()) {
      return {true, std::move(output), "image/png", {}};
    }
    return {false, {}, {}, "PNG optimization produced no size savings"};
  }

  return {false, {}, {}, "PNG optimization failed"};
}

TranscodeResult ImageTranscoder::OptimizePngWithC2paCarry(
    std::string_view input_data) {
  // Level A: recompress the PNG, then splice the ORIGINAL caBX/iTXt
  // manifest chunks back in immediately before the trailing IEND. Fail-safe to
  // serving the original bytes verbatim (skip-not-strip) on ANY anomaly -- the
  // recompress failed or saved nothing, the output is not a well-formed PNG, no
  // carrier chunk was found, or there is no terminating IEND -- so a manifest is
  // never silently dropped. The bytes are never parsed or re-authored (D3).
  TranscodeResult opt = OptimizePng(input_data);
  if (opt.success && opt.output_mime_type == "image/png") {
    const std::vector<std::string_view> chunks =
        ExtractPngC2paChunks(input_data);
    std::string& out = opt.output_data;
    const size_t n = out.size();
    // The trailing IEND chunk is exactly 12 bytes (length(4) + "IEND"(4) +
    // CRC(4)); its type sits at offset n-8.
    if (!chunks.empty() && n >= 12 && out.compare(n - 8, 4, "IEND") == 0) {
      std::string carrier;
      for (std::string_view chunk : chunks) {
        carrier.append(chunk.data(), chunk.size());
      }
      // Splice UNCONDITIONALLY before IEND. OptimizePng decodes to pixels and
      // re-encodes, ALWAYS stripping ancillary chunks, so the recompressed output
      // never already holds the carrier. We deliberately do NOT skip the splice on
      // a byte-scan match: such a scan could, astronomically, match the carrier
      // bytes inside the compressed IDAT and skip the splice -- silently stripping
      // the manifest, the exact failure this carry exists to prevent.
      out.insert(n - 12, carrier);
      // Keep the carried output only if it is still a genuine win over the original
      // (which already holds the manifest): re-adding the carrier can outweigh a
      // marginal pixel saving, in which case serving the original is both smaller
      // AND manifest-preserving. (OptimizePng's own success only guarantees the
      // stripped recompress beat the manifest-bearing input, not that it beats the
      // input once the manifest is re-added.)
      if (out.size() < input_data.size()) {
        return opt;  // Carried: recompressed + manifest re-spliced, net smaller.
      }
    }
  }
  // Any anomaly OR no net size win: serve the original verbatim so the manifest
  // survives (skip-not-strip).
  return {true, std::string(input_data), "image/png",
          "C2PA carry fallback (skip-not-strip)"};
}

TranscodeResult ImageTranscoder::ConvertToWebp(std::string_view input_data) {
  // Detect source format.
  auto image_type = ComputeImageType(input_data);

  // Map to scanline format, validating that the source is JPEG or PNG.
  ImageFormat reader_format;
  if (image_type == net_instaweb::IMAGE_JPEG) {
    reader_format = IMAGE_JPEG;
  } else if (image_type == net_instaweb::IMAGE_PNG) {
    reader_format = IMAGE_PNG;
  } else {
    return {false, {}, {}, "ConvertToWebp: unsupported source format"};
  }

  // Use the scanline API for format conversion.
  std::unique_ptr<ScanlineReaderInterface> reader(CreateScanlineReader(
      reader_format, input_data.data(), input_data.size(), handler_));

  if (!reader) {
    return {false, {}, {}, "Failed to create scanline reader"};
  }

  // Configure WebP output. Sources here are JPEG/PNG stills: encode
  // lossy so webp_quality controls fidelity (WebpConfiguration defaults
  // to lossless, under which photos either exceed the size gate below
  // and lose their WebP variant, or pass it and serve an oversized
  // lossless variant). This matches EncodeWebpFromPixels, the
  // multi-transcode WebP path, including the encoder effort (webp_method).
  auto cfg = config();
  WebpConfiguration webp_config;
  webp_config.lossless = 0;
  webp_config.quality = cfg.webp_quality;
  webp_config.method = cfg.webp_method;
  webp_config.alpha_quality = cfg.webp_alpha_quality;

  std::string output;
  std::unique_ptr<ScanlineWriterInterface> writer(CreateScanlineWriter(
      IMAGE_WEBP, reader->GetPixelFormat(), reader->GetImageWidth(),
      reader->GetImageHeight(), &webp_config, &output, handler_));

  if (!writer) {
    return {false, {}, {}, "Failed to create WebP writer"};
  }

  // Convert line by line.
  if (image_compression::ImageConverter::ConvertImage(reader.get(),
                                                      writer.get())) {
    // Only use WebP if it's actually smaller.
    if (output.size() < input_data.size()) {
      return {true, std::move(output), "image/webp", {}};
    }
    // WebP is larger - fall back to original.
    return {
        false, {}, {}, "WebP output larger than original, skipping conversion"};
  }

  return {false, {}, {}, "WebP conversion failed"};
}

TranscodeResult ImageTranscoder::ConvertToAvif(std::string_view input_data) {
  // Detect source format.
  auto image_type = ComputeImageType(input_data);

  // Map to scanline format. Only JPEG and PNG are supported as source.
  ImageFormat reader_format;
  if (image_type == net_instaweb::IMAGE_JPEG) {
    reader_format = IMAGE_JPEG;
  } else if (image_type == net_instaweb::IMAGE_PNG) {
    reader_format = IMAGE_PNG;
  } else if (image_type == net_instaweb::IMAGE_GIF) {
    reader_format = IMAGE_GIF;
  } else {
    return {false, {}, {}, "ConvertToAvif: unsupported source format"};
  }

  // Create scanline reader to decode source image to raw pixels.
  std::unique_ptr<ScanlineReaderInterface> reader(CreateScanlineReader(
      reader_format, input_data.data(), input_data.size(), handler_));
  if (!reader) {
    return {false, {}, {}, "Failed to create scanline reader for AVIF"};
  }

  image_compression::size_px width = reader->GetImageWidth();
  image_compression::size_px height = reader->GetImageHeight();
  auto pixel_format = reader->GetPixelFormat();

  // Determine AVIF parameters based on pixel format.
  avifPixelFormat avif_yuv_format = AVIF_PIXEL_FORMAT_YUV420;
  bool has_alpha = false;
  int bytes_per_pixel = 3;

  switch (pixel_format) {
    case image_compression::RGBA_8888:
      has_alpha = true;
      bytes_per_pixel = 4;
      break;
    case image_compression::RGB_888:
      break;
    case image_compression::GRAY_8:
      avif_yuv_format = AVIF_PIXEL_FORMAT_YUV400;
      bytes_per_pixel = 1;
      break;
    default:
      return {false, {}, {}, "Unsupported pixel format for AVIF"};
  }

  // Check decoded size against safety limit before allocating.
  // Use uint64_t to detect overflow before truncation to size_t.
  uint64_t buffer_size64 =
      static_cast<uint64_t>(width) * height * bytes_per_pixel;
  if (buffer_size64 > kMaxDecodedPixels || buffer_size64 > SIZE_MAX) {
    return {false,
            {},
            {},
            "ConvertToAvif: decoded image too large for AVIF conversion"};
  }
  auto buffer_size = static_cast<size_t>(buffer_size64);

  // Read all scanlines into a contiguous pixel buffer.
  std::string pixel_buffer;
  pixel_buffer.reserve(buffer_size);

  while (reader->HasMoreScanLines()) {
    void* scanline = nullptr;
    if (!reader->ReadNextScanline(&scanline)) {
      return {false, {}, {}, "Failed to read scanline for AVIF"};
    }
    pixel_buffer.append(static_cast<const char*>(scanline),
                        static_cast<size_t>(width) * bytes_per_pixel);
  }

  // Expand grayscale to RGB for libavif (avifRGBImage expects >= 3bpp).
  if (bytes_per_pixel == 1) {
    std::string rgb_buffer;
    rgb_buffer.reserve(static_cast<size_t>(width) * height * 3);
    for (char g : pixel_buffer) {
      rgb_buffer.push_back(g);
      rgb_buffer.push_back(g);
      rgb_buffer.push_back(g);
    }
    pixel_buffer = std::move(rgb_buffer);
    bytes_per_pixel = 3;
  }

  // Create avifImage and populate pixel data.
  avifImage* image = avifImageCreate(width, height, 8, avif_yuv_format);
  if (!image) {
    return {false, {}, {}, "Failed to create avifImage"};
  }

  // Convert RGB(A) pixels to YUV using libavif.
  avifRGBImage rgb;
  avifRGBImageSetDefaults(&rgb, image);
  rgb.depth = 8;
  rgb.format = has_alpha ? AVIF_RGB_FORMAT_RGBA : AVIF_RGB_FORMAT_RGB;
  rgb.pixels = reinterpret_cast<uint8_t*>(pixel_buffer.data());
  rgb.rowBytes = width * bytes_per_pixel;

  avifResult convert_result = avifImageRGBToYUV(image, &rgb);
  if (convert_result != AVIF_RESULT_OK) {
    avifImageDestroy(image);
    return {false,
            {},
            {},
            std::string("RGB to YUV conversion failed: ") +
                avifResultToString(convert_result)};
  }

  // Create encoder and set quality parameters.
  avifEncoder* encoder = avifEncoderCreate();
  if (!encoder) {
    avifImageDestroy(image);
    return {false, {}, {}, "Failed to create avifEncoder"};
  }

  auto cfg = config();
  encoder->quality = cfg.avif_quality;
  encoder->qualityAlpha = cfg.avif_quality;
  encoder->speed = cfg.avif_speed;

  // Encode.
  avifRWData output = AVIF_DATA_EMPTY;
  avifResult encode_result = avifEncoderWrite(encoder, image, &output);

  avifEncoderDestroy(encoder);
  avifImageDestroy(image);

  if (encode_result != AVIF_RESULT_OK) {
    avifRWDataFree(&output);
    return {false,
            {},
            {},
            std::string("AVIF encoding failed: ") +
                avifResultToString(encode_result)};
  }

  // Only return AVIF if it's smaller than the original.
  if (output.size < input_data.size()) {
    std::string result_data(reinterpret_cast<const char*>(output.data),
                            output.size);
    avifRWDataFree(&output);
    return {true, std::move(result_data), "image/avif", {}};
  }

  avifRWDataFree(&output);
  return {
      false, {}, {}, "AVIF output larger than original, skipping conversion"};
}

TranscodeResult ImageTranscoder::ConvertGifToWebp(std::string_view input_data) {
  std::string src(input_data);
  std::string output;
  WebpConfiguration config;
  auto cfg = this->config();
  // GIF sources are palettized/lossless; keep lossless WebP (the
  // constructor default, made explicit here) to avoid generational
  // loss. In lossless mode 'quality' controls compression effort.
  config.lossless = 1;
  config.quality = cfg.webp_quality;
  config.alpha_quality = cfg.webp_alpha_quality;
  if (ImageConverter::ConvertGifToWebp(src, config, &output, handler_)) {
    if (output.size() < input_data.size()) {
      return {true, std::move(output), "image/webp", {}};
    }
    return {false, {}, {}, "WebP output larger than GIF"};
  }
  return {false, {}, {}, "GIF to WebP conversion failed"};
}

// ================================================================
// Multi-format transcoding (decode once, encode many)
// ================================================================

DecodedImage ImageTranscoder::DecodeToPixels(std::string_view input_data) {
  if (input_data.empty()) {
    return {};
  }
  decode_count_.fetch_add(1, std::memory_order_relaxed);

  auto image_type = ComputeImageType(input_data);
  if (image_type == net_instaweb::IMAGE_AVIF) {
    // No scanline reader exists for AVIF — decode via libavif.
    return DecodeAvifToPixels(input_data);
  }
  ImageFormat reader_format;
  switch (image_type) {
    case net_instaweb::IMAGE_JPEG:
      reader_format = IMAGE_JPEG;
      break;
    case net_instaweb::IMAGE_PNG:
      reader_format = IMAGE_PNG;
      break;
    case net_instaweb::IMAGE_GIF:
      reader_format = IMAGE_GIF;
      break;
    case net_instaweb::IMAGE_WEBP:
    case net_instaweb::IMAGE_WEBP_LOSSLESS_OR_ALPHA:
      reader_format = IMAGE_WEBP;
      break;
    default:
      return {};
  }

  std::unique_ptr<ScanlineReaderInterface> reader(CreateScanlineReader(
      reader_format, input_data.data(), input_data.size(), handler_));
  if (!reader) {
    return {};
  }

  DecodedImage decoded;
  decoded.width = reader->GetImageWidth();
  decoded.height = reader->GetImageHeight();
  auto pixel_format = reader->GetPixelFormat();

  switch (pixel_format) {
    case image_compression::RGBA_8888:
      decoded.has_alpha = true;
      decoded.bytes_per_pixel = 4;
      break;
    case image_compression::RGB_888:
      decoded.bytes_per_pixel = 3;
      break;
    case image_compression::GRAY_8:
      decoded.bytes_per_pixel = 1;
      break;
    default:
      return {};
  }

  uint64_t buffer_size64 = static_cast<uint64_t>(decoded.width) *
                           decoded.height * decoded.bytes_per_pixel;
  if (buffer_size64 > kMaxDecodedPixels || buffer_size64 > SIZE_MAX) {
    if (handler_ != nullptr) {
      handler_->Warning("Decoded image too large (%" PRIu64 " > %zu bytes)",
                        buffer_size64, kMaxDecodedPixels);
    }
    return {};
  }
  auto buffer_size = static_cast<size_t>(buffer_size64);

  decoded.pixel_buffer.reserve(buffer_size);
  while (reader->HasMoreScanLines()) {
    void* scanline = nullptr;
    if (!reader->ReadNextScanline(&scanline)) {
      return {};
    }
    decoded.pixel_buffer.append(
        static_cast<const char*>(scanline),
        static_cast<size_t>(decoded.width) * decoded.bytes_per_pixel);
  }

  return decoded;
}

DecodedImage ImageTranscoder::DecodeAvifToPixels(std::string_view input_data) {
  avifDecoder* decoder = avifDecoderCreate();
  if (decoder == nullptr) {
    return {};
  }
  avifResult result = avifDecoderSetIOMemory(
      decoder, reinterpret_cast<const uint8_t*>(input_data.data()),
      input_data.size());
  if (result == AVIF_RESULT_OK) {
    result = avifDecoderParse(decoder);
  }
  if (result == AVIF_RESULT_OK) {
    result = avifDecoderNextImage(decoder);
  }
  if (result != AVIF_RESULT_OK) {
    if (handler_ != nullptr) {
      handler_->Warning("AVIF decode failed: %s", avifResultToString(result));
    }
    avifDecoderDestroy(decoder);
    return {};
  }

  avifImage* image = decoder->image;
  const bool has_alpha = image->alphaPlane != nullptr;
  // Grayscale AVIF (YUV400, no alpha): the encoder wrote YUV400 exactly
  // when the source was bpp=1 (EncodeAvifFromPixels), so container truth
  // is enough — return 1-byte-per-pixel output. Without this the whole
  // grayscale class decoded at bpp=3 and every verify against a bpp=1
  // reference read as a format mismatch and shipped UNVERIFIED (#1274's
  // silent-skip class, mpp #790 D3). One channel (G) is extracted from
  // the RGB conversion below.
  const bool monochrome =
      !has_alpha && image->yuvFormat == AVIF_PIXEL_FORMAT_YUV400;
  const int bytes_per_pixel = has_alpha ? 4 : (monochrome ? 1 : 3);
  // The RGB conversion always fills 3 or 4 bytes per pixel; the mono
  // extraction reads from that wider buffer.
  const int rgb_bytes_per_pixel = has_alpha ? 4 : 3;

  // Cap the TRANSIENT PEAK, not the output size: avifRGBImageAllocatePixels
  // below allocates w*h*rgb_bytes_per_pixel before the mono extraction
  // narrows it, so accounting only the (1-byte) YUV400 output let a
  // just-under-cap monochrome AVIF transiently allocate ~3x the ceiling
  // (#1382). The output is never larger than the RGB buffer (1 <= 3), so
  // this bound covers both.
  const uint64_t rgb_buffer_size64 =
      static_cast<uint64_t>(image->width) * image->height * rgb_bytes_per_pixel;
  if (rgb_buffer_size64 > kMaxDecodedPixels || rgb_buffer_size64 > SIZE_MAX) {
    if (handler_ != nullptr) {
      handler_->Warning("Decoded AVIF too large (%" PRIu64 " > %zu bytes)",
                        rgb_buffer_size64, kMaxDecodedPixels);
    }
    avifDecoderDestroy(decoder);
    return {};
  }
  auto buffer_size = static_cast<size_t>(static_cast<uint64_t>(image->width) *
                                         image->height * bytes_per_pixel);

  avifRGBImage rgb;
  avifRGBImageSetDefaults(&rgb, image);
  rgb.format = has_alpha ? AVIF_RGB_FORMAT_RGBA : AVIF_RGB_FORMAT_RGB;
  rgb.depth = 8;
  result = avifRGBImageAllocatePixels(&rgb);
  if (result == AVIF_RESULT_OK) {
    result = avifImageYUVToRGB(image, &rgb);
  }

  DecodedImage decoded;
  if (result == AVIF_RESULT_OK &&
      rgb.rowBytes == static_cast<size_t>(image->width) * rgb_bytes_per_pixel) {
    decoded.width = image->width;
    decoded.height = image->height;
    decoded.bytes_per_pixel = bytes_per_pixel;
    decoded.has_alpha = has_alpha;
    if (monochrome) {
      // YUV400 -> RGB yields R=G=B=Y; keep one byte per pixel.
      const uint8_t* px = rgb.pixels;
      decoded.pixel_buffer.reserve(buffer_size);
      for (size_t i = 0; i < buffer_size; ++i) {
        decoded.pixel_buffer.push_back(
            static_cast<char>(px[i * rgb_bytes_per_pixel + 1]));
      }
    } else {
      decoded.pixel_buffer.assign(reinterpret_cast<const char*>(rgb.pixels),
                                  buffer_size);
    }
  } else if (handler_ != nullptr) {
    handler_->Warning("AVIF RGB conversion failed: %s",
                      avifResultToString(result));
  }
  avifRGBImageFreePixels(&rgb);
  avifDecoderDestroy(decoder);
  return decoded;
}

TranscodeResult ImageTranscoder::EncodeJpegFromPixels(
    const DecodedImage& decoded, const ImageTranscoderConfig& cfg) {
  // JPEG does not support alpha — reject RGBA input.
  if (decoded.has_alpha) {
    return {false, {}, {}, "JPEG does not support alpha channel"};
  }

  image_compression::PixelFormat pf = image_compression::RGB_888;
  if (decoded.bytes_per_pixel == 1) {
    pf = image_compression::GRAY_8;
  }

  JpegCompressionOptions options;
  options.lossy = cfg.lossy_jpeg;
  options.progressive = cfg.progressive_jpeg;
  if (cfg.lossy_jpeg) {
    options.lossy_options.quality = cfg.jpeg_quality;
  }

  std::string output;
  std::unique_ptr<ScanlineWriterInterface> writer(
      CreateScanlineWriter(IMAGE_JPEG, pf, decoded.width, decoded.height,
                           &options, &output, handler_));
  if (!writer) {
    return {false, {}, {}, "Failed to create JPEG writer from pixels"};
  }

  size_t row_bytes =
      static_cast<size_t>(decoded.width) * decoded.bytes_per_pixel;
  size_t total_bytes = static_cast<size_t>(decoded.height) * row_bytes;
  if (total_bytes > decoded.pixel_buffer.size()) {
    return {false, {}, {}, "Pixel buffer too small for declared dimensions"};
  }
  for (uint32_t y = 0; y < decoded.height; ++y) {
    const void* row = decoded.pixel_buffer.data() + y * row_bytes;
    if (!writer->WriteNextScanline(row)) {
      return {false, {}, {}, "Failed to write JPEG scanline"};
    }
  }

  if (!writer->FinalizeWrite()) {
    return {false, {}, {}, "Failed to finalize JPEG write"};
  }

  return {true, std::move(output), "image/jpeg", {}};
}

TranscodeResult ImageTranscoder::EncodeWebpFromPixels(
    const DecodedImage& decoded, const ImageTranscoderConfig& cfg) {
  // Use the scanline writer API with a memory-backed reader.
  // We need to feed pixel data through the scanline interface.
  WebpConfiguration webp_config;
  webp_config.lossless = 0;  // Lossy encoding for quality control.
  webp_config.quality = cfg.webp_quality;
  webp_config.method = cfg.webp_method;
  webp_config.alpha_quality = cfg.webp_alpha_quality;

  image_compression::PixelFormat pf = image_compression::RGB_888;
  if (decoded.has_alpha) {
    pf = image_compression::RGBA_8888;
  } else if (decoded.bytes_per_pixel == 1) {
    pf = image_compression::GRAY_8;
  }

  std::string output;
  std::unique_ptr<ScanlineWriterInterface> writer(
      CreateScanlineWriter(IMAGE_WEBP, pf, decoded.width, decoded.height,
                           &webp_config, &output, handler_));
  if (!writer) {
    return {false, {}, {}, "Failed to create WebP writer from pixels"};
  }

  size_t row_bytes =
      static_cast<size_t>(decoded.width) * decoded.bytes_per_pixel;
  size_t total_bytes = static_cast<size_t>(decoded.height) * row_bytes;
  if (total_bytes > decoded.pixel_buffer.size()) {
    return {false, {}, {}, "Pixel buffer too small for declared dimensions"};
  }
  for (uint32_t y = 0; y < decoded.height; ++y) {
    const void* row = decoded.pixel_buffer.data() + y * row_bytes;
    if (!writer->WriteNextScanline(row)) {
      return {false, {}, {}, "Failed to write WebP scanline"};
    }
  }

  if (!writer->FinalizeWrite()) {
    return {false, {}, {}, "Failed to finalize WebP write"};
  }

  return {true, std::move(output), "image/webp", {}};
}

TranscodeResult ImageTranscoder::EncodeAvifFromPixels(
    const DecodedImage& decoded, const ImageTranscoderConfig& cfg) {
  avifPixelFormat avif_yuv_format = AVIF_PIXEL_FORMAT_YUV420;
  if (decoded.bytes_per_pixel == 1) {
    avif_yuv_format = AVIF_PIXEL_FORMAT_YUV400;
  }

  avifImage* image =
      avifImageCreate(decoded.width, decoded.height, 8, avif_yuv_format);
  if (!image) {
    return {false, {}, {}, "Failed to create avifImage"};
  }

  // Expand grayscale to RGB for libavif (avifRGBImage expects >= 3bpp).
  std::string expanded_rgb;
  const char* avif_pixel_data = decoded.pixel_buffer.data();
  int avif_bpp = decoded.bytes_per_pixel;
  if (decoded.bytes_per_pixel == 1) {
    expanded_rgb.reserve(decoded.pixel_buffer.size() * 3);
    for (char g : decoded.pixel_buffer) {
      expanded_rgb.push_back(g);
      expanded_rgb.push_back(g);
      expanded_rgb.push_back(g);
    }
    avif_pixel_data = expanded_rgb.data();
    avif_bpp = 3;
  }

  size_t row_bytes = static_cast<size_t>(decoded.width) * avif_bpp;
  size_t total_bytes = static_cast<size_t>(decoded.height) * row_bytes;
  const std::string& source_buf =
      expanded_rgb.empty() ? decoded.pixel_buffer : expanded_rgb;
  if (total_bytes > source_buf.size()) {
    avifImageDestroy(image);
    return {false, {}, {}, "Pixel buffer too small for declared dimensions"};
  }

  avifRGBImage rgb;
  avifRGBImageSetDefaults(&rgb, image);
  rgb.depth = 8;
  rgb.format = decoded.has_alpha ? AVIF_RGB_FORMAT_RGBA : AVIF_RGB_FORMAT_RGB;
  rgb.pixels = reinterpret_cast<uint8_t*>(const_cast<char*>(avif_pixel_data));
  rgb.rowBytes = row_bytes;

  avifResult convert_result = avifImageRGBToYUV(image, &rgb);
  if (convert_result != AVIF_RESULT_OK) {
    avifImageDestroy(image);
    return {false,
            {},
            {},
            std::string("RGB to YUV conversion failed: ") +
                avifResultToString(convert_result)};
  }

  avifEncoder* encoder = avifEncoderCreate();
  if (!encoder) {
    avifImageDestroy(image);
    return {false, {}, {}, "Failed to create avifEncoder"};
  }

  encoder->quality = cfg.avif_quality;
  encoder->qualityAlpha = cfg.avif_quality;
  encoder->speed = cfg.avif_speed;

  avifRWData avif_output = AVIF_DATA_EMPTY;
  avifResult encode_result = avifEncoderWrite(encoder, image, &avif_output);

  avifEncoderDestroy(encoder);
  avifImageDestroy(image);

  if (encode_result != AVIF_RESULT_OK) {
    avifRWDataFree(&avif_output);
    return {false,
            {},
            {},
            std::string("AVIF encoding failed: ") +
                avifResultToString(encode_result)};
  }

  std::string result_data(reinterpret_cast<const char*>(avif_output.data),
                          avif_output.size);
  avifRWDataFree(&avif_output);
  return {true, std::move(result_data), "image/avif", {}};
}

// See image_transcoder.h for the contract and the reason this is a free
// function (mpp #790 D2: the inline form was unfalsifiable by the
// encoder-driven fixtures).
size_t SelectVerifyAttempt(const std::vector<VerifyAttempt>& attempts, float lo,
                           float hi, bool decline_below_floor) {
  auto band_distance = [lo, hi](float s) {
    if (s < lo) return lo - s;
    if (s > hi) return s - hi;
    return 0.0f;
  };
  // Asymmetry guard: with decline armed, restrict selection to shippable
  // (>= lo) attempts whenever at least one exists.  When none exists the
  // decline fires regardless, and plain band-closest picks the highest
  // score as the recorded evidence.
  bool any_shippable = false;
  if (decline_below_floor) {
    for (const VerifyAttempt& a : attempts) {
      if (a.score >= lo) {
        any_shippable = true;
        break;
      }
    }
  }
  const bool restrict_to_shippable = decline_below_floor && any_shippable;

  bool have_best = false;
  size_t best = 0;
  float best_distance = 0.0f;
  size_t best_size = 0;
  for (size_t i = 0; i < attempts.size(); ++i) {
    if (restrict_to_shippable && attempts[i].score < lo) continue;
    const float d = band_distance(attempts[i].score);
    if (!have_best || d < best_distance ||
        (d == best_distance && attempts[i].size <= best_size)) {
      have_best = true;
      best = i;
      best_distance = d;
      best_size = attempts[i].size;
    }
  }
  return best;
}

namespace {

// A converted-format slot may only ever hold bytes that ARE in that format.
// Transcode() falls through to the original-format optimizer whenever a
// conversion is refused or fails, so a per-format call can hand back the
// ORIGIN's bytes in the origin's own format -- a GIF, say, in answer to a
// request for AVIF. Storing that under the converted format's alternate id
// makes the entry selectable only by clients that advertise a format the
// content is not in, and the intra-notification content dedup then drops the
// identical original-format twin, so the family's only copy of those bytes
// ends up at an id most clients never ask for (#1374).
//
// Returns the result unchanged when it really is `mime`, and a failure
// carrying the reason when it is not. The unconverted bytes are not lost:
// the original-format leg of the same loop reproduces them at the
// original-format id, which every client can reach. When the original-format
// slot is not among the requested formats, it is because that alternate
// already exists.
TranscodeResult OnlyIfFormat(TranscodeResult result, std::string_view mime) {
  if (!result.success || std::string_view(result.output_mime_type) == mime) {
    return result;
  }
  return {false,
          {},
          {},
          "Fall-through produced " + result.output_mime_type + ", not " +
              std::string(mime) +
              "; unconverted bytes belong in the "
              "original-format slot"};
}

// Floor for quality cap: prevents over-aggressive capping from DQT
// misestimation on exotic encoders (Guetzli, Photoshop custom tables).
constexpr int kMinQualityCap = 30;

// Highest quality the encoders accept; the ceiling when nothing caps.
constexpr int kMaxEncoderQuality = 100;

// Ceiling for a same-format (JPEG->JPEG) re-encode: the detected source
// quality, floored at kMinQualityCap.  Returns kMaxEncoderQuality when the
// source quality is unknown or capping is disabled, i.e. "no ceiling".
//
// Encoding above the source cannot recover detail the source already
// discarded, so it only inflates the file (#1284).  The margin does not
// widen this; the floor is a separate guard against DQT UNDERestimation.
int EffectiveJpegQualityCap(int source_jpeg_quality,
                            const ImageTranscoderConfig& cfg) {
  if (cfg.no_quality_cap || source_jpeg_quality <= 0) {
    return kMaxEncoderQuality;
  }
  return std::clamp(source_jpeg_quality, kMinQualityCap, kMaxEncoderQuality);
}

// Apply the JPEG quality cap to |cfg|.  Returns the number of times the cap
// was applied (0 or 1).
int ApplyJpegQualityCap(int source_jpeg_quality, ImageTranscoderConfig* cfg,
                        pagespeed::MessageHandler* handler) {
  if (cfg->no_quality_cap || source_jpeg_quality <= 0) {
    return 0;
  }
  int cap = EffectiveJpegQualityCap(source_jpeg_quality, *cfg);
  if (cfg->jpeg_quality > cap) {
    if (handler != nullptr) {
      handler->Info("JPEG quality capped from %d to %d (source quality: %d)",
                    cfg->jpeg_quality, cap, source_jpeg_quality);
    }
    cfg->jpeg_quality = cap;
    return 1;
  }
  return 0;
}

// Monochrome tolerance for the gray 1<->3 comparability rule: channels
// must agree within this per pixel. NOT bit-exact on purpose -- VP8 chroma
// quantization rounds Cb/Cr for a gray source to 127/129, so the WebP
// decode-back of a grayscale encode comes back as not-quite-equal RGB.
constexpr int kMonochromeChannelTolerance = 2;

}  // namespace

// S3.2 comparability guard (declared in the header beside the verify
// template that consumes it): dims must match and bpp must match, with one
// carve-out -- a bpp=1 reference accepts a bpp=3 MONOCHROME candidate and
// the metric then runs at bpp=1 on the candidate's G channel. Everything
// else (1<->4, 4<->3, 3<->1, chroma ghosts) is not comparable.
ComparablePixels MakeComparable(const DecodedImage& reference,
                                const DecodedImage& candidate) {
  if (candidate.pixel_buffer.empty() || candidate.width != reference.width ||
      candidate.height != reference.height) {
    return {};
  }
  ComparablePixels comparable;
  if (candidate.bytes_per_pixel == reference.bytes_per_pixel) {
    comparable.ok = true;
    comparable.pixels =
        reinterpret_cast<const uint8_t*>(candidate.pixel_buffer.data());
    return comparable;
  }
  if (reference.bytes_per_pixel == 1 && candidate.bytes_per_pixel == 3) {
    const size_t count = static_cast<size_t>(candidate.width) *
                         static_cast<size_t>(candidate.height);
    const uint8_t* px =
        reinterpret_cast<const uint8_t*>(candidate.pixel_buffer.data());
    comparable.gray.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      const uint8_t r = px[i * 3];
      const uint8_t g = px[i * 3 + 1];
      const uint8_t b = px[i * 3 + 2];
      const uint8_t hi = std::max({r, g, b});
      const uint8_t lo = std::min({r, g, b});
      if (hi - lo > kMonochromeChannelTolerance) {
        return {};  // Chroma ghost: not the gray source re-encoded.
      }
      comparable.gray.push_back(static_cast<char>(g));
    }
    comparable.ok = true;
    comparable.pixels =
        reinterpret_cast<const uint8_t*>(comparable.gray.data());
    return comparable;
  }
  return {};
}

// The verify loop itself (VerifySsimulacra2Quality) is a template in the
// header, beside SelectVerifyAttempt: the decline-routing decision -- what
// happens when the metric produces no verdict -- has to be drivable with a
// synthetic scorer, because a metric-internal failure is not constructible
// through the encoders a transcoder-level fixture has (#1382).

namespace {

// Failure reason for a declined verify (S2). A measured score -- negative
// ones included -- is the decline evidence; a verdict that never existed
// (decode-back mismatch or a metric failure) says "no verdict" instead.
std::string DeclineReason(const char* format_name,
                          const Ssimulacra2VerifyResult& verify) {
  char reason[160];
  if (verify.verdict_missing) {
    std::snprintf(reason, sizeof(reason),
                  "%s quality verification failed (no verdict: output could "
                  "not be measured)",
                  format_name);
  } else {
    std::snprintf(reason, sizeof(reason),
                  "%s quality verification failed (score %.1f < floor %.1f)",
                  format_name, verify.score, verify.band_lo);
  }
  return std::string(reason);
}

}  // namespace

// OptimizeWebp is defined below the verify machinery it now consults
// (#1385); its declaration order in the class is unchanged.
TranscodeResult ImageTranscoder::OptimizeWebp(
    std::string_view input_data, const ImageTranscoderConfig& cfg,
    const DecodedImage* decoded, SameFormatWebpVerify* verify_out) {
  // A LOSSLESS origin keeps the old exemption (#1375): every
  // encoder this pipeline has for WebP is lossy, so re-encoding one is a
  // fidelity change the strictly-smaller gate cannot see -- it measures bytes,
  // not pixels, and a lossless source is an author decision about fidelity,
  // not an accident of quality settings.  Mirrors the GIF->WebP path, which
  // keeps palettized sources lossless for the same reason.  Lossy-with-alpha
  // (VP8X + ALPH + VP8) is NOT this class and re-encodes like any other lossy
  // origin.
  //
  // The guard lives HERE, not at the call site: this helper has more than one
  // caller -- the single-format original-format leg and the multi-format
  // original-format leg both reach it -- and a guard at one call site left the
  // other re-encoding a lossless origin. The verify below lives here for
  // the same reason.
  if (WebpIsLossless(input_data)) {
    return {true, std::string(input_data), "image/webp",
            "lossless WebP kept verbatim"};
  }

  DecodedImage own;
  if (decoded == nullptr) {
    own = DecodeToPixels(input_data);
    decoded = &own;
  }
  if (decoded->pixel_buffer.empty()) {
    return {false, {}, {}, "WebP decode failed"};
  }

  // The verify search drives the quality through this mutable copy, the
  // same way the cross-format arms drive their local_config.
  ImageTranscoderConfig vcfg = cfg;
  auto output = EncodeWebpFromPixels(*decoded, vcfg);
  if (!output.success) {
    return output;
  }

  // The binding verdict applies to the same-format re-encode too (#1385):
  // this lane used to ship on the size test alone, and a re-encode the
  // verifier condemns -- negative scores included -- or cannot measure
  // must not displace the origin's own bytes. Decline policy mirrors the
  // cross-format arms, NOT the capped same-format JPEG arm: the JPEG
  // accept-at-the-cap rule exists because a source-quality cap can make
  // the band unreachable by design (#1284), and no such cap binds this
  // search. On decline the caller falls back exactly as it does for a
  // no-savings re-encode: the origin keeps serving.
  if (cfg.quality_verify) {
    auto verify = VerifySsimulacra2Quality(
        *decoded, output,
        [&](const DecodedImage& src) {
          return EncodeWebpFromPixels(src, vcfg);
        },
        [this](std::string_view data) { return DecodeToPixels(data); },
        [&](const ComparablePixels& c) {
          return pagespeed::ComputeSSIMULACRA2(
              reinterpret_cast<const uint8_t*>(decoded->pixel_buffer.data()),
              c.pixels, decoded->width, decoded->height,
              decoded->bytes_per_pixel, handler_);
        },
        vcfg.webp_quality, 0, kMaxEncoderQuality, "WebP",
        vcfg.target_ssimulacra2, vcfg.ssimulacra2_tolerance,
        vcfg.ssimulacra2_max_attempts, vcfg.ssimulacra2_quality_step,
        VerifyDeclinePolicy::kDeclineBelowFloor, handler_);
    if (verify_out != nullptr) {
      verify_out->score = verify.score;
      verify_out->reencoded = verify.reencoded;
      verify_out->declined = verify.declined;
    }
    if (verify.declined) {
      if (verify_out != nullptr) {
        verify_out->reencoded = false;  // shipped-none
      }
      return {false, {}, {}, DeclineReason("WebP", verify)};
    }
  }

  // Only use the re-encode if it is actually smaller -- the same acceptance
  // rule OptimizeJpeg and OptimizePng apply. An origin the encoder cannot beat
  // keeps being served as it is.
  if (output.output_data.size() < input_data.size()) {
    return output;
  }
  return {false, {}, {}, "WebP optimization produced no size savings"};
}

int ImageTranscoder::DetectSourceJpegQuality(std::string_view input_data,
                                             MessageHandler* handler) {
  if (input_data.empty() ||
      ComputeImageType(input_data) != net_instaweb::IMAGE_JPEG) {
    return -1;
  }
  return image_compression::JpegUtils::GetImageQualityFromImage(
      input_data.data(), input_data.size(), handler);
}

MultiTranscodeResult ImageTranscoder::TranscodeMulti(
    std::string_view input_data,
    const std::vector<CapabilityMask::ImageFormat>& formats,
    int source_jpeg_quality) {
  return TranscodeMulti(input_data, formats, source_jpeg_quality, config());
}

MultiTranscodeResult ImageTranscoder::TranscodeMulti(
    std::string_view input_data,
    const std::vector<CapabilityMask::ImageFormat>& formats,
    int source_jpeg_quality, const ImageTranscoderConfig& config_override,
    bool skip_quality_cap, const DecodedImage* caller_decoded) {
  MultiTranscodeResult result;

  if (input_data.empty() || formats.empty()) {
    return result;
  }

  auto local_config = config_override;

  // Apply quality cap: cap is the "final authority" per design.
  // Skipped when the caller already applied it (e.g., TranscodeMultiResized
  // fast-path) to avoid double application.
  if (!skip_quality_cap) {
    result.quality_capped_count +=
        ApplyJpegQualityCap(source_jpeg_quality, &local_config, handler_);
  }
  result.source_jpeg_quality = source_jpeg_quality;
  auto image_type = ComputeImageType(input_data);

  // C2PA preserve fallback (multi-format). WebP/AVIF carry no manifest, and
  // the XMP/APP1 form is not reliably carried either. When a manifest is present,
  // skip the stripping variants and serve the ORIGINAL bytes (skip-not-strip); only
  // a JPEG->JPEG JUMBF manifest is preserved-and-optimized via OptimizeJpeg.
  // Detect-only; never decode/validate/re-emit.
  const bool mc2pa_jumbf =
      local_config.preserve_c2pa && ImageHasJumbfC2pa(input_data);
  const bool mc2pa_xmp =
      local_config.preserve_c2pa && ImageHasXmpC2pa(input_data);
  if (mc2pa_jumbf || mc2pa_xmp) {
    result.webp = {
        false, {}, {}, "C2PA present; WebP cannot carry the manifest"};
    result.avif = {
        false, {}, {}, "C2PA present; AVIF cannot carry the manifest"};
    if (image_type == net_instaweb::IMAGE_JPEG && mc2pa_jumbf && !mc2pa_xmp) {
      result.optimized_original = OptimizeJpeg(input_data, local_config);
      if (!result.optimized_original.success) {
        // No JPEG savings / failure: serve the original so the manifest survives.
        result.optimized_original = {true, std::string(input_data),
                                     "image/jpeg",
                                     "C2PA passthrough (no JPEG savings)"};
      }
    } else if (local_config.c2pa_carry &&
               image_type == net_instaweb::IMAGE_PNG) {
      // Level A carry (opt-in): recompress the PNG and re-splice its
      // caBX/iTXt chunks. WebP/AVIF still cannot carry the manifest and stay
      // unsuccessful (set above); only the optimized-original PNG carries it.
      result.optimized_original = OptimizePngWithC2paCarry(input_data);
    } else {
      result.optimized_original = {
          true, std::string(input_data),
          net_instaweb::ImageTypeToMimeTypeString(image_type),
          "C2PA passthrough (skip-not-strip)"};
    }
    return result;
  }

  // For GIF input, use specialized pipelines (animated GIF needs
  // its own path).  DecodeToPixels only handles static frames.
  bool is_gif = (image_type == net_instaweb::IMAGE_GIF);

  // Check if we need pixel decode (WebP or AVIF requested)
  bool need_pixels = false;
  for (auto fmt : formats) {
    if (fmt == CapabilityMask::ImageFormat::kWebP && !is_gif) {
      need_pixels = true;
    }
    if (fmt == CapabilityMask::ImageFormat::kAvif) {
      need_pixels = true;
    }
  }

  // A caller that already decoded these exact bytes hands the frame in via
  // |caller_decoded| so this call does not decode the source a second time
  // (#1407): the TranscodeMultiResized fast path decodes the source for
  // content analysis and viewport decisions before delegating here, and a
  // variant set that collapses to kOriginal-only raises no |need_pixels|
  // flag of its own -- without the hand-off the original-format arm decoded
  // the origin again, full size.
  DecodedImage decoded_storage;
  const DecodedImage* decoded =
      (caller_decoded != nullptr && !caller_decoded->pixel_buffer.empty())
          ? caller_decoded
          : nullptr;
  if (need_pixels && decoded == nullptr) {
    decoded_storage = DecodeToPixels(input_data);
    decoded = &decoded_storage;
    if (decoded->pixel_buffer.empty()) {
      // Decode failed - try individual paths as fallback.
      // This is expected for some small or unusual images where the
      // scanline reader can't extract raw pixels but the format-
      // specific optimizers work fine.
      if (handler_ != nullptr) {
        handler_->Info(
            "Multi-transcode: pixel decode failed, "
            "falling back to individual paths");
      }
      for (auto fmt : formats) {
        CapabilityMask mask;
        mask.set_image_format(fmt);
        auto single = Transcode(input_data, mask);
        // Whether the optimizer produced anything at all, remembered before
        // the slot rule can turn a success into a refusal.  The counter below
        // must record ONLY what the slot rule refused: OnlyIfFormat returns an
        // already-unsuccessful result unchanged, so keying the counter on the
        // result alone would also count decode failures and unsupported
        // formats -- cases where nothing was ever converted-and-then-rejected,
        // and where every description of this counter would be wrong.
        const bool single_ok = single.success;
        // Route by what came back, not by what was asked for: this path is
        // exactly where Transcode()'s fall-through can answer a conversion
        // request with the origin's own bytes (#1374). See OnlyIfFormat.
        switch (fmt) {
          case CapabilityMask::ImageFormat::kWebP:
            result.webp = OnlyIfFormat(std::move(single), "image/webp");
            if (single_ok && !result.webp.success) {
              ++result.unconverted_fallthrough;
            }
            break;
          case CapabilityMask::ImageFormat::kAvif:
            result.avif = OnlyIfFormat(std::move(single), "image/avif");
            if (single_ok && !result.avif.success) {
              ++result.unconverted_fallthrough;
            }
            break;
          case CapabilityMask::ImageFormat::kOriginal:
            // The original-format slot is the one slot whose contract is
            // "the bytes in the origin's format", so it takes the result
            // as-is.
            result.optimized_original = std::move(single);
            break;
          default:
            break;
        }
      }
      return result;
    }
  }

  // Apply save-data score reduction to the SSIMULACRA2 target so the
  // verification loops use the correct band.
  // Note: save-data quality overrides are already applied to local_config
  // by the caller (TranscodeMultiResized) when relevant.

  for (auto fmt : formats) {
    switch (fmt) {
      case CapabilityMask::ImageFormat::kWebP:
        if (is_gif) {
          result.webp = ConvertGifToWebp(input_data);
        } else if (WebpIsLossless(input_data)) {
          // Lossless origin: kept verbatim rather than re-encoded lossily
          // (#1375 -- same reasoning as the original-format leg).
          result.webp = {true, std::string(input_data), "image/webp",
                         "lossless WebP kept verbatim"};
        } else {
          // Lossy WebP inputs take this path too (#1375). They used to be
          // handed back untouched here, which left the slot every
          // WebP-negotiating client reads holding the origin's own bytes; the
          // size gate at the bottom of this arm is the acceptance rule that
          // makes re-encoding them safe.
          result.webp = EncodeWebpFromPixels(*decoded, local_config);

          // SSIMULACRA2 quality verification for WebP output.
          if (local_config.quality_verify && result.webp.success &&
              !decoded->pixel_buffer.empty()) {
            auto verify = VerifySsimulacra2Quality(
                *decoded, result.webp,
                [&](const DecodedImage& src) {
                  return EncodeWebpFromPixels(src, local_config);
                },
                [this](std::string_view data) { return DecodeToPixels(data); },
                [&](const ComparablePixels& c) {
                  return pagespeed::ComputeSSIMULACRA2(
                      reinterpret_cast<const uint8_t*>(
                          decoded->pixel_buffer.data()),
                      c.pixels, decoded->width, decoded->height,
                      decoded->bytes_per_pixel, handler_);
                },
                local_config.webp_quality, 0, kMaxEncoderQuality, "WebP",
                local_config.target_ssimulacra2,
                local_config.ssimulacra2_tolerance,
                local_config.ssimulacra2_max_attempts,
                local_config.ssimulacra2_quality_step,
                VerifyDeclinePolicy::kDeclineBelowFloor, handler_);
            result.webp_ssimulacra2_score = verify.score;
            result.webp_ssimulacra2_reencoded = verify.reencoded;
            if (verify.declined) {
              // The verdict is binding: a variant our own verifier condemns
              // or cannot measure must not ship (mpp #790). Keep the score
              // as decline evidence; nothing ships, so reencoded reads
              // false. The size gate below then sees !success.
              result.webp_ssimulacra2_declined = true;
              result.webp_ssimulacra2_reencoded = false;
              result.webp = {false, {}, {}, DeclineReason("WebP", verify)};
            }
          }

          // Only accept if smaller than original
          if (result.webp.success &&
              result.webp.output_data.size() >= input_data.size()) {
            result.webp = {false, {}, {}, "WebP output larger than original"};
          }
        }
        break;

      case CapabilityMask::ImageFormat::kAvif:
        if (image_type == net_instaweb::IMAGE_WEBP ||
            image_type == net_instaweb::IMAGE_WEBP_LOSSLESS_OR_ALPHA) {
          // Can't encode WebP -> AVIF
          result.avif = {false, {}, {}, "Cannot convert WebP to AVIF"};
        } else {
          result.avif = EncodeAvifFromPixels(*decoded, local_config);

          // SSIMULACRA2 quality verification for AVIF output.
          if (local_config.quality_verify && result.avif.success &&
              !decoded->pixel_buffer.empty()) {
            auto verify = VerifySsimulacra2Quality(
                *decoded, result.avif,
                [&](const DecodedImage& src) {
                  return EncodeAvifFromPixels(src, local_config);
                },
                [this](std::string_view data) { return DecodeToPixels(data); },
                [&](const ComparablePixels& c) {
                  return pagespeed::ComputeSSIMULACRA2(
                      reinterpret_cast<const uint8_t*>(
                          decoded->pixel_buffer.data()),
                      c.pixels, decoded->width, decoded->height,
                      decoded->bytes_per_pixel, handler_);
                },
                local_config.avif_quality, 0, kMaxEncoderQuality, "AVIF",
                local_config.target_ssimulacra2,
                local_config.ssimulacra2_tolerance,
                local_config.ssimulacra2_max_attempts,
                local_config.ssimulacra2_quality_step,
                VerifyDeclinePolicy::kDeclineBelowFloor, handler_);
            result.avif_ssimulacra2_score = verify.score;
            result.avif_ssimulacra2_reencoded = verify.reencoded;
            if (verify.declined) {
              result.avif_ssimulacra2_declined = true;
              result.avif_ssimulacra2_reencoded = false;
              result.avif = {false, {}, {}, DeclineReason("AVIF", verify)};
            }
          }

          // Only accept if smaller than original
          if (result.avif.success &&
              result.avif.output_data.size() >= input_data.size()) {
            result.avif = {false, {}, {}, "AVIF output larger than original"};
          }
        }
        break;

      case CapabilityMask::ImageFormat::kOriginal: {
        // Use the capped local_config for JPEG optimization so the
        // quality cap takes effect on the desktop fast-path.
        auto image_type_orig = ComputeImageType(input_data);
        if (image_type_orig == net_instaweb::IMAGE_JPEG) {
          // Flag when source quality is already at or below target:
          // TranscodeMulti always does stream-based optimization (no pixel
          // re-encode), so the flag records that we skipped a pixel round-trip.
          if (source_jpeg_quality > 0 &&
              source_jpeg_quality <= local_config.jpeg_quality) {
            result.skipped_jpeg_reencode = true;
            if (handler_) {
              handler_->Info(
                  "JPEG early exit (fast path): source quality %d <= target %d",
                  source_jpeg_quality, local_config.jpeg_quality);
            }
          }
          result.optimized_original = OptimizeJpeg(input_data, local_config);
        } else if (image_type_orig == net_instaweb::IMAGE_WEBP ||
                   image_type_orig ==
                       net_instaweb::IMAGE_WEBP_LOSSLESS_OR_ALPHA) {
          // Same shape as the JPEG leg above: call the format's optimizer
          // directly so the re-encode runs at THIS call's quality settings
          // rather than the transcoder's snapshot, and hand it the decode
          // available to this call when there is one -- a WebP origin then
          // costs one decode, not two (#1375, #1407).  On the
          // TranscodeMultiResized fast path that decode is the caller's,
          // threaded in via |caller_decoded|; on a kOriginal-only variant
          // set it is the ONLY decode the whole call performs.
          SameFormatWebpVerify vr;
          result.optimized_original =
              OptimizeWebp(input_data, local_config, decoded, &vr);
          // The original-format arm's evidence fields (#1385); with the
          // verify off they keep their defaults. Declines land in the
          // existing per-arm counter exactly like the JPEG arm's.
          result.ssimulacra2_score = vr.score;
          result.ssimulacra2_reencoded = vr.reencoded;
          result.ssimulacra2_declined = vr.declined;
        } else {
          CapabilityMask orig_mask;
          result.optimized_original = Transcode(input_data, orig_mask);
        }
        break;
      }

      default:
        break;
    }
  }

  // A converted variant replaces the original-format variant for the
  // clients that negotiate its format, so it must be smaller than THAT, not
  // only than the upload: a WebP or AVIF several times the size of the
  // optimized JPEG of the same size used to pass the gates above, which
  // compare with the original bytes.  The resizing path has always gated on
  // its own optimized original; this is the same rule for the full-size
  // path.  A WebP origin is exempt for the WebP arm: its original-format
  // slot is WebP too, and the WebP arm's own acceptance above decided it.
  if (result.optimized_original.success &&
      !result.optimized_original.output_data.empty()) {
    const size_t baseline = result.optimized_original.output_data.size();
    if (result.optimized_original.output_mime_type != "image/webp" &&
        result.webp.success && result.webp.output_data.size() >= baseline) {
      // A static GIF's WebP is the lossless conversion, which never went
      // through the verify and whose quality setting is only the default:
      // swapping it for a lossy re-encode would change the image class, so
      // it is refused without a recovery attempt.
      result.size_refused_webp = 1;
      if (decoded != nullptr && !is_gif) {
        RecoverOversizedConverted(result.webp, /*avif=*/false, *decoded,
                                  result.optimized_original, local_config,
                                  local_config.webp_quality, result);
      }
      if (result.webp.success && result.webp.output_data.size() >= baseline) {
        result.webp = {false,
                       {},
                       {},
                       "WebP output not smaller than the optimized original"};
      }
    }
    if (result.avif.success && result.avif.output_data.size() >= baseline) {
      result.size_refused_avif = 1;
      if (decoded != nullptr) {
        RecoverOversizedConverted(result.avif, /*avif=*/true, *decoded,
                                  result.optimized_original, local_config,
                                  local_config.avif_quality, result);
      }
      if (result.avif.success && result.avif.output_data.size() >= baseline) {
        result.avif = {false,
                       {},
                       {},
                       "AVIF output not smaller than the optimized original"};
      }
    }
  }

  // Record the final qualities so callers can carry them forward.
  result.final_webp_quality = local_config.webp_quality;
  result.final_avif_quality = local_config.avif_quality;

  return result;
}

void ImageTranscoder::RecoverOversizedConverted(
    TranscodeResult& arm, bool avif, const DecodedImage& reference,
    const TranscodeResult& baseline, const ImageTranscoderConfig& cfg,
    int& quality, MultiTranscodeResult& out) {
  const char* name = avif ? "AVIF" : "WebP";
  (avif ? out.size_refused_avif : out.size_refused_webp) = 1;
  if (reference.pixel_buffer.empty() || reference.width < 8 ||
      reference.height < 8) {
    return;  // Nothing to measure against.
  }
  auto score_fn = [&](const ComparablePixels& c) {
    return pagespeed::ComputeSSIMULACRA2(
        reinterpret_cast<const uint8_t*>(reference.pixel_buffer.data()),
        c.pixels, reference.width, reference.height, reference.bytes_per_pixel,
        handler_);
  };

  // What the competing image scores, once per call, for the log: the first
  // field data on whether it sits below the floor the converted arm must
  // reach.  No behaviour depends on it.
  if (out.refused_baseline_ssimulacra2 < 0.0f && cfg.quality_verify) {
    DecodedImage baseline_pixels = DecodeToPixels(baseline.output_data);
    ComparablePixels comparable = MakeComparable(reference, baseline_pixels);
    if (comparable.ok) {
      std::optional<float> verdict = score_fn(comparable);
      if (verdict.has_value()) {
        out.refused_baseline_ssimulacra2 = *verdict;
        if (handler_ != nullptr) {
          handler_->Info(
              "%s not smaller than the optimized original (%zu >= %zu bytes); "
              "the original-format image scores SSIMULACRA2 %.1f",
              name, arm.output_data.size(), baseline.output_data.size(),
              *verdict);
        }
      }
    }
  }

  const int attempts = avif ? std::min(1, cfg.budget_rescue_attempts)
                            : cfg.budget_rescue_attempts;
  if (!cfg.quality_verify || attempts <= 0) return;

  const float floor =
      BandFloor(cfg.target_ssimulacra2, cfg.ssimulacra2_tolerance);
  BudgetRescueResult rescue = RescueUnderBudget(
      reference, arm,
      [&](const DecodedImage& src) {
        return avif ? EncodeAvifFromPixels(src, cfg)
                    : EncodeWebpFromPixels(src, cfg);
      },
      [this](std::string_view data) { return DecodeToPixels(data); }, score_fn,
      quality, baseline.output_data.size(), floor, attempts,
      cfg.ssimulacra2_quality_step, name, handler_);
  if (!rescue.rescued) return;
  (avif ? out.size_rescued_avif : out.size_rescued_webp) = 1;
  (avif ? out.avif_ssimulacra2_score : out.webp_ssimulacra2_score) =
      rescue.score;
  (avif ? out.avif_ssimulacra2_reencoded : out.webp_ssimulacra2_reencoded) =
      true;
}

// Adapter: wraps a DecodedImage pixel buffer as a ScanlineReaderInterface
// so that ScanlineResizer can consume it.
namespace {
class PixelBufferReader : public ScanlineReaderInterface {
 public:
  PixelBufferReader(const DecodedImage& img, image_compression::PixelFormat pf)
      : data_(reinterpret_cast<const uint8_t*>(img.pixel_buffer.data())),
        width_(img.width),
        height_(img.height),
        bpp_(img.bytes_per_pixel),
        pixel_format_(pf) {}

  ScanlineStatus InitializeWithStatus(const void*, size_t) override {
    row_ = 0;
    return ScanlineStatus(net_instaweb::SCANLINE_STATUS_SUCCESS);
  }
  bool Reset() override {
    row_ = 0;
    return true;
  }
  size_t GetBytesPerScanline() override {
    return static_cast<size_t>(width_) * bpp_;
  }
  bool HasMoreScanLines() override { return row_ < height_; }
  ScanlineStatus ReadNextScanlineWithStatus(void** out) override {
    if (row_ >= height_) {
      return ScanlineStatus(net_instaweb::SCANLINE_STATUS_INVOCATION_ERROR);
    }
    scanline_ptr_ =
        const_cast<uint8_t*>(data_ + static_cast<size_t>(row_) * width_ * bpp_);
    *out = scanline_ptr_;
    ++row_;
    return ScanlineStatus(net_instaweb::SCANLINE_STATUS_SUCCESS);
  }
  size_t GetImageHeight() override { return height_; }
  size_t GetImageWidth() override { return width_; }
  image_compression::PixelFormat GetPixelFormat() override {
    return pixel_format_;
  }
  bool IsProgressive() override { return false; }

 private:
  const uint8_t* data_;
  uint32_t width_;
  uint32_t height_;
  int bpp_;
  uint32_t row_ = 0;
  uint8_t* scanline_ptr_ = nullptr;
  image_compression::PixelFormat pixel_format_;
};
}  // namespace

DecodedImage ImageTranscoder::ResizeForViewport(
    const DecodedImage& decoded, CapabilityMask::Viewport viewport,
    CapabilityMask::PixelDensity density) {
  auto cfg = config();
  uint32_t target_width = 0;
  switch (viewport) {
    case CapabilityMask::Viewport::kMobile:
      target_width = cfg.viewport_widths.mobile;
      break;
    case CapabilityMask::Viewport::kTablet:
      target_width = cfg.viewport_widths.tablet;
      break;
    case CapabilityMask::Viewport::kDesktop:
      target_width = cfg.viewport_widths.desktop;
      break;
  }

  // Apply density multiplier: 2x retina gets double the target width.
  if (target_width != 0 && density == CapabilityMask::PixelDensity::k2xPlus) {
    target_width *= 2;
  }

  // 0 = no resize, or image already fits within target.
  if (target_width == 0 || decoded.width <= target_width) {
    return {};
  }

  // Preserve aspect ratio.
  auto target_height = static_cast<uint32_t>(
      static_cast<uint64_t>(decoded.height) * target_width / decoded.width);
  if (target_height == 0) target_height = 1;

  image_compression::PixelFormat pf = image_compression::RGB_888;
  if (decoded.has_alpha) {
    pf = image_compression::RGBA_8888;
  } else if (decoded.bytes_per_pixel == 1) {
    pf = image_compression::GRAY_8;
  }

  PixelBufferReader reader(decoded, pf);
  reader.Reset();

  image_compression::ScanlineResizer resizer(handler_);
  if (!resizer.Initialize(&reader, target_width, target_height)) {
    if (handler_ != nullptr) {
      handler_->Warning("Image resize failed to initialize");
    }
    return {};
  }

  DecodedImage result;
  result.width = target_width;
  result.height = target_height;
  result.bytes_per_pixel = decoded.bytes_per_pixel;
  result.has_alpha = decoded.has_alpha;

  size_t row_bytes =
      static_cast<size_t>(target_width) * decoded.bytes_per_pixel;
  if (row_bytes > 0 &&
      static_cast<size_t>(target_height) > SIZE_MAX / row_bytes) {
    if (handler_ != nullptr) {
      handler_->Warning("Resized image dimensions overflow buffer size");
    }
    return {};
  }
  result.pixel_buffer.reserve(static_cast<size_t>(target_height) * row_bytes);

  while (resizer.HasMoreScanLines()) {
    void* scanline = nullptr;
    if (!resizer.ReadNextScanline(&scanline)) {
      if (handler_ != nullptr) {
        handler_->Warning("Image resize scanline read failed");
      }
      return {};
    }
    result.pixel_buffer.append(static_cast<const char*>(scanline), row_bytes);
  }

  return result;
}

bool ImageTranscoder::IsAnimatedGif(std::string_view data) {
  // A GIF with multiple Image Descriptor blocks (0x2C) is animated.
  if (data.size() < 13) return false;

  // Skip header (6B) + Logical Screen Descriptor (7B).
  // If Global Color Table present, skip it.
  size_t pos = 10;
  auto packed = static_cast<uint8_t>(data[pos]);
  bool has_gct = (packed & 0x80) != 0;
  if (has_gct) {
    int gct_size = 3 * (1 << ((packed & 0x07) + 1));
    pos = 13 + gct_size;
    if (pos > data.size()) return false;  // Malformed GIF.
  } else {
    pos = 13;
  }

  int image_count = 0;
  while (pos < data.size()) {
    auto block = static_cast<uint8_t>(data[pos]);
    if (block == 0x2C) {
      // Image Descriptor
      ++image_count;
      if (image_count > 1) return true;
      if (pos + 10 > data.size()) break;
      auto img_packed = static_cast<uint8_t>(data[pos + 9]);
      bool has_lct = (img_packed & 0x80) != 0;
      pos += 10;
      if (has_lct) {
        int lct_size = 3 * (1 << ((img_packed & 0x07) + 1));
        pos += lct_size;
        if (pos > data.size()) break;  // Malformed LCT.
      }
      // Skip LZW minimum code size byte
      if (pos >= data.size()) break;
      pos++;
      // Skip sub-blocks
      while (pos < data.size()) {
        auto sub_size = static_cast<uint8_t>(data[pos]);
        if (sub_size == 0) {
          pos++;
          break;
        }
        if (pos + 1 + sub_size > data.size()) {
          pos = data.size();
          break;
        }
        pos += 1 + sub_size;
      }
    } else if (block == 0x21) {
      // Extension block
      if (pos + 2 > data.size()) break;
      pos += 2;
      while (pos < data.size()) {
        auto sub_size = static_cast<uint8_t>(data[pos]);
        if (sub_size == 0) {
          pos++;
          break;
        }
        if (pos + 1 + sub_size > data.size()) {
          pos = data.size();
          break;
        }
        pos += 1 + sub_size;
      }
    } else {
      break;  // Trailer (0x3B) or unknown block
    }
  }
  return false;
}

MultiTranscodeResult ImageTranscoder::TranscodeMultiResized(
    std::string_view input_data,
    const std::vector<CapabilityMask::ImageFormat>& formats,
    CapabilityMask::Viewport viewport, CapabilityMask::PixelDensity density,
    CapabilityMask::SaveData save_data, JpegQualityInputs quality) {
  MultiTranscodeResult result;

  if (input_data.empty() || formats.empty()) {
    return result;
  }

  const int source_jpeg_quality = quality.source_quality;

  // Thread-safe: config() returns a snapshot from the accessor.
  // This is critical because multiple threads may call
  // TranscodeMultiResized() concurrently on the same ImageTranscoder.
  ImageTranscoderConfig local_config = config();

  bool use_save_data = (save_data == CapabilityMask::SaveData::kOn);

  // Apply save-data quality overrides AND the SSIMULACRA2 target reduction
  // together, ahead of every consumer of local_config below: the C2PA
  // delegation to TranscodeMulti, the learned-quality predictor's target,
  // and this path's own verification loops. The reduction used to happen
  // after the delegation, so the delegated call carried save-data quality
  // settings against the full-band floor -- a band the quality settings
  // were never chosen for (#1382).
  if (use_save_data) {
    local_config.jpeg_quality = local_config.savedata_jpeg_quality;
    local_config.webp_quality = local_config.savedata_webp_quality;
    local_config.avif_quality = local_config.savedata_avif_quality;
    local_config.target_ssimulacra2 =
        std::max(0.0f, local_config.target_ssimulacra2 -
                           local_config.savedata_score_reduction);
  }

  auto image_type = ComputeImageType(input_data);

  // C2PA preserve fallback: a resize or AVIF/WebP transcode would strip the
  // manifest. When a manifest is present, delegate to the non-resizing TranscodeMulti
  // path, whose gate serves the original (or JPEG-carried) bytes (skip-not-strip).
  // The PNG carry (recompress + re-splice) is intentionally NOT applied on the
  // resize path -- mirroring #214's !resized guard, a resized manifest-bearing PNG
  // is served byte-for-byte rather than recompressed -- so force c2pa_carry off for
  // the delegated call.
  if (local_config.preserve_c2pa && ImageHasC2paManifest(input_data)) {
    ImageTranscoderConfig no_png_carry = local_config;
    no_png_carry.c2pa_carry = false;
    return TranscodeMulti(input_data, formats, source_jpeg_quality,
                          no_png_carry);
  }

  bool is_gif = (image_type == net_instaweb::IMAGE_GIF);
  bool is_animated_gif = is_gif && IsAnimatedGif(input_data);

  // Check if viewport resizing applies (only meaningful if target > 0).
  uint32_t target_width = 0;
  switch (viewport) {
    case CapabilityMask::Viewport::kMobile:
      target_width = local_config.viewport_widths.mobile;
      break;
    case CapabilityMask::Viewport::kTablet:
      target_width = local_config.viewport_widths.tablet;
      break;
    case CapabilityMask::Viewport::kDesktop:
      target_width = local_config.viewport_widths.desktop;
      break;
  }

  // Apply density multiplier.
  if (target_width != 0 && density == CapabilityMask::PixelDensity::k2xPlus) {
    target_width *= 2;
  }

  // Animated GIFs can't be decoded to pixels; delegate directly.
  // The delegations below hand over local_config: it already carries the
  // Save-Data quality overrides, which a fresh config snapshot would drop.
  if (is_animated_gif) {
    return TranscodeMulti(input_data, formats, source_jpeg_quality,
                          local_config);
  }

  // Decode pixels.  Even paths that delegate to the stream-based
  // TranscodeMulti need decoded pixels for content analysis so the
  // result carries a valid content class.
  DecodedImage decoded = DecodeToPixels(input_data);
  if (decoded.pixel_buffer.empty()) {
    return TranscodeMulti(input_data, formats, source_jpeg_quality,
                          local_config);
  }

  // Resize if the image is wider than the viewport target.
  const DecodedImage* encode_src = &decoded;
  DecodedImage resized;
  if (target_width != 0 && decoded.width > target_width) {
    resized = ResizeForViewport(decoded, viewport, density);
    if (!resized.pixel_buffer.empty()) {
      encode_src = &resized;
    }
  }

  // The carried hint stands in for the JPEG verify search, which only runs on
  // resized pixels.  A full-size pass never searches, so it ignores the hint
  // and encodes at the quality chosen for the full-size image; otherwise the
  // full-size JPEG would depend on which smaller viewports ran before it in
  // the same loop, and the cross-format gate could refuse a WebP or AVIF
  // against that JPEG (#482).  The hint is passed through unchanged in
  // final_jpeg_quality so the caller's loop carries it on to the next
  // resized pass.
  const bool resized_pixels = encode_src != &decoded;
  const int jpeg_quality_hint = resized_pixels ? quality.carried_hint : -1;

  // Run content analysis on ORIGINAL (unresized) pixels so the
  // classification is consistent across all viewport sizes.  Content class
  // (photo/screenshot/illustration/noisy) is a property of the image
  // content, not its display resolution.
  std::optional<ContentAnalysisResult> cached_analysis;
  if (local_config.content_analysis) {
    cached_analysis = AnalyzeContent(
        reinterpret_cast<const uint8_t*>(decoded.pixel_buffer.data()),
        decoded.pixel_buffer.size(), decoded.width, decoded.height,
        decoded.bytes_per_pixel, handler_);
    if (cached_analysis.has_value()) {
      result.applied_preset = cached_analysis->quality_preset;
    }
  }

  // Content-aware quality adjustment: learned quality prediction and
  // content-class factors.  Runs BEFORE the fast-path so the desktop
  // path (target_width=0) gets adjusted quality settings too.
  //
  // For applies_at_viewport(): when target_width is 0 (desktop, no resize),
  // use the decoded image width so screenshot detection (requires >= 768px)
  // works correctly for full-size images.
  uint32_t viewport_for_preset =
      (target_width == 0) ? decoded.width : target_width;

  if (cached_analysis.has_value()) {
    const auto& analysis = *cached_analysis;
    result.applied_preset = analysis.quality_preset;

    // Learned quality prediction: use ML models to predict encoder quality
    // for the target SSIMULACRA2 score. Falls back to base*factor on invalid.
    bool used_learned = false;
    if (local_config.learned_quality &&
        result.applied_preset.applies_at_viewport(viewport_for_preset)) {
      // Map source image type to feature format code.
      uint8_t source_format = 0;  // JPEG
      switch (image_type) {
        case net_instaweb::IMAGE_PNG:
          source_format = 1;
          break;
        case net_instaweb::IMAGE_GIF:
          source_format = 2;
          break;
        case net_instaweb::IMAGE_WEBP:
        case net_instaweb::IMAGE_WEBP_LOSSLESS_OR_ALPHA:
          source_format = 3;
          break;
        default:
          break;
      }

      auto features = ExtractImageFeatures(
          analysis,
          reinterpret_cast<const uint8_t*>(encode_src->pixel_buffer.data()),
          encode_src->width, encode_src->height, encode_src->bytes_per_pixel,
          source_format, source_jpeg_quality);

      // Already carries the save-data reduction (applied with the quality
      // overrides at the top of this function).
      float target = local_config.target_ssimulacra2;

      if (local_config.learned_quality_jpeg) {
        int q = PredictQuality(PredictorFormat::kJpeg, features, target);
        if (IsValidPrediction(PredictorFormat::kJpeg, q)) {
          local_config.jpeg_quality = q;
          used_learned = true;
        } else {
          ++result.learned_quality_fallbacks;
        }
      }
      if (local_config.learned_quality_webp) {
        int q = PredictQuality(PredictorFormat::kWebP, features, target);
        if (IsValidPrediction(PredictorFormat::kWebP, q)) {
          local_config.webp_quality = q;
          used_learned = true;
        } else {
          ++result.learned_quality_fallbacks;
        }
      }
      if (local_config.learned_quality_avif) {
        int q = PredictQuality(PredictorFormat::kAvif, features, target);
        if (IsValidPrediction(PredictorFormat::kAvif, q)) {
          local_config.avif_quality = q;
          used_learned = true;
        } else {
          ++result.learned_quality_fallbacks;
        }
      }
      result.used_learned_quality = used_learned;
    }

    // Fallback: apply content-class quality factors if learned prediction
    // was not used (disabled, model unavailable, or invalid prediction).
    if (!used_learned &&
        result.applied_preset.applies_at_viewport(viewport_for_preset)) {
      auto apply_factor = [](int quality, float factor, int min_q, int max_q) {
        return std::clamp(static_cast<int>(quality * factor), min_q, max_q);
      };
      local_config.jpeg_quality =
          apply_factor(local_config.jpeg_quality,
                       result.applied_preset.jpeg_quality_factor, 30, 100);
      local_config.webp_quality =
          apply_factor(local_config.webp_quality,
                       result.applied_preset.webp_quality_factor, 20, 100);
      local_config.avif_quality = std::clamp(
          static_cast<int>(local_config.avif_quality *
                           result.applied_preset.avif_quality_factor),
          0, 100);
    }
  }

  // Apply JPEG quality hint from a previous call in the same proactive
  // loop.  This skips the SSIMULACRA2 search when the optimal quality
  // has already been discovered for this image.
  if (jpeg_quality_hint > 0) {
    local_config.jpeg_quality = jpeg_quality_hint;
  }

  // Quality cap: never re-encode JPEG higher than source quality + margin.
  // Applied after content-preset, learned quality, and hint — cap is the
  // final authority.
  result.quality_capped_count +=
      ApplyJpegQualityCap(source_jpeg_quality, &local_config, handler_);
  result.source_jpeg_quality = source_jpeg_quality;

  // Desktop (target_width=0) or no-resize without save-data: use the
  // stream-based encoder with pre-adjusted quality settings.  Content
  // analysis, learned quality prediction, and quality cap have already
  // been applied to local_config.  Both legs of the condition guarantee
  // no resize happened, so |decoded| still holds the full-size frame and
  // is handed over: the delegated call reuses it instead of decoding the
  // source a second time (#1407).
  if ((target_width == 0 ||
       (encode_src == &decoded && jpeg_quality_hint <= 0)) &&
      !use_save_data) {
    auto fast = TranscodeMulti(input_data, formats, source_jpeg_quality,
                               local_config, /*skip_quality_cap=*/true,
                               /*caller_decoded=*/&decoded);
    fast.applied_preset = result.applied_preset;
    fast.used_learned_quality = result.used_learned_quality;
    fast.learned_quality_fallbacks = result.learned_quality_fallbacks;
    fast.quality_capped_count += result.quality_capped_count;
    if (quality.carried_hint > 0) {
      fast.final_jpeg_quality = quality.carried_hint;  // Passed through.
    }
    return fast;
  }

  // Encode each requested format from the (possibly resized) pixels,
  // using the adjusted local_config for quality settings.
  // Process kOriginal first so we have a resized-original baseline for the
  // WebP/AVIF size gate (otherwise the gate compares against the full original
  // input, which is much larger when the image was resized).
  auto sorted_formats = formats;
  std::stable_partition(sorted_formats.begin(), sorted_formats.end(),
                        [](CapabilityMask::ImageFormat f) {
                          return f == CapabilityMask::ImageFormat::kOriginal;
                        });
  for (auto fmt : sorted_formats) {
    switch (fmt) {
      case CapabilityMask::ImageFormat::kWebP: {
        if (WebpIsLossless(input_data)) {
          // The lossless exemption has to hold on this path too (#1375).
          // This is the fourth route into a WebP encode and the only one that
          // encodes from RESIZED pixels, so it is the one place the exemption
          // could be read as "resize instead of re-encode" -- it is not.
          // Every encoder here is lossy, and a resized lossy re-encode is
          // still a lossy re-encode; the size gate below would accept it
          // (fewer pixels are always smaller), so the gate cannot be relied on
          // to catch it.  The origin is kept verbatim, exactly as on the
          // non-resizing path, and the viewport slot then holds the same bytes
          // the original-format slot does.
          result.webp = {true, std::string(input_data), "image/webp",
                         "lossless WebP kept verbatim"};
          break;
        }
        auto webp = EncodeWebpFromPixels(*encode_src, local_config);

        // SSIMULACRA2 quality verification for WebP output.
        if (local_config.quality_verify && webp.success &&
            !encode_src->pixel_buffer.empty()) {
          auto verify = VerifySsimulacra2Quality(
              *encode_src, webp,
              [&](const DecodedImage& src) {
                return EncodeWebpFromPixels(src, local_config);
              },
              [this](std::string_view data) { return DecodeToPixels(data); },
              [&](const ComparablePixels& c) {
                return pagespeed::ComputeSSIMULACRA2(
                    reinterpret_cast<const uint8_t*>(
                        encode_src->pixel_buffer.data()),
                    c.pixels, encode_src->width, encode_src->height,
                    encode_src->bytes_per_pixel, handler_);
              },
              local_config.webp_quality, 0, kMaxEncoderQuality, "WebP",
              local_config.target_ssimulacra2,
              local_config.ssimulacra2_tolerance,
              local_config.ssimulacra2_max_attempts,
              local_config.ssimulacra2_quality_step,
              VerifyDeclinePolicy::kDeclineBelowFloor, handler_);
          result.webp_ssimulacra2_score = verify.score;
          result.webp_ssimulacra2_reencoded = verify.reencoded;
          if (verify.declined) {
            result.webp_ssimulacra2_declined = true;
            result.webp_ssimulacra2_reencoded = false;
            webp = {false, {}, {}, DeclineReason("WebP", verify)};
          }
        }

        // Use the optimized original (resized JPEG) as the size gate
        // baseline when available, instead of the full original input --
        // except for a same-format origin (#1380): a WebP origin's
        // original-format slot is itself WebP and holds this arm's own
        // encode, so "strictly smaller than the optimized original" reads
        // as "strictly smaller than itself" and would refuse every
        // candidate.  For a same-format origin the baseline is the ORIGIN
        // bytes, the rule the non-resizing path and the #1375 re-encode
        // acceptance use.  ("Holds this arm's own encode" is exact with
        // verification off; under quality_verify this arm may ship a
        // verified re-encode while the slot holds the initial encode.  The
        // carve-out keys on the slot's produced MIME type, not byte
        // identity, so the origin-bytes baseline holds either way.)
        // Never larger than the source itself: the original wins.
        size_t webp_baseline = std::min(
            (result.optimized_original.success &&
             !result.optimized_original.output_data.empty() &&
             result.optimized_original.output_mime_type != "image/webp")
                ? result.optimized_original.output_data.size()
                : input_data.size(),
            input_data.size());
        if (webp.success && webp.output_data.size() >= webp_baseline) {
          // Only the cross-format gate gets a second try (#406): a WebP
          // origin's baseline is its own bytes, and a missing optimized
          // original leaves the upload gate.
          if (result.optimized_original.success &&
              !result.optimized_original.output_data.empty() &&
              result.optimized_original.output_mime_type != "image/webp") {
            RecoverOversizedConverted(webp, /*avif=*/false, *encode_src,
                                      result.optimized_original, local_config,
                                      local_config.webp_quality, result);
          }
          if (webp.success && webp.output_data.size() >= webp_baseline) {
            webp = {false, {}, {}, "WebP output larger than original"};
          }
        }
        result.webp = std::move(webp);
        break;
      }
      case CapabilityMask::ImageFormat::kAvif: {
        auto avif = EncodeAvifFromPixels(*encode_src, local_config);

        // SSIMULACRA2 quality verification for AVIF output.
        if (local_config.quality_verify && avif.success &&
            !encode_src->pixel_buffer.empty()) {
          auto verify = VerifySsimulacra2Quality(
              *encode_src, avif,
              [&](const DecodedImage& src) {
                return EncodeAvifFromPixels(src, local_config);
              },
              [this](std::string_view data) { return DecodeToPixels(data); },
              [&](const ComparablePixels& c) {
                return pagespeed::ComputeSSIMULACRA2(
                    reinterpret_cast<const uint8_t*>(
                        encode_src->pixel_buffer.data()),
                    c.pixels, encode_src->width, encode_src->height,
                    encode_src->bytes_per_pixel, handler_);
              },
              local_config.avif_quality, 0, kMaxEncoderQuality, "AVIF",
              local_config.target_ssimulacra2,
              local_config.ssimulacra2_tolerance,
              local_config.ssimulacra2_max_attempts,
              local_config.ssimulacra2_quality_step,
              VerifyDeclinePolicy::kDeclineBelowFloor, handler_);
          result.avif_ssimulacra2_score = verify.score;
          result.avif_ssimulacra2_reencoded = verify.reencoded;
          if (verify.declined) {
            result.avif_ssimulacra2_declined = true;
            result.avif_ssimulacra2_reencoded = false;
            avif = {false, {}, {}, DeclineReason("AVIF", verify)};
          }
        }

        // Use the optimized original (resized JPEG) as the size gate
        // baseline when available, instead of the full original input.
        size_t avif_baseline =
            std::min((result.optimized_original.success &&
                      !result.optimized_original.output_data.empty())
                         ? result.optimized_original.output_data.size()
                         : input_data.size(),
                     input_data.size());
        if (avif.success && avif.output_data.size() >= avif_baseline) {
          if (result.optimized_original.success &&
              !result.optimized_original.output_data.empty()) {
            RecoverOversizedConverted(avif, /*avif=*/true, *encode_src,
                                      result.optimized_original, local_config,
                                      local_config.avif_quality, result);
          }
          if (avif.success && avif.output_data.size() >= avif_baseline) {
            avif = {false, {}, {}, "AVIF output larger than original"};
          }
        }
        result.avif = std::move(avif);
        break;
      }
      case CapabilityMask::ImageFormat::kOriginal: {
        // Early exit: if source JPEG is already at or below the target
        // quality and the image was not resized, skip the full
        // pixel decode→encode round-trip.  Use stream-based Transcode()
        // for lightweight optimization (lossless Huffman, progressive).
        if (source_jpeg_quality > 0 &&
            source_jpeg_quality <= local_config.jpeg_quality &&
            encode_src == &decoded && image_type == net_instaweb::IMAGE_JPEG) {
          // OptimizeJpeg at THIS call's local_config, not Transcode(): that
          // takes a fresh config snapshot and so drops the Save-Data
          // quality and the content tuning applied above.
          result.optimized_original = OptimizeJpeg(input_data, local_config);
          result.skipped_jpeg_reencode = true;
          if (handler_) {
            handler_->Info(
                "JPEG early exit: source quality %d <= target %d, "
                "skipping pixel re-encode",
                source_jpeg_quality, local_config.jpeg_quality);
          }
          break;
        }

        if (image_type == net_instaweb::IMAGE_JPEG) {
          if (encode_src != &decoded) {
            // Pixels were resized: encode from the modified
            // pixel buffer.  This ensures resized variants get correct
            // dimensions, unlike OptimizeJpeg(input_data) which re-encodes
            // the full-size original.
            result.optimized_original =
                EncodeJpegFromPixels(*encode_src, local_config);
            if (!result.optimized_original.success) {
              // Pixel encode failed: stream-optimize the original bytes.
              result.optimized_original =
                  OptimizeJpeg(input_data, local_config);
              break;
            }
            // Fall through to the verify loop and size gate below.
          } else {
            // No resize (and the early exit above did not fire):
            // stream-based optimization of the original bytes, at THIS
            // call's local_config.  This is the full-size Save-Data JPEG
            // (or one carrying a quality hint; the fast path above takes
            // every other full-size case), and it used to go through
            // Transcode(), whose fresh config snapshot dropped the
            // Save-Data quality: the Save-Data JPEG came out at the default
            // quality, larger than the normal one.
            result.optimized_original = OptimizeJpeg(input_data, local_config);
            break;
          }
        } else if (image_type == net_instaweb::IMAGE_WEBP ||
                   image_type == net_instaweb::IMAGE_WEBP_LOSSLESS_OR_ALPHA) {
          // Explicit original-format leg (#1380), the same shape the
          // non-resizing path has had since #1375: re-encode at THIS call's
          // local_config -- save-data overrides, content-class factors and
          // learned-quality predictions included -- reusing the decode this
          // call already did.  The old fall-through to Transcode() re-read
          // a fresh config snapshot (dropping exactly the save-data
          // override) and decoded the origin a second time, full size.
          if (WebpIsLossless(input_data)) {
            // The #1375 lossless exemption (see OptimizeWebp) binds here
            // too: every encoder this pipeline has for WebP is lossy, so
            // the origin's fidelity decision wins over any size win, and
            // the slot carries the origin bytes verbatim.
            result.optimized_original = {true, std::string(input_data),
                                         "image/webp",
                                         "lossless WebP kept verbatim"};
            break;
          }
          // Encode from the encode source (the resized pixels when a
          // resize applied) so the variant written at a resized viewport
          // id carries the viewport's dimensions, like the JPEG leg above.
          // (#1380: decided rather than inherited; this slot used to
          // re-encode the FULL-SIZE origin under a resized viewport id.)
          auto webp = EncodeWebpFromPixels(*encode_src, local_config);
          if (!webp.success) {
            result.optimized_original = std::move(webp);
            break;
          }
          // The binding verdict guards this leg exactly as it guards the
          // non-resizing same-format lane (#1385): a re-encode the
          // verifier condemns -- negative scores included -- or cannot
          // measure must not displace the origin. Verified at THIS
          // call's config against the pixels actually encoded (the
          // resized encode source), the same reference the negotiated
          // resized WebP arm verifies against; the decline lands on the
          // original-format arm's evidence fields like every other
          // route's.
          if (local_config.quality_verify &&
              !encode_src->pixel_buffer.empty()) {
            auto verify = VerifySsimulacra2Quality(
                *encode_src, webp,
                [&](const DecodedImage& src) {
                  return EncodeWebpFromPixels(src, local_config);
                },
                [this](std::string_view data) { return DecodeToPixels(data); },
                [&](const ComparablePixels& c) {
                  return pagespeed::ComputeSSIMULACRA2(
                      reinterpret_cast<const uint8_t*>(
                          encode_src->pixel_buffer.data()),
                      c.pixels, encode_src->width, encode_src->height,
                      encode_src->bytes_per_pixel, handler_);
                },
                local_config.webp_quality, 0, kMaxEncoderQuality, "WebP",
                local_config.target_ssimulacra2,
                local_config.ssimulacra2_tolerance,
                local_config.ssimulacra2_max_attempts,
                local_config.ssimulacra2_quality_step,
                VerifyDeclinePolicy::kDeclineBelowFloor, handler_);
            result.ssimulacra2_score = verify.score;
            result.ssimulacra2_reencoded = verify.reencoded;
            if (verify.declined) {
              result.ssimulacra2_declined = true;
              result.ssimulacra2_reencoded = false;
              result.optimized_original = {
                  false, {}, {}, DeclineReason("WebP", verify)};
              break;
            }
          }
          // Acceptance: strictly smaller than the origin bytes (#1375).
          if (webp.output_data.size() < input_data.size()) {
            result.optimized_original = std::move(webp);
          } else {
            result.optimized_original = {
                false, {}, {}, "WebP optimization produced no size savings"};
          }
          break;
        } else if (image_type == net_instaweb::IMAGE_PNG) {
          // Explicit leg (#1380): the PNG original-format slot keeps the
          // stream-based optimizer, deliberately full-size.  This pipeline
          // has no palette-preserving PNG pixel encoder, and a
          // decode-pixels-re-encode round
          // trip would un-palette palettized origins (more bytes for the
          // same pixels), so the slot stays at origin dimensions at every
          // viewport, like GIF below.
          result.optimized_original = OptimizePng(input_data);
          break;
        } else if (image_type == net_instaweb::IMAGE_GIF) {
          // Explicit leg (#1380): GIF has no in-place optimizer in this
          // pipeline (animated GIF never reaches here; it delegates to
          // TranscodeMulti above), so the slot carries the origin verbatim.
          result.optimized_original = {
              true, std::string(input_data), "image/gif", {}};
          break;
        } else {
          // Anything else this pipeline cannot re-encode in its own format
          // (AVIF origins today): the stream fallback reports it.
          CapabilityMask orig_mask;
          result.optimized_original = Transcode(input_data, orig_mask);
          break;
        }

        // SSIMULACRA2 quality verification for JPEG output.  The search runs
        // under the same-format quality cap: chasing the score band by
        // encoding above the source quality would undo the cap (#1284).
        // Decline is NOT armed on the score: below the cap the band can be
        // genuinely unreachable and the best encode under the cap must
        // still ship (accept-at-ceiling, #1284). A candidate with NO
        // verdict -- unmeasurable decode-back or a metric failure -- is
        // still declined: fail-closed applies at every call site (#1382).
        if (local_config.quality_verify && result.optimized_original.success &&
            result.optimized_original.output_mime_type == "image/jpeg" &&
            !encode_src->pixel_buffer.empty()) {
          const int jpeg_ceiling =
              EffectiveJpegQualityCap(source_jpeg_quality, local_config);
          auto verify = VerifySsimulacra2Quality(
              *encode_src, result.optimized_original,
              [&](const DecodedImage& src) {
                return EncodeJpegFromPixels(src, local_config);
              },
              [this](std::string_view data) { return DecodeToPixels(data); },
              [&](const ComparablePixels& c) {
                return pagespeed::ComputeSSIMULACRA2(
                    reinterpret_cast<const uint8_t*>(
                        encode_src->pixel_buffer.data()),
                    c.pixels, encode_src->width, encode_src->height,
                    encode_src->bytes_per_pixel, handler_);
              },
              local_config.jpeg_quality, 1, jpeg_ceiling, "",
              local_config.target_ssimulacra2,
              local_config.ssimulacra2_tolerance,
              local_config.ssimulacra2_max_attempts,
              local_config.ssimulacra2_quality_step,
              VerifyDeclinePolicy::kShipBelowFloor, handler_);
          result.ssimulacra2_score = verify.score;
          result.ssimulacra2_reencoded = verify.reencoded;
          if (verify.declined) {
            result.ssimulacra2_declined = true;
            result.ssimulacra2_reencoded = false;
            result.optimized_original = {
                false, {}, {}, DeclineReason("JPEG", verify)};
          }
        }

        // Size gate: a pixel-based JPEG that is not smaller than the source
        // is not stored; the original is served unchanged.  This holds for
        // resized encodes too: the quality search above never trades the
        // size rule for the score band.
        if (result.optimized_original.success &&
            result.optimized_original.output_data.size() >= input_data.size()) {
          result.optimized_original = {
              false, {}, {}, "JPEG re-encode produced no size savings"};
        }
        break;
      }
      default:
        break;
    }
  }

  // Record the final qualities so callers can carry them forward.
  result.final_jpeg_quality = (!resized_pixels && quality.carried_hint > 0)
                                  ? quality.carried_hint
                                  : local_config.jpeg_quality;
  result.final_webp_quality = local_config.webp_quality;
  result.final_avif_quality = local_config.avif_quality;

  return result;
}

}  // namespace pagespeed
