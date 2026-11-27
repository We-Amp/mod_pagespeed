// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// This file is derived from mod_pagespeed and has been substantially modified.
// Originally licensed under Apache License, Version 2.0.
// Copyright (c) 2010-2017 Google Inc.
// Copyright (c) 2018 The Apache Software Foundation.

// GIF codec implementation.
// Ported from mod_pagespeed's pagespeed/kernel/image/gif_reader.cc
//
// Modernization changes:
// - GoogleString -> std::string
// - net_instaweb namespace -> pagespeed namespace
// - scoped_array -> std::unique_ptr<T[]>
// - DCHECK -> assert
// - DVLOG removed (debug-only logging)
// - LOG_IF(ERROR, ...) -> PS_LOG_INFO

#include "lib/image/gif_reader.h"

#include <cassert>
#include <csetjmp>
#include <cstddef>
#include <cstring>
#include <memory>

#include "lib/base/basictypes.h"
#include "lib/base/message_handler.h"
#include "lib/image/image_frame_interface.h"
#include "lib/image/image_util.h"
#include "lib/image/scanline_utils.h"

extern "C" {
#include "gif_lib.h"
#include "png.h"
}

#if GIFLIB_MAJOR < 5 || (GIFLIB_MAJOR == 5 && GIFLIB_MINOR == 0)
#define DGifCloseFile(a, b) DGifCloseFile(a)
#endif

using pagespeed::image_compression::kScanlineMaxDimension;
using pagespeed::image_compression::kScanlineMaxImagePixels;
using pagespeed::image_compression::PngProtectedCall;
using pagespeed::image_compression::ScopedPngStruct;

namespace {

// GIF interlace tables.
static const int kInterlaceOffsets[] = {0, 4, 2, 1};
static const int kInterlaceJumps[] = {8, 8, 4, 2};
const int kInterlaceNumPass = arraysize(kInterlaceOffsets);
const int kNumColorForUint8 = 256;
const int kGifPaletteSize = 256;

// Maximum number of frames allowed in a GIF to prevent resource exhaustion
// from malicious or pathological files. 10,000 frames is far beyond any
// legitimate animated GIF (typical: 10-500 frames).
constexpr size_t kMaxGifFrames = 10000;

// Flag used to indicate that a gif extension contains transparency
// information.
static const unsigned char kTransparentFlag = 0x01;

int ReadGifFromStream(GifFileType* gif_file, GifByteType* data, int length) {
  auto* input = static_cast<pagespeed::image_compression::ScanlineStreamInput*>(
      gif_file->UserData);
  if (input->offset() + length <= input->length()) {
    memcpy(data, input->data() + input->offset(), length);
    input->set_offset(input->offset() + length);
    return length;
  } else {
    PS_LOG_INFO(input->message_handler(), "Unexpected EOF.");
    return 0;
  }
}

bool AddTransparencyChunk(png_structp png_ptr, png_infop info_ptr,
                          int transparent_palette_index,
                          pagespeed::MessageHandler* handler) {
  const int num_trans = transparent_palette_index + 1;
  png_colorp palette;
  int num_palette = 0;
  png_get_PLTE(png_ptr, info_ptr, &palette, &num_palette);
  if (num_trans <= 0 || num_trans > num_palette) {
    PS_LOG_INFO(handler, "Transparent palette index out of bounds.");
    return false;
  }

  return PngProtectedCall(png_ptr, [&] {
    png_byte trans[kNumColorForUint8];
    // First, set all palette indices to fully opaque.
    memset(trans, 0xff, num_trans);
    // Set the one transparent index to fully transparent.
    trans[transparent_palette_index] =
        pagespeed::image_compression::kAlphaTransparent;
    png_set_tRNS(png_ptr, info_ptr, trans, num_trans, nullptr);
  });
}

bool ReadImageDescriptor(GifFileType* gif_file, png_structp png_ptr,
                         png_infop info_ptr, png_color* palette,
                         pagespeed::MessageHandler* handler) {
  if (DGifGetImageDesc(gif_file) == GIF_ERROR) {
    PS_DLOG_INFO(handler, "Failed to get image descriptor.");
    return false;
  }
  if (gif_file->ImageCount != 1) {
    PS_DLOG_INFO(handler, "Unable to optimize image with %d frames.",
                 gif_file->ImageCount);
    return false;
  }
  const GifWord row = gif_file->Image.Top;
  const GifWord pixel = gif_file->Image.Left;
  const GifWord width = gif_file->Image.Width;
  const GifWord height = gif_file->Image.Height;

  // Validate that frame coordinates are non-negative and within PNG image
  // dimensions. GIF coordinates are signed (GifWord = int); a malformed GIF
  // could provide negative values or trigger signed overflow in the sum.
  // Cast each operand individually to unsigned to avoid overflow UB.
  const png_uint_32 png_width = png_get_image_width(png_ptr, info_ptr);
  const png_uint_32 png_height = png_get_image_height(png_ptr, info_ptr);
  if (pixel < 0 || row < 0 || width <= 0 || height <= 0 ||
      static_cast<png_uint_32>(pixel) + static_cast<png_uint_32>(width) >
          png_width ||
      static_cast<png_uint_32>(row) + static_cast<png_uint_32>(height) >
          png_height) {
    PS_DLOG_INFO(handler, "GIF frame coordinates out of range.");
    return false;
  }

  // Populate the color map.
  ColorMapObject* color_map = gif_file->Image.ColorMap != nullptr
                                  ? gif_file->Image.ColorMap
                                  : gif_file->SColorMap;

  if (color_map == nullptr) {
    PS_DLOG_INFO(handler, "Failed to find color map.");
    return false;
  }

  if (color_map->ColorCount < 0 || color_map->ColorCount > kNumColorForUint8) {
    PS_DLOG_INFO(handler, "Invalid color count %d", color_map->ColorCount);
    return false;
  }
  for (int i = 0; i < color_map->ColorCount; ++i) {
    palette[i].red = color_map->Colors[i].Red;
    palette[i].green = color_map->Colors[i].Green;
    palette[i].blue = color_map->Colors[i].Blue;
  }
  if (!PngProtectedCall(png_ptr, [&] {
        png_set_PLTE(png_ptr, info_ptr, palette, color_map->ColorCount);
      })) {
    return false;
  }

  png_bytepp rows = png_get_rows(png_ptr, info_ptr);
  if (rows == nullptr) {
    PS_DLOG_INFO(handler, "PNG row buffer not allocated.");
    return false;
  }

  if (gif_file->Image.Interlace == 0) {
    // Not interlaced. Read each line into the PNG buffer.
    for (GifWord i = 0; i < height; ++i) {
      if (DGifGetLine(gif_file,
                      static_cast<GifPixelType*>(&rows[row + i][pixel]),
                      width) == GIF_ERROR) {
        PS_DLOG_INFO(handler, "Failed to DGifGetLine");
        return false;
      }
    }
  } else {
    // Need to deinterlace. The deinterlace code is based on algorithm
    // in giflib.
    for (int i = 0; i < kInterlaceNumPass; ++i) {
      for (int j = row + kInterlaceOffsets[i]; j < row + height;
           j += kInterlaceJumps[i]) {
        if (DGifGetLine(gif_file, static_cast<GifPixelType*>(&rows[j][pixel]),
                        width) == GIF_ERROR) {
          PS_DLOG_INFO(handler, "Failed to DGifGetLine");
          return false;
        }
      }
    }
  }

  png_set_rows(png_ptr, info_ptr, rows);  // Marks iDAT as valid.
  return true;
}

// Read a GIF extension. There are various extensions. The only one we
// care about is the transparency extension, so we ignore all other
// extensions.
bool ReadExtension(GifFileType* gif_file, png_structp /*png_ptr*/,
                   png_infop /*info_ptr*/, int* out_transparent_index,
                   pagespeed::MessageHandler* handler) {
  GifByteType* extension = nullptr;
  int ext_code = 0;
  if (DGifGetExtension(gif_file, &ext_code, &extension) == GIF_ERROR) {
    PS_DLOG_INFO(handler, "Failed to read extension.");
    return false;
  }

  // We only care about one extension type, the graphics extension,
  // which can contain transparency information.
  if (ext_code == GRAPHICS_EXT_FUNC_CODE) {
    // A zero-length sub-block yields a null extension pointer.
    if (extension == nullptr) {
      PS_DLOG_INFO(handler, "Received graphics extension with no data.");
      return false;
    }
    // Make sure that the extension has the expected length.
    if (extension[0] < 4) {
      PS_DLOG_INFO(handler,
                   "Received graphics extension with unexpected length.");
      return false;
    }
    // The first payload byte contains the flags. Check to see whether the
    // transparency flag is set.
    if ((extension[1] & kTransparentFlag) != 0) {
      if (*out_transparent_index >= 0) {
        // The transparent index has already been set. Ignore new
        // values.
        PS_DLOG_INFO(handler,
                     "Found multiple transparency entries. Using first entry.");
      } else {
        // We found a transparency entry. The transparent index is in
        // the 4th payload byte.
        *out_transparent_index = extension[4];
      }
    }
  }

  // Some extensions (i.e. the comment extension, the text extension)
  // allow multiple sub-blocks. However, the graphics extension can
  // contain only one sub-block (handled above). Since we only care
  // about the graphics extension, we can safely ignore all subsequent
  // blocks.
  while (extension != nullptr) {
    if (DGifGetExtensionNext(gif_file, &extension) == GIF_ERROR) {
      PS_DLOG_INFO(handler, "Failed to read next extension.");
      return false;
    }
  }

  return true;
}

png_uint_32 AllocatePngPixels(png_structp png_ptr, png_infop info_ptr) {
  // Like libpng's png_read_png, we free the row pointers unless they
  // weren't allocated by libpng, in which case we reuse them.
  png_uint_32 row_size = png_get_rowbytes(png_ptr, info_ptr);
  if (row_size == 0) {
    return 0;
  }

  png_free_data(png_ptr, info_ptr, PNG_FREE_ROWS, 0);
  if (png_get_rows(png_ptr, info_ptr) == nullptr) {
    png_uint_32 height = png_get_image_height(png_ptr, info_ptr);

    // Allocate the array of pointers to each row.
    const png_size_t row_pointers_size =
        static_cast<size_t>(height) * sizeof(png_bytep);
    png_bytepp row_pointers =
        static_cast<png_bytepp>(png_malloc(png_ptr, row_pointers_size));
    memset(static_cast<void*>(row_pointers), 0, row_pointers_size);
    png_set_rows(png_ptr, info_ptr, row_pointers);
    png_data_freer(png_ptr, info_ptr, PNG_DESTROY_WILL_FREE_DATA,
                   PNG_FREE_ROWS);

    // Allocate memory for each row.
    for (png_uint_32 row = 0; row < height; ++row) {
      row_pointers[row] = static_cast<png_bytep>(png_malloc(png_ptr, row_size));
    }
  }
  return row_size;
}

bool ExpandColorMap(png_structp paletted_png_ptr, png_infop paletted_info_ptr,
                    png_color* palette, int transparent_palette_index,
                    png_structp rgb_png_ptr, png_infop rgb_info_ptr) {
  png_uint_32 height =
      png_get_image_height(paletted_png_ptr, paletted_info_ptr);
  png_uint_32 width = png_get_image_width(paletted_png_ptr, paletted_info_ptr);
  bool have_alpha = (transparent_palette_index >= 0);
  if (setjmp(png_jmpbuf(rgb_png_ptr))) {
    return false;
  }
  png_set_IHDR(rgb_png_ptr, rgb_info_ptr, width, height,
               8,  // bit depth
               have_alpha ? PNG_COLOR_TYPE_RGB_ALPHA : PNG_COLOR_TYPE_RGB,
               PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_BASE,
               PNG_FILTER_TYPE_BASE);
  png_uint_32 row_size = AllocatePngPixels(rgb_png_ptr, rgb_info_ptr);
  png_byte bytes_per_pixel = have_alpha ? 4 : 3;

  if (row_size == 0) {
    return false;
  }

  png_bytepp rgb_row_pointers = png_get_rows(rgb_png_ptr, rgb_info_ptr);
  png_bytepp pal_row_pointers =
      png_get_rows(paletted_png_ptr, paletted_info_ptr);
  for (png_uint_32 row = 0; row < height; ++row) {
    png_bytep rgb_next_byte = rgb_row_pointers[row];
    if (have_alpha) {
      // Make the row opaque initially.
      memset(rgb_next_byte, 0xff, row_size);
    }
    for (png_uint_32 column = 0; column < width; ++column) {
      png_byte palette_entry = pal_row_pointers[row][column];
      if (have_alpha &&
          (palette_entry == static_cast<png_byte>(transparent_palette_index))) {
        // Transparent: clear RGBA bytes.
        memset(rgb_next_byte, 0x00, bytes_per_pixel);
      } else {
        // Opaque: Copy RGB, keeping A opaque from above
        memcpy(rgb_next_byte, &(palette[palette_entry]), 3);
      }
      rgb_next_byte += bytes_per_pixel;
    }
  }

  // Marks iDAT as valid.
  png_set_rows(rgb_png_ptr, rgb_info_ptr, rgb_row_pointers);
  return true;
}

bool ReadGifToPng(GifFileType* gif_file, png_structp png_ptr,
                  png_infop info_ptr, bool expand_colormap, bool strip_alpha,
                  bool require_opaque, pagespeed::MessageHandler* handler) {
  // The rows below are allocated from the logical-screen dimensions before
  // any frame data is looked at, so refuse a screen beyond the decode caps
  // up front (this also subsumes the old row-pointer overflow check).
  if (static_cast<uint32_t>(gif_file->SWidth) > kScanlineMaxDimension ||
      static_cast<uint32_t>(gif_file->SHeight) > kScanlineMaxDimension ||
      static_cast<uint64_t>(gif_file->SWidth) * gif_file->SHeight >
          kScanlineMaxImagePixels) {
    PS_DLOG_INFO(handler, "GIF image is too big to process.");
    return false;
  }

  // If expand_colormap is true, the color-indexed GIF_file is read
  // into paletted_png, and then transformed into a bona fide RGB(A)
  // PNG in png_ptr and info_ptr. If expand_colormap is false, we just
  // read the color-indexed GIF file directly into png_ptr and
  // info_ptr.
  std::unique_ptr<ScopedPngStruct> paletted_png;
  png_structp paletted_png_ptr = nullptr;
  png_infop paletted_info_ptr = nullptr;

  if (expand_colormap) {
    // We read the image into a separate struct before expanding the
    // colormap.
    paletted_png =
        std::make_unique<ScopedPngStruct>(ScopedPngStruct::READ, handler);
    if (!paletted_png->valid()) {
      PS_LOG_DFATAL(handler, "Invalid ScopedPngStruct r: %d",
                    paletted_png->valid());
      return false;
    }
    paletted_png_ptr = paletted_png->png_ptr();
    paletted_info_ptr = paletted_png->info_ptr();
  } else {
    // We read the image directly into the pointers that was passed in.
    paletted_png_ptr = png_ptr;
    paletted_info_ptr = info_ptr;
  }

  if (!PngProtectedCall(paletted_png_ptr, [&] {
        png_set_IHDR(paletted_png_ptr, paletted_info_ptr, gif_file->SWidth,
                     gif_file->SHeight, 8, PNG_COLOR_TYPE_PALETTE,
                     PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_BASE,
                     PNG_FILTER_TYPE_BASE);
      })) {
    return false;
  }

  png_uint_32 row_size = AllocatePngPixels(paletted_png_ptr, paletted_info_ptr);
  if (row_size == 0) {
    return false;
  }

  // Fill the rows with the background color.
  png_bytepp row_pointers = png_get_rows(paletted_png_ptr, paletted_info_ptr);
  memset(row_pointers[0], gif_file->SBackGroundColor, row_size);
  png_uint_32 height =
      png_get_image_height(paletted_png_ptr, paletted_info_ptr);
  for (png_uint_32 row = 1; row < height; ++row) {
    memcpy(row_pointers[row], row_pointers[0], row_size);
  }

  int transparent_palette_index = -1;
  bool found_terminator = false;
  png_color palette[kNumColorForUint8] = {};
  while (!found_terminator) {
    GifRecordType record_type = UNDEFINED_RECORD_TYPE;
    if (DGifGetRecordType(gif_file, &record_type) == GIF_ERROR) {
      PS_DLOG_INFO(handler, "Failed to read GifRecordType");
      return false;
    }
    switch (record_type) {
      case IMAGE_DESC_RECORD_TYPE:
        if (!ReadImageDescriptor(gif_file, paletted_png_ptr, paletted_info_ptr,
                                 palette, handler)) {
          return false;
        }
        break;

      case EXTENSION_RECORD_TYPE:
        if (!ReadExtension(gif_file, paletted_png_ptr, paletted_info_ptr,
                           &transparent_palette_index, handler)) {
          return false;
        }
        break;

      case TERMINATE_RECORD_TYPE:
        found_terminator = true;
        break;

      default:
        PS_DLOG_INFO(handler, "Found unexpected record type %d", record_type);
        return false;
    }
  }

  // Process transparency.
  if (transparent_palette_index >= 0) {
    if (require_opaque) {
      return false;
    }
    if (!strip_alpha && !expand_colormap) {
      if (!AddTransparencyChunk(paletted_png_ptr, paletted_info_ptr,
                                transparent_palette_index, handler)) {
        return false;
      }
    }
  }

  if (expand_colormap) {
    if (!ExpandColorMap(paletted_png_ptr, paletted_info_ptr, palette,
                        strip_alpha ? -1 : transparent_palette_index, png_ptr,
                        info_ptr)) {
      return false;
    }
  }
  return true;
}

}  // namespace

namespace pagespeed {

namespace image_compression {

using net_instaweb::FRAME_GIFREADER;
using net_instaweb::SCANLINE_STATUS_INTERNAL_ERROR;
using net_instaweb::SCANLINE_STATUS_INVOCATION_ERROR;
using net_instaweb::SCANLINE_STATUS_MEMORY_ERROR;
using net_instaweb::SCANLINE_STATUS_PARSE_ERROR;
using net_instaweb::SCANLINE_STATUS_SUCCESS;
using net_instaweb::SCANLINE_STATUS_UNSUPPORTED_FEATURE;
using net_instaweb::SCANLINE_STATUS_UNSUPPORTED_FORMAT;
using net_instaweb::ScanlineStatus;

const int GifFrameReader::kNoTransparentIndex = -1;

// Utility to skip over the extra subblocks in a GIF file extension so
// we leave the file in a state where we've finished processing this
// extension and are ready to begin parsing another.
ScanlineStatus SkipOverGifExtensionSubblocks(GifFileType* gif_file,
                                             GifByteType* extension,
                                             MessageHandler* message_handler) {
  while (extension != nullptr) {
    if (DGifGetExtensionNext(gif_file, &extension) == GIF_ERROR) {
      return PS_LOGGED_STATUS(PS_LOG_INFO, message_handler,
                              SCANLINE_STATUS_PARSE_ERROR, FRAME_GIFREADER,
                              "Failed to read next extension.");
    }
  }
  return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
}

// Utility to expand a color index from a palette.
void ExpandColorIndex(const ColorMapObject* colormap, const int color_index,
                      PixelRgbaChannels rgba) {
  if ((colormap != nullptr) && (colormap->Colors != nullptr) &&
      (color_index < colormap->ColorCount)) {
    const GifColorType color = colormap->Colors[color_index];
    rgba[RGBA_RED] = color.Red;
    rgba[RGBA_GREEN] = color.Green;
    rgba[RGBA_BLUE] = color.Blue;
    rgba[RGBA_ALPHA] = kAlphaOpaque;
  }
}

GifReader::GifReader(MessageHandler* handler) : message_handler_(handler) {}

GifReader::~GifReader() = default;

bool GifReader::ReadPng(const std::string& body, png_structp png_ptr,
                        png_infop info_ptr, int transforms,
                        bool require_opaque) const {
  int allowed_transforms =
      // These transforms are no-ops when reading a .gif file.
      PNG_TRANSFORM_STRIP_16 | PNG_TRANSFORM_GRAY_TO_RGB |
      // We implement this transform explicitly.
      PNG_TRANSFORM_EXPAND |
      // We implement this transform explicitly, regardless of require_opaque.
      PNG_TRANSFORM_STRIP_ALPHA;

  if ((transforms & ~allowed_transforms) != 0) {
    PS_LOG_DFATAL(message_handler_, "Unsupported transform %d", transforms);
    return false;
  }

  bool expand_colormap = ((transforms & PNG_TRANSFORM_EXPAND) != 0);
  bool strip_alpha = ((transforms & PNG_TRANSFORM_STRIP_ALPHA) != 0);

  ScanlineStreamInput input(message_handler_);
  input.Initialize(body);

  GifFileType* gif_file = DGifOpen(&input, ReadGifFromStream, nullptr);
  if (gif_file == nullptr) {
    return false;
  }

  bool result = ReadGifToPng(gif_file, png_ptr, info_ptr, expand_colormap,
                             strip_alpha, require_opaque, message_handler_);
  if (DGifCloseFile(gif_file, nullptr) == GIF_ERROR) {
    PS_DLOG_INFO(message_handler_, "Failed to close GIF.");
  }

  return result;
}

bool GifReader::GetAttributes(const std::string& body, int* out_width,
                              int* out_height, int* out_bit_depth,
                              int* out_color_type) const {
  // We need the length of the magic bytes (GIF_STAMP_LEN), plus 2
  // bytes for width, plus 2 bytes for height.
  const size_t kGifMinHeaderSize = GIF_STAMP_LEN + 2 + 2;
  if (body.size() < kGifMinHeaderSize) {
    return false;
  }

  // Make sure this looks like a GIF. Either GIF87a or GIF89a.
  if (strncmp(GIF_STAMP, body.data(), GIF_VERSION_POS) != 0) {
    return false;
  }
  const auto* body_data = reinterpret_cast<const unsigned char*>(body.data());
  const unsigned char* width_data = body_data + GIF_STAMP_LEN;
  const unsigned char* height_data = width_data + 2;

  *out_width = (static_cast<unsigned int>(width_data[1]) << 8) + width_data[0];
  *out_height =
      (static_cast<unsigned int>(height_data[1]) << 8) + height_data[0];

  // GIFs are always 8 bits per channel, paletted images.
  *out_bit_depth = 8;
  *out_color_type = PNG_COLOR_TYPE_PALETTE;
  return true;
}

class ScopedGifStruct {
 public:
  explicit ScopedGifStruct(MessageHandler* handler)
      : gif_file_(nullptr),
        message_handler_(handler),
        gif_input_(ScanlineStreamInput(handler)) {}

  ~ScopedGifStruct() {
    ScanlineStatus status;
    Reset(&status);
    if (!status.Success()) {
      PS_LOG_INFO(message_handler_, "ScopedGifStruct destructor error");
    }
  }

  bool Initialize(const void* image_buffer, size_t buffer_length,
                  ScanlineStatus* status) {
    if (Reset(status)) {
      gif_input_.Initialize(image_buffer, buffer_length);
      return InitializeGifFile(status);
    }
    return false;
  }

  bool Initialize(const std::string& image_string, ScanlineStatus* status) {
    if (Reset(status)) {
      gif_input_.Initialize(image_string);
      return InitializeGifFile(status);
    }
    return false;
  }

  bool Reset(ScanlineStatus* status) {
    if (gif_file_ != nullptr) {
      if (DGifCloseFile(gif_file_, nullptr) == GIF_ERROR) {
        *status = PS_LOGGED_STATUS(
            PS_LOG_INFO, message_handler_, SCANLINE_STATUS_INTERNAL_ERROR,
            FRAME_GIFREADER, "Failed to close GIF file.");
        return false;
      }
      gif_file_ = nullptr;
    }
    gif_input_.Reset();
    *status = ScanlineStatus(SCANLINE_STATUS_SUCCESS);
    return true;
  }

  GifFileType* gif_file() { return gif_file_; }
  size_t offset() { return gif_input_.offset(); }
  void set_offset(size_t val) { gif_input_.set_offset(val); }

 private:
  bool InitializeGifFile(ScanlineStatus* status) {
    gif_file_ = DGifOpen(&gif_input_, ReadGifFromStream, nullptr);

    if (gif_file_ == nullptr) {
      *status = PS_LOGGED_STATUS(PS_LOG_INFO, message_handler_,
                                 SCANLINE_STATUS_INTERNAL_ERROR,
                                 FRAME_GIFREADER, "Failed to open GIF file.");
      return false;
    }
    *status = ScanlineStatus(SCANLINE_STATUS_SUCCESS);
    return true;
  }

 private:
  GifFileType* gif_file_;
  MessageHandler* message_handler_;
  ScanlineStreamInput gif_input_;
};

FrameSpec::DisposalMethod GifDisposalToFrameSpecDisposal(int gif_disposal) {
  if ((gif_disposal > FrameSpec::DISPOSAL_UNKNOWN) &&
      (gif_disposal <= FrameSpec::DISPOSAL_RESTORE)) {
    return static_cast<FrameSpec::DisposalMethod>(gif_disposal);
  }
  return FrameSpec::DISPOSAL_NONE;
}

GifFrameReader::GifFrameReader(MessageHandler* handler)
    : MultipleFrameReader(handler) {
  Reset();
}

GifFrameReader::~GifFrameReader() = default;

ScanlineStatus GifFrameReader::Reset() {
  image_initialized_ = false;
  frame_initialized_ = false;

  image_spec_.Reset();
  frame_spec_.Reset();

  has_loop_count_ = false;
  next_frame_ = 0;

  next_row_ = 0;
  frame_transparent_index_ = kNoTransparentIndex;

  // Note that gif_struct and gif_palette_ are allocated once in the
  // first call to Initialize(), and re-used across Reset()s

  // Note that frame_buffer_ and frame_index_ are allocated each time
  // PrepareNextFrame() is called.

  ScanlineStatus status(SCANLINE_STATUS_SUCCESS);
  if (gif_struct_.get() != nullptr) {
    gif_struct_->Reset(&status);
  }

  frame_palette_size_ = 0;
  frame_eagerly_read_ = false;

  return status;
}

ScanlineStatus GifFrameReader::ProcessExtensionAffectingFrame() {
  static const int kGifGceExpectedSize = 4;
  static const int kGifGceDisposeMask = 0x07;
  static const int kGifGceDisposeShift = 2;
  static const int kGifGceTransparentMask = 0x01;

  static const int kGifGceSizeIndex = 0;
  static const int kGifGceFlagsIndex = 1;
  static const int kGifGceDelayLoIndex = 2;
  static const int kGifGceDelayHiIndex = 3;
  static const int kGifGceTransparentIndexIndex = 4;

  GifFileType* gif_file = gif_struct_->gif_file();

  GifByteType* extension = nullptr;
  int ext_code = 0;
  if (DGifGetExtension(gif_file, &ext_code, &extension) == GIF_ERROR) {
    return PS_LOGGED_STATUS(PS_LOG_INFO, message_handler(),
                            SCANLINE_STATUS_PARSE_ERROR, FRAME_GIFREADER,
                            "Failed to read extension.");
  }

  if (ext_code == GRAPHICS_EXT_FUNC_CODE) {
    if (extension == nullptr) {
      return PS_LOGGED_STATUS(PS_LOG_INFO, message_handler(),
                              SCANLINE_STATUS_PARSE_ERROR, FRAME_GIFREADER,
                              "NULL graphics control extension block.");
    }
    if (extension[kGifGceSizeIndex] != kGifGceExpectedSize) {
      return PS_LOGGED_STATUS(
          PS_LOG_INFO, message_handler(), SCANLINE_STATUS_PARSE_ERROR,
          FRAME_GIFREADER,
          "Received graphics extension with unexpected length.");
    }
    const int flags = extension[kGifGceFlagsIndex];
    const int dispose = (flags >> kGifGceDisposeShift) & kGifGceDisposeMask;
    const int delay =
        extension[kGifGceDelayLoIndex] | (extension[kGifGceDelayHiIndex] << 8);
    frame_spec_.duration_ms = static_cast<size_t>(delay) * 10;

    FrameSpec::DisposalMethod frame_dispose =
        GifDisposalToFrameSpecDisposal(dispose);
    if (frame_dispose != FrameSpec::DISPOSAL_UNKNOWN) {
      frame_spec_.disposal = frame_dispose;
    } else {
      if (image_spec_.num_frames == 1) {
        PS_LOG_INFO(message_handler(), "Unrecognized disposal method %d.",
                    dispose);
        frame_spec_.disposal = FrameSpec::DISPOSAL_NONE;
      } else {
        return PS_LOGGED_STATUS(PS_LOG_INFO, message_handler(),
                                SCANLINE_STATUS_PARSE_ERROR, FRAME_GIFREADER,
                                "Unrecognized disposal method %d.", dispose);
      }
    }

    frame_transparent_index_ = (((flags & kGifGceTransparentMask) != 0)
                                    ? extension[kGifGceTransparentIndexIndex]
                                    : kNoTransparentIndex);
  }

  return SkipOverGifExtensionSubblocks(gif_file, extension, message_handler());
}

ScanlineStatus GifFrameReader::ProcessExtensionAffectingImage(
    bool past_first_frame) {
  static const char* kGifAeIdentifier = "NETSCAPE2.0";
  static const int kGifAeIdentifierLength = strlen(kGifAeIdentifier);
  static const int kGifAeLoopCountExpectedLength = 3;
  static const int kGifAeLoopCountExpectedFixedConst = 1;

  static const int kGifAeIdentifierLengthIndex = 0;
  static const int kGifAeLoopCountLengthIndex = 0;
  static const int kGifAeLoopCountFixedConstIndex = 1;
  static const int kGifAeLoopCountLoIndex = 2;
  static const int kGifAeLoopCountHiIndex = 3;

  GifFileType* gif_file = gif_struct_->gif_file();
  GifByteType* extension = nullptr;
  int ext_code = 0;

  if (DGifGetExtension(gif_file, &ext_code, &extension) == GIF_ERROR) {
    return PS_LOGGED_STATUS(PS_LOG_INFO, message_handler(),
                            SCANLINE_STATUS_PARSE_ERROR, FRAME_GIFREADER,
                            "Failed to read extension.");
  }

  if (ext_code == APPLICATION_EXT_FUNC_CODE) {
    if (extension == nullptr) {
      return PS_LOGGED_STATUS(PS_LOG_INFO, message_handler(),
                              SCANLINE_STATUS_PARSE_ERROR, FRAME_GIFREADER,
                              "NULL Application Extension Block.");
    }
    if (extension[kGifAeIdentifierLengthIndex] != kGifAeIdentifierLength) {
      return PS_LOGGED_STATUS(
          PS_LOG_INFO, message_handler(), SCANLINE_STATUS_PARSE_ERROR,
          FRAME_GIFREADER,
          "Application extension block size has unexpected size.");
    }
    if (!memcmp(extension + 1, kGifAeIdentifier, kGifAeIdentifierLength)) {
      // Recognize and parse Netscape2.0 NAB extension for loop count.
      if (DGifGetExtensionNext(gif_file, &extension) == GIF_ERROR) {
        return PS_LOGGED_STATUS(
            PS_LOG_INFO, message_handler(), SCANLINE_STATUS_PARSE_ERROR,
            FRAME_GIFREADER,
            "DGifGetExtensionNext failed while trying to get loop count");
      }
      if (extension == nullptr) {
        return PS_LOGGED_STATUS(PS_LOG_INFO, message_handler(),
                                SCANLINE_STATUS_PARSE_ERROR, FRAME_GIFREADER,
                                "Missing loop count sub-block");
      }
      // Longer sub-blocks occur in practice and still carry the loop count
      // in their first three bytes.
      if ((extension[kGifAeLoopCountLengthIndex] <
           kGifAeLoopCountExpectedLength) ||
          (extension[kGifAeLoopCountFixedConstIndex] !=
           kGifAeLoopCountExpectedFixedConst)) {
        return PS_LOGGED_STATUS(PS_LOG_INFO, message_handler(),
                                SCANLINE_STATUS_PARSE_ERROR, FRAME_GIFREADER,
                                "animation loop count: wrong size/marker");
      }
      if (past_first_frame) {
        PS_LOG_INFO(message_handler(),
                    "Animation loop count in unexpected location.");
      }
      if (has_loop_count_) {
        PS_LOG_INFO(message_handler(),
                    "Multiple loop counts encountered. Using the last one.");
      }
      has_loop_count_ = true;
      image_spec_.loop_count = (extension[kGifAeLoopCountLoIndex] |
                                (extension[kGifAeLoopCountHiIndex] << 8));
    }
  }

  return SkipOverGifExtensionSubblocks(gif_file, extension, message_handler());
}

ScanlineStatus GifFrameReader::Initialize() {
  if (image_buffer_ == nullptr) {
    return PS_LOGGED_STATUS(PS_LOG_DFATAL, message_handler(),
                            SCANLINE_STATUS_INVOCATION_ERROR, FRAME_GIFREADER,
                            "null or empty image buffer.");
  }
  if (image_initialized_) {
    Reset();
  } else {
    if (gif_struct_ == nullptr) {
      gif_struct_ = std::make_unique<ScopedGifStruct>(message_handler());
      if (gif_struct_ == nullptr) {
        return PS_LOGGED_STATUS(PS_LOG_ERROR, message_handler(),
                                SCANLINE_STATUS_MEMORY_ERROR, FRAME_GIFREADER,
                                "Failed to allocate ScopedGifStruct.");
      }
    }
    if (gif_palette_ == nullptr) {
      gif_palette_.reset(new PaletteRGBA[kGifPaletteSize]);
      if (gif_palette_ == nullptr) {
        return PS_LOGGED_STATUS(PS_LOG_ERROR, message_handler(),
                                SCANLINE_STATUS_MEMORY_ERROR, FRAME_GIFREADER,
                                "Failed to allocate PaletteRGBA.");
      }
    }
  }

  ScanlineStatus status(SCANLINE_STATUS_SUCCESS);
  if (gif_struct_->Initialize(image_buffer_, buffer_length_, &status)) {
    status = GetImageData();
  }
  if (!status.Success()) {
    Reset();
    return status;
  }

  next_frame_ = 0;
  image_initialized_ = true;
  return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
}

ScanlineStatus GifFrameReader::GetImageData() {
  GifFileType* gif_file = gif_struct_->gif_file();
  size_t offset = gif_struct_->offset();
  image_spec_.width = gif_file->SWidth;
  image_spec_.height = gif_file->SHeight;

  // Buffers proportional to the screen area (the whole-image reader's rows,
  // the interlaced frame index) are driven by these dimensions, not by the
  // bytes actually present, so refuse a screen beyond the decode caps before
  // any frame is prepared.
  if (image_spec_.width > kScanlineMaxDimension ||
      image_spec_.height > kScanlineMaxDimension ||
      static_cast<uint64_t>(image_spec_.width) * image_spec_.height >
          kScanlineMaxImagePixels) {
    return PS_LOGGED_STATUS(PS_LOG_INFO, message_handler(),
                            SCANLINE_STATUS_UNSUPPORTED_FORMAT, FRAME_GIFREADER,
                            "Image dimensions %ux%u exceed the supported "
                            "maximum.",
                            static_cast<unsigned>(image_spec_.width),
                            static_cast<unsigned>(image_spec_.height));
  }

  GifRecordType record_type = UNDEFINED_RECORD_TYPE;
  while (record_type != TERMINATE_RECORD_TYPE) {
    if (DGifGetRecordType(gif_file, &record_type) == GIF_ERROR) {
      return PS_LOGGED_STATUS(PS_LOG_INFO, message_handler(),
                              SCANLINE_STATUS_PARSE_ERROR, FRAME_GIFREADER,
                              "DGifGetRecordType()");
    }
    switch (record_type) {
      case IMAGE_DESC_RECORD_TYPE:
        if (DGifGetImageDesc(gif_file) == GIF_ERROR) {
          return PS_LOGGED_STATUS(PS_LOG_INFO, message_handler(),
                                  SCANLINE_STATUS_PARSE_ERROR, FRAME_GIFREADER,
                                  "DGifGetImageDesc()");
        }

        ++image_spec_.num_frames;
        if (image_spec_.num_frames > kMaxGifFrames) {
          return PS_LOGGED_STATUS(
              PS_LOG_ERROR, message_handler(), SCANLINE_STATUS_PARSE_ERROR,
              FRAME_GIFREADER, "GIF frame count exceeds maximum allowed limit");
        }
        if (image_spec_.num_frames == 1) {
          frame_spec_.top = gif_file->Image.Top;
          frame_spec_.left = gif_file->Image.Left;
          frame_spec_.height = gif_file->Image.Height;
          frame_spec_.width = gif_file->Image.Width;
        }

        // Bypass the pixel data without decoding it.
        {
          int code_size;
          GifByteType* code_block;
          if (DGifGetCode(gif_file, &code_size, &code_block) == GIF_ERROR) {
            return PS_LOGGED_STATUS(PS_LOG_INFO, message_handler(),
                                    SCANLINE_STATUS_PARSE_ERROR,
                                    FRAME_GIFREADER, "DGifGetCode()");
          }
          while (code_block != nullptr) {
            if (DGifGetCodeNext(gif_file, &code_block) == GIF_ERROR) {
              return PS_LOGGED_STATUS(PS_LOG_INFO, message_handler(),
                                      SCANLINE_STATUS_PARSE_ERROR,
                                      FRAME_GIFREADER, "DGifGetCodeNext()");
            }
          }
        }
        break;

      case EXTENSION_RECORD_TYPE: {
        ScanlineStatus status =
            ProcessExtensionAffectingImage(image_spec_.num_frames > 0);
        if (!status.Success()) {
          return status;
        }
        break;
      }

      case TERMINATE_RECORD_TYPE:
        break;

      default:
        return PS_LOGGED_STATUS(PS_LOG_INFO, message_handler(),
                                SCANLINE_STATUS_PARSE_ERROR, FRAME_GIFREADER,
                                "unexpected record %d", record_type);
        break;
    }
  }

  ApplyQuirksModeToImage(quirks_mode(), has_loop_count_, frame_spec_,
                         &image_spec_);

  // Chrome quirks mode may have enlarged the screen to the first frame
  // after the checks above ran on the declared screen, so recheck the
  // final dimensions before the spec is published: consumers size output
  // buffers from GetImageSpec() ahead of any per-frame check.
  if (image_spec_.width > kScanlineMaxDimension ||
      image_spec_.height > kScanlineMaxDimension ||
      static_cast<uint64_t>(image_spec_.width) * image_spec_.height >
          kScanlineMaxImagePixels) {
    return PS_LOGGED_STATUS(PS_LOG_INFO, message_handler(),
                            SCANLINE_STATUS_UNSUPPORTED_FORMAT, FRAME_GIFREADER,
                            "Image dimensions %ux%u exceed the supported "
                            "maximum.",
                            static_cast<unsigned>(image_spec_.width),
                            static_cast<unsigned>(image_spec_.height));
  }

  ExpandColorIndex(gif_file->SColorMap, gif_file->SBackGroundColor,
                   image_spec_.bg_color);
  image_spec_.use_bg_color = false;

  gif_struct_->set_offset(offset);
  return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
}

ScanlineStatus GifFrameReader::set_quirks_mode(QuirksMode quirks_mode) {
  if (image_initialized_) {
    return PS_LOGGED_STATUS(PS_LOG_DFATAL, message_handler(),
                            SCANLINE_STATUS_INVOCATION_ERROR, FRAME_GIFREADER,
                            "Can't change quirks mode for initialized image.");
  }
  return MultipleFrameReader::set_quirks_mode(quirks_mode);
}

static void ClearBgColor(ImageSpec* image_spec) {
  image_spec->bg_color[RGBA_ALPHA] = kAlphaTransparent;
  image_spec->use_bg_color = true;
  image_spec->bg_color[RGBA_RED] = 0;
  image_spec->bg_color[RGBA_GREEN] = 0;
  image_spec->bg_color[RGBA_BLUE] = 0;
}

void GifFrameReader::ApplyQuirksModeToImage(QuirksMode quirks_mode,
                                            const bool got_loop_count,
                                            const FrameSpec& frame_spec,
                                            ImageSpec* image_spec) {
  if (quirks_mode == QUIRKS_NONE) {
    return;
  }

  if (quirks_mode == QUIRKS_CHROME) {
    bool image_smaller_than_first_frame =
        (frame_spec.width > image_spec->width) ||
        (frame_spec.height > image_spec->height);

    if (image_smaller_than_first_frame) {
      image_spec->image_size_adjusted = true;
      image_spec->width = frame_spec.width;
      image_spec->height = frame_spec.height;
      ClearBgColor(image_spec);
    }
    if (got_loop_count) {
      image_spec->loop_count++;
    }
    return;
  }

  // QUIRKS_FIREFOX
  bool image_and_frame_match = (frame_spec.width == image_spec->width) &&
                               (frame_spec.height == image_spec->height) &&
                               (frame_spec.left == 0) && (frame_spec.top == 0);
  if (!image_and_frame_match) {
    ClearBgColor(image_spec);
  }
}

void GifFrameReader::ApplyQuirksModeToFirstFrame(const QuirksMode quirks_mode,
                                                 const ImageSpec& image_spec,
                                                 FrameSpec* frame_spec) {
  if (quirks_mode == QUIRKS_NONE) {
    return;
  }

  if (quirks_mode == QUIRKS_CHROME) {
    if (frame_spec->width == 0 || frame_spec->height == 0) {
      frame_spec->width = image_spec.width;
      frame_spec->height = image_spec.height;
      frame_spec->left = 0;
      frame_spec->top = 0;
    }
    if (image_spec.image_size_adjusted) {
      frame_spec->left = 0;
      frame_spec->top = 0;
    }
    return;
  }

  // QUIRKS_FIREFOX
  bool image_and_frame_match = (frame_spec->width == image_spec.width) &&
                               (frame_spec->height == image_spec.height) &&
                               (frame_spec->left == 0) &&
                               (frame_spec->top == 0);
  if (!image_and_frame_match) {
    frame_spec->height = 0;
    frame_spec->width = 0;
    frame_spec->top = 0;
    frame_spec->left = 0;
  }
}

ScanlineStatus GifFrameReader::CreateColorMap() {
  GifFileType* gif_file = gif_struct_->gif_file();

  ColorMapObject* color_map = gif_file->Image.ColorMap != nullptr
                                  ? gif_file->Image.ColorMap
                                  : gif_file->SColorMap;
  if (color_map == nullptr) {
    return PS_LOGGED_STATUS(PS_LOG_INFO, message_handler(),
                            SCANLINE_STATUS_INTERNAL_ERROR, FRAME_GIFREADER,
                            "missing colormap in image and screen");
  }

  GifColorType* palette_in = color_map->Colors;
  if (palette_in == nullptr) {
    return PS_LOGGED_STATUS(PS_LOG_INFO, message_handler(),
                            SCANLINE_STATUS_INTERNAL_ERROR, FRAME_GIFREADER,
                            "Could not find colormap in the GIF image.");
  }
  if (color_map->ColorCount > kGifPaletteSize) {
    return PS_LOGGED_STATUS(
        PS_LOG_INFO, message_handler(), SCANLINE_STATUS_INTERNAL_ERROR,
        FRAME_GIFREADER, "ColorCount is too large: %d", color_map->ColorCount);
  }

  frame_palette_size_ = color_map->ColorCount;
  for (int i = 0; i < frame_palette_size_; ++i) {
    gif_palette_[i].red_ = palette_in[i].Red;
    gif_palette_[i].green_ = palette_in[i].Green;
    gif_palette_[i].blue_ = palette_in[i].Blue;
    gif_palette_[i].alpha_ = kAlphaOpaque;
  }
  memset(gif_palette_.get() + frame_palette_size_, kAlphaTransparent,
         (kGifPaletteSize - frame_palette_size_) * sizeof(PaletteRGBA));

  if (frame_transparent_index_ >= 0) {
    frame_spec_.pixel_format = RGBA_8888;
    if (frame_transparent_index_ < frame_palette_size_) {
      memset(&(gif_palette_[frame_transparent_index_]), kAlphaTransparent,
             sizeof(PaletteRGBA));
    }
  } else {
    frame_spec_.pixel_format = RGB_888;
  }

  return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
}

ScanlineStatus GifFrameReader::DecodeProgressiveGif() {
  GifFileType* gif_file = gif_struct_->gif_file();
  for (int pass = 0; pass < kInterlaceNumPass; ++pass) {
    for (size_px y = kInterlaceOffsets[pass]; y < frame_spec_.height;
         y += kInterlaceJumps[pass]) {
      GifPixelType* row_pointer =
          frame_index_.get() + static_cast<size_t>(y) * frame_spec_.width;
      if (DGifGetLine(gif_file, row_pointer, frame_spec_.width) == GIF_ERROR) {
        return PS_LOGGED_STATUS(PS_LOG_INFO, message_handler(),
                                SCANLINE_STATUS_INTERNAL_ERROR, FRAME_GIFREADER,
                                "DGifGetLine()");
      }
    }
  }
  return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
}

ScanlineStatus GifFrameReader::DecodeNonProgressiveGif() {
  GifFileType* gif_file = gif_struct_->gif_file();
  GifPixelType* row_pointer = frame_index_.get();
  const GifPixelType* row_pointer_end =
      frame_index_.get() +
      static_cast<size_t>(frame_spec_.height) * frame_spec_.width;
  for (; row_pointer < row_pointer_end; row_pointer += frame_spec_.width) {
    if (DGifGetLine(gif_file, row_pointer, frame_spec_.width) == GIF_ERROR) {
      return PS_LOGGED_STATUS(PS_LOG_INFO, message_handler(),
                              SCANLINE_STATUS_INTERNAL_ERROR, FRAME_GIFREADER,
                              "DGifGetLine()");
    }
  }
  return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
}

inline bool FrameHasOutOfRangePixels(GifPixelType* px, const size_px num_pixels,
                                     const int frame_palette_size) {
  const GifPixelType* end_px = px + num_pixels;
  for (; px < end_px; ++px) {
    if ((*px) >= frame_palette_size) {
      return true;
    }
  }
  return false;
}

ScanlineStatus GifFrameReader::PrepareNextFrame() {
  // Consume any remaining scanlines
  const void* out_bytes;
  ScanlineStatus status;
  while (HasMoreScanlines()) {
    if (!MultipleFrameReader::ReadNextScanline(&out_bytes, &status)) {
      return status;
    }
  }

  frame_initialized_ = false;
  frame_spec_.Reset();
  frame_transparent_index_ = kNoTransparentIndex;

  if (next_frame_ >= image_spec_.num_frames) {
    return PS_LOGGED_STATUS(PS_LOG_DFATAL, message_handler(),
                            SCANLINE_STATUS_INVOCATION_ERROR, FRAME_GIFREADER,
                            "PrepareNextFrame: no more frames.");
  }

  GifFileType* gif_file = gif_struct_->gif_file();
  bool found_frame = false;

  while (!found_frame) {
    GifRecordType record_type = UNDEFINED_RECORD_TYPE;
    if (DGifGetRecordType(gif_file, &record_type) == GIF_ERROR) {
      return PS_LOGGED_STATUS(PS_LOG_INFO, message_handler(),
                              SCANLINE_STATUS_PARSE_ERROR, FRAME_GIFREADER,
                              "DGifGetRecordType()");
    }

    switch (record_type) {
      case IMAGE_DESC_RECORD_TYPE: {
        if (DGifGetImageDesc(gif_file) == GIF_ERROR) {
          return PS_LOGGED_STATUS(PS_LOG_INFO, message_handler(),
                                  SCANLINE_STATUS_PARSE_ERROR, FRAME_GIFREADER,
                                  "DGifGetImageDesc()");
        }
        frame_spec_.top = gif_file->Image.Top;
        frame_spec_.left = gif_file->Image.Left;
        frame_spec_.height = gif_file->Image.Height;
        frame_spec_.width = gif_file->Image.Width;
        if (next_frame_ == 0) {
          ApplyQuirksModeToFirstFrame(quirks_mode(), image_spec_, &frame_spec_);
        }
        ++next_frame_;
        found_frame = true;
        break;
      }

      case EXTENSION_RECORD_TYPE: {
        ScanlineStatus status = ProcessExtensionAffectingFrame();
        if (!status.Success()) {
          return status;
        }
        break;
      }

      case TERMINATE_RECORD_TYPE: {
        return PS_LOGGED_STATUS(
            PS_LOG_INFO, message_handler(), SCANLINE_STATUS_INTERNAL_ERROR,
            FRAME_GIFREADER,
            "PrepareNextFrame: expected to find the next frame, failed.");
        break;
      }

      default: {
        return PS_LOGGED_STATUS(PS_LOG_INFO, message_handler(),
                                SCANLINE_STATUS_PARSE_ERROR, FRAME_GIFREADER,
                                "unexpected record %d", record_type);
        break;
      }
    }
  }

  status = CreateColorMap();

  if (!status.Success()) {
    Reset();
    return status;
  }

  frame_spec_.hint_progressive = (gif_file->Image.Interlace != 0);
  const bool is_originally_rgba = (frame_spec_.pixel_format == RGBA_8888);
  frame_eagerly_read_ = frame_spec_.hint_progressive || !is_originally_rgba;

  // A frame may declare dimensions beyond the logical screen, and the
  // buffers below are proportional to width * height, so apply the same
  // decode caps per frame as the image-level check applies to the screen.
  if (frame_spec_.width > kScanlineMaxDimension ||
      frame_spec_.height > kScanlineMaxDimension ||
      static_cast<uint64_t>(frame_spec_.width) * frame_spec_.height >
          kScanlineMaxImagePixels) {
    Reset();
    return PS_LOGGED_STATUS(PS_LOG_INFO, message_handler(),
                            SCANLINE_STATUS_UNSUPPORTED_FORMAT, FRAME_GIFREADER,
                            "Frame dimensions %ux%u exceed the supported "
                            "maximum.",
                            static_cast<unsigned>(frame_spec_.width),
                            static_cast<unsigned>(frame_spec_.height));
  }

  // Guard against overflow in frame pixel allocation.
  if (frame_spec_.width > 0 &&
      frame_spec_.height > SIZE_MAX / frame_spec_.width) {
    Reset();
    return PS_LOGGED_STATUS(
        PS_LOG_ERROR, message_handler(), SCANLINE_STATUS_UNSUPPORTED_FEATURE,
        FRAME_GIFREADER, "GIF frame dimensions too large for allocation");
  }
  const auto alloc_count =
      frame_eagerly_read_
          ? static_cast<size_t>(frame_spec_.width) * frame_spec_.height
          : static_cast<size_t>(frame_spec_.width);
  frame_index_.reset(new GifPixelType[alloc_count]);
  if (frame_index_ == nullptr) {
    Reset();
    return PS_LOGGED_STATUS(PS_LOG_ERROR, message_handler(),
                            SCANLINE_STATUS_MEMORY_ERROR, FRAME_GIFREADER,
                            "new GiPixelType[] for frame_index_");
  }

  next_row_ = 0;
  frame_initialized_ = true;

  if (frame_eagerly_read_) {
    ScanlineStatus status =
        (frame_spec_.hint_progressive ? DecodeProgressiveGif()
                                      : DecodeNonProgressiveGif());
    if (!status.Success()) {
      PS_LOG_INFO(message_handler(), "Failed to decode entire GIF frame.");
      Reset();
      return status;
    }

    if (!is_originally_rgba &&
        FrameHasOutOfRangePixels(frame_index_.get(),
                                 frame_spec_.width * frame_spec_.height,
                                 frame_palette_size_)) {
      frame_spec_.pixel_format = RGBA_8888;
      PS_DLOG_INFO(
          message_handler(),
          "Found out-of-range pixel in frame %i. Switching frame to %s",
          next_frame_ - 1, GetPixelFormatString(frame_spec_.pixel_format));
    }
  }

  size_t bytes_per_row =
      GetBytesPerPixel(frame_spec_.pixel_format) * frame_spec_.width;
  frame_buffer_.reset(new GifByteType[bytes_per_row]);
  if (frame_buffer_ == nullptr) {
    Reset();
    return PS_LOGGED_STATUS(PS_LOG_ERROR, message_handler(),
                            SCANLINE_STATUS_MEMORY_ERROR, FRAME_GIFREADER,
                            "new GiPixelType[] for frame_buffer_ ");
  }

  return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
}

ScanlineStatus GifFrameReader::ReadNextScanline(
    const void** out_scanline_bytes) {
  if (!frame_initialized_ || !HasMoreScanlines()) {
    return PS_LOGGED_STATUS(PS_LOG_DFATAL, message_handler(),
                            SCANLINE_STATUS_INVOCATION_ERROR, FRAME_GIFREADER,
                            "The GIF image was not initialized or does not "
                            "have more scanlines.");
  }

  GifPixelType* color_buffer = frame_buffer_.get();
  const size_t pixel_size = GetBytesPerPixel(frame_spec_.pixel_format);

  GifPixelType* index_buffer = nullptr;
  GifFileType* gif_file = gif_struct_->gif_file();
  if (!frame_eagerly_read_) {
    index_buffer = frame_index_.get();
    if (DGifGetLine(gif_file, index_buffer, frame_spec_.width) == GIF_ERROR) {
      Reset();
      return PS_LOGGED_STATUS(PS_LOG_INFO, message_handler(),
                              SCANLINE_STATUS_INTERNAL_ERROR, FRAME_GIFREADER,
                              "DGifGetLine()");
    }
  } else {
    index_buffer =
        frame_index_.get() + static_cast<size_t>(next_row_) * frame_spec_.width;
  }

  for (size_px pixel_index = 0; pixel_index < frame_spec_.width;
       ++pixel_index) {
    GifPixelType color_index = *(index_buffer++);
    memcpy(color_buffer, gif_palette_.get() + color_index, pixel_size);
    color_buffer += pixel_size;
  }

  *out_scanline_bytes = static_cast<void*>(frame_buffer_.get());
  ++next_row_;
  return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
}

}  // namespace image_compression

}  // namespace pagespeed
