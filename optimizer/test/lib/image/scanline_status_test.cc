// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Unit tests for ScanlineStatus.

#include "lib/image/scanline_status.h"

#include <string>

#include "gtest/gtest.h"

namespace net_instaweb {
namespace {

// =============================================================================
// ScanlineStatus tests
// =============================================================================

TEST(ScanlineStatusTest, DefaultConstruction) {
  ScanlineStatus status;
  EXPECT_TRUE(status.Success());
  EXPECT_EQ(SCANLINE_STATUS_SUCCESS, status.type());
  EXPECT_EQ(SCANLINE_UNKNOWN, status.source());
  EXPECT_TRUE(status.details().empty());
}

TEST(ScanlineStatusTest, TypeOnlyConstruction) {
  ScanlineStatus status(SCANLINE_STATUS_PARSE_ERROR);
  EXPECT_FALSE(status.Success());
  EXPECT_EQ(SCANLINE_STATUS_PARSE_ERROR, status.type());
  EXPECT_EQ(SCANLINE_UNKNOWN, status.source());
  EXPECT_TRUE(status.details().empty());
}

TEST(ScanlineStatusTest, FullConstruction) {
  ScanlineStatus status(SCANLINE_STATUS_MEMORY_ERROR, SCANLINE_PNGREADER,
                        "allocation failed");
  EXPECT_FALSE(status.Success());
  EXPECT_EQ(SCANLINE_STATUS_MEMORY_ERROR, status.type());
  EXPECT_EQ(SCANLINE_PNGREADER, status.source());
  EXPECT_EQ("allocation failed", status.details());
}

TEST(ScanlineStatusTest, SuccessStatus) {
  ScanlineStatus status(SCANLINE_STATUS_SUCCESS, SCANLINE_JPEGREADER,
                        "completed");
  EXPECT_TRUE(status.Success());
}

TEST(ScanlineStatusTest, NewWithFormat) {
  ScanlineStatus status =
      ScanlineStatus::New(SCANLINE_STATUS_PARSE_ERROR, SCANLINE_GIFREADER,
                          "invalid header at offset %d", 42);
  EXPECT_FALSE(status.Success());
  EXPECT_EQ(SCANLINE_STATUS_PARSE_ERROR, status.type());
  EXPECT_EQ(SCANLINE_GIFREADER, status.source());
  EXPECT_EQ("invalid header at offset 42", status.details());
}

TEST(ScanlineStatusTest, NewWithMultipleFormatArgs) {
  ScanlineStatus status = ScanlineStatus::New(
      SCANLINE_STATUS_INTERNAL_ERROR, SCANLINE_WEBPREADER,
      "expected %d bytes, got %d at position %lu", 100, 50, 1024UL);
  EXPECT_EQ("expected 100 bytes, got 50 at position 1024", status.details());
}

TEST(ScanlineStatusTest, TypeStr) {
  ScanlineStatus status(SCANLINE_STATUS_UNSUPPORTED_FORMAT);
  EXPECT_STREQ("SCANLINE_STATUS_UNSUPPORTED_FORMAT", status.TypeStr());

  ScanlineStatus success_status(SCANLINE_STATUS_SUCCESS);
  EXPECT_STREQ("SCANLINE_STATUS_SUCCESS", success_status.TypeStr());

  ScanlineStatus timeout_status(SCANLINE_STATUS_TIMEOUT_ERROR);
  EXPECT_STREQ("SCANLINE_STATUS_TIMEOUT_ERROR", timeout_status.TypeStr());
}

TEST(ScanlineStatusTest, SourceStr) {
  ScanlineStatus png_status(SCANLINE_STATUS_SUCCESS, SCANLINE_PNGREADER, "");
  EXPECT_STREQ("SCANLINE_PNGREADER", png_status.SourceStr());

  ScanlineStatus jpeg_status(SCANLINE_STATUS_SUCCESS, SCANLINE_JPEGREADER, "");
  EXPECT_STREQ("SCANLINE_JPEGREADER", jpeg_status.SourceStr());

  ScanlineStatus webp_status(SCANLINE_STATUS_SUCCESS, SCANLINE_WEBPWRITER, "");
  EXPECT_STREQ("SCANLINE_WEBPWRITER", webp_status.SourceStr());
}

TEST(ScanlineStatusTest, ToString) {
  ScanlineStatus status(SCANLINE_STATUS_PARSE_ERROR, SCANLINE_GIFREADER,
                        "bad frame data");
  std::string str = status.ToString();
  EXPECT_NE(str.find("SCANLINE_GIFREADER"), std::string::npos);
  EXPECT_NE(str.find("SCANLINE_STATUS_PARSE_ERROR"), std::string::npos);
  EXPECT_NE(str.find("bad frame data"), std::string::npos);
}

TEST(ScanlineStatusTest, ComesFromReader) {
  // Reader sources
  EXPECT_TRUE(ScanlineStatus(SCANLINE_STATUS_SUCCESS, SCANLINE_PNGREADER, "")
                  .ComesFromReader());
  EXPECT_TRUE(ScanlineStatus(SCANLINE_STATUS_SUCCESS, SCANLINE_PNGREADERRAW, "")
                  .ComesFromReader());
  EXPECT_TRUE(ScanlineStatus(SCANLINE_STATUS_SUCCESS, SCANLINE_GIFREADER, "")
                  .ComesFromReader());
  EXPECT_TRUE(ScanlineStatus(SCANLINE_STATUS_SUCCESS, SCANLINE_GIFREADERRAW, "")
                  .ComesFromReader());
  EXPECT_TRUE(ScanlineStatus(SCANLINE_STATUS_SUCCESS, SCANLINE_JPEGREADER, "")
                  .ComesFromReader());
  EXPECT_TRUE(ScanlineStatus(SCANLINE_STATUS_SUCCESS, SCANLINE_WEBPREADER, "")
                  .ComesFromReader());
  EXPECT_TRUE(ScanlineStatus(SCANLINE_STATUS_SUCCESS,
                             FRAME_TO_SCANLINE_READER_ADAPTER, "")
                  .ComesFromReader());
  EXPECT_TRUE(ScanlineStatus(SCANLINE_STATUS_SUCCESS,
                             SCANLINE_TO_FRAME_READER_ADAPTER, "")
                  .ComesFromReader());
  EXPECT_TRUE(ScanlineStatus(SCANLINE_STATUS_SUCCESS, FRAME_GIFREADER, "")
                  .ComesFromReader());
  EXPECT_TRUE(ScanlineStatus(SCANLINE_STATUS_SUCCESS, FRAME_PADDING_READER, "")
                  .ComesFromReader());

  // Non-reader sources
  EXPECT_FALSE(ScanlineStatus(SCANLINE_STATUS_SUCCESS, SCANLINE_UNKNOWN, "")
                   .ComesFromReader());
  EXPECT_FALSE(ScanlineStatus(SCANLINE_STATUS_SUCCESS, SCANLINE_PNGWRITER, "")
                   .ComesFromReader());
  EXPECT_FALSE(ScanlineStatus(SCANLINE_STATUS_SUCCESS, SCANLINE_JPEGWRITER, "")
                   .ComesFromReader());
  EXPECT_FALSE(ScanlineStatus(SCANLINE_STATUS_SUCCESS, SCANLINE_WEBPWRITER, "")
                   .ComesFromReader());
  EXPECT_FALSE(ScanlineStatus(SCANLINE_STATUS_SUCCESS, SCANLINE_RESIZER, "")
                   .ComesFromReader());
  EXPECT_FALSE(ScanlineStatus(SCANLINE_STATUS_SUCCESS, SCANLINE_UTIL, "")
                   .ComesFromReader());
}

TEST(ScanlineStatusTest, CopyConstruction) {
  ScanlineStatus original(SCANLINE_STATUS_MEMORY_ERROR, SCANLINE_WEBPREADER,
                          "out of memory");
  const ScanlineStatus& copy(original);

  EXPECT_EQ(original.type(), copy.type());
  EXPECT_EQ(original.source(), copy.source());
  EXPECT_EQ(original.details(), copy.details());
  EXPECT_FALSE(copy.Success());
}

TEST(ScanlineStatusTest, Assignment) {
  ScanlineStatus original(SCANLINE_STATUS_TIMEOUT_ERROR, SCANLINE_JPEGWRITER,
                          "encode timeout");
  ScanlineStatus assigned;
  assigned = original;

  EXPECT_EQ(original.type(), assigned.type());
  EXPECT_EQ(original.source(), assigned.source());
  EXPECT_EQ(original.details(), assigned.details());
}

// =============================================================================
// ScanlineStatusType enum tests
// =============================================================================

TEST(ScanlineStatusTypeTest, AllStatusTypesExist) {
  // Verify all expected status types exist and have distinct values
  EXPECT_EQ(0, SCANLINE_STATUS_UNINITIALIZED);
  EXPECT_EQ(1, SCANLINE_STATUS_SUCCESS);
  EXPECT_EQ(2, SCANLINE_STATUS_UNSUPPORTED_FORMAT);
  EXPECT_EQ(3, SCANLINE_STATUS_UNSUPPORTED_FEATURE);
  EXPECT_EQ(4, SCANLINE_STATUS_PARSE_ERROR);
  EXPECT_EQ(5, SCANLINE_STATUS_MEMORY_ERROR);
  EXPECT_EQ(6, SCANLINE_STATUS_INTERNAL_ERROR);
  EXPECT_EQ(7, SCANLINE_STATUS_TIMEOUT_ERROR);
  EXPECT_EQ(8, SCANLINE_STATUS_INVOCATION_ERROR);
  EXPECT_EQ(9, NUM_SCANLINE_STATUS);
}

// =============================================================================
// ScanlineStatusSource enum tests
// =============================================================================

TEST(ScanlineStatusSourceTest, AllSourcesExist) {
  // Verify key sources exist (not exhaustive, just sampling)
  EXPECT_EQ(0, SCANLINE_UNKNOWN);
  EXPECT_LT(SCANLINE_PNGREADER, NUM_SCANLINE_SOURCE);
  EXPECT_LT(SCANLINE_JPEGREADER, NUM_SCANLINE_SOURCE);
  EXPECT_LT(SCANLINE_GIFREADER, NUM_SCANLINE_SOURCE);
  EXPECT_LT(SCANLINE_WEBPREADER, NUM_SCANLINE_SOURCE);
  EXPECT_LT(SCANLINE_PNGWRITER, NUM_SCANLINE_SOURCE);
  EXPECT_LT(SCANLINE_JPEGWRITER, NUM_SCANLINE_SOURCE);
  EXPECT_LT(SCANLINE_WEBPWRITER, NUM_SCANLINE_SOURCE);
}

}  // namespace
}  // namespace net_instaweb
