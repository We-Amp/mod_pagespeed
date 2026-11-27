// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Text Compression Library

#include "src/worker/text_compressor.h"

#include <climits>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "brotli/decode.h"
#include "brotli/encode.h"
#include "zlib.h"

namespace pagespeed {

std::string GzipCompress(std::string_view input, int level) {
  if (level <= 0 || level > 9) return {};
  if (input.empty()) return {};
  // zlib uses uInt (32-bit); reject inputs that would silently truncate.
  if (input.size() > UINT_MAX) return {};

  z_stream stream;
  std::memset(&stream, 0, sizeof(stream));

  // windowBits=15+16 for gzip format (16 = gzip wrapper)
  if (deflateInit2(&stream, level, Z_DEFLATED, 15 + 16, 8,
                   Z_DEFAULT_STRATEGY) != Z_OK) {
    return {};
  }

  stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
  stream.avail_in = static_cast<uInt>(input.size());

  std::string output;
  // deflateBound gives an upper bound on compressed size.
  output.resize(deflateBound(&stream, input.size()));

  stream.next_out = reinterpret_cast<Bytef*>(output.data());
  stream.avail_out = static_cast<uInt>(output.size());

  int ret = deflate(&stream, Z_FINISH);
  deflateEnd(&stream);

  if (ret != Z_STREAM_END) return {};

  output.resize(stream.total_out);
  return output;
}

std::string GzipDecompress(std::string_view input, size_t max_output_size) {
  if (input.empty()) return {};
  // zlib uses uInt (32-bit); reject inputs that would silently truncate.
  if (input.size() > UINT_MAX) return {};

  z_stream stream;
  std::memset(&stream, 0, sizeof(stream));

  // windowBits=15+16 for gzip format
  if (inflateInit2(&stream, 15 + 16) != Z_OK) {
    return {};
  }

  stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
  stream.avail_in = static_cast<uInt>(input.size());

  std::string output;
  constexpr size_t kChunkSize = 65536;
  std::vector<char> chunk(kChunkSize);

  int ret;
  do {
    stream.next_out = reinterpret_cast<Bytef*>(chunk.data());
    stream.avail_out = static_cast<uInt>(chunk.size());

    ret = inflate(&stream, Z_NO_FLUSH);
    if (ret != Z_OK && ret != Z_STREAM_END) {
      inflateEnd(&stream);
      return {};
    }

    size_t have = chunk.size() - stream.avail_out;
    if (output.size() + have > max_output_size) {
      inflateEnd(&stream);
      return {};
    }
    output.append(chunk.data(), have);
  } while (ret != Z_STREAM_END);

  inflateEnd(&stream);
  return output;
}

std::string BrotliCompress(std::string_view input, int quality) {
  if (quality <= 0 || quality > BROTLI_MAX_QUALITY) return {};
  if (input.empty()) return {};

  size_t max_size = BrotliEncoderMaxCompressedSize(input.size());
  if (max_size == 0) return {};
  std::string output(max_size, '\0');

  size_t encoded_size = max_size;
  if (!BrotliEncoderCompress(
          quality, BROTLI_DEFAULT_WINDOW, BROTLI_MODE_TEXT, input.size(),
          reinterpret_cast<const uint8_t*>(input.data()), &encoded_size,
          reinterpret_cast<uint8_t*>(output.data()))) {
    return {};
  }

  output.resize(encoded_size);
  return output;
}

std::string BrotliDecompress(std::string_view input, size_t max_output_size) {
  if (input.empty()) return {};

  size_t available_in = input.size();
  const uint8_t* next_in = reinterpret_cast<const uint8_t*>(input.data());

  BrotliDecoderState* state =
      BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
  if (!state) return {};

  std::string output;
  constexpr size_t kChunkSize = 65536;
  std::vector<uint8_t> chunk(kChunkSize);

  BrotliDecoderResult result;
  do {
    size_t available_out = chunk.size();
    uint8_t* next_out = chunk.data();

    result = BrotliDecoderDecompressStream(state, &available_in, &next_in,
                                           &available_out, &next_out, nullptr);

    size_t have = chunk.size() - available_out;
    if (output.size() + have > max_output_size) {
      BrotliDecoderDestroyInstance(state);
      return {};
    }
    output.append(reinterpret_cast<char*>(chunk.data()), have);

    if (result == BROTLI_DECODER_RESULT_ERROR) {
      BrotliDecoderDestroyInstance(state);
      return {};
    }
  } while (result == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT);

  BrotliDecoderDestroyInstance(state);

  if (result != BROTLI_DECODER_RESULT_SUCCESS) return {};
  return output;
}

}  // namespace pagespeed
