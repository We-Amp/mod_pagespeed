// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// This file is derived from mod_pagespeed and has been substantially modified.
// Originally licensed under Apache License, Version 2.0.
// Copyright (c) 2010-2017 Google Inc.
// Copyright (c) 2018 The Apache Software Foundation.

// Image analysis: gradient, histogram, photo detection.
// Ported from mod_pagespeed's
// pagespeed/kernel/image/image_analysis.h
// GIF support has been removed.

#ifndef PAGESPEED_LIB_IMAGE_IMAGE_ANALYSIS_H_
#define PAGESPEED_LIB_IMAGE_IMAGE_ANALYSIS_H_

#include <cstddef>
#include <cstdint>

#include "lib/base/basictypes.h"
#include "lib/image/image_util.h"
#include "lib/image/scanline_interface.h"

namespace pagespeed {

class MessageHandler;

namespace image_compression {

using net_instaweb::ScanlineReaderInterface;

const int kNumColorHistogramBins = 256;

// Computes image gradient (i.e., image edge) from the luminance
// using Sobel operator.
// Supports GRAY_8, RGB_888, and RGBA_8888 formats. Alpha is ignored
// if it exists. The gradient has the same size as the input image.
bool SobelGradient(const uint8_t* image, int width, int height,
                   int bytes_per_line, PixelFormat pixel_format,
                   MessageHandler* handler, uint8_t* gradient);

// Returns histogram of a grayscale image. The histogram has 256 bins,
// and is normalized such that the sum is 1. Pixels at the following
// locations will be used to compute the histogram:
//   x0 <= x < x0 + width
//   y0 <= y < y0 + height.
void Histogram(const uint8_t* image, int width, int height, int bytes_per_line,
               int x0, int y0, float* hist);

// Returns the photographic metric. Photos will have large metric
// values, while computer generated graphics will have small values.
float PhotoMetric(const uint8_t* image, int width, int height,
                  int bytes_per_line, PixelFormat pixel_format, float threshold,
                  MessageHandler* handler);

// Returns the width of the widest peak in the color histogram.
float WidestPeakWidth(const float* hist, float threshold);

// Returns true if the image looks like a photo, or false if it looks
// like computer generated graphics. The reader must be initialized
// with the image to be processed.
bool IsPhoto(ScanlineReaderInterface* reader, MessageHandler* handler);

// Return key information of the image. For the information which you
// do not need, set the arguments to NULL so they will not be
// computed.
//
// "is_progressive" is only valid for single frame images.
bool AnalyzeImage(ImageFormat image_type, const void* image_buffer,
                  size_t buffer_length, int* width, int* height,
                  bool* is_progressive, bool* is_animated,
                  bool* has_transparency, bool* is_photo, int* quality,
                  ScanlineReaderInterface** reader, MessageHandler* handler);

}  // namespace image_compression

}  // namespace pagespeed

#endif  // PAGESPEED_LIB_IMAGE_IMAGE_ANALYSIS_H_
