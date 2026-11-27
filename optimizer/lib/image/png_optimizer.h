// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// This file is derived from mod_pagespeed and has been substantially modified.
// Originally licensed under Apache License, Version 2.0.
// Copyright (c) 2010-2017 Google Inc.
// Copyright (c) 2018 The Apache Software Foundation.

// PNG codec: reading, writing, and basic optimization of PNG images.
// Ported from mod_pagespeed's pagespeed/kernel/image/png_optimizer.h
//
// Modernization changes:
// - GoogleString -> std::string
// - StringPiece -> std::string_view
// - net_instaweb namespace -> pagespeed namespace (for new types)
// - DISALLOW_COPY_AND_ASSIGN -> = delete
// - scoped_ptr -> std::unique_ptr
// - scoped_array -> std::unique_ptr<T[]>
// - Uses optipng's opngreduc for lossless PNG reduction

#ifndef PAGESPEED_LIB_IMAGE_PNG_OPTIMIZER_H_
#define PAGESPEED_LIB_IMAGE_PNG_OPTIMIZER_H_

// Note: we should not include setjmp.h here, since libpng 1.2 headers
// include it themselves, and get unhappy if we do it ourselves.

extern "C" {
#include "png.h"  // NOLINT
}  // extern "C"

#include <setjmp.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "lib/base/basictypes.h"
#include "lib/image/image_util.h"
#include "lib/image/scanline_interface.h"
#include "lib/image/scanline_status.h"
#include "lib/image/scanline_utils.h"

namespace pagespeed {
class MessageHandler;
}  // namespace pagespeed

namespace pagespeed {

namespace image_compression {

// Wraps a libpng operation in a setjmp(png_jmpbuf()) guard. Returns true if
// the operation completed without triggering longjmp (i.e. no libpng error).
//
// SAFETY: longjmp unwinds the stack without calling destructors. The lambda
// body must contain only trivially-destructible locals (POD, raw pointers,
// C arrays). Using std::string, std::vector, or any RAII object inside the
// lambda is undefined behavior if libpng triggers an error.
template <typename Func>
bool PngProtectedCall(png_structp png_ptr, Func&& operation) {
  if (setjmp(png_jmpbuf(png_ptr))) {
    return false;
  }
  operation();
  return true;
}

using net_instaweb::SCANLINE_PNGREADER;
using net_instaweb::SCANLINE_PNGREADERRAW;
using net_instaweb::SCANLINE_PNGWRITER;
using net_instaweb::SCANLINE_STATUS_INTERNAL_ERROR;
using net_instaweb::SCANLINE_STATUS_INVOCATION_ERROR;
using net_instaweb::SCANLINE_STATUS_MEMORY_ERROR;
using net_instaweb::SCANLINE_STATUS_SUCCESS;
using net_instaweb::SCANLINE_STATUS_UNSUPPORTED_FORMAT;
using net_instaweb::ScanlineStatus;

struct PngCompressParams : public ScanlineWriterConfig {
  PngCompressParams(int level, int strategy, bool is_progressive);
  PngCompressParams(bool try_best_compression, bool is_progressive);
  ~PngCompressParams() override;

  // Indicates what png filter type to be used while compressing the image.
  // Valid values for this are
  //   PNG_FILTER_NONE
  //   PNG_FILTER_SUB
  //   PNG_FILTER_UP
  //   PNG_FILTER_AVG
  //   PNG_FILTER_PAETH
  //   PNG_ALL_FILTERS
  int filter_level;
  // Indicates which compression strategy to use while compressing the image.
  // Valid values for this are
  //   Z_FILTERED
  //   Z_HUFFMAN_ONLY
  //   Z_RLE
  //   Z_FIXED
  //   Z_DEFAULT_STRATEGY
  int compression_strategy;
  // Indicates whether to search for the smallest output by using
  // multiple runs of compression. This mode will use more computation.
  bool try_best_compression;
  // Indicates whether to encode the image in progressive / interlacing
  // format.
  bool is_progressive;
};

// Helper that manages the lifetime of the png_ptr and info_ptr.
class ScopedPngStruct {
 public:
  enum Type : std::uint8_t { READ, WRITE };

  ScopedPngStruct(Type type, MessageHandler* handler);
  ~ScopedPngStruct();

  bool valid() const { return png_ptr_ != nullptr && info_ptr_ != nullptr; }

  // This will only return false as a result of a longjmp due to an
  // unhandled libpng error.
  bool reset();

  png_structp png_ptr() const { return png_ptr_; }
  png_infop info_ptr() const { return info_ptr_; }

 private:
  png_structp png_ptr_;
  png_infop info_ptr_;
  Type type_;
  MessageHandler* message_handler_;
};

// Helper class that provides an API to read a PNG image from some
// source.
class PngReaderInterface {
 public:
  PngReaderInterface();
  virtual ~PngReaderInterface();

  // Parse the contents of body, convert to a PNG, and populate the
  // PNG structures with the PNG representation. If 'require_opaque'
  // is true, returns an image without an alpha channel if the
  // original image has no transparent pixels, and fails
  // otherwise. Returns true on success, false on failure.
  virtual bool ReadPng(const std::string& body, png_structp png_ptr,
                       png_infop info_ptr, int transforms,
                       bool require_opaque) const = 0;

  // Parse the contents of body, convert to a PNG, and populate the
  // PNG structures with the PNG representation. Returns true on
  // success, false on failure.
  bool ReadPng(const std::string& body, png_structp png_ptr, png_infop info_ptr,
               int transforms) const {
    return ReadPng(body, png_ptr, info_ptr, transforms, false);
  }

  // Get just the attributes of the given image. out_bit_depth is the
  // number of bits per channel. out_color_type is one of the
  // PNG_COLOR_TYPE_* declared in png.h.
  virtual bool GetAttributes(const std::string& body, int* out_width,
                             int* out_height, int* out_bit_depth,
                             int* out_color_type) const = 0;

  // Get the background color, in the form of 8-bit RGB triplets. Note
  // that if the underlying image uses a bit_depth other than 8, the
  // background color will be scaled to 8-bits per channel.
  static bool GetBackgroundColor(png_structp png_ptr, png_infop info_ptr,
                                 unsigned char* red, unsigned char* green,
                                 unsigned char* blue, MessageHandler* handler);

  // Returns true if the alpha channel is actually opaque. Returns
  // false otherwise. It is an error to call this method for an image
  // that does not have an alpha channel.
  static bool IsAlphaChannelOpaque(png_structp png_ptr, png_infop info_ptr,
                                   MessageHandler* handler);

 private:
  DISALLOW_COPY_AND_ASSIGN(PngReaderInterface);
};

// Reader for PNG-encoded data.
// This is sample code on how someone can use the scanline reader
// interface.
// bool func() {
//   if (setjmp(*GetJmpBuf())) {
//     return false;
//   }
//
//   InitializeRead(...)
//   while (HasMoreScanlines()) {
//     Scanline line;
//     ReadNextScanline(line);
//     ....
//     ....
//   }
// }
class PngScanlineReader : public net_instaweb::ScanlineReaderInterface {
 public:
  explicit PngScanlineReader(MessageHandler* handler);
  ~PngScanlineReader() override;

  jmp_buf* GetJmpBuf();

  // This will only return false as a result of a longjmp due to an
  // unhandled libpng error.
  bool Reset() override;

  // Initializes the read structures with the given input.
  bool InitializeRead(const PngReaderInterface& reader, const std::string& in);
  bool InitializeRead(const PngReaderInterface& reader, const std::string& in,
                      bool* is_opaque);

  size_t GetBytesPerScanline() override;
  bool HasMoreScanLines() override;
  ScanlineStatus ReadNextScanlineWithStatus(void** out_scanline_bytes) override;
  size_t GetImageHeight() override;
  size_t GetImageWidth() override;
  PixelFormat GetPixelFormat() override;
  bool IsProgressive() override;

  void set_transform(int transform);
  void set_require_opaque(bool require_opaque);
  int GetColorType();
  bool GetBackgroundColor(unsigned char* red, unsigned char* green,
                          unsigned char* blue);

  // This is a no-op and should not be called.
  ScanlineStatus InitializeWithStatus(const void* image_buffer,
                                      size_t buffer_length) override;

 private:
  ScopedPngStruct read_;
  size_t current_scanline_;
  int transform_;
  bool require_opaque_;
  MessageHandler* message_handler_;

  DISALLOW_COPY_AND_ASSIGN(PngScanlineReader);
};

class PngOptimizer {
 public:
  static bool OptimizePng(const PngReaderInterface& reader,
                          const std::string& in, std::string* out,
                          MessageHandler* handler);

  static bool OptimizePngBestCompression(const PngReaderInterface& reader,
                                         const std::string& in,
                                         std::string* out,
                                         MessageHandler* handler);

  static bool CopyPngStructs(const ScopedPngStruct& from, ScopedPngStruct* to);

 private:
  explicit PngOptimizer(MessageHandler* handler);
  ~PngOptimizer();

  // Take the given input and losslessly compress it by removing
  // all unnecessary chunks, and by choosing an optimal PNG encoding.
  // @return true on success, false on failure.
  bool CreateOptimizedPng(const PngReaderInterface& reader,
                          const std::string& in, std::string* out,
                          MessageHandler* handler);

  // Turn on best compression. Requires additional CPU but produces
  // smaller files.
  void EnableBestCompression() { best_compression_ = true; }

  bool WritePng(ScopedPngStruct* write, std::string* buffer);
  bool CopyReadToWrite();
  bool CreateBestOptimizedPngForParams(const PngCompressParams* param_list,
                                       size_t param_list_size,
                                       std::string* out);
  bool CreateOptimizedPngWithParams(ScopedPngStruct* write,
                                    const PngCompressParams& params,
                                    std::string* out);
  ScopedPngStruct read_;
  ScopedPngStruct write_;
  bool best_compression_;
  MessageHandler* message_handler_;

  DISALLOW_COPY_AND_ASSIGN(PngOptimizer);
};

// Reader for PNG-encoded data.
class PngReader : public PngReaderInterface {
 public:
  explicit PngReader(MessageHandler* handler);
  ~PngReader() override;
  bool ReadPng(const std::string& body, png_structp png_ptr, png_infop info_ptr,
               int transforms, bool require_opaque) const override;

  bool GetAttributes(const std::string& body, int* out_width, int* out_height,
                     int* out_bit_depth, int* out_color_type) const override;

 private:
  MessageHandler* message_handler_;
  DISALLOW_COPY_AND_ASSIGN(PngReader);
};

// Class PngScanlineReaderRaw decodes PNG images and outputs the raw
// pixel data, image size, pixel type, etc. The class accepts all
// formats supported by libpng. The output is Gray_8, RGB_888, or
// RGBA_8888. The following transformations are used:
//   - Image with depth other than 8 bits/pixel is expanded or stripped
//     to 8 bits/pixel.
//   - Paletted image is converted to RGB or RGBA depending on whether
//     transparency is specified.
//   - Gray_Alpha is converted to RGBA.
//
// Note: The input image stream must be valid throughout the life of
//   the object. In other words, the image_buffer input you set to the
//   Initialize() method cannot be changed until your last call to the
//   ReadNextScanline() method.
//
class PngScanlineReaderRaw : public net_instaweb::ScanlineReaderInterface {
 public:
  explicit PngScanlineReaderRaw(MessageHandler* handler);
  ~PngScanlineReaderRaw() override;

  // This will only return false as a result of a longjmp due to an
  // unhandled libpng error.
  bool Reset() override;

  // Initialize the reader with the given image stream. Note that
  // image_buffer must remain unchanged until the last call to
  // ReadNextScanline().
  ScanlineStatus InitializeWithStatus(const void* image_buffer,
                                      size_t buffer_length) override;

  // Return the next row of pixels. For non-progressive PNG,
  // ReadNextScanlineWithStatus will decode one row of pixels each
  // time when it is called, but for progressive PNG,
  // ReadNextScanlineWithStatus will decode the entire image at the
  // first time when it is called.
  ScanlineStatus ReadNextScanlineWithStatus(void** out_scanline_bytes) override;

  // Return the number of bytes in a row (without padding).
  size_t GetBytesPerScanline() override { return bytes_per_row_; }

  bool HasMoreScanLines() override { return (row_ < height_); }
  PixelFormat GetPixelFormat() override { return pixel_format_; }
  size_t GetImageHeight() override { return height_; }
  size_t GetImageWidth() override { return width_; }
  bool IsProgressive() override { return is_progressive_; }

  // Maximum total decoded image size (in bytes) this reader will allocate.
  // Mirrors the pipeline's downstream decoded-size cap and bounds the up-front
  // allocation an interlaced PNG triggers (it buffers the whole image), so a
  // malicious image within libpng's own (1M x 1M) dimension limits cannot
  // drive a multi-terabyte allocation.
  static constexpr size_t kMaxDecodedImageBytes = 50 * 1024 * 1024;

  // Validates that an image of the given dimensions can be decoded without
  // integer overflow or exceeding kMaxDecodedImageBytes. Exposed for testing.
  // On success returns true and sets *bytes_per_row (= width*channels) and
  // *total_bytes (= width*height*channels, the interlaced buffer size);
  // otherwise returns false and leaves the outputs unchanged.
  static bool ValidateDecodedSize(size_t width, size_t height, size_t channels,
                                  size_t* bytes_per_row, size_t* total_bytes);

 private:
  PixelFormat pixel_format_;
  bool is_progressive_;
  size_t height_;
  size_t width_;
  size_t bytes_per_row_;
  size_t row_;
  bool was_initialized_;
  std::unique_ptr<png_byte[]> image_buffer_;
  std::unique_ptr<png_bytep[]> row_pointers_;
  std::unique_ptr<ScopedPngStruct> png_struct_;
  // png_input_ stores a pointer to the input image stream. It also
  // keeps tracking the length of data that libpng has read. It is
  // initialized in Initialize() and is updated in ReadNextScanline().
  std::unique_ptr<ScanlineStreamInput> png_input_;
  MessageHandler* message_handler_;

  DISALLOW_COPY_AND_ASSIGN(PngScanlineReaderRaw);
};

// Class PngScanlineWriter writes a PNG image. It supports Gray_8,
// RGB_888, and RGBA_8888 formats.
class PngScanlineWriter : public net_instaweb::ScanlineWriterInterface {
 public:
  explicit PngScanlineWriter(MessageHandler* handler);
  ~PngScanlineWriter() override;

  // Initialize the basic parameters for writing the image. Size of the
  // image must be 1-by-1 or larger.
  ScanlineStatus InitWithStatus(size_t width, size_t height,
                                PixelFormat pixel_format) override;

  // Initialize additional parameters for writing the image using
  // 'params', which should be a PngCompressParams*. You can set
  // 'params' to NULL to use the default compression configuration.
  ScanlineStatus InitializeWriteWithStatus(const void* params,
                                           std::string* png_image) override;

  // Write a scanline with the data provided. Return false in case of
  // error.
  ScanlineStatus WriteNextScanlineWithStatus(
      const void* scanline_bytes) override;

  // Finalize write structure once all scanlines are written.
  // If FinalizeWriter() is called before all of the scanlines have
  // been written, the object will be reset to the initial state.
  ScanlineStatus FinalizeWriteWithStatus() override;

 private:
  // Reset the object to the usable state.
  bool Reset();

  // Validate the input parameters.
  bool Validate(const PngCompressParams* params, std::string* png_image);

  bool DoBestCompression();

 private:
  size_t width_;
  size_t height_;
  size_t bytes_per_row_;
  size_t row_;
  PixelFormat pixel_format_;
  std::unique_ptr<ScopedPngStruct> png_struct_;
  bool was_initialized_;
  bool try_best_compression_;
  std::unique_ptr<unsigned char[]> pixel_buffer_;
  MessageHandler* message_handler_;

  DISALLOW_COPY_AND_ASSIGN(PngScanlineWriter);
};

}  // namespace image_compression

}  // namespace pagespeed

#endif  // PAGESPEED_LIB_IMAGE_PNG_OPTIMIZER_H_
