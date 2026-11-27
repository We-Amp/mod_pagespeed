// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// This file is derived from mod_pagespeed and has been substantially modified.
// Originally licensed under Apache License, Version 2.0.
// Copyright (c) 2010-2017 Google Inc.
// Copyright (c) 2018 The Apache Software Foundation.

// JPEG optimization and scanline writing.
// Ported from mod_pagespeed's pagespeed/kernel/image/jpeg_optimizer.h

#ifndef PAGESPEED_LIB_IMAGE_JPEG_OPTIMIZER_H_
#define PAGESPEED_LIB_IMAGE_JPEG_OPTIMIZER_H_

#include <setjmp.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "lib/base/basictypes.h"
#include "lib/image/image_util.h"
#include "lib/image/scanline_interface.h"
#include "lib/image/scanline_status.h"

// DO NOT INCLUDE LIBJPEG HEADERS HERE. Doing so causes build errors
// on Windows.

namespace pagespeed {
class MessageHandler;
}

namespace pagespeed {

namespace image_compression {

using ::net_instaweb::ScanlineStatus;

enum ColorSampling : std::uint8_t { RETAIN, YUV420, YUV422, YUV444 };

struct JpegLossyOptions {
  JpegLossyOptions() : quality(85), num_scans(-1), color_sampling(YUV420) {}
  // jpeg_quality - Can take values in the range [1,100].
  // For web images, the preferred value for quality is 85.
  // For smaller images like thumbnails, the preferred value for
  // quality is 75.
  // Setting it to values below 50 is generally not preferable.
  int quality;

  // No. of progressive scan that needs to be included in the final
  // output. -1 indicates to use all scans that are present.
  int num_scans;

  // Color sampling that needs to be used while recompressing the
  // image.
  ColorSampling color_sampling;
};

struct JpegCompressionOptions : public ScanlineWriterConfig {
  JpegCompressionOptions()
      : progressive(false),
        retain_color_profile(false),
        retain_exif_data(false),
        preserve_c2pa(true),
        lossy(false) {}

  ~JpegCompressionOptions() override;

  // Whether or not to produce a progressive JPEG. This parameter
  // will only be applied for images with YCbCr colorspace, and it
  // is ignored for other colorspaces.
  bool progressive;

  // If set to 'true' any color profile information is retained.
  bool retain_color_profile;

  // If set to 'true' any exif information is retained.
  bool retain_exif_data;

  // If set to 'true' (the default) any C2PA / Content Credentials provenance
  // manifest (carried in APP11 / JUMBF segments) is preserved through
  // recompression. Independent of retain_exif_data: stripping EXIF for size
  // does not drop provenance.
  bool preserve_c2pa;

  // Whether or not to use lossy compression.
  bool lossy;

  // Lossy compression options. Only applicable if lossy (above) is
  // set to true.
  JpegLossyOptions lossy_options;
};

// Performs lossless optimization, that is, the output image will be
// pixel-for-pixel identical to the input image.
bool OptimizeJpeg(const std::string& original, std::string* compressed,
                  MessageHandler* handler);

// Performs JPEG optimizations with the provided options.
bool OptimizeJpegWithOptions(const std::string& original,
                             std::string* compressed,
                             const JpegCompressionOptions& options,
                             MessageHandler* handler);

// Errors raised by libjpeg are reported through a failed ScanlineStatus
// from the method that triggered them; each method guards its own libjpeg
// calls with a jump buffer that lives for the duration of the call. A
// caller that wants the longjmp to land in its own frame may still install
// one with SetJmpBufEnv.
//
// User of this class must call this functions in the following
// sequence:
// func () {
//   JpegScanlineWriter jpeg_writer;
//   if (jpeg_writer.Init(width, height, format)) {

//     jpeg_writer.SetJpegCompressParams(quality);
//     jpeg_writer.InitializeWrite(out);
//     while(has_lines_to_write) {
//       writer.WriteNextScanline(next_scan_line);
//     }
//     writer.FinalizeWrite()
//   }
// }
class JpegScanlineWriter : public ::net_instaweb::ScanlineWriterInterface {
 public:
  explicit JpegScanlineWriter(MessageHandler* handler);
  ~JpegScanlineWriter() override;

  // Set the environment for longjmp calls. The buffer must outlive every
  // call on this writer. If no buffer is installed, each method guards its
  // own libjpeg calls and reports a failure as a ScanlineStatus.
  void SetJmpBufEnv(jmp_buf* env);

  // This function is only called when jpeg library call longjmp for
  // cleaning up the jpeg structs.
  void AbortWrite();

  ScanlineStatus InitWithStatus(size_t width, size_t height,
                                PixelFormat pixel_format) override;
  // Set the compression options via 'params', which should be a
  // JpegCompressionOptions*. Since writer only supports lossy
  // encoding, it is an error to pass in a 'params' that has the
  // lossy field set to false.
  ScanlineStatus InitializeWriteWithStatus(const void* params,
                                           std::string* compressed) override;
  ScanlineStatus WriteNextScanlineWithStatus(
      const void* scanline_bytes) override;
  ScanlineStatus FinalizeWriteWithStatus() override;

 private:
  // Since writer only supports lossy encoding, it is an error to
  // pass in a compression options that has lossy field set to false.
  void SetJpegCompressParams(const JpegCompressionOptions& options);

  // Opaque struct that is defined in the cc file and contains our
  // JPEG-compressor-specific structures.
  struct Data;
  Data* const data_;
  MessageHandler* message_handler_;

  DISALLOW_COPY_AND_ASSIGN(JpegScanlineWriter);
};

}  // namespace image_compression

}  // namespace pagespeed

#endif  // PAGESPEED_LIB_IMAGE_JPEG_OPTIMIZER_H_
