// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Text Compression Library
//
// Gzip and Brotli compression/decompression for pre-compressed cache
// variants.  Separated from worker.cc for testability.

#ifndef PAGESPEED_SRC_WORKER_TEXT_COMPRESSOR_H_
#define PAGESPEED_SRC_WORKER_TEXT_COMPRESSOR_H_

#include <cstddef>
#include <string>
#include <string_view>

namespace pagespeed {

// Default decompression output limit (50 MB).  Prevents decompression
// bombs from exhausting memory.
inline constexpr size_t kDefaultMaxDecompressSize = 50ULL * 1024 * 1024;

// Compress data using gzip.  Returns compressed data on success,
// empty string on failure.  Level 0 is treated as "disabled" and
// returns empty.  Valid levels: 1-9.
std::string GzipCompress(std::string_view input, int level);

// Decompress gzip data.  Returns decompressed data on success,
// empty string on failure.  Aborts if output exceeds max_output_size.
std::string GzipDecompress(std::string_view input,
                           size_t max_output_size = kDefaultMaxDecompressSize);

// Compress data using Brotli.  Returns compressed data on success,
// empty string on failure.  Quality 0 is treated as "disabled" and
// returns empty.  Valid qualities: 1-11.
std::string BrotliCompress(std::string_view input, int quality);

// Decompress Brotli data.  Returns decompressed data on success,
// empty string on failure.  Aborts if output exceeds max_output_size.
std::string BrotliDecompress(
    std::string_view input, size_t max_output_size = kDefaultMaxDecompressSize);

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_WORKER_TEXT_COMPRESSOR_H_
