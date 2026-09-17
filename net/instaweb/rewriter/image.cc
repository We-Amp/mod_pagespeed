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

#include "net/instaweb/rewriter/public/image.h"

#include <algorithm>
#include <cstddef>
#include <memory>

extern "C" {
#include <zlib.h>  // Provided by @envoy//bazel:zlib
}  // extern "C"

#include "base/logging.h"
#include "net/instaweb/rewriter/cached_result.pb.h"
#include "net/instaweb/rewriter/public/image_data_lookup.h"
#include "net/instaweb/rewriter/public/image_url_encoder.h"
#include "net/instaweb/rewriter/public/webp_optimizer.h"
#include "pagespeed/kernel/base/annotated_message_handler.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/content_type.h"
#include "pagespeed/kernel/image/avif_optimizer.h"
#include "pagespeed/kernel/image/gif_reader.h"
#include "pagespeed/kernel/image/image_analysis.h"
#include "pagespeed/kernel/image/image_converter.h"
#include "pagespeed/kernel/image/image_frame_interface.h"
#include "pagespeed/kernel/image/image_resizer.h"
#include "pagespeed/kernel/image/image_util.h"
#include "pagespeed/kernel/image/jpeg_optimizer.h"
#include "pagespeed/kernel/image/jpeg_utils.h"
#include "pagespeed/kernel/image/png_optimizer.h"
#include "pagespeed/kernel/image/read_image.h"
#include "pagespeed/kernel/image/scanline_interface.h"
#include "pagespeed/kernel/image/scanline_status.h"
#include "pagespeed/kernel/image/scanline_utils.h"
#include "pagespeed/kernel/image/webp_optimizer.h"

extern "C" {
#ifdef USE_SYSTEM_LIBWEBP
#include "webp/decode.h"
#else
#include "external/libwebp/src/webp/decode.h"
#endif
#ifdef USE_SYSTEM_LIBPNG
#include "png.h"  // NOLINT
#else
#include "external/libpng/png.h"
#endif
}

using pagespeed::image_compression::AnalyzeImage;
using pagespeed::image_compression::ConversionTimeoutHandler;
using pagespeed::image_compression::CreateScanlineReader;
using pagespeed::image_compression::CreateScanlineWriter;
using pagespeed::image_compression::GifReader;
using pagespeed::image_compression::GRAY_8;
using pagespeed::image_compression::ImageConverter;
using pagespeed::image_compression::ImageFormat;
using pagespeed::image_compression::ImageFormatToString;
using pagespeed::image_compression::JpegCompressionOptions;
using pagespeed::image_compression::JpegScanlineWriter;
using pagespeed::image_compression::JpegUtils;
using pagespeed::image_compression::OptimizeJpegWithOptions;
using pagespeed::image_compression::PixelFormat;
using pagespeed::image_compression::PngCompressParams;
using pagespeed::image_compression::PngOptimizer;
using pagespeed::image_compression::PngReader;
using pagespeed::image_compression::PngReaderInterface;
using pagespeed::image_compression::PngScanlineWriter;
using pagespeed::image_compression::PreferredLibwebpLevel;
using pagespeed::image_compression::RETAIN;
using pagespeed::image_compression::RGB_888;
using pagespeed::image_compression::RGBA_8888;
using pagespeed::image_compression::ScanlineReaderInterface;
using pagespeed::image_compression::ScanlineResizer;
using pagespeed::image_compression::ScanlineWriterInterface;
using pagespeed::image_compression::WEBP_ANIMATED;
using pagespeed::image_compression::WEBP_LOSSLESS;
using pagespeed::image_compression::WEBP_LOSSY;
using pagespeed::image_compression::WEBP_NONE;
using pagespeed::image_compression::WebpConfiguration;

namespace net_instaweb {

namespace ImageHeaders {

const char kPngHeader[] = "\x89PNG\r\n\x1a\n";
const size_t kPngHeaderLength = STATIC_STRLEN(kPngHeader);
const char kPngIHDR[] = "\0\0\0\x0dIHDR";
const size_t kPngIntSize = 4;
const size_t kPngSectionHeaderLength = 2 * kPngIntSize;
const size_t kIHDRDataStart = kPngHeaderLength + kPngSectionHeaderLength;
const size_t kPngSectionMinSize = kPngSectionHeaderLength + kPngIntSize;
const size_t kPngColourTypeOffset = kIHDRDataStart + 2 * kPngIntSize + 1;
const char kPngAlphaChannel = 0x4;  // bit of ColourType set for alpha channel
const char kPngIDAT[] = "IDAT";
const char kPngtRNS[] = "tRNS";

const char kGifHeader[] = "GIF8";
const size_t kGifHeaderLength = STATIC_STRLEN(kGifHeader);
const size_t kGifDimStart = kGifHeaderLength + 2;
const size_t kGifIntSize = 2;

const size_t kJpegIntSize = 2;
const int64 kMaxJpegQuality = 100;
const int64 kQualityForJpegWithUnkownQuality = 85;

}  // namespace ImageHeaders

namespace {

const char kGifString[] = "gif";
const char kPngString[] = "png";
const uint8 kAlphaOpaque = 255;

// Records the outcome of one encode attempt into the per-source-format stats
// bucket `var_type` of `conversion_vars` (the WebP family or the AVIF family).
// Each individual pointer is null-checked: a ConversionVariables family only
// registers the buckets it actually uses, so an unregistered bucket must be a
// no-op rather than a crash.
//
// `was_timed_out` and `overran_budget` describe two DIFFERENT things and are
// deliberately not folded together:
//
//   was_timed_out  The conversion produced NO usable output because of the
//                  timeout -- it was aborted at a frame boundary, or (an AVIF
//                  still) declined before it started.  Mutually exclusive with
//                  ok, which the DCHECK below still enforces.
//   overran_budget The conversion DID produce usable output, but took longer
//                  than the caller's timeout allowed.  This is orthogonal to
//                  the timeout/success/failure trichotomy, not a fourth value
//                  of it: the encode succeeded, so it is also recorded in
//                  success_ms and `ok` stays true.  Counting it as a timeout
//                  instead would both trip that DCHECK and misreport a served
//                  image as a conversion that never happened.
void UpdateConversionStats(bool ok, bool was_timed_out, bool overran_budget,
                           int64 time_elapsed_ms,
                           Image::ConversionVariables::VariableType var_type,
                           Image::ConversionVariables* conversion_vars) {
  if (conversion_vars != nullptr) {
    Image::ConversionBySourceVariable* the_var = conversion_vars->Get(var_type);
    if (the_var != nullptr) {
      if (overran_budget && the_var->overrun_count != nullptr) {
        the_var->overrun_count->Add(1);
      }
      if (was_timed_out) {
        DCHECK(!ok);
        if (the_var->timeout_count != nullptr) {
          the_var->timeout_count->Add(1);
        }
      } else if (ok) {
        if (the_var->success_ms != nullptr) {
          the_var->success_ms->Add(time_elapsed_ms);
        }
      } else {
        if (the_var->failure_ms != nullptr) {
          the_var->failure_ms->Add(time_elapsed_ms);
        }
      }
    }
  }
}

// TODO(huibao): Unify ImageType and ImageFormat.
ImageFormat ImageTypeToImageFormat(ImageType type) {
  ImageFormat format = pagespeed::image_compression::IMAGE_UNKNOWN;
  switch (type) {
    case IMAGE_UNKNOWN:
      format = pagespeed::image_compression::IMAGE_UNKNOWN;
      break;
    case IMAGE_JPEG:
      format = pagespeed::image_compression::IMAGE_JPEG;
      break;
    case IMAGE_PNG:
      format = pagespeed::image_compression::IMAGE_PNG;
      break;
    case IMAGE_GIF:
      format = pagespeed::image_compression::IMAGE_GIF;
      break;
    case IMAGE_WEBP:
    case IMAGE_WEBP_LOSSLESS_OR_ALPHA:
    case IMAGE_WEBP_ANIMATED:
      format = pagespeed::image_compression::IMAGE_WEBP;
      break;
    case IMAGE_AVIF:
    case IMAGE_AVIF_LOSSLESS_OR_ALPHA:
    case IMAGE_AVIF_ANIMATED:
      // AVIF sub-variants collapse to the single IMAGE_AVIF ImageFormat,
      // mirroring the WebP collapse above.
      format = pagespeed::image_compression::IMAGE_AVIF;
      break;
  }
  return format;
}

ImageFormat GetOutputImageFormat(ImageFormat in_format) {
  // GIF is decoded/re-encoded as PNG; every other format (JPEG, PNG, WebP, and
  // now IMAGE_AVIF) is its own output format and passes through unchanged. AVIF
  // therefore needs no explicit case here.
  if (in_format == pagespeed::image_compression::IMAGE_GIF) {
    return pagespeed::image_compression::IMAGE_PNG;
  } else {
    return in_format;
  }
}

ScanlineWriterInterface* CreateUncompressedPngWriter(
    size_t width, size_t height, GoogleString* output, MessageHandler* handler,
    bool use_transparent_for_blank_image) {
  PngCompressParams config(PNG_FILTER_NONE, Z_NO_COMPRESSION, false);
  PixelFormat pixel_format =
      use_transparent_for_blank_image ? RGBA_8888 : RGB_888;
  return CreateScanlineWriter(pagespeed::image_compression::IMAGE_PNG,
                              pixel_format, width, height, &config, output,
                              handler);
}

}  // namespace

// TODO(jmaessen): Put ImageImpl into private namespace.

class ImageImpl : public Image {
 public:
  ImageImpl(const StringPiece& original_contents, const GoogleString& url,
            const StringPiece& file_prefix, CompressionOptions* options,
            Timer* timer, MessageHandler* handler);
  ImageImpl(int width, int height, ImageType type, const StringPiece& tmp_dir,
            Timer* timer, MessageHandler* handler, CompressionOptions* options);

  void Dimensions(ImageDim* natural_dim) override;
  bool ResizeTo(const ImageDim& new_dim) override;
  bool DrawImage(Image* image, int x, int y) override;
  bool EnsureLoaded(bool output_useful) override;
  bool ShouldConvertToProgressive(int64 quality) const override;
  void SetResizedDimensions(const ImageDim& dims) override { dims_ = dims; }
  void SetTransformToLowRes() override;
  const GoogleString& url() override { return url_; }
  const GoogleString& debug_message() override { return debug_message_; }
  const GoogleString& resize_debug_message() override {
    return resize_debug_message_;
  }

  void SetDebugMessageUrl(const GoogleString& url) override {
    // We add a space here so we can format-in empty one by default.
    debug_message_url_ = StrCat(" ", url);
  }

  bool GenerateBlankImage();

  StringPiece original_contents() { return original_contents_; }

 private:
  // Maximum number of libpagespeed conversion attempts per image. Three
  // conversion sites can share this budget on the JPEG path: the speculative
  // WebP probe, the speculative AVIF probe (pick-smaller), and the
  // guaranteed jpeg-recompress fallback. The cap bounds CPU spent on
  // pathological images while guaranteeing the recompress fallback stays
  // reachable after BOTH speculative lossy probes fail; at the pre-AVIF value
  // of 2, a both-capable request whose WebP and AVIF encodes both failed would
  // exhaust the budget and serve the original bytes.
  // TODO(vchudnov): Consider making this tunable.
  static const int kMaxConversionAttempts = 3;

  // Concrete helper methods called by parent class
  void ComputeImageType() override;
  bool ComputeOutputContents() override;

  // 'suppress_avif_candidate' disables the speculative AVIF pick-smaller
  // probe; used by the PNG provenance-carry path, where a winning AVIF
  // candidate would be discarded anyway (the caBX/iTXt manifest can only be
  // spliced back into a PNG output).
  bool ComputeOutputContentsFromGifOrPng(
      const GoogleString& string_for_image,
      const PngReaderInterface* png_reader, bool fall_back_to_png,
      const char* dbg_input_format, ImageType input_type,
      ConversionVariables::VariableType var_type, bool suppress_avif_candidate);

  // Helper methods
  static bool ComputePngTransparency(const StringPiece& buf);

  // Internal methods used only in the implementation
  void UndoChange();
  void FindJpegSize();
  void FindPngSize();
  void FindGifSize();
  void FindWebpSize();

  // Convert the given options object to jpeg compression options.
  void ConvertToJpegOptions(const Image::CompressionOptions& options,
                            JpegCompressionOptions* jpeg_options);

  // Optimizes the png image_data, readable via png_reader.
  bool OptimizePng(const PngReaderInterface& png_reader,
                   const GoogleString& image_data);

  // Converts image_data, readable via png_reader, to a jpeg if
  // possible or a png if not, using the settings in options_.
  bool OptimizePngOrConvertToJpeg(const PngReaderInterface& png_reader,
                                  const GoogleString& image_data);

  // Converts image_data, readable via png_reader, to a webp using the
  // settings in options_, if allowed by those settings. The alpha channel
  // is always losslessly compressed, while the color may be lossily or
  // or losslessly compressed, depending on 'compress_color_losslessly'.
  bool ConvertPngToWebp(const PngReaderInterface& png_reader,
                        const GoogleString& image_data,
                        bool compress_color_losslessly, bool has_transparency,
                        ConversionVariables::VariableType var_type);

  // Convert the JPEG in original_jpeg to WebP format in
  // compressed_webp using the quality specified in
  // configured_quality.
  bool ConvertJpegToWebp(const GoogleString& original_jpeg,
                         int configured_quality, GoogleString* compressed_webp);

  static bool ContinueWebpConversion(int percent, void* user_data);

  // AVIF encode entry points, siblings of the WebP converters
  // above. All drive the AVIF frame writer (AvifFrameWriter + AvifConfiguration)
  // through the shared RewriteToAvif() pipe, which reads `src_format` frames and
  // encodes them as AVIF. The per-image AVIF-vs-WebP-vs-original choice
  // (pick-smaller) is made by the callers in ComputeOutputContents /
  // ComputeOutputContentsFromGifOrPng, never here.
  bool ConvertJpegToAvif(const GoogleString& original_jpeg,
                         int configured_quality, GoogleString* compressed_avif);
  bool ConvertPngToAvif(const GoogleString& original_png, bool lossless,
                        int configured_quality, GoogleString* compressed_avif);
  bool ConvertAnimatedGifToAvif(const GoogleString& original_gif,
                                int configured_quality,
                                GoogleString* compressed_avif);
  // Recompress an existing AVIF (decode + re-encode at configured_quality),
  // carrying EXIF/ICC/XMP through the transcode. Sibling of ReduceWebpImageQuality
  // (whose body lives in webp_optimizer); the AVIF body reuses the RewriteToAvif
  // pipe since the frame reader/writer already round-trips AVIF.
  bool ReduceAvifImageQuality(const GoogleString& original_avif,
                              int configured_quality,
                              GoogleString* compressed_avif);
  // Shared decode->AVIF-encode pipe. `src_format` is the input format
  // (IMAGE_JPEG/PNG/GIF/AVIF). `lossless` selects the lossless encoder path.
  // When `src_format` is IMAGE_AVIF the source's EXIF/ICC/XMP are carried through
  // (Stream H); source-metadata extraction for JPEG/PNG inputs is a TODO.
  bool RewriteToAvif(pagespeed::image_compression::ImageFormat src_format,
                     const GoogleString& src, bool lossless,
                     int configured_quality, GoogleString* output);

  // Determines whether we can attempt a libpagespeed conversion
  // without exceeding kMaxConversionAttempts. If so, increments the
  // number of attempts.
  bool MayConvert() {
    if (options_.get()) {
      VLOG(1) << "Conversions attempted: " << options_->conversions_attempted;
      if (options_->conversions_attempted < kMaxConversionAttempts) {
        ++options_->conversions_attempted;
        return true;
      }
    }
    return false;
  }

  int GetJpegQualityFromImage(const StringPiece& contents) {
    const int quality = JpegUtils::GetImageQualityFromImage(
        contents.data(), contents.size(), handler_.get());
    return quality;
  }

  // Quality level for compressing the resized image.
  int EstimateQualityForResizedJpeg();

  bool ConvertAnimatedGifToWebp(bool has_transparency);

  const GoogleString file_prefix_;
  std::unique_ptr<MessageHandler> handler_;
  bool changed_;
  const GoogleString url_;
  ImageDim dims_;
  ImageDim resized_dimensions_;
  GoogleString resized_image_;
  std::unique_ptr<Image::CompressionOptions> options_;
  bool low_quality_enabled_;
  Timer* timer_;
  GoogleString debug_message_;
  GoogleString resize_debug_message_;
  GoogleString debug_message_url_;

  ImageImpl(const ImageImpl&) = delete;
  ImageImpl& operator=(const ImageImpl&) = delete;
};

void ImageImpl::SetTransformToLowRes() {
  // TODO(vchudnov): Deprecate low_quality_enabled_.
  low_quality_enabled_ = true;
  // TODO(vchudnov): All these settings should probably be tunable.
  if (options_->preferred_webp != WEBP_NONE) {
    options_->preferred_webp = WEBP_LOSSY;
  }
  options_->webp_quality = 10;
  options_->webp_animated_quality = 10;
  options_->jpeg_quality = 10;
}

Image::Image(const StringPiece& original_contents)
    : image_type_(IMAGE_UNKNOWN),
      original_contents_(original_contents),
      output_contents_(),
      output_valid_(false),
      rewrite_attempted_(false) {}

ImageImpl::ImageImpl(const StringPiece& original_contents,
                     const GoogleString& url, const StringPiece& file_prefix,
                     CompressionOptions* options, Timer* timer,
                     MessageHandler* handler)
    : Image(original_contents),
      file_prefix_(file_prefix.data(), file_prefix.size()),
      changed_(false),
      url_(url),
      options_(options),
      low_quality_enabled_(false),
      timer_(timer) {
  const GoogleString annotation = StrCat(url, ": ");
  handler_ = std::make_unique<AnnotatedMessageHandler>(annotation, handler);
}

Image* NewImage(const StringPiece& original_contents, const GoogleString& url,
                const StringPiece& file_prefix,
                Image::CompressionOptions* options, Timer* timer,
                MessageHandler* handler) {
  return new ImageImpl(original_contents, url, file_prefix, options, timer,
                       handler);
}

Image::Image(ImageType type)
    : image_type_(type),
      original_contents_(),
      output_contents_(),
      output_valid_(false),
      rewrite_attempted_(false) {}

ImageImpl::ImageImpl(int width, int height, ImageType type,
                     const StringPiece& tmp_dir, Timer* timer,
                     MessageHandler* handler, CompressionOptions* options)
    : Image(type),
      file_prefix_(tmp_dir.data(), tmp_dir.size()),
      changed_(false),
      low_quality_enabled_(false),
      timer_(timer) {
  options_.reset(options);
  dims_.set_width(width);
  dims_.set_height(height);
  handler_ = std::make_unique<AnnotatedMessageHandler>(handler);
}

bool ImageImpl::GenerateBlankImage() {
  DCHECK(image_type_ == IMAGE_PNG) << "Blank image must be a PNG.";

  if (pagespeed::image_compression::GenerateBlankImage(
          dims_.width(), dims_.height(),
          options_->use_transparent_for_blank_image, &output_contents_,
          handler_.get())) {
    output_valid_ = true;
    return true;
  }
  return false;
}

Image* BlankImageWithOptions(int width, int height, ImageType type,
                             const StringPiece& tmp_dir, Timer* timer,
                             MessageHandler* handler,
                             Image::CompressionOptions* options) {
  std::unique_ptr<ImageImpl> image(
      new ImageImpl(width, height, type, tmp_dir, timer, handler, options));
  if (image != nullptr && image->GenerateBlankImage()) {
    return image.release();
  }
  return nullptr;
}

Image::~Image() {}

// Looks through blocks of jpeg stream to find SOFn block
// indicating encoding and dimensions of image.
// Loosely based on code and FAQs found here:
//    http://www.faqs.org/faqs/jpeg-faq/part1/
void ImageImpl::FindJpegSize() {
  const StringPiece& buf = original_contents_;
  size_t pos = 2;  // Position of first data block after header.
  while (pos < buf.size()) {
    // Read block identifier
    int id = CharToInt(buf[pos++]);
    if (id == 0xff) {  // Padding byte
      continue;
    }
    // At this point pos points to first data byte in block.  In any block,
    // first two data bytes are size (including these 2 bytes).  But first,
    // make sure block wasn't truncated on download.
    if (pos + ImageHeaders::kJpegIntSize > buf.size()) {
      break;
    }
    int length = JpegIntAtPosition(buf, pos);
    // Now check for a SOFn header, which describes image dimensions.
    if (0xc0 <= id && id <= 0xcf &&  // SOFn header
        length >= 8 &&               // Valid SOFn block size
        pos + 1 + 3 * ImageHeaders::kJpegIntSize <= buf.size() &&
        // Above avoids case where dimension data was truncated
        id != 0xc4 && id != 0xc8 && id != 0xcc) {
      // 0xc4, 0xc8, 0xcc aren't actually valid SOFn headers.
      // NOTE: we don't care if we have the whole SOFn block,
      // just that we can fetch both dimensions without trouble.
      // Our image download could be truncated at this point for
      // all we care.
      // We're a bit sloppy about SOFn block size, as it's
      // actually 8 + 3 * buf[pos+2], but for our purposes this
      // will suffice as we don't parse subsequent metadata (which
      // describes the formatting of chunks of image data).
      dims_.set_height(
          JpegIntAtPosition(buf, pos + 1 + ImageHeaders::kJpegIntSize));
      dims_.set_width(
          JpegIntAtPosition(buf, pos + 1 + 2 * ImageHeaders::kJpegIntSize));
      break;
    }
    pos += length;
  }
  if (!ImageUrlEncoder::HasValidDimensions(dims_) || (dims_.height() <= 0) ||
      (dims_.width() <= 0)) {
    dims_.Clear();
    PS_LOG_INFO(handler_, "Couldn't find jpeg dimensions (data truncated?).");
  }
}

// Looks at first (IHDR) block of png stream to find image dimensions.
// See also: http://www.w3.org/TR/PNG/
void ImageImpl::FindPngSize() {
  const StringPiece& buf = original_contents_;
  // Here we make sure that buf contains at least enough data that we'll be able
  // to decipher the image dimensions first, before we actually check for the
  // headers and attempt to decode the dimensions (which are the first two ints
  // after the IHDR section label).
  if ((buf.size() >=  // Not truncated
       ImageHeaders::kIHDRDataStart + 2 * ImageHeaders::kPngIntSize) &&
      (StringPiece(buf.data() + ImageHeaders::kPngHeaderLength,
                   ImageHeaders::kPngSectionHeaderLength) ==
       StringPiece(ImageHeaders::kPngIHDR,
                   ImageHeaders::kPngSectionHeaderLength))) {
    dims_.set_width(PngIntAtPosition(buf, ImageHeaders::kIHDRDataStart));
    dims_.set_height(PngIntAtPosition(
        buf, ImageHeaders::kIHDRDataStart + ImageHeaders::kPngIntSize));
  } else {
    PS_LOG_INFO(handler_,
                "Couldn't find png dimensions "
                "(data truncated or IHDR missing).");
  }
}

// Looks at header of GIF file to extract image dimensions.
// See also: http://en.wikipedia.org/wiki/Graphics_Interchange_Format
void ImageImpl::FindGifSize() {
  const StringPiece& buf = original_contents_;
  // Make sure that buf contains enough data that we'll be able to
  // decipher the image dimensions before we attempt to do so.
  if (buf.size() >=
      ImageHeaders::kGifDimStart + 2 * ImageHeaders::kGifIntSize) {
    // Not truncated
    dims_.set_width(GifIntAtPosition(buf, ImageHeaders::kGifDimStart));
    dims_.set_height(GifIntAtPosition(
        buf, ImageHeaders::kGifDimStart + ImageHeaders::kGifIntSize));
  } else {
    PS_LOG_INFO(handler_, "Couldn't find gif dimensions (data truncated)");
  }
}

void ImageImpl::FindWebpSize() {
  const uint8* webp = reinterpret_cast<const uint8*>(original_contents_.data());
  const int webp_size = original_contents_.size();
  int width = 0, height = 0;
  if (WebPGetInfo(webp, webp_size, &width, &height) > 0) {
    dims_.set_width(width);
    dims_.set_height(height);
  } else {
    PS_LOG_INFO(handler_, "Couldn't find webp dimensions ");
  }
}

// Looks at image data in order to determine image type, and also fills in any
// dimension information it can (setting image_type_ and dims_).
void ImageImpl::ComputeImageType() {
  image_type_ =
      pagespeed::image_compression::ComputeImageType(original_contents_);

  switch (image_type_) {
    case IMAGE_JPEG:
      FindJpegSize();
      break;
    case IMAGE_PNG:
      FindPngSize();
      break;
    case IMAGE_GIF:
      FindGifSize();
      break;
    case IMAGE_WEBP:
    case IMAGE_WEBP_LOSSLESS_OR_ALPHA:
    case IMAGE_WEBP_ANIMATED:
      FindWebpSize();
      break;
    case IMAGE_AVIF:
    case IMAGE_AVIF_LOSSLESS_OR_ALPHA:
    case IMAGE_AVIF_ANIMATED:
      // TODO(avif): implement FindAvifSize() (parse the ISO-BMFF ispe box) to
      // fill dims_, mirroring FindWebpSize(). Deliberate M2 follow-up: the M1
      // cut ships without AVIF dimension extraction, so dims_ stays unset for
      // AVIF sources (ResizeTo guards against this). image_type_ is still
      // classified correctly by ComputeImageType() above; only dimension
      // extraction is pending.
    case IMAGE_UNKNOWN:
      break;
  }
}

const ContentType* Image::TypeToContentType(ImageType image_type) {
  const ContentType* res = nullptr;
  switch (image_type) {
    case IMAGE_UNKNOWN:
      break;
    case IMAGE_JPEG:
      res = &kContentTypeJpeg;
      break;
    case IMAGE_PNG:
      res = &kContentTypePng;
      break;
    case IMAGE_GIF:
      res = &kContentTypeGif;
      break;
    case IMAGE_WEBP:
    case IMAGE_WEBP_LOSSLESS_OR_ALPHA:
    case IMAGE_WEBP_ANIMATED:
      res = &kContentTypeWebp;
      break;
    case IMAGE_AVIF:
    case IMAGE_AVIF_LOSSLESS_OR_ALPHA:
    case IMAGE_AVIF_ANIMATED:
      res = &kContentTypeAvif;
      break;
  }
  return res;
}

// Compute whether a PNG can have transparent / semi-transparent pixels
// by walking the image data in accordance with the spec:
//   http://www.w3.org/TR/PNG/
// If the colour type (UK spelling from spec) includes an alpha channel, or
// there is a tRNS section with at least one entry before IDAT, then we assume
// the image contains non-opaque pixels and return true.
bool ImageImpl::ComputePngTransparency(const StringPiece& buf) {
  // We assume the image has transparency until we prove otherwise.
  // This allows us to deal conservatively with truncation etc.
  bool has_transparency = true;
  if (buf.size() > ImageHeaders::kPngColourTypeOffset &&
      ((buf[ImageHeaders::kPngColourTypeOffset] &
        ImageHeaders::kPngAlphaChannel) == 0)) {
    // The colour type indicates that there is no dedicated alpha channel.  Now
    // we must look for a tRNS section indicating the existence of transparent
    // colors or palette entries.
    size_t section_start = ImageHeaders::kPngHeaderLength;
    while (section_start + ImageHeaders::kPngSectionHeaderLength < buf.size()) {
      size_t section_size = PngIntAtPosition(buf, section_start);
      if (PngSectionIdIs(ImageHeaders::kPngIDAT, buf, section_start)) {
        // tRNS section must occur before first IDAT.  This image doesn't have a
        // tRNS section, and thus doesn't have transparency.
        has_transparency = false;
        break;
      } else if (PngSectionIdIs(ImageHeaders::kPngtRNS, buf, section_start) &&
                 section_size > 0) {
        // Found a nonempty tRNS section.  This image has_transparency.
        break;
      } else {
        // Move on to next section.
        section_start += section_size + ImageHeaders::kPngSectionMinSize;
      }
    }
  }
  return has_transparency;
}

bool ImageImpl::EnsureLoaded(bool output_useful) { return true; }

// Determine the quality level for compressing the resized image.
// If a JPEG image needs resizing, we decompress it first, then resize it,
// and finally compress it into a new JPEG image. To compress the output image,
// We would like to use the quality level that was used in the input image,
// if such information can be calculated from the input image; otherwise, we
// will use the quality level set in the configuration; otherwise, we will use
// a predefined default quality.
int ImageImpl::EstimateQualityForResizedJpeg() {
  int input_quality = GetJpegQualityFromImage(original_contents_);
  int output_quality =
      std::min(ImageHeaders::kMaxJpegQuality, options_->jpeg_quality);
  if (input_quality > 0 && output_quality > 0) {
    return std::min(input_quality, output_quality);
  } else if (input_quality > 0) {
    return input_quality;
  } else if (output_quality > 0) {
    return output_quality;
  } else {
    return ImageHeaders::kQualityForJpegWithUnkownQuality;
  }
}

void ImageImpl::Dimensions(ImageDim* natural_dim) {
  if (!ImageUrlEncoder::HasValidDimensions(dims_)) {
    ComputeImageType();
  }
  *natural_dim = dims_;
}

bool ImageImpl::ResizeTo(const ImageDim& new_dim) {
  CHECK(ImageUrlEncoder::HasValidDimensions(new_dim));
  if ((new_dim.width() <= 0) || (new_dim.height() <= 0)) {
    return false;
  }

  if (changed_) {
    // If we already resized, drop data and work with original image.
    UndoChange();
  }

  // TODO(huibao): Enable resizing for WebP and images with alpha channel.
  // We have the tools ready but no tests.
  const ImageFormat original_format = ImageTypeToImageFormat(image_type());
  if (original_format == pagespeed::image_compression::IMAGE_WEBP) {
    return false;
  }
  // AVIF sources are likewise not resized in M1 (no FindAvifSize yet, so dims_
  // is never valid for them anyway); bail out here like WebP so a future
  // dimension source cannot route AVIF into the DFATAL default of the writer
  // switch below.
  if (original_format == pagespeed::image_compression::IMAGE_AVIF) {
    return false;
  }

  std::unique_ptr<ScanlineReaderInterface> image_reader(
      CreateScanlineReader(original_format, original_contents_.data(),
                           original_contents_.length(), handler_.get()));
  if (image_reader == nullptr) {
    resize_debug_message_ =
        absl::StrFormat("Cannot resize: Cannot open the image%s to resize",
                        debug_message_url_.c_str());
    PS_LOG_INFO(handler_, "Cannot open the image to resize.");
    return false;
  }

  ScanlineResizer resizer(handler_.get());
  if (!resizer.Initialize(image_reader.get(), new_dim.width(),
                          new_dim.height())) {
    resize_debug_message_ =
        absl::StrFormat("Cannot resize%s: Unable to initialize resizer",
                        debug_message_url_.c_str());
    return false;
  }

  // Create a writer.
  std::unique_ptr<ScanlineWriterInterface> writer;
  const ImageFormat resized_format = GetOutputImageFormat(original_format);
  switch (resized_format) {
    case pagespeed::image_compression::IMAGE_JPEG: {
      JpegCompressionOptions jpeg_config;
      jpeg_config.lossy = true;
      jpeg_config.lossy_options.quality = EstimateQualityForResizedJpeg();
      writer.reset(CreateScanlineWriter(
          resized_format, resizer.GetPixelFormat(), resizer.GetImageWidth(),
          resizer.GetImageHeight(), &jpeg_config, &resized_image_,
          handler_.get()));
    } break;

    case pagespeed::image_compression::IMAGE_PNG: {
      PngCompressParams png_config(PNG_FILTER_NONE, Z_DEFAULT_STRATEGY, false);
      writer.reset(CreateScanlineWriter(
          resized_format, resizer.GetPixelFormat(), resizer.GetImageWidth(),
          resizer.GetImageHeight(), &png_config, &resized_image_,
          handler_.get()));
    } break;

    default:
      resize_debug_message_ =
          absl::StrFormat("Cannot resize%s: Unsupported image format",
                          debug_message_url_.c_str());
      PS_LOG_DFATAL(handler_, "Unsupported image format");
  }

  if (writer == nullptr) {
    return false;
  }

  // Resize the image and save the results in 'resized_image_'.
  void* scanline = nullptr;
  while (resizer.HasMoreScanLines()) {
    if (!resizer.ReadNextScanline(&scanline)) {
      resize_debug_message_ = absl::StrFormat(
          "Cannot resize%s: Reading image failed", debug_message_url_.c_str());
      return false;
    }
    if (!writer->WriteNextScanline(scanline)) {
      resize_debug_message_ = absl::StrFormat(
          "Cannot resize%s: Writing image failed", debug_message_url_.c_str());
      return false;
    }
  }
  if (!writer->FinalizeWrite()) {
    resize_debug_message_ =
        absl::StrFormat("Cannot resize%s: Finalizing writing image failed",
                        debug_message_url_.c_str());
    return false;
  }

  changed_ = true;
  output_valid_ = false;
  rewrite_attempted_ = false;
  output_contents_.clear();
  resized_dimensions_ = new_dim;
  resize_debug_message_ = absl::StrFormat(
      "Resized image%s from %dx%d to %dx%d", debug_message_url_.c_str(),
      dims_.width(), dims_.height(), resized_dimensions_.width(),
      resized_dimensions_.height());
  return true;
}

void ImageImpl::UndoChange() {
  if (changed_) {
    output_valid_ = false;
    rewrite_attempted_ = false;
    output_contents_.clear();
    resized_image_.clear();
    image_type_ = IMAGE_UNKNOWN;
    changed_ = false;
  }
}

// TODO(huibao): Refactor image rewriting. We may have a centralized
// controller and a set of naive image writers. The controller looks at
// the input image type and the filter settings, and decides which output
// format(s) to try and the configuration for each output format. The writers
// simply write the output based on the specified configurations and should not
// be aware of the input type nor the filters.
//
// Here are some thoughts for the new design.
// 1. Create a scanline reader based on the type of input image.
// 2. If the image is going to be resized, wrap the reader into a resizer, which
//    is also a scanline reader.
// 3. Create a scanline writer or mutliple writers based the filter settings.
//    The parameters for the writer will also be determined by the filters.
//
// Transfer all of the scanlines from the reader to the writer and the image is
// rewritten (and resized)!

// Performs image optimization and output
bool ImageImpl::ComputeOutputContents() {
  if (rewrite_attempted_) {
    return output_valid_;
  }
  rewrite_attempted_ = true;
  if (!output_valid_) {
    StringPiece contents;
    bool resized;

    // Choose appropriate source for image contents.
    // Favor original contents if image unchanged.
    resized = !resized_image_.empty();
    if (resized) {
      contents = resized_image_;
    } else {
      contents = original_contents_;
    }

    // C2PA / Content-Credentials preserve-by-default fallback.
    // The JPEG codec carries the APP11/JUMBF manifest THROUGH a recompress
    // (jpeg_optimizer.cc), but only for a JPEG that is not resized and stays JPEG:
    // the resize path re-encodes via the ScanlineWriter (which copies no markers),
    // and the JPEG->WebP / PNG / GIF encoders do not carry the manifest at all. For
    // every path the codec cannot cover, fail safe -- pass the ORIGINAL bytes through
    // byte-for-byte (skip optimization) rather than silently strip provenance. This
    // only detects and preserves; it never parses, validates, or re-emits the
    // manifest. Tradeoff: a manifest-bearing image is not resized or
    // format-converted under the default; opt out with `PreserveImageProvenance off`.
    // Computed once here so the byte scan does not run per format branch.
    const bool has_c2pa =
        options_.get() != nullptr && options_->preserve_c2pa &&
        pagespeed::image_compression::ImageHasC2paManifest(original_contents_);
    // The JPEG codec carry (jpeg_optimizer.cc) preserves the APP11/JUMBF form, but a
    // Content-Credentials manifest carried in XMP lives in APP1 (shared with EXIF) and
    // survives a recompress only when EXIF/APP1 is retained -- which is FALSE under
    // StripImageMetaData, a DEFAULT CoreFilters / optimize-for-bandwidth filter. So a
    // non-resized JPEG carrying the XMP form that would be stripped must also fall back
    // to skip-not-strip, or provenance would be silently lost.
    const bool xmp_would_strip =
        has_c2pa && !options_->retain_exif_data &&
        pagespeed::image_compression::ImageHasXmpC2pa(original_contents_);
    // Carry-through (opt-in, ImageProvenanceCarry): a non-resized PNG carrying a
    // manifest is NOT skipped -- it is recompressed and its caBX/iTXt chunks are
    // spliced back into the optimized output below. The PNG optimizer strips
    // ancillary chunks, so unlike the JPEG codec (jpeg_optimizer.cc, which already
    // carries APP11/JUMBF through a recompress) it cannot preserve the manifest on
    // its own. The carry flag is therefore PNG-only here; every other manifest
    // case still falls back to skip-not-strip below.
    const bool png_carry = has_c2pa && options_->c2pa_carry && !resized &&
                           image_type() == IMAGE_PNG;
    if (has_c2pa && !png_carry &&
        (resized || image_type() != IMAGE_JPEG || xmp_would_strip)) {
      output_contents_ = original_contents_.as_string();
      output_valid_ = true;
      return true;
    }

    // Take image contents and re-compress them.
    // The basic logic is this:
    // * low_quality_enabled_ acts as though convert_gif_to_png and
    //   convert_png_to_webp were both set for this image.
    // * We compute the intended final end state of all the
    //   convert_X_to_Y options, and try to convert to the final
    //   option in one shot. If that fails, we back off by each of the stages.
    // * We return as soon as any applicable conversion succeeds. We
    //   do not compare the sizes of alternative conversions.
    // If we can't optimize the image, we'll fail.
    bool ok = false;
    // We copy the data to a string eagerly as we're very likely to need it
    // (only unrecognized formats don't require it, in which case we probably
    // don't get this far in the first place).
    // TODO(jmarantz): The PageSpeed library should, ideally, take StringPiece
    // args rather than const string&.  We would save lots of string-copying
    // if we made that change.
    GoogleString string_for_image(contents.data(), contents.size());
    std::unique_ptr<PngReaderInterface> png_reader;
    switch (image_type()) {
      case IMAGE_UNKNOWN:
        break;
      case IMAGE_WEBP:
      case IMAGE_WEBP_LOSSLESS_OR_ALPHA:
        if (resized || options_->recompress_webp) {
          ok = MayConvert() &&
               ReduceWebpImageQuality(string_for_image, options_->webp_quality,
                                      &output_contents_);
        }
        // TODO(pulkitg): Convert a webp image to jpeg image if
        // web_preferred_ is false.
        break;
      case IMAGE_WEBP_ANIMATED:
        // TODO(huibao): Recompress animated WebP.
        ok = false;
        break;
      case IMAGE_AVIF:
      case IMAGE_AVIF_LOSSLESS_OR_ALPHA:
      case IMAGE_AVIF_ANIMATED:
        // Recompress a stored-as-AVIF input in place at the
        // configured AVIF quality, carrying EXIF/ICC/XMP through the transcode
        // (Stream H). image_type_ is left unchanged (the AVIF subtype is
        // preserved). The has_c2pa guard is upstream (a manifest-bearing AVIF is
        // a non-JPEG type, so it already returned as skip-not-strip above); the
        // explicit !has_c2pa here documents that floor at the branch.
        if ((resized || options_->recompress_avif) && !has_c2pa &&
            MayConvert()) {
          ok = ReduceAvifImageQuality(string_for_image, options_->avif_quality,
                                      &output_contents_);
          VLOG(1) << "Image conversion: " << ok << " avif->avif for " << url_;
        }
        break;
      case IMAGE_JPEG:
        // A manifest-bearing JPEG must not be converted to WebP/AVIF
        // (neither encoder carries the APP11/JUMBF or APP1/XMP manifest yet).
        // !has_c2pa skips both conversions so it falls through to the JPEG
        // recompress branch below, where the codec preserves the manifest.
        // MayConvert() is checked LAST (matching the AVIF block below) so a
        // disabled/inapplicable webp path does not burn a conversion attempt and
        // starve the jpeg-recompress fallback.
        if (options_->convert_jpeg_to_webp && !has_c2pa &&
            (options_->preferred_webp != WEBP_NONE) && MayConvert()) {
          ok = ConvertJpegToWebp(string_for_image, options_->webp_quality,
                                 &output_contents_);
          VLOG(1) << "Image conversion: " << ok << " jpeg->webp for " << url_;
          if (ok) {
            image_type_ = IMAGE_WEBP;
          } else {
            // Image is not going to be webp-converted!
            PS_LOG_INFO(handler_, "Failed to create webp!");
          }
        }
        // AVIF candidate (pick-smaller). Both encoders are set
        // up when both capabilities are present, so this is a genuine per-image
        // decision: encode AVIF into a scratch buffer and adopt it only if it
        // beats the WebP/keep candidate (or if WebP was not produced). AVIF
        // generally wins on lossy photographic JPEGs; where it regresses the
        // smaller WebP/JPEG output is kept. The chosen format is recorded via
        // image_type_ (-> the .avif/.webp output extension), never in the key.
        // NOTE: MayConvert() is checked LAST (it consumes one of the
        // kMaxConversionAttempts). Gating it behind the cheap flag checks means
        // a non-AVIF request does not burn a conversion attempt here and starve
        // the JPEG-recompress fallback below.
        if (options_->convert_jpeg_to_avif && !has_c2pa &&
            (options_->preferred_avif !=
             pagespeed::image_compression::LIBAVIF_NONE) &&
            MayConvert()) {
          GoogleString avif_out;
          if (ConvertJpegToAvif(string_for_image, options_->avif_quality,
                                &avif_out) &&
              (!ok || avif_out.size() < output_contents_.size())) {
            output_contents_.swap(avif_out);
            image_type_ = IMAGE_AVIF;
            ok = true;
            VLOG(1) << "Image conversion: chose jpeg->avif for " << url_;
          }
        }
        if (!ok && MayConvert() && (resized || options_->recompress_jpeg)) {
          JpegCompressionOptions jpeg_options;
          ConvertToJpegOptions(*options_.get(), &jpeg_options);
          ok = OptimizeJpegWithOptions(string_for_image, &output_contents_,
                                       jpeg_options, handler_.get());
          VLOG(1) << "Image conversion: " << ok << " jpeg->jpeg for " << url_;
        }
        break;
      case IMAGE_PNG:
        png_reader = std::make_unique<PngReader>(handler_.get());
        // When png_carry is active, suppress the AVIF pick-smaller candidate:
        // the carry splice below only fits a PNG output, so a winning AVIF
        // would fail the carry and regress to the original bytes (Level B),
        // wasting the expensive AVIF encode on the way.
        ok = ComputeOutputContentsFromGifOrPng(
            string_for_image, png_reader.get(),
            (resized || options_->recompress_png) /* fall_back_to_png */,
            kPngString, IMAGE_PNG, Image::ConversionVariables::FROM_PNG,
            png_carry /* suppress_avif_candidate */);
        break;
      case IMAGE_GIF:
        ImageType current_image_type = IMAGE_GIF;
        if (resized) {
          // If the GIF image has been resized, it has already been
          // converted to a PNG image.
          png_reader = std::make_unique<PngReader>(handler_.get());
          current_image_type = IMAGE_PNG;
        } else if (options_->convert_gif_to_png || low_quality_enabled_ ||
                   options_->allow_webp_animated ||
                   options_->convert_gif_to_avif ||
                   options_->allow_avif_animated) {
          png_reader = std::make_unique<GifReader>(handler_.get());
        } else {
          break;
        }
        ok = ComputeOutputContentsFromGifOrPng(
            string_for_image, png_reader.get(),
            options_->convert_gif_to_png /* fall_back_to_png */, kGifString,
            current_image_type, Image::ConversionVariables::FROM_GIF,
            false /* suppress_avif_candidate */);
        break;
    }
    // PNG carry-through: splice the ORIGINAL caBX/iTXt manifest chunks into
    // the recompressed PNG, immediately before the trailing IEND chunk.
    // Fail-safe to skip-not-strip (serve the original bytes byte-for-byte) on
    // ANY anomaly -- the PNG was converted to another format (the PNG carrier
    // no longer fits), recompression failed, extraction found no carrier, or
    // the output has no well-formed IEND -- so a manifest is never silently
    // dropped.
    // The bytes are never parsed or re-authored.
    if (png_carry) {
      bool carried = false;
      if (ok && image_type() == IMAGE_PNG) {
        const StringPieceVector chunks =
            pagespeed::image_compression::ExtractPngC2paChunks(
                original_contents_);
        const size_t n = output_contents_.size();
        // The trailing IEND chunk is exactly 12 bytes:
        // length(4) + "IEND"(4) + CRC(4); its type sits at offset n-8.
        if (!chunks.empty() && n >= 12 &&
            output_contents_.compare(n - 8, 4, "IEND") == 0) {
          GoogleString carrier;
          for (const StringPiece& chunk : chunks) {
            chunk.AppendToString(&carrier);
          }
          // Splice unless the recompressed output already contains these EXACT
          // carrier bytes (the PNG optimizer strips ancillary chunks, so it
          // normally has not -- this guards only against a future chunk-preserving
          // optimizer double-adding them). This is a content-EXACT check on the
          // full carrier, never a short signature scan, so a chance token
          // collision in the compressed IDAT cannot mistakenly skip the splice
          // and silently strip the manifest.
          if (output_contents_.find(carrier) == GoogleString::npos) {
            output_contents_.insert(n - 12, carrier);
          }
          carried = true;
        }
      }
      if (!carried) {
        output_contents_ = original_contents_.as_string();
        image_type_ = IMAGE_PNG;
        ok = true;
      }
    }
    output_valid_ = ok;
  }
  return output_valid_;
}

inline bool ImageImpl::ConvertJpegToWebp(const GoogleString& original_jpeg,
                                         int configured_quality,
                                         GoogleString* compressed_webp) {
  ConversionTimeoutHandler timeout_handler(options_->webp_conversion_timeout_ms,
                                           timer_, handler_.get());
  timeout_handler.Start(compressed_webp);
  bool ok = OptimizeWebp(original_jpeg, configured_quality,
                         ConversionTimeoutHandler::Continue, &timeout_handler,
                         compressed_webp, handler_.get());
  timeout_handler.Stop();

  bool was_timed_out = timeout_handler.was_timed_out();
  int64 time_elapsed_ms = timeout_handler.time_elapsed_ms();

  UpdateConversionStats(ok, was_timed_out, false /* overran_budget */,
                        time_elapsed_ms, Image::ConversionVariables::FROM_JPEG,
                        options_->webp_conversion_variables);

  UpdateConversionStats(ok, was_timed_out, false /* overran_budget */,
                        time_elapsed_ms, Image::ConversionVariables::OPAQUE,
                        options_->webp_conversion_variables);
  return ok;
}

bool ImageImpl::ConvertAnimatedGifToWebp(bool has_transparency) {
  ConversionTimeoutHandler timeout_handler(options_->webp_conversion_timeout_ms,
                                           timer_, handler_.get());
  timeout_handler.Start(&output_contents_);

  // Parameters controlling WebP compression.
  WebpConfiguration webp_config;
  webp_config.quality = options_->webp_animated_quality;
  webp_config.progress_hook = ConversionTimeoutHandler::Continue;
  webp_config.user_data = &timeout_handler;
  // TODO(huibao): Evaluate the following parameters.
  webp_config.method = 3;
  webp_config.kmin = 3;
  webp_config.kmax = 5;
  webp_config.lossless = false;
  webp_config.alpha_quality = 100;
  webp_config.alpha_compression = 1;  // alpha plane compressed losslessly

  pagespeed::image_compression::ScanlineStatus status;
  std::unique_ptr<pagespeed::image_compression::MultipleFrameReader> reader(
      CreateImageFrameReader(
          pagespeed::image_compression::IMAGE_GIF, original_contents_.data(),
          original_contents_.length(), handler_.get(), &status));
  if (!status.Success()) {
    PS_LOG_ERROR(handler_, "Cannot read the animated GIF image.");
    return false;
  }

  std::unique_ptr<pagespeed::image_compression::MultipleFrameWriter> writer(
      CreateImageFrameWriter(pagespeed::image_compression::IMAGE_WEBP,
                             &webp_config, &output_contents_, handler_.get(),
                             &status));
  if (!status.Success()) {
    PS_LOG_ERROR(handler_, "Cannot create an animated WebP image for output.");
    return false;
  }

  // Copy all pixels in all frames from the reader to the writer. This will do
  // format conversion and compression.
  pagespeed::image_compression::ImageSpec image_spec;
  pagespeed::image_compression::FrameSpec frame_spec;
  const void* scan_row = nullptr;
  if (reader->GetImageSpec(&image_spec, &status) &&
      writer->PrepareImage(&image_spec, &status)) {
    while (reader->HasMoreFrames() && reader->PrepareNextFrame(&status) &&
           reader->GetFrameSpec(&frame_spec, &status) &&
           writer->PrepareNextFrame(&frame_spec, &status)) {
      while (reader->HasMoreScanlines() &&
             reader->ReadNextScanline(&scan_row, &status) &&
             writer->WriteNextScanline(scan_row, &status)) {
        // intentional empty loop body
      }
    }
  }
  writer->FinalizeWrite(&status);

  timeout_handler.Stop();
  bool was_timed_out = timeout_handler.was_timed_out();
  int64 time_elapsed_ms = timeout_handler.time_elapsed_ms();
  bool ok = status.Success();

  UpdateConversionStats(ok, was_timed_out, false /* overran_budget */,
                        time_elapsed_ms,
                        Image::ConversionVariables::FROM_GIF_ANIMATED,
                        options_->webp_conversion_variables);

  UpdateConversionStats(
      ok, was_timed_out, false /* overran_budget */, time_elapsed_ms,
      (has_transparency ? Image::ConversionVariables::NONOPAQUE
                        : Image::ConversionVariables::OPAQUE),
      options_->webp_conversion_variables);

  return ok;
}

namespace {
// Maps the source format handed to RewriteToAvif() onto the AVIF stats bucket
// it should be counted in.  Every encode entry point funnels through
// RewriteToAvif(), so this is the single place the mapping is decided.
Image::ConversionVariables::VariableType AvifVariableTypeForSource(
    pagespeed::image_compression::ImageFormat src_format) {
  switch (src_format) {
    case pagespeed::image_compression::IMAGE_JPEG:
      return Image::ConversionVariables::FROM_JPEG;
    case pagespeed::image_compression::IMAGE_PNG:
      return Image::ConversionVariables::FROM_PNG;
    case pagespeed::image_compression::IMAGE_GIF:
      // The only GIF->AVIF caller is ConvertAnimatedGifToAvif().
      return Image::ConversionVariables::FROM_GIF_ANIMATED;
    case pagespeed::image_compression::IMAGE_AVIF:
      return Image::ConversionVariables::FROM_AVIF;
    default:
      return Image::ConversionVariables::FROM_UNKNOWN_FORMAT;
  }
}
}  // namespace

bool ImageImpl::RewriteToAvif(
    pagespeed::image_compression::ImageFormat src_format,
    const GoogleString& src, bool lossless, int configured_quality,
    GoogleString* output) {
  output->clear();

  // avif_conversion_timeout_ms is enforced by two different mechanisms,
  // because AV1 has no single one that works for both shapes of input:
  //
  //   * ANIMATION -- the frame-boundary progress hook below. Frames are handed
  //     to libavif one at a time, so refusing at a boundary really does stop
  //     the remaining work. This is a deadline.
  //   * STILL -- an up-front admission test inside the writer
  //     (AvifConfiguration::encode_budget_ms). libavif encodes a single-frame
  //     image entirely inside one avifEncoderAddImage() call, and AV1 offers
  //     no way to interrupt it (aom_codec_encode() has no deadline parameter;
  //     avifEncoder has no cancel flag), so a check at the frame boundary
  //     would fire only after the encode was already paid for and would
  //     discard a finished result. Instead the writer estimates the cost from
  //     pixel count and speed and declines BEFORE encoding. This is a
  //     refusal, not a deadline: a still encode that is admitted always runs
  //     to completion, even if it overruns.
  ConversionTimeoutHandler timeout_handler(options_->avif_conversion_timeout_ms,
                                           timer_, handler_.get());

  pagespeed::image_compression::AvifConfiguration avif_config;
  avif_config.lossless = lossless ? 1 : 0;
  if (!lossless && configured_quality > 0) {
    avif_config.quality = configured_quality;
  }
  // Speed knob (aom cpu-used analogue): we set only the FLOOR here, the
  // AvifConfiguration default of 6, and let the writer decide the rest.
  //
  // This used to be chosen here, by stepping on a fixed timeout threshold: a
  // sub-2s avif_conversion_timeout_ms selected speed 8, anything else speed 6.
  // That was a defect, because the same option also funds the
  // writer's admission test and the two disagreed about direction. Crossing the
  // threshold changes encode cost by ~5x (single-threaded, x86-64: ~445 ms/Mpx
  // at speed 6 vs ~86 at speed 8; 200 vs 31 on 64-bit ARM), so a 1999 ms budget
  // admitted several times more megapixels than a 2000 ms one -- RAISING the
  // timeout made the rewriter refuse SMALLER images, and the 5000 ms default
  // admitted less than a "tighter" 1999 ms.
  //
  // The speed is now derived per image inside
  // AvifFrameWriter::PrepareImage(), which is the only place that knows both
  // the budget AND the pixel count: it picks the slowest (best
  // quality-per-byte) speed at or above this floor whose estimated cost fits
  // the budget, and declines only if not even the fastest known speed fits.
  // That makes the option monotone -- more budget never admits less and never
  // silently degrades quality -- and it keeps a tight budget emitting AVIF at
  // a faster speed rather than routinely aborting to WebP, which was the
  // original point of the speed-8 branch.
  //
  // Consequence worth knowing when reading logs: a sub-2s budget does not imply
  // speed 8. It selects speed 6 on images that fit at speed 6 and steps up
  // (7, 8, ...) only on images that do not, so a small image under a tight
  // budget encodes slower, and better, than the budget alone would suggest.
  //
  // (The per-megapixel calibration lives in kAvifEncodeMsPerMpxBySpeed,
  // avif_optimizer.cc -- keep the figures quoted above in step with it.)
  avif_config.progress_hook = ConversionTimeoutHandler::Continue;
  avif_config.user_data = &timeout_handler;
  // Still-image admission budget. Set from the same option that drives the
  // animation deadline, so one configured number governs both shapes; the
  // writer applies it only to stills, and derives the encode speed from it.
  avif_config.encode_budget_ms = options_->avif_conversion_timeout_ms;

  // Stream H metadata carry. libavif exposes the source's EXIF/ICC/XMP only for
  // an AVIF input (post-parse); JPEG/PNG source-metadata extraction is a TODO,
  // so carry is wired for the AVIF->AVIF recompress path here. Empty blobs are a
  // no-op in the codec.
  if (src_format == pagespeed::image_compression::IMAGE_AVIF) {
    GoogleString exif, icc, xmp;
    if (pagespeed::image_compression::AvifExtractMetadata(
            src, &exif, &icc, &xmp, handler_.get())) {
      // retain_exif_data gates both EXIF and the APP1/XMP-carried provenance,
      // matching how the JPEG codec treats APP1; retain_color_profile gates ICC.
      avif_config.retain_exif = options_->retain_exif_data;
      avif_config.retain_xmp = options_->retain_exif_data;
      avif_config.retain_color_profile = options_->retain_color_profile;
      avif_config.exif_data.swap(exif);
      avif_config.icc_data.swap(icc);
      avif_config.xmp_data.swap(xmp);
    }
  }

  // Every exit path below records into the AVIF stats family, including the
  // two early codec-setup failures: an AVIF encode that fails or times out has
  // to be visible on the stats page, not merely logged at INFO.
  const Image::ConversionVariables::VariableType var_type =
      AvifVariableTypeForSource(src_format);

  pagespeed::image_compression::ScanlineStatus status;
  std::unique_ptr<pagespeed::image_compression::MultipleFrameReader> reader(
      pagespeed::image_compression::CreateImageFrameReader(
          src_format, src.data(), src.length(), handler_.get(), &status));
  if (!status.Success()) {
    PS_LOG_INFO(handler_, "AVIF: cannot read source image for encode.");
    timeout_handler.Stop();
    UpdateConversionStats(false /* ok */, false /* was_timed_out */,
                          false /* overran_budget */,
                          timeout_handler.time_elapsed_ms(), var_type,
                          options_->avif_conversion_variables);
    return false;
  }

  std::unique_ptr<pagespeed::image_compression::MultipleFrameWriter> writer(
      pagespeed::image_compression::CreateImageFrameWriter(
          pagespeed::image_compression::IMAGE_AVIF, &avif_config, output,
          handler_.get(), &status));
  if (!status.Success()) {
    PS_LOG_INFO(handler_, "AVIF: cannot create AVIF writer for output.");
    timeout_handler.Stop();
    UpdateConversionStats(false /* ok */, false /* was_timed_out */,
                          false /* overran_budget */,
                          timeout_handler.time_elapsed_ms(), var_type,
                          options_->avif_conversion_variables);
    return false;
  }

  timeout_handler.Start(output);

  pagespeed::image_compression::ImageSpec image_spec;
  pagespeed::image_compression::FrameSpec frame_spec;
  const void* scan_row = nullptr;
  const bool prepared = reader->GetImageSpec(&image_spec, &status) &&
                        writer->PrepareImage(&image_spec, &status);
  // Whether PrepareImage() refused the image up front because its estimated
  // encode cost did not fit avif_conversion_timeout_ms. This has to be latched
  // here: FinalizeWrite() below runs unconditionally and overwrites 'status'
  // with its own "no frames written" error, which would otherwise erase the
  // distinction between a budget refusal and an ordinary codec failure.
  const bool declined_over_budget =
      !prepared &&
      status.type() ==
          pagespeed::image_compression::SCANLINE_STATUS_TIMEOUT_ERROR;
  if (prepared) {
    while (reader->HasMoreFrames() && reader->PrepareNextFrame(&status) &&
           reader->GetFrameSpec(&frame_spec, &status) &&
           writer->PrepareNextFrame(&frame_spec, &status)) {
      while (reader->HasMoreScanlines() &&
             reader->ReadNextScanline(&scan_row, &status) &&
             writer->WriteNextScanline(scan_row, &status)) {
        // intentional empty loop body
      }
    }
  }
  writer->FinalizeWrite(&status);

  timeout_handler.Stop();
  bool ok = status.Success();
  // SEMANTIC CHANGE, deliberate: this counter no longer means only "an encode
  // was started and ran out of time". For a still image it now also -- in
  // practice, always -- means "an encode was REFUSED before it started,
  // because its estimated cost did not fit the budget". Both are the same
  // operational signal (AvifTimeoutMs is turning conversions away, consider
  // raising it), which is why they share a bucket, but they are not the same
  // event: a refusal costs no CPU, whereas the expiry it replaced burned a
  // full encode and then discarded the result.
  //
  // The two are still separable by BUCKET, which is why they were not split
  // into two counters: the admission test applies only to stills and the
  // progress hook only to animation, so image_avif_conversion_gif_animated_*
  // counts genuine expiries while the still buckets (png/jpeg/avif) count
  // refusals.
  //
  // What this counter never covers is an admitted still that ran over budget
  // anyway -- that produces an image, so it cannot be a timeout. It is counted
  // separately, as an overrun, just below.
  const bool was_timed_out =
      timeout_handler.was_timed_out() || declined_over_budget;
  if (!ok) {
    // Never leave a partial .avif for a caller to serve; the fallback chain
    // (avif->webp->original) relies on a clean empty result here.
    output->clear();
  }

  // Still-image OVERRUN detection. The admission test in PrepareImage() works
  // off an ESTIMATE, so it is wrong sometimes: an image whose estimated cost
  // fit the budget can still exceed it in reality (an unusually hard source, a
  // loaded or slower machine, an arch the table under-predicts). Because a
  // still encode cannot be interrupted, there is nothing to do about that
  // while it happens -- but it must not be SILENT, which is what it would
  // otherwise be: the admission test only ever counts refusals, so a genuine
  // expiry would leave no trace on the stats page at all.
  //
  // The result is deliberately KEPT. Discarding a finished encode is the exact
  // defect this whole change exists to remove; the CPU is already spent, and
  // throwing the output away converts a slow success into a total loss for no
  // gain. So this reports and moves on.
  //
  // Consequently ok stays true here while a timeout-flavoured event is
  // recorded -- which is precisely why this is a SEPARATE counter and not
  // `was_timed_out`. Feeding it into was_timed_out would trip
  // UpdateConversionStats' DCHECK(!ok) in debug builds, and would also be a
  // lie: the conversion did happen and the image is being served.
  //
  // Stills only. An animated encode has a real deadline (the frame-boundary
  // progress hook), so for animation an over-budget encode is aborted and
  // already counted as a timeout; it can never reach here having succeeded.
  // "Still" is spelled num_frames <= 1 to match AvifFrameWriter's own
  // definition of `animated` exactly -- the two must not be able to disagree
  // about which mechanism governed a given image.
  const bool overran_budget =
      ok && image_spec.num_frames <= 1 &&
      options_->avif_conversion_timeout_ms > 0 &&
      timeout_handler.time_elapsed_ms() > options_->avif_conversion_timeout_ms;
  if (overran_budget) {
    PS_LOG_INFO(
        handler_,
        "AVIF: still-image encode overran its budget (%s ms elapsed "
        "vs %s ms allowed); keeping the result -- an AV1 still encode "
        "cannot be interrupted once started.",
        Integer64ToString(timeout_handler.time_elapsed_ms()).c_str(),
        Integer64ToString(options_->avif_conversion_timeout_ms).c_str());
  }

  UpdateConversionStats(ok, was_timed_out, overran_budget,
                        timeout_handler.time_elapsed_ms(), var_type,
                        options_->avif_conversion_variables);
  return ok;
}

bool ImageImpl::ConvertJpegToAvif(const GoogleString& original_jpeg,
                                  int configured_quality,
                                  GoogleString* compressed_avif) {
  return RewriteToAvif(pagespeed::image_compression::IMAGE_JPEG, original_jpeg,
                       false /* lossless */, configured_quality,
                       compressed_avif);
}

bool ImageImpl::ConvertPngToAvif(const GoogleString& original_png,
                                 bool lossless, int configured_quality,
                                 GoogleString* compressed_avif) {
  return RewriteToAvif(pagespeed::image_compression::IMAGE_PNG, original_png,
                       lossless, configured_quality, compressed_avif);
}

bool ImageImpl::ConvertAnimatedGifToAvif(const GoogleString& original_gif,
                                         int configured_quality,
                                         GoogleString* compressed_avif) {
  return RewriteToAvif(pagespeed::image_compression::IMAGE_GIF, original_gif,
                       false /* lossless */, configured_quality,
                       compressed_avif);
}

bool ImageImpl::ReduceAvifImageQuality(const GoogleString& original_avif,
                                       int configured_quality,
                                       GoogleString* compressed_avif) {
  return RewriteToAvif(pagespeed::image_compression::IMAGE_AVIF, original_avif,
                       false /* lossless */, configured_quality,
                       compressed_avif);
}

inline bool ImageImpl::ComputeOutputContentsFromGifOrPng(
    const GoogleString& string_for_image, const PngReaderInterface* png_reader,
    bool fall_back_to_png, const char* dbg_input_format, ImageType input_type,
    ConversionVariables::VariableType var_type, bool suppress_avif_candidate) {
  // Don't try to optimize empty images, it just messes things up.
  if (dims_.width() <= 0 || dims_.height() <= 0) {
    return false;
  }

  bool ok = false;
  bool is_animated = false;
  bool has_transparency = false;
  bool is_photo = false;
  bool compress_color_losslessly = false;
  ImageType output_type = IMAGE_UNKNOWN;

  AnalyzeImage(ImageTypeToImageFormat(input_type), string_for_image.data(),
               string_for_image.length(), nullptr /* width */,
               nullptr /* height */, nullptr /* is_progressive */, &is_animated,
               &has_transparency, &is_photo, nullptr /* quality */,
               nullptr /* reader */, handler_.get());

  debug_message_ = absl::StrFormat(
      "Image%s has%s transparent pixels,"
      " is%s sensitive to compression noise, and"
      " has%s animation.",
      debug_message_url_.c_str(), (has_transparency ? "" : " no"),
      (is_photo ? " not" : ""), (is_animated ? "" : " no"));

  // By default, a lossless image conversion is eligible for lossless webp
  // conversion.
  if (is_animated) {
    if (options_->preferred_webp == WEBP_ANIMATED &&
        options_->webp_animated_quality > 0) {
      output_type = IMAGE_WEBP_ANIMATED;
    }
    // else we can't recompress this image
  } else if (is_photo && options_->convert_png_to_jpeg &&
             (input_type == IMAGE_PNG ||
              (input_type == IMAGE_GIF && options_->convert_gif_to_png))) {
    // Can be converted to lossy format.
    if (!has_transparency) {
      // No alpha; can be converted to WebP lossy or JPEG.
      if (options_->preferred_webp != WEBP_NONE &&
          options_->convert_jpeg_to_webp && options_->webp_quality > 0) {
        compress_color_losslessly = false;
        output_type = IMAGE_WEBP;
      } else if (options_->jpeg_quality > 0) {
        output_type = IMAGE_JPEG;
      }
    } else {
      if (options_->allow_webp_alpha && options_->convert_jpeg_to_webp &&
          options_->webp_quality > 0) {
        compress_color_losslessly = false;
        output_type = IMAGE_WEBP_LOSSLESS_OR_ALPHA;
      }
    }
  } else {
    // Must be converted to lossless format.
    if (options_->preferred_webp == WEBP_ANIMATED ||
        options_->preferred_webp == WEBP_LOSSLESS) {
      compress_color_losslessly = true;
      output_type = IMAGE_WEBP_LOSSLESS_OR_ALPHA;
    }
  }

  if (output_type == IMAGE_WEBP_ANIMATED) {
    ok = ConvertAnimatedGifToWebp(has_transparency);
  } else {
    if (output_type == IMAGE_WEBP ||
        output_type == IMAGE_WEBP_LOSSLESS_OR_ALPHA) {
      ok = MayConvert() && ConvertPngToWebp(*png_reader, string_for_image,
                                            compress_color_losslessly,
                                            has_transparency, var_type);
      // TODO(huibao): Re-evaluate why we need to try a different format, if the
      // conversion to WebP failed.
      if (!ok) {
        // If the conversion to WebP failed, we will try converting the image to
        // jpeg or png.
        if (output_type == IMAGE_WEBP) {
          output_type = IMAGE_JPEG;
        } else {
          fall_back_to_png = true;
        }
      }
    }

    if (output_type == IMAGE_JPEG) {
      JpegCompressionOptions jpeg_options;
      ConvertToJpegOptions(*options_.get(), &jpeg_options);
      ok = MayConvert() && ImageConverter::ConvertPngToJpeg(
                               *png_reader, string_for_image, jpeg_options,
                               &output_contents_, handler_.get());
    }

    if (!ok && fall_back_to_png) {
      ok = MayConvert() && PngOptimizer::OptimizePngBestCompression(
                               *png_reader, string_for_image, &output_contents_,
                               handler_.get());
      output_type = IMAGE_PNG;
    }
  }

  // AVIF pick-smaller for GIF/PNG sources. Attempt an AVIF encode of
  // the same source and adopt it only if it succeeds and is smaller than the
  // WebP/JPEG/PNG candidate produced above (or if nothing was produced). The
  // AVIF class mirrors the WebP ladder: animated -> AVIS, photographic -> lossy
  // AVIF (alpha gated by allow_avif_alpha), otherwise lossless AVIF. C2PA is
  // guarded upstream: a manifest-bearing non-JPEG source already returned
  // skip-not-strip in ComputeOutputContents, EXCEPT the PNG carry
  // path, which reaches here with suppress_avif_candidate set (the carry
  // splice only fits a PNG output, so an AVIF winner would be discarded).
  // Gate on the cheap capability check first; MayConvert() (which consumes a
  // conversion attempt) is deferred to the actual encode below so a non-AVIF
  // request does not burn an attempt and starve the WebP/PNG/JPEG fallbacks.
  if (!suppress_avif_candidate &&
      options_->preferred_avif != pagespeed::image_compression::LIBAVIF_NONE) {
    bool avif_eligible = false;
    bool avif_lossless = false;
    int avif_quality = options_->avif_quality;
    ImageType avif_type = IMAGE_UNKNOWN;
    // Per-source lossy gate mirrors WebP's reuse of convert_jpeg_to_webp: the
    // lossy-AVIF-allowed flag is convert_png_to_avif for a PNG source and
    // convert_gif_to_avif for a GIF source.
    const bool lossy_avif_allowed = (input_type == IMAGE_PNG)
                                        ? options_->convert_png_to_avif
                                        : options_->convert_gif_to_avif;
    if (is_animated) {
      if (options_->allow_avif_animated &&
          options_->preferred_avif ==
              pagespeed::image_compression::LIBAVIF_ANIMATED &&
          options_->avif_animated_quality > 0) {
        avif_eligible = true;
        avif_quality = options_->avif_animated_quality;
        avif_type = IMAGE_AVIF_ANIMATED;
      }
    } else if (is_photo && lossy_avif_allowed && options_->avif_quality > 0) {
      if (!has_transparency) {
        avif_eligible = true;
        avif_type = IMAGE_AVIF;
      } else if (options_->allow_avif_alpha) {
        avif_eligible = true;
        avif_type = IMAGE_AVIF_LOSSLESS_OR_ALPHA;
      }
    } else if (options_->preferred_avif ==
                   pagespeed::image_compression::LIBAVIF_LOSSLESS ||
               options_->preferred_avif ==
                   pagespeed::image_compression::LIBAVIF_ANIMATED) {
      // Non-photo -> lossless AVIF (mirrors the WebP lossless arm).
      avif_eligible = true;
      avif_lossless = true;
      avif_type = IMAGE_AVIF_LOSSLESS_OR_ALPHA;
    }

    if (avif_eligible && MayConvert()) {
      GoogleString avif_out;
      bool avif_ok;
      if (is_animated) {
        avif_ok =
            ConvertAnimatedGifToAvif(string_for_image, avif_quality, &avif_out);
      } else if (input_type == IMAGE_PNG) {
        avif_ok = ConvertPngToAvif(string_for_image, avif_lossless,
                                   avif_quality, &avif_out);
      } else {
        // Still GIF (or other frame-readable source): read via its native
        // format rather than the PNG path.
        avif_ok =
            RewriteToAvif(ImageTypeToImageFormat(input_type), string_for_image,
                          avif_lossless, avif_quality, &avif_out);
      }
      if (avif_ok && (!ok || avif_out.size() < output_contents_.size())) {
        output_contents_.swap(avif_out);
        output_type = avif_type;
        ok = true;
      }
    }
  }

  if (ok) {
    image_type_ = output_type;
  } else {
    image_type_ = input_type;
  }

  VLOG(1) << "Image conversion: " << ok << " " << dbg_input_format << "->"
          << ImageFormatToString(ImageTypeToImageFormat(image_type_)) << " for "
          << url_;

  return ok;
}

bool ImageImpl::ConvertPngToWebp(const PngReaderInterface& png_reader,
                                 const GoogleString& input_image,
                                 bool compress_color_losslessly,
                                 bool has_transparency,
                                 ConversionVariables::VariableType var_type) {
  ConversionTimeoutHandler timeout_handler(options_->webp_conversion_timeout_ms,
                                           timer_, handler_.get());
  WebpConfiguration webp_config;

  // Quality/speed trade-off (0=fast, 6=slower-better).
  // This is the default value in libpagespeed. We should evaluate
  // whether this is the optimal value, and consider making it
  // tunable.
  webp_config.method = 3;
  webp_config.quality = options_->webp_quality;
  webp_config.progress_hook = ConversionTimeoutHandler::Continue;
  webp_config.user_data = &timeout_handler;

  ImageType target_image_type = IMAGE_WEBP_LOSSLESS_OR_ALPHA;
  if (compress_color_losslessly) {
    // Note that webp_config.alpha_quality and
    // webp_config.alpha_compression are only meaningful in the
    // lossy compression case.
    webp_config.lossless = true;
  } else {
    webp_config.lossless = false;
    if (has_transparency) {
      webp_config.alpha_quality = 100;
      webp_config.alpha_compression = 1;
    } else {
      webp_config.alpha_quality = 0;
      webp_config.alpha_compression = 0;
      image_type_ = IMAGE_WEBP;
    }
  }

  // TODO(huibao): Remove "is_opaque" from the returned arguments in
  // ConvertPngToWebp() and PngScanlineReader::InitializeRead().
  // The technique they use can only detect some of the opaque images.
  // PixelFormatOptimizer has a more expensive, but comprehensive solution.
  bool not_used;
  timeout_handler.Start(&output_contents_);
  bool ok = ImageConverter::ConvertPngToWebp(png_reader, input_image,
                                             webp_config, &output_contents_,
                                             &not_used, handler_.get());

  if (ok) {
    image_type_ = target_image_type;
  }
  timeout_handler.Stop();

  bool was_timed_out = timeout_handler.was_timed_out();
  int64 time_elapsed_ms = timeout_handler.time_elapsed_ms();

  UpdateConversionStats(ok, was_timed_out, false /* overran_budget */,
                        time_elapsed_ms, var_type,
                        options_->webp_conversion_variables);

  UpdateConversionStats(
      ok, was_timed_out, false /* overran_budget */, time_elapsed_ms,
      (has_transparency ? Image::ConversionVariables::NONOPAQUE
                        : Image::ConversionVariables::OPAQUE),
      options_->webp_conversion_variables);

  return ok;
}

bool ImageImpl::OptimizePng(const PngReaderInterface& png_reader,
                            const GoogleString& image_data) {
  bool ok = MayConvert() &&
            PngOptimizer::OptimizePngBestCompression(
                png_reader, image_data, &output_contents_, handler_.get());
  if (ok) {
    image_type_ = IMAGE_PNG;
  }
  return ok;
}

bool ImageImpl::OptimizePngOrConvertToJpeg(const PngReaderInterface& png_reader,
                                           const GoogleString& image_data) {
  bool is_png;
  JpegCompressionOptions jpeg_options;
  ConvertToJpegOptions(*options_.get(), &jpeg_options);
  bool ok = MayConvert() && ImageConverter::OptimizePngOrConvertToJpeg(
                                png_reader, image_data, jpeg_options,
                                &output_contents_, &is_png, handler_.get());
  if (ok) {
    if (is_png) {
      image_type_ = IMAGE_PNG;
    } else {
      image_type_ = IMAGE_JPEG;
    }
  }
  return ok;
}

void ImageImpl::ConvertToJpegOptions(const Image::CompressionOptions& options,
                                     JpegCompressionOptions* jpeg_options) {
  int input_quality = GetJpegQualityFromImage(original_contents_);
  jpeg_options->retain_color_profile = options.retain_color_profile;
  jpeg_options->retain_exif_data = options.retain_exif_data;
  jpeg_options->preserve_c2pa = options.preserve_c2pa;
  int output_quality = EstimateQualityForResizedJpeg();

  if (options.jpeg_quality > 0) {
    // If the source image is JPEG we want to fallback to lossless if the input
    // quality is less than the quality we want to set for final compression and
    // num progressive scans is not set. Incase we are not able to decode the
    // input image quality, then we use lossless path.
    if (image_type() != IMAGE_JPEG || options.jpeg_num_progressive_scans > 0 ||
        input_quality > output_quality) {
      jpeg_options->lossy = true;
      jpeg_options->lossy_options.quality = output_quality;
      if (options.progressive_jpeg) {
        jpeg_options->lossy_options.num_scans =
            options.jpeg_num_progressive_scans;
      }

      if (options.retain_color_sampling) {
        jpeg_options->lossy_options.color_sampling = RETAIN;
      }
    }
  }

  jpeg_options->progressive =
      options.progressive_jpeg && ShouldConvertToProgressive(output_quality);
}

bool ImageImpl::ShouldConvertToProgressive(int64 quality) const {
  bool progressive = false;
  const ImageDim* expected_dimensions = &dims_;
  if (ImageUrlEncoder::HasValidDimensions(resized_dimensions_)) {
    expected_dimensions = &resized_dimensions_;
  }
  if (ImageUrlEncoder::HasValidDimensions(*expected_dimensions)) {
    progressive = pagespeed::image_compression::ShouldConvertToProgressive(
        quality, options_->progressive_jpeg_min_bytes,
        original_contents_.size(), expected_dimensions->width(),
        expected_dimensions->height());
  } else {
    progressive = (static_cast<int64>(original_contents_.size()) >=
                   options_->progressive_jpeg_min_bytes);
  }
  return progressive;
}

StringPiece Image::Contents() {
  StringPiece contents;
  if (this->image_type() != IMAGE_UNKNOWN) {
    contents = original_contents_;
    if (output_valid_ || ComputeOutputContents()) {
      contents = output_contents_;
    }
  }
  return contents;
}

bool ImageImpl::DrawImage(Image* image, int x, int y) {
  // Create a reader for reading the original canvas image.
  std::unique_ptr<ScanlineReaderInterface> canvas_reader(CreateScanlineReader(
      pagespeed::image_compression::IMAGE_PNG, output_contents_.data(),
      output_contents_.length(), handler_.get()));
  if (canvas_reader == nullptr) {
    PS_LOG_ERROR(handler_, "Cannot open canvas image.");
    return false;
  }

  // Get the size and pixel format of the original canvas image.
  const size_t canvas_width = canvas_reader->GetImageWidth();
  const size_t canvas_height = canvas_reader->GetImageHeight();
  const PixelFormat canvas_pixel_format = canvas_reader->GetPixelFormat();

  // Initialize a reader for reading the image which will be sprited.
  ImageImpl* impl = static_cast<ImageImpl*>(image);
  std::unique_ptr<ScanlineReaderInterface> image_reader(
      CreateScanlineReader(ImageTypeToImageFormat(impl->image_type()),
                           impl->original_contents().data(),
                           impl->original_contents().length(), handler_.get()));
  if (image_reader == nullptr) {
    PS_LOG_INFO(handler_, "Cannot open the image which will be sprited.");
    return false;
  }

  // Get the size of the image which will be sprited.
  const size_t image_width = image_reader->GetImageWidth();
  const size_t image_height = image_reader->GetImageHeight();
  const PixelFormat image_pixel_format = image_reader->GetPixelFormat();

  if (x + image_width > canvas_width || y + image_height > canvas_height) {
    PS_LOG_INFO(handler_, "The new image cannot fit into the canvas.");
    return false;
  }

  bool has_transparency = false;
  PixelFormat output_pixel_format = RGB_888;
  if (image_pixel_format == RGBA_8888 || canvas_pixel_format == RGBA_8888) {
    has_transparency = true;
    output_pixel_format = RGBA_8888;
  }

  const size_t bytes_per_pixel =
      GetNumChannelsFromPixelFormat(output_pixel_format, handler_.get());
  const size_t bytes_per_scanline = canvas_width * bytes_per_pixel;
  std::unique_ptr<uint8[]> scanline(new uint8[bytes_per_scanline]);

  // Create a writer for writing the new canvas image.
  GoogleString canvas_image;
  std::unique_ptr<ScanlineWriterInterface> canvas_writer(
      CreateUncompressedPngWriter(canvas_width, canvas_height, &canvas_image,
                                  handler_.get(), has_transparency));
  if (canvas_writer == nullptr) {
    PS_LOG_ERROR(handler_, "Failed to create canvas writer.");
    return false;
  }

  // Overlay the new image onto the canvas image.
  for (int row = 0; row < static_cast<int>(canvas_height); ++row) {
    uint8* canvas_line = nullptr;
    if (!canvas_reader->ReadNextScanline(
            reinterpret_cast<void**>(&canvas_line))) {
      PS_LOG_ERROR(handler_, "Failed to read canvas image.");
      return false;
    }

    if (row >= y && row < y + static_cast<int>(image_height)) {
      uint8* image_line = nullptr;
      if (!image_reader->ReadNextScanline(
              reinterpret_cast<void**>(&image_line))) {
        PS_LOG_INFO(handler_,
                    "Failed to read the image which will be sprited.");
        return false;
      }

      // Set the entire scanline to white. This operation has no effect
      // on the webpage; it just gives a clean background to the
      // sprite image.
      memset(scanline.get(), kAlphaOpaque, x * bytes_per_pixel);
      memset(scanline.get() + (x + image_width) * bytes_per_pixel, kAlphaOpaque,
             (canvas_width - image_width - x) * bytes_per_pixel);

      ExpandPixelFormat(image_width, image_pixel_format, 0, image_line,
                        output_pixel_format, x, scanline.get(), handler_.get());
    } else {
      ExpandPixelFormat(canvas_width, canvas_pixel_format, 0, canvas_line,
                        output_pixel_format, 0, scanline.get(), handler_.get());
    }

    if (!canvas_writer->WriteNextScanline(
            reinterpret_cast<void*>(scanline.get()))) {
      PS_LOG_ERROR(handler_, "Failed to write canvas image.");
      return false;
    }
  }

  if (!canvas_writer->FinalizeWrite()) {
    PS_LOG_ERROR(handler_, "Failed to close canvas file.");
    return false;
  }

  output_contents_ = canvas_image;
  output_valid_ = true;
  return true;
}

}  // namespace net_instaweb
