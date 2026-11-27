// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Text Compressor Unit Tests

#include "src/worker/text_compressor.h"

#include <string>

#include "gtest/gtest.h"

namespace pagespeed {
namespace {

const char kTestData[] =
    "Hello, World! This is a test of the PageSpeed text compression "
    "library. Repeating content helps compression ratios. "
    "Hello, World! This is a test of the PageSpeed text compression "
    "library. Repeating content helps compression ratios.";

// --- Gzip roundtrip tests ---

TEST(TextCompressorTest, GzipRoundtrip) {
  auto compressed = GzipCompress(kTestData, 6);
  ASSERT_FALSE(compressed.empty());
  EXPECT_LT(compressed.size(), strlen(kTestData));

  auto decompressed = GzipDecompress(compressed);
  EXPECT_EQ(decompressed, kTestData);
}

TEST(TextCompressorTest, GzipMagicBytes) {
  auto compressed = GzipCompress(kTestData, 6);
  ASSERT_GE(compressed.size(), 2u);
  EXPECT_EQ(static_cast<uint8_t>(compressed[0]), 0x1f);
  EXPECT_EQ(static_cast<uint8_t>(compressed[1]), 0x8b);
}

TEST(TextCompressorTest, GzipEmptyInput) {
  auto compressed = GzipCompress("", 6);
  EXPECT_TRUE(compressed.empty());
}

TEST(TextCompressorTest, GzipLevel0Disabled) {
  auto compressed = GzipCompress(kTestData, 0);
  EXPECT_TRUE(compressed.empty());
}

TEST(TextCompressorTest, GzipNegativeLevelDisabled) {
  auto compressed = GzipCompress(kTestData, -1);
  EXPECT_TRUE(compressed.empty());
}

TEST(TextCompressorTest, GzipInvalidLevel) {
  auto compressed = GzipCompress(kTestData, 10);
  EXPECT_TRUE(compressed.empty());
}

TEST(TextCompressorTest, GzipAllValidLevels) {
  for (int level = 1; level <= 9; ++level) {
    auto compressed = GzipCompress(kTestData, level);
    ASSERT_FALSE(compressed.empty()) << "level=" << level;
    auto decompressed = GzipDecompress(compressed);
    EXPECT_EQ(decompressed, kTestData) << "level=" << level;
  }
}

TEST(TextCompressorTest, GzipDecompressInvalidData) {
  auto result = GzipDecompress("not gzip data");
  EXPECT_TRUE(result.empty());
}

TEST(TextCompressorTest, GzipDecompressEmpty) {
  auto result = GzipDecompress("");
  EXPECT_TRUE(result.empty());
}

// --- Brotli roundtrip tests ---

TEST(TextCompressorTest, BrotliRoundtrip) {
  auto compressed = BrotliCompress(kTestData, 6);
  ASSERT_FALSE(compressed.empty());
  EXPECT_LT(compressed.size(), strlen(kTestData));

  auto decompressed = BrotliDecompress(compressed);
  EXPECT_EQ(decompressed, kTestData);
}

TEST(TextCompressorTest, BrotliEmptyInput) {
  auto compressed = BrotliCompress("", 6);
  EXPECT_TRUE(compressed.empty());
}

TEST(TextCompressorTest, BrotliQuality0Disabled) {
  auto compressed = BrotliCompress(kTestData, 0);
  EXPECT_TRUE(compressed.empty());
}

TEST(TextCompressorTest, BrotliNegativeQualityDisabled) {
  auto compressed = BrotliCompress(kTestData, -1);
  EXPECT_TRUE(compressed.empty());
}

TEST(TextCompressorTest, BrotliInvalidQuality) {
  auto compressed = BrotliCompress(kTestData, 12);
  EXPECT_TRUE(compressed.empty());
}

TEST(TextCompressorTest, BrotliAllValidQualities) {
  for (int quality = 1; quality <= 11; ++quality) {
    auto compressed = BrotliCompress(kTestData, quality);
    ASSERT_FALSE(compressed.empty()) << "quality=" << quality;
    auto decompressed = BrotliDecompress(compressed);
    EXPECT_EQ(decompressed, kTestData) << "quality=" << quality;
  }
}

TEST(TextCompressorTest, BrotliDecompressInvalidData) {
  auto result = BrotliDecompress("not brotli data");
  EXPECT_TRUE(result.empty());
}

TEST(TextCompressorTest, BrotliDecompressEmpty) {
  auto result = BrotliDecompress("");
  EXPECT_TRUE(result.empty());
}

// --- Large data tests ---

TEST(TextCompressorTest, GzipLargeData) {
  std::string large(static_cast<size_t>(1024) * 1024, 'A');  // 1MB of 'A'
  auto compressed = GzipCompress(large, 6);
  ASSERT_FALSE(compressed.empty());
  // Highly repetitive data should compress very well.
  EXPECT_LT(compressed.size(), large.size() / 100);
  auto decompressed = GzipDecompress(compressed);
  EXPECT_EQ(decompressed, large);
}

TEST(TextCompressorTest, BrotliLargeData) {
  std::string large(static_cast<size_t>(1024) * 1024, 'A');  // 1MB of 'A'
  auto compressed = BrotliCompress(large, 6);
  ASSERT_FALSE(compressed.empty());
  EXPECT_LT(compressed.size(), large.size() / 100);
  auto decompressed = BrotliDecompress(compressed);
  EXPECT_EQ(decompressed, large);
}

// --- Cross-format rejection ---

TEST(TextCompressorTest, GzipCannotDecompressBrotli) {
  auto brotli_data = BrotliCompress(kTestData, 6);
  ASSERT_FALSE(brotli_data.empty());
  auto result = GzipDecompress(brotli_data);
  EXPECT_TRUE(result.empty());
}

TEST(TextCompressorTest, BrotliCannotDecompressGzip) {
  auto gzip_data = GzipCompress(kTestData, 6);
  ASSERT_FALSE(gzip_data.empty());
  auto result = BrotliDecompress(gzip_data);
  EXPECT_TRUE(result.empty());
}

// --- Truncated data tests ---

TEST(TextCompressorTest, GzipDecompressTruncated) {
  auto compressed = GzipCompress(kTestData, 6);
  ASSERT_GT(compressed.size(), 10u);
  // Chop the last 10 bytes to produce truncated-but-valid-header data.
  std::string truncated = compressed.substr(0, compressed.size() - 10);
  auto result = GzipDecompress(truncated);
  EXPECT_TRUE(result.empty());
}

TEST(TextCompressorTest, BrotliDecompressTruncated) {
  auto compressed = BrotliCompress(kTestData, 6);
  ASSERT_GT(compressed.size(), 10u);
  std::string truncated = compressed.substr(0, compressed.size() - 10);
  auto result = BrotliDecompress(truncated);
  EXPECT_TRUE(result.empty());
}

// --- Binary content tests ---

TEST(TextCompressorTest, GzipBinaryRoundtrip) {
  // Build a string with all 256 byte values including null bytes.
  std::string binary(256, '\0');
  for (int i = 0; i < 256; ++i) binary[i] = static_cast<char>(i);
  auto compressed = GzipCompress(binary, 6);
  ASSERT_FALSE(compressed.empty());
  auto decompressed = GzipDecompress(compressed);
  EXPECT_EQ(decompressed, binary);
}

TEST(TextCompressorTest, BrotliBinaryRoundtrip) {
  std::string binary(256, '\0');
  for (int i = 0; i < 256; ++i) binary[i] = static_cast<char>(i);
  auto compressed = BrotliCompress(binary, 6);
  ASSERT_FALSE(compressed.empty());
  auto decompressed = BrotliDecompress(compressed);
  EXPECT_EQ(decompressed, binary);
}

// --- Single-byte input tests ---

TEST(TextCompressorTest, GzipSingleByte) {
  auto compressed = GzipCompress("X", 6);
  ASSERT_FALSE(compressed.empty());
  // Single byte compresses to something (larger due to headers).
  auto decompressed = GzipDecompress(compressed);
  EXPECT_EQ(decompressed, "X");
}

TEST(TextCompressorTest, BrotliSingleByte) {
  auto compressed = BrotliCompress("X", 6);
  ASSERT_FALSE(compressed.empty());
  auto decompressed = BrotliDecompress(compressed);
  EXPECT_EQ(decompressed, "X");
}

// --- Decompression output limit tests ---

TEST(TextCompressorTest, GzipDecompressOutputLimit) {
  // Compress 1MB, then decompress with a 1KB limit.
  std::string large(static_cast<size_t>(1024) * 1024, 'B');
  auto compressed = GzipCompress(large, 1);
  ASSERT_FALSE(compressed.empty());
  auto result = GzipDecompress(compressed, 1024);
  EXPECT_TRUE(result.empty());
}

TEST(TextCompressorTest, BrotliDecompressOutputLimit) {
  std::string large(static_cast<size_t>(1024) * 1024, 'B');
  auto compressed = BrotliCompress(large, 1);
  ASSERT_FALSE(compressed.empty());
  auto result = BrotliDecompress(compressed, 1024);
  EXPECT_TRUE(result.empty());
}

// --- Corrupted gzip stream tests (inflate error mid-stream) ---

TEST(TextCompressorTest, GzipDecompressCorruptedMidStream) {
  // Compress valid data, then corrupt bytes in the middle of the stream
  // (past the gzip header) to trigger inflate() returning an error.
  auto compressed = GzipCompress(kTestData, 6);
  ASSERT_GT(compressed.size(), 20u);
  // Corrupt several bytes in the payload area (well past 10-byte header).
  compressed[15] ^= 0xFF;
  compressed[16] ^= 0xFF;
  compressed[17] ^= 0xFF;
  auto result = GzipDecompress(compressed);
  EXPECT_TRUE(result.empty());
}

TEST(TextCompressorTest, BrotliDecompressCorruptedMidStream) {
  // Compress valid data, then corrupt bytes in the middle to trigger
  // BROTLI_DECODER_RESULT_ERROR mid-stream.
  auto compressed = BrotliCompress(kTestData, 6);
  ASSERT_GT(compressed.size(), 10u);
  compressed[5] ^= 0xFF;
  compressed[6] ^= 0xFF;
  compressed[7] ^= 0xFF;
  auto result = BrotliDecompress(compressed);
  EXPECT_TRUE(result.empty());
}

// Note: Lines 29, 61, and 106 of text_compressor.cc are defensive error
// paths for library initialization/compression failures (deflateInit2,
// inflateInit2, BrotliEncoderCompress).  These cannot be triggered without
// mocking the underlying libraries and remain uncovered by design.

}  // namespace
}  // namespace pagespeed
