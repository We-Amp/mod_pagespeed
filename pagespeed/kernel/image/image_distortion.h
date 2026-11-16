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

// Local distortion measurement between a PNG and a lossy WebP made from it.

#ifndef PAGESPEED_KERNEL_IMAGE_IMAGE_DISTORTION_H_
#define PAGESPEED_KERNEL_IMAGE_IMAGE_DISTORTION_H_

#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/image/png_optimizer.h"

namespace pagespeed {

namespace image_compression {

using net_instaweb::MessageHandler;

// The PSNR reported for a tile whose pixels are identical in both images.
constexpr double kIdenticalTilePsnrDb = 99.0;

// Computes the worst (lowest) tile PSNR, in dB, between a PNG and a WebP that
// was made from it, and stores it in *worst_db.
//
// The PNG is decoded the way the PNG-to-WebP conversion decodes it (8 bits per
// channel, gray expanded to RGB, a fully opaque alpha channel dropped); the
// WebP is decoded with the library's default (fancy) chroma upsampling, as a
// browser does. Both are split into tile x tile pixel tiles, and each tile's
// mean squared error over R, G and B is converted to PSNR against 255. A tile
// with less than a quarter of tile*tile pixels (a sliver at the right or
// bottom edge) is skipped. A tile with no error scores kIdenticalTilePsnrDb.
//
// Unlike an image-wide PSNR, the minimum is not diluted by a large clean area,
// which is what lets it catch local damage such as smeared coloured text.
//
// Returns false, leaving *worst_db unchanged, when either image cannot be
// decoded, when they are not both 8-bit RGB of identical dimensions (an image
// with a real alpha channel is not supported), when tile < 2, or when the
// image is too small to contain any scored tile.
//
// Memory: the readers decode a full frame of each image (RGB, 3 bytes per
// pixel); the error accumulators are O(width).
bool WorstTilePsnr(const PngReaderInterface& png_reader,
                   const GoogleString& png, const GoogleString& webp, int tile,
                   double* worst_db, MessageHandler* handler);

}  // namespace image_compression

}  // namespace pagespeed

#endif  // PAGESPEED_KERNEL_IMAGE_IMAGE_DISTORTION_H_
