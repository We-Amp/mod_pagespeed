// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// This file is derived from mod_pagespeed and has been substantially modified.
// Originally licensed under Apache License, Version 2.0.
// Copyright (c) 2010-2017 Google Inc.
// Copyright (c) 2018 The Apache Software Foundation.

// JPEG optimization and scanline writing implementation.
// Ported from mod_pagespeed's pagespeed/kernel/image/jpeg_optimizer.cc

#include "lib/image/jpeg_optimizer.h"

#include <csetjmp>
// 'stdio.h' provides FILE for jpeglib (needed for certain builds)
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "lib/base/message_handler.h"
#include "lib/image/exif_orientation.h"
#include "lib/image/jpeg_reader.h"
#include "lib/image/scanline_utils.h"

extern "C" {
#include "jpeglib.h"  // NOLINT
}

using pagespeed::MessageHandler;
using pagespeed::image_compression::BuildExifOrientationApp1;
using pagespeed::image_compression::ColorSampling;
using pagespeed::image_compression::ExifOrientationSwapsDimensions;
using pagespeed::image_compression::JpegCompressionOptions;
using pagespeed::image_compression::JpegLossyOptions;
using pagespeed::image_compression::kExifOrientationApp1Size;
using pagespeed::image_compression::kScanlineMaxDimension;
using pagespeed::image_compression::kScanlineMaxImagePixels;
using pagespeed::image_compression::ReadJpegExifOrientation;
using pagespeed::image_compression::RETAIN;
using pagespeed::image_compression::YUV420;
using pagespeed::image_compression::YUV422;
using pagespeed::image_compression::YUV444;

namespace {

// Wraps a libjpeg compress operation in a local jmp_buf guard when no outer
// guard exists (client_data is null). When an outer guard is already installed,
// the operation runs unguarded — errors longjmp directly to the outer caller.
//
// SAFETY: longjmp unwinds the stack without calling destructors. The lambda
// body must contain only trivially-destructible locals (POD, raw pointers,
// C arrays). Using std::string, std::vector, or any RAII object inside the
// lambda is undefined behavior if libjpeg triggers an error. On the outer-guard
// path (client_data != nullptr), longjmp bypasses this frame entirely.
template <typename Func>
bool JpegProtectedCall(jpeg_compress_struct& cinfo, Func&& operation) {
  if (cinfo.client_data != nullptr) {
    operation();
    return true;
  }
  jmp_buf local_env;
  cinfo.client_data = static_cast<void*>(&local_env);
  if (setjmp(local_env)) {
    cinfo.client_data = nullptr;
    return false;
  }
  operation();
  cinfo.client_data = nullptr;
  return true;
}
// Unfortunately, libjpeg normally only supports writing images to C
// FILE pointers, wheras we want to write to a C++ string.
// Fortunately, libjpeg also provides an extension mechanism. Below,
// we define a new kind of jpeg_destination_mgr for writing to strings.

// The below code was adapted from the JPEGMemoryReader class that can
// be found in src/o3d/core/cross/bitmap_jpg.cc in the Chromium source
// tree (r29423).

#define DESTINATION_MANAGER_BUFFER_SIZE 4096
struct DestinationManager : public jpeg_destination_mgr {
  JOCTET buffer[DESTINATION_MANAGER_BUFFER_SIZE];
  std::string* str;
};

METHODDEF(void) InitDestination(j_compress_ptr cinfo) {
  DestinationManager& dest =
      *reinterpret_cast<DestinationManager*>(cinfo->dest);

  dest.next_output_byte = dest.buffer;
  dest.free_in_buffer = DESTINATION_MANAGER_BUFFER_SIZE;
}

METHODDEF(boolean) EmptyOutputBuffer(j_compress_ptr cinfo) {
  DestinationManager& dest =
      *reinterpret_cast<DestinationManager*>(cinfo->dest);

  dest.str->append(reinterpret_cast<char*>(dest.buffer),
                   DESTINATION_MANAGER_BUFFER_SIZE);

  dest.free_in_buffer = DESTINATION_MANAGER_BUFFER_SIZE;
  dest.next_output_byte = dest.buffer;

  return TRUE;
}

METHODDEF(void) TermDestination(j_compress_ptr cinfo) {
  DestinationManager& dest =
      *reinterpret_cast<DestinationManager*>(cinfo->dest);

  const size_t datacount =
      DESTINATION_MANAGER_BUFFER_SIZE - dest.free_in_buffer;
  if (datacount > 0) {
    dest.str->append(reinterpret_cast<char*>(dest.buffer), datacount);
  }
}

// Call this function on a j_compress_ptr to install a writer that
// will write to the given string.
void JpegStringWriter(j_compress_ptr cinfo, std::string* data_dest) {
  if (cinfo->dest == nullptr) {
    cinfo->dest = (struct jpeg_destination_mgr*)(*cinfo->mem->alloc_small)(
        (j_common_ptr)cinfo, JPOOL_PERMANENT, sizeof(DestinationManager));
  }
  DestinationManager& dest =
      *reinterpret_cast<DestinationManager*>(cinfo->dest);

  dest.str = data_dest;

  dest.init_destination = InitDestination;
  dest.empty_output_buffer = EmptyOutputBuffer;
  dest.term_destination = TermDestination;
}

// ErrorExit() is installed as a callback, called on errors
// encountered within libjpeg.  The longjmp jumps back
// to the setjmp in JpegOptimizer::CreateOptimizedJpeg().
void ErrorExit(j_common_ptr jpeg_state_struct) {
  jmp_buf* env = static_cast<jmp_buf*>(jpeg_state_struct->client_data);
  (*jpeg_state_struct->err->output_message)(jpeg_state_struct);
  if (env) {
    longjmp(*env, 1);
  }
  // libjpeg requires error_exit to never return. If no jmp_buf is set,
  // abort to prevent continuing with corrupt library state.
  abort();
}

// OutputMessage is called by libjpeg code on an error when reading.
// Without this function, a default function would print to standard
// error.
void OutputMessage(j_common_ptr /*jpeg_decompress*/) {
  // The following code is handy for debugging.
  /*
  char buf[JMSG_LENGTH_MAX];
  (*jpeg_decompress->err->format_message)(jpeg_decompress, buf);
  fprintf(stderr, "JPEG Reader Error: %s\n", buf);
  */
}

// Marker for APPN segment is obtained by adding N to JPEG_APP0.
const int kColorProfileMarker = JPEG_APP0 + 2;
const int kExifDataMarker = JPEG_APP0 + 1;
// C2PA / Content Credentials provenance manifests are carried in APP11
// (JUMBF) segments. A single manifest may span multiple consecutive APP11
// markers when it exceeds the 64KB per-marker limit.
const int kC2paMarker = JPEG_APP0 + 11;
// Signifies max bytes that needs to read, while reading jpeg segments
// like exif data, color profiles and etc.
const int kMaxSegmentSize = 0xFFFF;

// Initializes the jpeg compress struct.
void InitJpegCompress(j_compress_ptr cinfo, jpeg_error_mgr* compress_error) {
  memset(cinfo, 0, sizeof(jpeg_compress_struct));
  memset(compress_error, 0, sizeof(jpeg_error_mgr));

  cinfo->err = jpeg_std_error(compress_error);
  compress_error->error_exit = &ErrorExit;
  compress_error->output_message = &OutputMessage;
  jpeg_create_compress(cinfo);
}

void SetJpegCompressBeforeStartCompress(
    const JpegCompressionOptions& options,
    const jpeg_decompress_struct* jpeg_decompress,
    jpeg_compress_struct* jpeg_compress) {
  if (options.lossy) {
    const JpegLossyOptions& lossy_options = options.lossy_options;
    // Set the compression parameters if set and lossy compression
    // is enabled, else use the defaults. Last parameter to
    // jpeg_set_quality resticts the jpeg quantizer values in 8 bit
    // values, even though jpeg support 12 bit quantizer values, it
    // is not supported widely.
    jpeg_set_quality(jpeg_compress, lossy_options.quality, 1);

    // Set the color subsampling if applicable.
    if (jpeg_compress->jpeg_color_space == JCS_YCbCr) {
      // Set the color sampling.
      if (lossy_options.color_sampling == YUV444) {
        jpeg_compress->comp_info[0].h_samp_factor = 1;
        jpeg_compress->comp_info[0].v_samp_factor = 1;
      } else if (lossy_options.color_sampling == YUV422) {
        jpeg_compress->comp_info[0].h_samp_factor = 2;
        jpeg_compress->comp_info[0].v_samp_factor = 1;
      } else if (lossy_options.color_sampling == YUV420) {
        jpeg_compress->comp_info[0].h_samp_factor = 2;
        jpeg_compress->comp_info[0].v_samp_factor = 2;
      } else if (lossy_options.color_sampling == RETAIN &&
                 jpeg_decompress != nullptr) {
        // Retain the input.
        for (int idx = 0; idx < jpeg_compress->num_components; ++idx) {
          jpeg_compress->comp_info[idx].h_samp_factor =
              jpeg_decompress->comp_info[idx].h_samp_factor;
          jpeg_compress->comp_info[idx].v_samp_factor =
              jpeg_decompress->comp_info[idx].v_samp_factor;
        }
      }
    }
  }

  if (options.progressive) {
    jpeg_simple_progression(jpeg_compress);

    if (options.lossy && options.lossy_options.num_scans > 0) {
      // We can honour the num scans only if the number of scans
      // we want is less than or equals to total number of scans
      // defined for this image, else compress will fail.
      jpeg_compress->num_scans =
          std::min(jpeg_compress->num_scans, options.lossy_options.num_scans);
    }
  }
}

// Emits a minimal APP1 segment carrying ONLY an accurate Orientation tag
// (issue #1005). Used by JPEG->JPEG recompress paths that drop the original
// EXIF but cannot bake the orientation into the pixels (lossless coefficient
// copy, oversized rasters): the stored pixels stay rotated, so the tag must
// survive for the image to render upright. Carries no other metadata.
void WriteExifOrientationMarker(uint8_t exif_orientation,
                                jpeg_compress_struct* jpeg_compress) {
  // POD buffer only: this runs inside a libjpeg setjmp/longjmp region.
  uint8_t app1[kExifOrientationApp1Size];
  BuildExifOrientationApp1(exif_orientation, app1);
  jpeg_write_marker(jpeg_compress, kExifDataMarker, app1,
                    kExifOrientationApp1Size);
}

void SetJpegCompressAfterStartCompress(
    const JpegCompressionOptions& options,
    const jpeg_decompress_struct& jpeg_decompress,
    jpeg_compress_struct* jpeg_compress) {
  if (options.retain_color_profile || options.retain_exif_data ||
      options.preserve_c2pa) {
    jpeg_saved_marker_ptr marker;
    for (marker = jpeg_decompress.marker_list; marker != nullptr;
         marker = marker->next) {
      // We only copy these headers if present in the decompress
      // struct. Each saved APP11 marker is copied verbatim so a
      // multi-marker C2PA manifest survives intact.
      if ((marker->marker == kExifDataMarker && options.retain_exif_data) ||
          (marker->marker == kColorProfileMarker &&
           options.retain_color_profile) ||
          (marker->marker == kC2paMarker && options.preserve_c2pa)) {
        jpeg_write_marker(jpeg_compress, marker->marker, marker->data,
                          marker->data_length);
      }
    }
  }
}

class JpegOptimizer {
 public:
  explicit JpegOptimizer(MessageHandler* handler);
  ~JpegOptimizer();

  // Take the given input file and compress it, either losslessly or
  // lossily, depending on the passed in options. Note that the
  // options parameter can be null, in which case the default options
  // are used.
  // If this function fails (returns false), it can be called again.
  // @return true on success, false on failure.
  bool CreateOptimizedJpeg(const std::string& original, std::string* compressed,
                           const JpegCompressionOptions& options);

 private:
  bool DoCreateOptimizedJpeg(const std::string& original,
                             jpeg_decompress_struct* jpeg_decompress,
                             std::string* compressed,
                             const JpegCompressionOptions& options);

  bool OptimizeLossless(jpeg_decompress_struct* jpeg_decompress,
                        std::string* compressed,
                        const JpegCompressionOptions& options,
                        uint8_t exif_orientation);

  bool OptimizeLossy(jpeg_decompress_struct* jpeg_decompress,
                     std::string* compressed,
                     const JpegCompressionOptions& options,
                     uint8_t exif_orientation);

  // Structures for jpeg compression.
  jpeg_compress_struct jpeg_compress_;
  jpeg_error_mgr compress_error_;
  MessageHandler* message_handler_;
  pagespeed::image_compression::JpegReader reader_;
  // Scanline buffer for lossy optimization. Stored as a member so the
  // destructor can free it after libjpeg's longjmp on corrupt images.
  JSAMPLE* lossy_row_buffer_ = nullptr;
  // Full-raster buffer for baking EXIF orientation into the pixels on the
  // lossy path (issue #1005). Member for the same longjmp-safety reason.
  // Holds the stored (as-decoded) raster; upright rows are gathered from it
  // one at a time into lossy_row_buffer_.
  JSAMPLE* bake_stored_buffer_ = nullptr;

  DISALLOW_COPY_AND_ASSIGN(JpegOptimizer);
};

JpegOptimizer::JpegOptimizer(MessageHandler* handler)
    : message_handler_(handler), reader_(handler) {
  InitJpegCompress(&jpeg_compress_, &compress_error_);
}

JpegOptimizer::~JpegOptimizer() {
  free(lossy_row_buffer_);
  free(bake_stored_buffer_);
  jpeg_destroy_compress(&jpeg_compress_);
}

bool JpegOptimizer::OptimizeLossy(jpeg_decompress_struct* jpeg_decompress,
                                  std::string* compressed,
                                  const JpegCompressionOptions& options,
                                  uint8_t exif_orientation) {
  if (!options.lossy) {
    PS_LOG_DFATAL(message_handler_,
                  "lossy is not set in options for lossy jpeg compression");
    return false;
  }

  const size_t stored_width = jpeg_decompress->image_width;
  const size_t stored_height = jpeg_decompress->image_height;
  const size_t components = jpeg_decompress->num_components;

  // Bake EXIF orientation into the pixels (issue #1005): this path already
  // decodes and re-encodes, so rotating costs only the full-raster buffer a
  // rotation inherently needs. Oversized rasters skip the bake and get an
  // accurate minimal Orientation tag instead (see below), so they still
  // render upright.
  const uint64_t raster_bytes =
      static_cast<uint64_t>(stored_width) * stored_height * components;
  const bool bake_orientation =
      exif_orientation != 1 && raster_bytes > 0 &&
      raster_bytes <= pagespeed::image_compression::OrientationBakeByteLimit();
  const bool swap_dimensions =
      bake_orientation && ExifOrientationSwapsDimensions(exif_orientation);
  const size_t out_width = swap_dimensions ? stored_height : stored_width;
  const size_t out_height = swap_dimensions ? stored_width : stored_height;

  // Copy data from the source to the dest.
  jpeg_compress_.image_width = out_width;
  jpeg_compress_.image_height = out_height;
  jpeg_compress_.input_components = jpeg_decompress->num_components;

  // Persist the input file's colorspace.
  jpeg_decompress->out_color_space = jpeg_decompress->jpeg_color_space;
  jpeg_compress_.in_color_space = jpeg_decompress->jpeg_color_space;

  // Set the default options.
  jpeg_set_defaults(&jpeg_compress_);

  // Set optimize huffman to true.
  jpeg_compress_.optimize_coding = TRUE;

  SetJpegCompressBeforeStartCompress(options, jpeg_decompress, &jpeg_compress_);

  // Prepare to write to a string.
  JpegStringWriter(&jpeg_compress_, compressed);

  jpeg_start_compress(&jpeg_compress_, TRUE);
  jpeg_start_decompress(jpeg_decompress);

  // Write any markers if needed.
  SetJpegCompressAfterStartCompress(options, *jpeg_decompress, &jpeg_compress_);
  if (exif_orientation != 1 && !bake_orientation && !options.retain_exif_data) {
    // Raster too large to bake: keep the image rendering upright via an
    // accurate minimal Orientation tag on the untransformed pixels.
    WriteExifOrientationMarker(exif_orientation, &jpeg_compress_);
  }

  // Make sure input/output parameters are configured correctly.
  assert(stored_width == jpeg_decompress->output_width);
  assert(stored_height == jpeg_decompress->output_height);
  assert(jpeg_compress_.input_components == jpeg_decompress->output_components);
  assert(jpeg_compress_.in_color_space == jpeg_decompress->out_color_space);

  bool valid_jpeg = true;

  if (bake_orientation) {
    // Decode the full stored raster, then feed upright rows to the encoder.
    // Free any buffer left over from a failed prior attempt on this
    // optimizer (CreateOptimizedJpeg's call-again-after-failure contract):
    // a libjpeg longjmp skips the in-function frees below.
    free(bake_stored_buffer_);
    free(lossy_row_buffer_);
    bake_stored_buffer_ =
        static_cast<JSAMPLE*>(malloc(static_cast<size_t>(raster_bytes)));
    lossy_row_buffer_ = static_cast<JSAMPLE*>(malloc(out_width * components));
    if (bake_stored_buffer_ == nullptr || lossy_row_buffer_ == nullptr) {
      free(bake_stored_buffer_);
      bake_stored_buffer_ = nullptr;
      free(lossy_row_buffer_);
      lossy_row_buffer_ = nullptr;
      return false;
    }
    const size_t stored_row_bytes = stored_width * components;
    JSAMPROW row_pointer[1];
    while (jpeg_decompress->output_scanline < jpeg_decompress->output_height) {
      row_pointer[0] = bake_stored_buffer_ +
                       static_cast<size_t>(jpeg_decompress->output_scanline) *
                           stored_row_bytes;
      if (jpeg_read_scanlines(jpeg_decompress, row_pointer, 1) != 1) {
        valid_jpeg = false;
        break;
      }
    }
    if (valid_jpeg) {
      row_pointer[0] = lossy_row_buffer_;
      for (size_t y = 0; y < out_height; ++y) {
        for (size_t x = 0; x < out_width; ++x) {
          size_t sx = 0;
          size_t sy = 0;
          pagespeed::image_compression::MapUprightToStored(
              exif_orientation, x, y, stored_width, stored_height, &sx, &sy);
          memcpy(lossy_row_buffer_ + x * components,
                 bake_stored_buffer_ + sy * stored_row_bytes + sx * components,
                 components);
        }
        if (jpeg_write_scanlines(&jpeg_compress_, row_pointer, 1) != 1) {
          valid_jpeg = false;
          break;
        }
      }
    }
    free(bake_stored_buffer_);
    bake_stored_buffer_ = nullptr;
    free(lossy_row_buffer_);
    lossy_row_buffer_ = nullptr;
    return valid_jpeg;
  }

  JSAMPROW row_pointer[1];
  lossy_row_buffer_ = static_cast<JSAMPLE*>(
      malloc(static_cast<size_t>(jpeg_decompress->output_width) *
             jpeg_decompress->output_components));
  if (lossy_row_buffer_ == nullptr) {
    return false;
  }
  row_pointer[0] = lossy_row_buffer_;
  while (jpeg_compress_.next_scanline < jpeg_compress_.image_height) {
    const JDIMENSION num_scanlines_read =
        jpeg_read_scanlines(jpeg_decompress, row_pointer, 1);
    if (num_scanlines_read != 1) {
      valid_jpeg = false;
      break;
    }

    if (jpeg_write_scanlines(&jpeg_compress_, row_pointer, 1) != 1) {
      // We failed to write all the row. Abort.
      valid_jpeg = false;
      break;
    }
  }

  free(lossy_row_buffer_);
  lossy_row_buffer_ = nullptr;
  return valid_jpeg;
}

bool JpegOptimizer::OptimizeLossless(jpeg_decompress_struct* jpeg_decompress,
                                     std::string* compressed,
                                     const JpegCompressionOptions& options,
                                     uint8_t exif_orientation) {
  if (options.lossy) {
    PS_LOG_DFATAL(message_handler_,
                  "Lossy options are not allowed in lossless compression.");
    return false;
  }

  // Unlike the scanline paths, which hold one row at a time,
  // jpeg_read_coefficients buffers the DCT coefficients of the whole image,
  // an allocation proportional to the dimensions declared in the header.
  // Refuse anything beyond the decode caps before that allocation happens.
  if (jpeg_decompress->image_width > kScanlineMaxDimension ||
      jpeg_decompress->image_height > kScanlineMaxDimension ||
      static_cast<uint64_t>(jpeg_decompress->image_width) *
              jpeg_decompress->image_height >
          kScanlineMaxImagePixels) {
    PS_LOG_INFO(message_handler_,
                "Image dimensions %ux%u exceed the supported maximum.",
                jpeg_decompress->image_width, jpeg_decompress->image_height);
    return false;
  }

  jvirt_barray_ptr* coefficients = jpeg_read_coefficients(jpeg_decompress);
  bool valid_jpeg = (coefficients != nullptr);

  if (valid_jpeg) {
    // Copy data from the source to the dest.
    jpeg_copy_critical_parameters(jpeg_decompress, &jpeg_compress_);

    SetJpegCompressBeforeStartCompress(options, jpeg_decompress,
                                       &jpeg_compress_);

    // Set optimize huffman to true.
    jpeg_compress_.optimize_coding = TRUE;

    // Prepare to write to a string.
    JpegStringWriter(&jpeg_compress_, compressed);

    // Copy the coefficients into the compression struct.
    jpeg_write_coefficients(&jpeg_compress_, coefficients);

    // Write any markers if needed.
    SetJpegCompressAfterStartCompress(options, *jpeg_decompress,
                                      &jpeg_compress_);
    if (exif_orientation != 1 && !options.retain_exif_data) {
      // The lossless coefficient copy cannot rotate pixels, and the
      // original EXIF (holding the Orientation tag) is being dropped: keep
      // the image rendering upright via an accurate minimal tag
      // (issue #1005).
      WriteExifOrientationMarker(exif_orientation, &jpeg_compress_);
    }
  }

  return valid_jpeg;
}

// Helper for JpegOptimizer::CreateOptimizedJpeg(). This function
// does the work, and CreateOptimizedJpeg() does some cleanup.
bool JpegOptimizer::DoCreateOptimizedJpeg(
    const std::string& original, jpeg_decompress_struct* jpeg_decompress,
    std::string* compressed, const JpegCompressionOptions& options) {
  // libjpeg's error handling mechanism requires that longjmp be
  // used to get control after an error.
  jmp_buf env;
  if (setjmp(env)) {
    // This code is run only when libjpeg hit an error, and called
    // longjmp(env). Returning false will cause
    // jpeg_abort_(de)compress to be called on
    // jpeg_(de)compress_, putting those structures back into a
    // state where they can be used again.
    return false;
  }

  // Need to install env so that it will be longjmp()ed to on error.
  jpeg_decompress->client_data = static_cast<void*>(&env);
  jpeg_compress_.client_data = static_cast<void*>(&env);

  reader_.PrepareForRead(original.data(), original.size());

  if (options.retain_color_profile) {
    jpeg_save_markers(jpeg_decompress, kColorProfileMarker, kMaxSegmentSize);
  }

  if (options.retain_exif_data) {
    jpeg_save_markers(jpeg_decompress, kExifDataMarker, kMaxSegmentSize);
  }

  if (options.preserve_c2pa) {
    // Ask libjpeg to retain APP11 segments so a C2PA / Content Credentials
    // provenance manifest survives recompression.
    jpeg_save_markers(jpeg_decompress, kC2paMarker, kMaxSegmentSize);
  }

  // Read jpeg data into the decompression struct.
  jpeg_read_header(jpeg_decompress, TRUE);

  // EXIF orientation (issue #1005): production strips metadata, which would
  // drop a portrait image's Orientation tag while leaving its pixels stored
  // rotated. The lossy path bakes the orientation into the pixels; paths
  // that cannot bake emit an accurate minimal tag instead. When the caller
  // retains EXIF the original tag survives verbatim and the pixels are left
  // untouched, so the output is consistent either way.
  const uint8_t exif_orientation =
      options.retain_exif_data
          ? 1
          : ReadJpegExifOrientation(original.data(), original.size());

  bool valid_jpeg = false;
  if (options.lossy) {
    valid_jpeg =
        OptimizeLossy(jpeg_decompress, compressed, options, exif_orientation);
  } else {
    valid_jpeg = OptimizeLossless(jpeg_decompress, compressed, options,
                                  exif_orientation);
  }

  // Finish the compression process.
  jpeg_finish_compress(&jpeg_compress_);
  jpeg_finish_decompress(jpeg_decompress);

  return valid_jpeg;
}

bool JpegOptimizer::CreateOptimizedJpeg(const std::string& original,
                                        std::string* compressed,
                                        const JpegCompressionOptions& options) {
  if (!reader_.is_valid()) {
    return false;
  }
  jpeg_decompress_struct* jpeg_decompress = reader_.decompress_struct();

  bool result =
      DoCreateOptimizedJpeg(original, jpeg_decompress, compressed, options);

  jpeg_decompress->client_data = nullptr;
  jpeg_compress_.client_data = nullptr;

  if (!result) {
    // Clean up the state of jpeglib structures. It is okay to
    // abort even if no (de)compression is in progress. This is
    // crucial because we enter this block even if no jpeg-related
    // error happened.
    jpeg_abort_decompress(jpeg_decompress);
    jpeg_abort_compress(&jpeg_compress_);
  }

  return result;
}

}  // namespace

namespace pagespeed::image_compression {

using ::net_instaweb::SCANLINE_JPEGWRITER;
using ::net_instaweb::SCANLINE_STATUS_INTERNAL_ERROR;
using ::net_instaweb::SCANLINE_STATUS_INVOCATION_ERROR;
using ::net_instaweb::SCANLINE_STATUS_SUCCESS;
using ::net_instaweb::SCANLINE_STATUS_UNSUPPORTED_FEATURE;
using ::net_instaweb::ScanlineStatus;

JpegCompressionOptions::~JpegCompressionOptions() = default;

struct JpegScanlineWriter::Data {
  Data() { InitJpegCompress(&jpeg_compress_, &compress_error_); }

  ~Data() { jpeg_destroy_compress(&jpeg_compress_); }

  // Structures for jpeg compression.
  jpeg_compress_struct jpeg_compress_;
  jpeg_error_mgr compress_error_;
};

JpegScanlineWriter::JpegScanlineWriter(MessageHandler* handler)
    : data_(new Data()), message_handler_(handler) {}

JpegScanlineWriter::~JpegScanlineWriter() { delete data_; }

void JpegScanlineWriter::SetJmpBufEnv(jmp_buf* env) {
  data_->jpeg_compress_.client_data = static_cast<void*>(env);
}

ScanlineStatus JpegScanlineWriter::InitWithStatus(const size_t width,
                                                  const size_t height,
                                                  PixelFormat pixel_format) {
  data_->jpeg_compress_.image_width = width;
  data_->jpeg_compress_.image_height = height;

  switch (pixel_format) {
    case RGB_888:
      data_->jpeg_compress_.input_components = 3;
      data_->jpeg_compress_.in_color_space = JCS_RGB;
      break;
    case GRAY_8:
      data_->jpeg_compress_.input_components = 1;
      data_->jpeg_compress_.in_color_space = JCS_GRAYSCALE;
      break;
    case RGBA_8888:
      return PS_LOGGED_STATUS(PS_DLOG_INFO, message_handler_,
                              SCANLINE_STATUS_UNSUPPORTED_FEATURE,
                              SCANLINE_JPEGWRITER, "transparency");
      break;
    default:
      return PS_LOGGED_STATUS(PS_LOG_DFATAL, message_handler_,
                              SCANLINE_STATUS_INTERNAL_ERROR,
                              SCANLINE_JPEGWRITER, "unknown pixel format: %s",
                              GetPixelFormatString(pixel_format));
  }

  if (!JpegProtectedCall(data_->jpeg_compress_, [&] {
        jpeg_set_defaults(&data_->jpeg_compress_);
        data_->jpeg_compress_.optimize_coding = TRUE;
      })) {
    return PS_LOGGED_STATUS(PS_LOG_ERROR, message_handler_,
                            SCANLINE_STATUS_INTERNAL_ERROR, SCANLINE_JPEGWRITER,
                            "libjpeg error in InitWithStatus");
  }
  return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
}

void JpegScanlineWriter::SetJpegCompressParams(
    const JpegCompressionOptions& options) {
  if (!options.lossy) {
    PS_LOG_DFATAL(message_handler_,
                  "Unable to perform lossless encoding in "
                  "JpegScanlineWriter."
                  " Using jpeg default lossy encoding options.");
  }
  SetJpegCompressBeforeStartCompress(options, nullptr, &data_->jpeg_compress_);
}

ScanlineStatus JpegScanlineWriter::InitializeWriteWithStatus(
    const void* const params, std::string* const compressed) {
  if (params == nullptr) {
    return PS_LOGGED_STATUS(
        PS_LOG_DFATAL, message_handler_, SCANLINE_STATUS_INVOCATION_ERROR,
        SCANLINE_JPEGWRITER, "missing JpegCompressionOptions*");
  }

  if (!JpegProtectedCall(data_->jpeg_compress_, [&] {
        const auto* jpeg_compression_options =
            static_cast<const JpegCompressionOptions*>(params);
        SetJpegCompressParams(*jpeg_compression_options);
        JpegStringWriter(&data_->jpeg_compress_, compressed);
        jpeg_start_compress(&data_->jpeg_compress_, TRUE);
      })) {
    return PS_LOGGED_STATUS(PS_LOG_ERROR, message_handler_,
                            SCANLINE_STATUS_INTERNAL_ERROR, SCANLINE_JPEGWRITER,
                            "libjpeg error in InitializeWriteWithStatus");
  }
  return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
}

ScanlineStatus JpegScanlineWriter::WriteNextScanlineWithStatus(
    const void* const scanline_bytes) {
  unsigned int result = 0;
  if (!JpegProtectedCall(data_->jpeg_compress_, [&] {
        JSAMPROW row_pointer[1] = {
            static_cast<JSAMPLE*>(const_cast<void*>(scanline_bytes))};
        result = jpeg_write_scanlines(&data_->jpeg_compress_, row_pointer, 1);
      })) {
    return PS_LOGGED_STATUS(PS_LOG_ERROR, message_handler_,
                            SCANLINE_STATUS_INTERNAL_ERROR, SCANLINE_JPEGWRITER,
                            "libjpeg error in WriteNextScanlineWithStatus");
  }
  if (result == 1) {
    return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
  } else {
    return PS_LOGGED_STATUS(PS_LOG_ERROR, message_handler_,
                            SCANLINE_STATUS_INTERNAL_ERROR, SCANLINE_JPEGWRITER,
                            "jpeg_write_scanlines()");
  }
}

ScanlineStatus JpegScanlineWriter::FinalizeWriteWithStatus() {
  if (!JpegProtectedCall(data_->jpeg_compress_, [&] {
        jpeg_finish_compress(&data_->jpeg_compress_);
      })) {
    return PS_LOGGED_STATUS(PS_LOG_ERROR, message_handler_,
                            SCANLINE_STATUS_INTERNAL_ERROR, SCANLINE_JPEGWRITER,
                            "libjpeg error in FinalizeWriteWithStatus");
  }
  return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
}

void JpegScanlineWriter::AbortWrite() {
  data_->jpeg_compress_.client_data = nullptr;
  jpeg_abort_compress(&data_->jpeg_compress_);
}

bool OptimizeJpeg(const std::string& original, std::string* compressed,
                  MessageHandler* handler) {
  JpegOptimizer optimizer(handler);
  JpegCompressionOptions options;
  return optimizer.CreateOptimizedJpeg(original, compressed, options);
}

bool OptimizeJpegWithOptions(const std::string& original,
                             std::string* compressed,
                             const JpegCompressionOptions& options,
                             MessageHandler* handler) {
  JpegOptimizer optimizer(handler);
  return optimizer.CreateOptimizedJpeg(original, compressed, options);
}

}  // namespace pagespeed::image_compression
