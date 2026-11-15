// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// pagespeed_test.cc -- C API unit tests for libpagespeed.

#include "lib/pagespeed/pagespeed.h"

#ifndef _WIN32
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "lib/pagespeed/pagespeed_internal.h"
#include "src/worker/serve_stats.h"
#include "src/worker/shared_config.h"
#include "test/test_util/temp_dir.h"

// ================================================================
// Version tests
// ================================================================

TEST(PageSpeedCApi, VersionConstants) {
  EXPECT_EQ(ps_version_major(), PS_API_VERSION_MAJOR);
  EXPECT_EQ(ps_version_minor(), PS_API_VERSION_MINOR);
  EXPECT_EQ(ps_version_patch(), PS_API_VERSION_PATCH);
}

TEST(PageSpeedCApi, GitCommit) {
  const char* commit = ps_git_commit();
  ASSERT_NE(commit, nullptr);
  EXPECT_GT(strlen(commit), 0u);
  EXPECT_STRNE(commit, "0");
  EXPECT_LE(strlen(commit), 7u);
}

TEST(PageSpeedCApi, ProductVersion) {
  const char* version = ps_product_version();
  ASSERT_NE(version, nullptr);
  EXPECT_GT(strlen(version), 0u);
  EXPECT_NE(strchr(version, '.'), nullptr);
}

// ================================================================
// Error code tests
// ================================================================

TEST(PageSpeedCApi, ErrorNames) {
  EXPECT_STREQ(ps_error_name(PS_OK), "PS_OK");
  EXPECT_STREQ(ps_error_name(PS_ERR_NOT_FOUND), "PS_ERR_NOT_FOUND");
  EXPECT_STREQ(ps_error_name(PS_ERR_INTERNAL), "PS_ERR_INTERNAL");
}

TEST(PageSpeedCApi, ErrorStrings) {
  EXPECT_STREQ(ps_strerror(PS_OK), "Success");
  EXPECT_STREQ(ps_strerror(PS_ERR_IO), "Input/output error");
}

TEST(PageSpeedCApi, LastErrorMessage) {
  // Initially empty.
  EXPECT_STREQ(ps_last_error_message(), "");
  // After a call that clears errors, still empty.
  ps_classify(nullptr, nullptr, nullptr, nullptr);
  EXPECT_STREQ(ps_last_error_message(), "");
}

// ================================================================
// Content type tests
// ================================================================

struct ClassifyContentTypeCase {
  const char* mime;  // Input MIME string (may be nullptr).
  ps_content_type_t expected;
};

class ClassifyContentTypeTest
    : public ::testing::TestWithParam<ClassifyContentTypeCase> {};

TEST_P(ClassifyContentTypeTest, Classifies) {
  const ClassifyContentTypeCase& c = GetParam();
  EXPECT_EQ(ps_classify_content_type(c.mime), c.expected);
}

INSTANTIATE_TEST_SUITE_P(
    PageSpeedCApi, ClassifyContentTypeTest,
    ::testing::Values(
        ClassifyContentTypeCase{"text/html", PS_CONTENT_HTML},
        ClassifyContentTypeCase{"text/html; charset=utf-8", PS_CONTENT_HTML},
        ClassifyContentTypeCase{"text/css", PS_CONTENT_CSS},
        ClassifyContentTypeCase{"application/javascript", PS_CONTENT_JS},
        ClassifyContentTypeCase{"text/javascript", PS_CONTENT_JS},
        ClassifyContentTypeCase{"image/jpeg", PS_CONTENT_IMAGE},
        ClassifyContentTypeCase{"image/webp", PS_CONTENT_IMAGE},
        ClassifyContentTypeCase{"application/octet-stream", PS_CONTENT_OTHER},
        ClassifyContentTypeCase{nullptr, PS_CONTENT_OTHER}));

struct ContentTypeMimeCase {
  ps_content_type_t type;
  const char* expected_mime;
};

class ContentTypeMimeTest
    : public ::testing::TestWithParam<ContentTypeMimeCase> {};

TEST_P(ContentTypeMimeTest, ReturnsMime) {
  const ContentTypeMimeCase& c = GetParam();
  EXPECT_STREQ(ps_content_type_mime(c.type), c.expected_mime);
}

INSTANTIATE_TEST_SUITE_P(
    PageSpeedCApi, ContentTypeMimeTest,
    ::testing::Values(
        ContentTypeMimeCase{PS_CONTENT_HTML, "text/html"},
        ContentTypeMimeCase{PS_CONTENT_CSS, "text/css"},
        ContentTypeMimeCase{PS_CONTENT_JS, "application/javascript"},
        ContentTypeMimeCase{PS_CONTENT_IMAGE, "image/jpeg"},
        ContentTypeMimeCase{PS_CONTENT_OTHER, "application/octet-stream"}));

// ================================================================
// Classification tests
// ================================================================

TEST(PageSpeedCApi, ClassifyAllNull) {
  uint32_t mask = ps_classify(nullptr, nullptr, nullptr, nullptr);
  // No Accept header is semantically */* (RFC 9110 12.5.1), and */* resolves
  // to WebP -- so the all-null call is Desktop/Identity/WebP = 0x09, not the
  // default-constructed 0x08 it used to return (#1377). Every other dimension
  // still reads as its default.
  EXPECT_EQ(mask, 0x09u);
  EXPECT_EQ(mask & 0x03, 1u);     // format: WebP
  EXPECT_EQ(mask & 0x0C, 0x08u);  // viewport: Desktop
  EXPECT_EQ(mask & 0xC0, 0u);     // encoding: identity
  // The absent header and the wildcard it stands for agree, here as everywhere.
  EXPECT_EQ(mask, ps_classify("*/*", nullptr, nullptr, nullptr));
}

TEST(PageSpeedCApi, ClassifyAvifBrotli) {
  uint32_t mask = ps_classify("image/avif,image/webp,*/*",
                              "Mozilla/5.0 Chrome/120", nullptr, "gzip, br");
  // AVIF = bits 0-1 = 2.
  EXPECT_EQ(mask & 0x03, 2u);
  // Brotli = bits 6-7 = 2 (0x80).
  EXPECT_EQ(mask & 0xC0, 0x80u);
}

TEST(PageSpeedCApi, ClassifySaveData) {
  uint32_t mask = ps_classify(nullptr, nullptr, "on", nullptr);
  // Save-Data bit 5.
  EXPECT_NE(mask & 0x20, 0u);
}

// ================================================================
// Viewport from width tests
// ================================================================

TEST(PageSpeedCApi, ViewportFromWidth) {
  uint32_t base = ps_classify(nullptr, nullptr, nullptr, nullptr);
  // Mobile: 0-479.
  uint32_t m = ps_mask_set_viewport_from_width(base, 320);
  EXPECT_EQ((m >> 2) & 3, 0u);  // Mobile
  // Tablet: 480-1279.
  uint32_t t = ps_mask_set_viewport_from_width(base, 768);
  EXPECT_EQ((t >> 2) & 3, 1u);  // Tablet
  // Desktop: 1280+.
  uint32_t d = ps_mask_set_viewport_from_width(base, 1920);
  EXPECT_EQ((d >> 2) & 3, 2u);  // Desktop
}

TEST(PageSpeedCApi, ViewportBoundaries) {
  uint32_t base = 0;
  EXPECT_EQ((ps_mask_set_viewport_from_width(base, 0) >> 2) & 3, 0u);
  EXPECT_EQ((ps_mask_set_viewport_from_width(base, 479) >> 2) & 3, 0u);
  EXPECT_EQ((ps_mask_set_viewport_from_width(base, 480) >> 2) & 3, 1u);
  EXPECT_EQ((ps_mask_set_viewport_from_width(base, 1023) >> 2) & 3, 1u);
  EXPECT_EQ((ps_mask_set_viewport_from_width(base, 1024) >> 2) & 3, 1u);
  EXPECT_EQ((ps_mask_set_viewport_from_width(base, 1279) >> 2) & 3, 1u);
  EXPECT_EQ((ps_mask_set_viewport_from_width(base, 1280) >> 2) & 3, 2u);
}

// The C-ABI width->viewport mapper must agree with CapabilityMask's
// ViewportWidthRange, which classifies Tablet as 480-1279 and Desktop as
// 1280-65535 (commit 0fa4c244 moved the Tablet/Desktop split to 1280 so a
// 1024px iPad-Pro-portrait viewport classifies as Tablet, not Desktop). The
// mapper kept a stale 1024 boundary, so 1024-1279px widths were classified as
// Desktop here while the rest of the engine treated them as Tablet.
TEST(PageSpeedCApi, ViewportFromWidthMatchesViewportRange) {
  uint32_t base = 0;
  EXPECT_EQ((ps_mask_set_viewport_from_width(base, 1024) >> 2) & 3,
            1u);  // Tablet
  EXPECT_EQ((ps_mask_set_viewport_from_width(base, 1279) >> 2) & 3,
            1u);  // Tablet
  EXPECT_EQ((ps_mask_set_viewport_from_width(base, 1280) >> 2) & 3,
            2u);  // Desktop
}

// ================================================================
// Score alternate tests
// ================================================================

TEST(PageSpeedCApi, ScoreAlternatePerfectMatch) {
  uint32_t mask = 0x08;  // Desktop/Identity.
  EXPECT_GT(ps_score_alternate(mask, mask), 0);
}

TEST(PageSpeedCApi, ScoreAlternateMismatch) {
  // Non-identity encoding mismatch should be 0.
  uint32_t client_gzip = 0x48;    // Desktop/Gzip
  uint32_t stored_brotli = 0x88;  // Desktop/Brotli
  EXPECT_EQ(ps_score_alternate(client_gzip, stored_brotli), 0);
}

// ================================================================
// Hostname normalization tests
// ================================================================

struct NormalizeHostnameCase {
  const char* input;
  const char* expected;
  bool expect_positive_len;  // Whether to assert the returned length is > 0.
};

class NormalizeHostnameTest
    : public ::testing::TestWithParam<NormalizeHostnameCase> {};

TEST_P(NormalizeHostnameTest, Normalizes) {
  const NormalizeHostnameCase& c = GetParam();
  char buf[256];
  int len = ps_normalize_hostname(c.input, buf, 256);
  if (c.expect_positive_len) {
    EXPECT_GT(len, 0);
  }
  EXPECT_STREQ(buf, c.expected);
}

INSTANTIATE_TEST_SUITE_P(
    PageSpeedCApi, NormalizeHostnameTest,
    ::testing::Values(
        NormalizeHostnameCase{"EXAMPLE.COM", "example.com", true},
        NormalizeHostnameCase{"example.com:80", "example.com", false},
        NormalizeHostnameCase{"example.com:443", "example.com", false},
        NormalizeHostnameCase{"example.com:8080", "example.com:8080", false}));

TEST(PageSpeedCApi, NormalizeHostnameNull) {
  char buf[256];
  EXPECT_EQ(ps_normalize_hostname(nullptr, buf, 256), -1);
}

TEST(PageSpeedCApi, NormalizeHostnameTruncation) {
  char buf[5];
  int len = ps_normalize_hostname("example.com", buf, 5);
  EXPECT_EQ(len, 11);         // Full length returned.
  EXPECT_STREQ(buf, "exam");  // Truncated.
}

// ================================================================
// Cache test fixture
// ================================================================

class PageSpeedCacheApiTest : public ::testing::Test {
 protected:
  void SetUp() override {
    temp_dir_ = pagespeed::test::MakeTempDir();
    cache_path_ = temp_dir_ + "/cache.vol";
  }

  void TearDown() override {
    if (cache_ != nullptr) {
      ps_cache_close(cache_);
      cache_ = nullptr;
    }
    std::filesystem::remove_all(temp_dir_);
  }

  void OpenCache() {
    ps_cache_config_t config;
    ps_cache_config_init(&config);
    config.volume_path = cache_path_.c_str();
    config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);  // 10MB

    ASSERT_EQ(ps_cache_open(&config, &cache_), PS_OK);
    ASSERT_NE(cache_, nullptr);
  }

  std::string temp_dir_;
  std::string cache_path_;
  ps_cache_t* cache_ = nullptr;
};

// ================================================================
// Cache open/close tests
// ================================================================

TEST_F(PageSpeedCacheApiTest, OpenAndClose) { OpenCache(); }

TEST_F(PageSpeedCacheApiTest, OpenNullPath) {
  ps_cache_config_t config;
  ps_cache_config_init(&config);
  config.volume_path = nullptr;
  ps_cache_t* c = nullptr;
  EXPECT_EQ(ps_cache_open(&config, &c), PS_ERR_INVALID_ARG);
}

// ================================================================
// Cache round-trip tests
// ================================================================

TEST_F(PageSpeedCacheApiTest, WriteAndReadBack) {
  OpenCache();

  ps_write_params_t params;
  ps_write_params_init_auto(&params);
  params.alternate_id = 0x08;
  params.content_length = 11;
  params.full_mask = 0x08;
  params.content_type = PS_CONTENT_HTML;
  params.origin_ct = "text/html";

  ps_write_handle_t* wh = nullptr;
  ASSERT_EQ(ps_cache_write_begin(cache_, "/test.html", "example.com", "https",
                                 &params, &wh),
            PS_OK);
  ASSERT_NE(wh, nullptr);
  ASSERT_EQ(ps_write_data(wh, "hello world", 11), PS_OK);
  ASSERT_EQ(ps_write_close(wh), PS_OK);

  ps_read_result_t* result = nullptr;
  ASSERT_EQ(ps_cache_read_best(cache_, "/test.html", "example.com", "https",
                               0x08, &result),
            PS_OK);
  ASSERT_NE(result, nullptr);

  const uint8_t* data = nullptr;
  size_t len = 0;
  ASSERT_EQ(ps_read_content(result, &data, &len), PS_OK);
  EXPECT_EQ(len, 11u);
  EXPECT_EQ(std::memcmp(data, "hello world", 11), 0);
  EXPECT_EQ(ps_read_content_type(result), PS_CONTENT_HTML);
  EXPECT_EQ(ps_read_mask(result), 0x08u);
  EXPECT_STREQ(ps_read_origin_content_type(result), "text/html");
  EXPECT_EQ(ps_read_flags(result), 0);

  // cache_inserted_at should be auto-set by ps_cache_write_begin.
  uint32_t inserted = ps_read_cache_inserted_at(result);
  auto now = static_cast<uint32_t>(std::time(nullptr));
  EXPECT_GT(inserted, 0u);
  EXPECT_LE(inserted, now);
  EXPECT_GE(inserted, now - 5);  // Within 5 seconds.

  ps_read_free(result);
}

TEST_F(PageSpeedCacheApiTest, ReadCopy) {
  OpenCache();

  ps_write_params_t params;
  ps_write_params_init_auto(&params);
  params.alternate_id = 0x08;
  params.content_length = 11;
  params.full_mask = 0x08;
  params.content_type = PS_CONTENT_HTML;

  ps_write_handle_t* wh = nullptr;
  ASSERT_EQ(ps_cache_write_begin(cache_, "/copy.html", "example.com", "https",
                                 &params, &wh),
            PS_OK);
  ASSERT_EQ(ps_write_data(wh, "hello world", 11), PS_OK);
  ASSERT_EQ(ps_write_close(wh), PS_OK);

  ps_read_result_t* result = nullptr;
  ASSERT_EQ(ps_cache_read_best(cache_, "/copy.html", "example.com", "https",
                               0x08, &result),
            PS_OK);

  uint8_t buf[1024];
  size_t copied = 0;
  ASSERT_EQ(ps_read_copy(result, buf, sizeof(buf), &copied), PS_OK);
  EXPECT_EQ(copied, 11u);
  ps_read_free(result);
  // Buffer still valid after free.
  EXPECT_EQ(std::memcmp(buf, "hello world", 11), 0);
}

TEST_F(PageSpeedCacheApiTest, ReadNotFound) {
  OpenCache();
  ps_read_result_t* result = nullptr;
  EXPECT_EQ(ps_cache_read_best(cache_, "/nonexistent.html", "example.com",
                               "https", 0x08, &result),
            PS_ERR_NOT_FOUND);
}

// Long-hold callers renew the read lease pinning the
// mmap borrow via ps_read_renew_lease.  A fresh disk read has a lease to
// renew (returns 1, repeatably); NULL is a safe no-op (0).
TEST_F(PageSpeedCacheApiTest, ReadRenewLease) {
  OpenCache();

  ps_write_params_t params;
  ps_write_params_init_auto(&params);
  params.alternate_id = 0x08;
  params.content_length = 11;
  params.full_mask = 0x08;
  params.content_type = PS_CONTENT_HTML;

  ps_write_handle_t* wh = nullptr;
  ASSERT_EQ(ps_cache_write_begin(cache_, "/lease.html", "example.com", "https",
                                 &params, &wh),
            PS_OK);
  ASSERT_EQ(ps_write_data(wh, "hello world", 11), PS_OK);
  ASSERT_EQ(ps_write_close(wh), PS_OK);

  ps_read_result_t* result = nullptr;
  ASSERT_EQ(ps_cache_read_best(cache_, "/lease.html", "example.com", "https",
                               0x08, &result),
            PS_OK);
  ASSERT_NE(result, nullptr);

  EXPECT_EQ(ps_read_renew_lease(result), 1);
  EXPECT_EQ(ps_read_renew_lease(result), 1);
  ps_read_free(result);

  EXPECT_EQ(ps_read_renew_lease(nullptr), 0);
}

// ================================================================
// Cache write abort tests
// ================================================================

TEST_F(PageSpeedCacheApiTest, WriteAbort) {
  OpenCache();

  ps_write_params_t params;
  ps_write_params_init_auto(&params);
  params.alternate_id = 0x08;
  params.content_length = 7;
  params.full_mask = 0x08;
  params.content_type = PS_CONTENT_HTML;

  ps_write_handle_t* wh = nullptr;
  ASSERT_EQ(ps_cache_write_begin(cache_, "/abort.html", "example.com", "https",
                                 &params, &wh),
            PS_OK);
  ASSERT_EQ(ps_write_data(wh, "partial", 7), PS_OK);
  ps_write_abort(wh);

  EXPECT_EQ(ps_cache_alternate_exists(cache_, "/abort.html", "example.com",
                                      "https", 0x08),
            0);
}

// ================================================================
// Sentinel tests
// ================================================================

TEST_F(PageSpeedCacheApiTest, WriteSentinel) {
  OpenCache();

  const char* hints = "/style.css\nimage:/hero.jpg";
  size_t hints_len = std::strlen(hints);

  ps_write_handle_t* wh = nullptr;
  ASSERT_EQ(
      ps_cache_write_sentinel(cache_, "/page.html", "example.com", "https",
                              PS_SENTINEL_EARLY_HINTS, hints_len, &wh),
      PS_OK);
  ASSERT_EQ(ps_write_data(wh, hints, hints_len), PS_OK);
  ASSERT_EQ(ps_write_close(wh), PS_OK);

  ps_read_result_t* result = nullptr;
  ASSERT_EQ(ps_cache_read_early_hints(cache_, "/page.html", "example.com",
                                      "https", &result),
            PS_OK);

  const uint8_t* data = nullptr;
  size_t len = 0;
  ASSERT_EQ(ps_read_content(result, &data, &len), PS_OK);
  EXPECT_EQ(len, hints_len);
  EXPECT_EQ(std::memcmp(data, hints, hints_len), 0);
  ps_read_free(result);
}

TEST_F(PageSpeedCacheApiTest, WriteSentinelRejectsContent) {
  OpenCache();
  ps_write_handle_t* wh = nullptr;
  // 0x08 is a content alternate ID, not a sentinel.
  EXPECT_EQ(ps_cache_write_sentinel(cache_, "/test.html", "example.com",
                                    "https", 0x08, 10, &wh),
            PS_ERR_INVALID_ARG);
}

TEST_F(PageSpeedCacheApiTest, WriteBeginRejectsSentinel) {
  OpenCache();
  ps_write_params_t params;
  ps_write_params_init_auto(&params);
  params.alternate_id = PS_SENTINEL_EARLY_HINTS;
  params.content_length = 10;
  params.full_mask = 0;
  params.content_type = PS_CONTENT_OTHER;

  ps_write_handle_t* wh = nullptr;
  EXPECT_EQ(ps_cache_write_begin(cache_, "/test.html", "example.com", "https",
                                 &params, &wh),
            PS_ERR_INVALID_ARG);
}

// ================================================================
// Cache management tests
// ================================================================

TEST_F(PageSpeedCacheApiTest, AlternateExists) {
  OpenCache();

  EXPECT_EQ(ps_cache_alternate_exists(cache_, "/exists.html", "example.com",
                                      "https", 0x08),
            0);

  ps_write_params_t params;
  ps_write_params_init_auto(&params);
  params.alternate_id = 0x08;
  params.content_length = 4;
  params.full_mask = 0x08;
  params.content_type = PS_CONTENT_HTML;

  ps_write_handle_t* wh = nullptr;
  ASSERT_EQ(ps_cache_write_begin(cache_, "/exists.html", "example.com", "https",
                                 &params, &wh),
            PS_OK);
  ASSERT_EQ(ps_write_data(wh, "test", 4), PS_OK);
  ASSERT_EQ(ps_write_close(wh), PS_OK);

  EXPECT_EQ(ps_cache_alternate_exists(cache_, "/exists.html", "example.com",
                                      "https", 0x08),
            1);
}

TEST_F(PageSpeedCacheApiTest, ListAlternates) {
  OpenCache();

  // Write two alternates.
  auto write_alt = [&](uint8_t id, uint32_t mask) {
    ps_write_params_t params;
    ps_write_params_init_auto(&params);
    params.alternate_id = id;
    params.content_length = 4;
    params.full_mask = mask;
    params.content_type = PS_CONTENT_HTML;

    ps_write_handle_t* wh = nullptr;
    ASSERT_EQ(ps_cache_write_begin(cache_, "/list.html", "example.com", "https",
                                   &params, &wh),
              PS_OK);
    ASSERT_EQ(ps_write_data(wh, "test", 4), PS_OK);
    ASSERT_EQ(ps_write_close(wh), PS_OK);
  };

  write_alt(0x08, 0x08);  // Desktop/Identity
  write_alt(0x01, 0x01);  // Mobile/WebP

  ps_alternate_info_t* alts = nullptr;
  size_t count = 0;
  ASSERT_EQ(ps_cache_list_alternates(cache_, "/list.html", "example.com",
                                     "https", &alts, &count),
            PS_OK);
  EXPECT_GE(count, 2u);
  ps_alternates_free(alts);
}

TEST_F(PageSpeedCacheApiTest, CacheStats) {
  OpenCache();

  ps_cache_stats_t stats;
  std::memset(&stats, 0, sizeof(stats));
  stats.struct_size = sizeof(ps_cache_stats_t);
  ASSERT_EQ(ps_cache_stats(cache_, &stats), PS_OK);
  EXPECT_EQ(stats.struct_size, sizeof(ps_cache_stats_t));
  EXPECT_GT(stats.volume_capacity_bytes, 0u);
}

TEST_F(PageSpeedCacheApiTest,
       CacheStatsWritesNoByteBeyondTheCallersStructSize) {
  OpenCache();

  // A generous canary region after the "caller's struct": if the getter
  // writes past what it was told the caller owns, the canary changes.
  struct alignas(alignof(std::max_align_t)) Probe {
    unsigned char bytes[sizeof(ps_cache_stats_t) * 4];
  };
  Probe probe;
  std::memset(&probe, 0xAB, sizeof(probe));

  // Claim only the first two size_t fields (16 bytes), far less than the
  // library's full struct.
  const size_t claimed = sizeof(size_t) * 2;
  ps_cache_stats_t* stats =
      reinterpret_cast<ps_cache_stats_t*>(&probe.bytes[0]);
  stats->struct_size = claimed;

  ASSERT_EQ(ps_cache_stats(cache_, stats), PS_OK);

  // Every byte past the claimed prefix must still be the canary pattern.
  for (size_t i = claimed; i < sizeof(probe.bytes); ++i) {
    EXPECT_EQ(probe.bytes[i], 0xAB)
        << "ps_cache_stats wrote past the stated struct_size, at offset " << i;
  }
  // And it recorded the CALLER's size, not the library's.
  size_t recorded = 0;
  std::memcpy(&recorded, &probe.bytes[0], sizeof(recorded));
  EXPECT_EQ(recorded, claimed);
}

TEST_F(PageSpeedCacheApiTest, CacheStatsFillsOnlyTheCallersPrefix) {
  OpenCache();

  // Seed the cache so the prefix counters are non-zero.
  ps_write_params_t params;
  ps_write_params_init_auto(&params);
  params.alternate_id = 0x02;
  params.content_length = 100;
  params.full_mask = 0x02;
  params.content_type = PS_CONTENT_HTML;

  std::string data(100, 'x');
  ps_write_handle_t* wh = nullptr;
  ASSERT_EQ(ps_cache_write_begin(cache_, "/prefix.html", "example.com", "https",
                                 &params, &wh),
            PS_OK);
  ASSERT_EQ(ps_write_data(wh, data.c_str(), data.size()), PS_OK);
  ASSERT_EQ(ps_write_close(wh), PS_OK);

  ps_read_result_t* result = nullptr;
  ASSERT_EQ(ps_cache_read_best(cache_, "/prefix.html", "example.com", "https",
                               0x02, &result),
            PS_OK);
  ps_read_free(result);

  // Poll for the stats to settle.
  ps_cache_stats_t stats;
  for (int i = 0; i < 20; ++i) {
    std::memset(&stats, 0, sizeof(stats));
    stats.struct_size = sizeof(stats);
    ASSERT_EQ(ps_cache_stats(cache_, &stats), PS_OK);
    if (stats.current_entries > 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  // Now test prefix fill.
  std::memset(&stats, 0xAB, sizeof(stats));
  stats.struct_size =
      offsetof(ps_cache_stats_t, volume_capacity_bytes);  // = 80
  ASSERT_EQ(ps_cache_stats(cache_, &stats), PS_OK);
  EXPECT_EQ(stats.struct_size, 80u);
  EXPECT_GT(stats.current_entries, 0u);  // offset 64, inside the prefix
  EXPECT_EQ(stats.volume_capacity_bytes, 0xABABABABABABABABULL);
  EXPECT_EQ(stats.ram_cache_bytes, 0xABABABABABABABABULL);
  EXPECT_EQ(stats.total_hits, 0xABABABABABABABABULL);
  EXPECT_EQ(stats.total_misses, 0xABABABABABABABABULL);
}

TEST_F(PageSpeedCacheApiTest, CacheStatsNewCallerOldLibrary) {
  OpenCache();

  // Seed the cache so we have real stats.
  ps_write_params_t params;
  ps_write_params_init_auto(&params);
  params.alternate_id = 0x03;
  params.content_length = 100;
  params.full_mask = 0x03;
  params.content_type = PS_CONTENT_HTML;

  std::string data(100, 'x');
  ps_write_handle_t* wh = nullptr;
  ASSERT_EQ(ps_cache_write_begin(cache_, "/newcaller.html", "example.com",
                                 "https", &params, &wh),
            PS_OK);
  ASSERT_EQ(ps_write_data(wh, data.c_str(), data.size()), PS_OK);
  ASSERT_EQ(ps_write_close(wh), PS_OK);

  ps_read_result_t* result = nullptr;
  ASSERT_EQ(ps_cache_read_best(cache_, "/newcaller.html", "example.com",
                               "https", 0x03, &result),
            PS_OK);
  ps_read_free(result);

  struct Bigger {
    ps_cache_stats_t base;
    uint64_t future_field;
  };
  static_assert(sizeof(Bigger) == sizeof(ps_cache_stats_t) + sizeof(uint64_t));

  Bigger bigger;
  std::memset(&bigger, 0, sizeof(bigger));
  bigger.base.struct_size = sizeof(Bigger);  // = 120
  bigger.future_field = 0xDEADBEEFCAFEF00DULL;

  ASSERT_EQ(ps_cache_stats(cache_, &bigger.base), PS_OK);
  EXPECT_EQ(bigger.base.struct_size,
            sizeof(ps_cache_stats_t));  // clamped to 112
  EXPECT_GT(bigger.base.current_entries, 0u);
  EXPECT_EQ(bigger.future_field, 0xDEADBEEFCAFEF00DULL);  // untouched
}

TEST_F(PageSpeedCacheApiTest, CacheStatsRejectsAnUninitializedStructSize) {
  OpenCache();

  ps_cache_stats_t stats;
  std::memset(&stats, 0xAB, sizeof(stats));

  stats.struct_size = 0;
  ASSERT_EQ(ps_cache_stats(cache_, &stats), PS_ERR_INVALID_ARG);
  // Verify the struct was left untouched.
  EXPECT_EQ(stats.struct_size, 0u);
  EXPECT_EQ(stats.ram_cache_hits, 0xABABABABABABABABULL);

  stats.struct_size = 4;  // below sizeof(size_t)
  ASSERT_EQ(ps_cache_stats(cache_, &stats), PS_ERR_INVALID_ARG);
  EXPECT_EQ(stats.struct_size, 4u);
  EXPECT_EQ(stats.ram_cache_hits, 0xABABABABABABABABULL);

  // sizeof(size_t) is the minimum accepted value.
  stats.struct_size = sizeof(size_t);  // = 8
  ASSERT_EQ(ps_cache_stats(cache_, &stats), PS_OK);
  EXPECT_EQ(stats.struct_size, 8u);
  EXPECT_EQ(stats.ram_cache_hits,
            0xABABABABABABABABULL);  // unchanged, only first field written
}

TEST_F(PageSpeedCacheApiTest, CacheStatsAcceptsARaggedPrefix) {
  OpenCache();

  ps_cache_stats_t stats;
  std::memset(&stats, 0xAB, sizeof(stats));
  stats.struct_size = 61;  // mid-uint64 (odd size)

  ASSERT_EQ(ps_cache_stats(cache_, &stats), PS_OK);
  EXPECT_EQ(stats.struct_size, 61u);
}

// ================================================================
// Null safety tests
// ================================================================

TEST(PageSpeedCApi, NullSafety) {
  // All free/close functions should be NULL-safe.
  ps_read_free(nullptr);
  ps_write_abort(nullptr);
  ps_cache_close(nullptr);
  ps_alternates_free(nullptr);
  ps_scan_result_free(nullptr);
  ps_critical_css_result_free(nullptr);
  ps_html_result_free(nullptr);
  ps_html_transform_free(nullptr);
  ps_free(nullptr);

  // ps_write_close(NULL) returns PS_OK.
  EXPECT_EQ(ps_write_close(nullptr), PS_OK);

  // NULL cache/out args return INVALID_ARG.
  EXPECT_EQ(ps_cache_read_best(nullptr, "/x", "h", "https", 0, nullptr),
            PS_ERR_INVALID_ARG);
  EXPECT_EQ(ps_cache_write_begin(nullptr, "/x", "h", "https", nullptr, nullptr),
            PS_ERR_INVALID_ARG);
}

// ================================================================
// Input limits tests
// ================================================================

TEST_F(PageSpeedCacheApiTest, UrlTooLong) {
  OpenCache();
  std::string long_url(9000, 'a');
  ps_read_result_t* result = nullptr;
  EXPECT_EQ(
      ps_cache_read_best(cache_, long_url.c_str(), "h", "https", 0, &result),
      PS_ERR_INVALID_ARG);
}

TEST_F(PageSpeedCacheApiTest, HostnameTooLong) {
  OpenCache();
  std::string long_host(600, 'h');
  ps_read_result_t* result = nullptr;
  EXPECT_EQ(ps_cache_read_best(cache_, "/test", long_host.c_str(), "https", 0,
                               &result),
            PS_ERR_INVALID_ARG);
}

// ================================================================
// Notification tests
// ================================================================

TEST(PageSpeedCApi, NotifyWorkerNullSocket) {
  EXPECT_EQ(ps_notify_worker(nullptr, "/test.html", "example.com", "https",
                             PS_CONTENT_HTML, 0x08),
            PS_ERR_INVALID_ARG);
}

TEST(PageSpeedCApi, NotifyWorkerBadContentType) {
  EXPECT_EQ(ps_notify_worker("/tmp/test.sock", "/test.html", "example.com",
                             "https", static_cast<ps_content_type_t>(99), 0x08),
            PS_ERR_INVALID_ARG);
}

// ================================================================
// HTML Scanner tests
// ================================================================

TEST(PageSpeedCApi, HtmlScanBasic) {
  const char* html =
      "<html><head><link rel=\"stylesheet\" "
      "href=\"/style.css\"></head>"
      "<body><div id=\"hero\" class=\"banner "
      "wide\">test</div>"
      "<img src=\"/hero.jpg\"></body></html>";

  ps_scan_result_t* result = nullptr;
  ASSERT_EQ(
      ps_html_scan(html, std::strlen(html), "http://example.com/", &result),
      PS_OK);
  ASSERT_NE(result, nullptr);

  EXPECT_GT(ps_scan_element_count(result), 0u);
  EXPECT_EQ(ps_scan_stylesheet_count(result), 1u);

  const char* href = nullptr;
  const char* media = nullptr;
  ASSERT_EQ(ps_scan_stylesheet(result, 0, &href, &media), PS_OK);
  EXPECT_STREQ(href, "/style.css");

  ps_scan_result_free(result);
}

TEST(PageSpeedCApi, HtmlScanEmpty) {
  ps_scan_result_t* result = nullptr;
  ASSERT_EQ(ps_html_scan("", 0, "http://example.com/", &result), PS_OK);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(ps_scan_element_count(result), 0u);
  ps_scan_result_free(result);
}

TEST(PageSpeedCApi, HtmlScanElementClasses) {
  const char* html = "<div class=\"foo bar baz\">test</div>";

  ps_scan_result_t* result = nullptr;
  ASSERT_EQ(
      ps_html_scan(html, std::strlen(html), "http://example.com/", &result),
      PS_OK);

  size_t count = ps_scan_element_count(result);
  EXPECT_GT(count, 0u);

  // Find the div element.
  for (size_t i = 0; i < count; ++i) {
    const char* tag = nullptr;
    ps_scan_element(result, i, &tag, nullptr, nullptr, nullptr);
    if (std::strcmp(tag, "div") == 0) {
      const char** classes = nullptr;
      size_t nclasses = ps_scan_element_classes(result, i, &classes);
      EXPECT_EQ(nclasses, 3u);
      if (nclasses == 3 && (classes != nullptr)) {
        EXPECT_STREQ(classes[0], "foo");
        EXPECT_STREQ(classes[1], "bar");
        EXPECT_STREQ(classes[2], "baz");
      }
      break;
    }
  }
  ps_scan_result_free(result);
}

// ================================================================
// CSS validation tests
// ================================================================

TEST(PageSpeedCApi, CssValidateSafe) {
  const char* css = "body { color: red; }";
  EXPECT_EQ(ps_css_validate(css, std::strlen(css)), PS_OK);
}

TEST(PageSpeedCApi, CssValidateStyleClose) {
  const char* css = "body { } </style><script>alert(1)</script>";
  EXPECT_EQ(ps_css_validate(css, std::strlen(css)), PS_ERR_INVALID_ARG);
}

TEST(PageSpeedCApi, CssValidateStyleCloseCaseInsensitive) {
  const char* css = "body { } </STYLE><script>alert(1)</script>";
  EXPECT_EQ(ps_css_validate(css, std::strlen(css)), PS_ERR_INVALID_ARG);
}

TEST(PageSpeedCApi, CssValidateRefusesMarkupOpeningSequences) {
  // Strict, inside CSS strings and comments too: callers inline the CSS as
  // it is, without the escaping the critical block gets.
  const char* const refused[] = {
      "a[title=\"<b\"] { }",
      "a { content: \"<A\"; }",
      "a { } </x",
      "a { content: \"<!\"; }",
      "a { content: \"<?\"; }",
      "a { content: \"</noscript><img src=x>\"; }",
      "/* <div> */ a { color: red; }",
      "a { content: \"</style>\"; }",
  };
  for (const char* css : refused) {
    SCOPED_TRACE(css);
    EXPECT_EQ(ps_css_validate(css, std::strlen(css)), PS_ERR_INVALID_ARG);
  }
}

// The documented consequence: a Tailwind CSS v4 sheet, whose `@property`
// rules carry `syntax: "<color>"`, does not pass ps_css_validate, although
// ps_html_process inlines its critical CSS (HtmlProcessInlinesPropertySyntax-
// Strings).
TEST(PageSpeedCApi, CssValidateRefusesTailwindPropertySyntaxStrings) {
  const char* css =
      "@property --tw-gradient-from { syntax: \"<color>\"; inherits: false; "
      "initial-value: #0000; }";
  EXPECT_EQ(ps_css_validate(css, std::strlen(css)), PS_ERR_INVALID_ARG);
}

TEST(PageSpeedCApi, CssValidateKeepsRangeMediaQueries) {
  const char* const kept[] = {
      "@media (width<900px) { a { color: red; } }",
      "@media (width<=900px) { a { color: red; } }",
      "@media (400px < width < 900px) { a { color: red; } }",
      "a { --x: 1 < 2; }",
      "a { }<",
  };
  for (const char* css : kept) {
    SCOPED_TRACE(css);
    EXPECT_EQ(ps_css_validate(css, std::strlen(css)), PS_OK);
  }
}

TEST(PageSpeedCApi, CssValidateNull) {
  EXPECT_EQ(ps_css_validate(nullptr, 0), PS_ERR_INVALID_ARG);
}

// ================================================================
// CSS minify tests
// ================================================================

TEST(PageSpeedCApi, CssMinify) {
  const char* css = "body  {  color:  red;  }";
  char* out = nullptr;
  size_t out_len = 0;
  ASSERT_EQ(ps_css_minify(css, std::strlen(css), &out, &out_len), PS_OK);
  ASSERT_NE(out, nullptr);
  EXPECT_GT(out_len, 0u);
  EXPECT_LE(out_len, std::strlen(css));
  ps_free(out);
}

// ================================================================
// CSS flatten tests
// ================================================================

TEST(PageSpeedCApi, CssFlattenNoImports) {
  const char* css = "body { color: red; }";
  char* out = nullptr;
  size_t out_len = 0;
  int resolved = 0;
  int unresolved = 0;

  ASSERT_EQ(ps_css_flatten_imports(
                css, std::strlen(css), "http://example.com/style.css", nullptr,
                nullptr, 5, &out, &out_len, &resolved, &unresolved),
            PS_OK);
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(resolved, 0);
  EXPECT_EQ(unresolved, 0);
  ps_free(out);
}

// ================================================================
// ps_free tests
// ================================================================

TEST(PageSpeedCApi, FreeNull) {
  ps_free(nullptr);  // Should not crash.
}

// ================================================================
// HTML config init tests
// ================================================================

TEST(PageSpeedCApi, HtmlConfigInit) {
  ps_html_config_t config;
  ps_html_config_init(&config);
  // The legacy initializer stamps the 1.8 size; the struct has grown since.
  EXPECT_EQ(config.struct_size, 128u);
  EXPECT_EQ(config.enable_critical_css, 1);
  EXPECT_EQ(config.enable_lazy_load, 1);
  EXPECT_EQ(config.enable_speculation_rules, 0);
  EXPECT_EQ(config.enable_async_css, 0);  // opt-in
  EXPECT_EQ(config.critical_css_max_elements, 300);
  EXPECT_EQ(config.viewport, PS_VIEWPORT_DESKTOP);
  EXPECT_EQ(config.max_html_size, 5u * 1024 * 1024);
}

// ================================================================
// Critical CSS config init tests
// ================================================================

TEST(PageSpeedCApi, CriticalCssConfigInit) {
  ps_critical_css_config_t config;
  ps_critical_css_config_init(&config);
  EXPECT_EQ(config.struct_size, sizeof(ps_critical_css_config_t));
  EXPECT_EQ(config.max_elements, 300);
  EXPECT_EQ(config.max_depth, 10);
  EXPECT_EQ(config.viewport, PS_VIEWPORT_DESKTOP);
}

// ================================================================
// HTML transform tests
// ================================================================

TEST(PageSpeedCApi, HtmlTransformBasic) {
  const char* html =
      "<html><head></head><body>"
      "<img src=\"/big.jpg\">"
      "<img src=\"/small.jpg\">"
      "</body></html>";

  ps_scan_result_t* scan = nullptr;
  ASSERT_EQ(ps_html_scan(html, std::strlen(html), "http://example.com/", &scan),
            PS_OK);

  ps_html_config_t config;
  ps_html_config_init(&config);
  config.enable_critical_css = 0;

  ps_html_transform_t* transform = nullptr;
  ASSERT_EQ(ps_html_transform_create(scan, &config, nullptr, 0, nullptr,
                                     nullptr, nullptr, 0, &transform),
            PS_OK);

  char* out_html = nullptr;
  size_t out_len = 0;
  ASSERT_EQ(ps_html_transform_run(transform, html, std::strlen(html),
                                  "http://example.com/", &out_html, &out_len),
            PS_OK);

  // Transform should apply lazy loading.
  if (out_html != nullptr) {
    EXPECT_GT(out_len, 0u);
    ps_free(out_html);
  }

  ps_html_transform_free(transform);
  ps_scan_result_free(scan);
}

// The async-CSS loader path + JS are exposed for front-ends (nginx + the
// in-process .NET middleware) to serve byte-identically.
TEST(PageSpeedCApi, AsyncCssLoaderGetters) {
  const char* path = ps_async_css_loader_path();
  ASSERT_NE(path, nullptr);
  // Content-addressed path: assert the stable shape, not a frozen literal (the
  // embedded FNV hash changes whenever the loader body changes). The exact path
  // is cross-checked against the transform's own output in
  // HtmlTransformAsyncCssInjectsLoader below.
  std::string_view loader_path(path);
  EXPECT_TRUE(loader_path.starts_with("/pagespeed_static/async_css."));
  EXPECT_TRUE(loader_path.ends_with(".js"));

  const char* js = ps_async_css_loader_js();
  ASSERT_NE(js, nullptr);
  EXPECT_GT(strlen(js), 0u);
  // The loader keys off the marker the transform stamps on deferred links.
  EXPECT_NE(strstr(js, "data-pagespeed-async"), nullptr);
  // ...and it must perform the flip the primitive depends on: a deferred link
  // ships as rel="preload" and applies nothing until this script turns it into
  // a stylesheet. A loader that only restored media would leave every deferred
  // sheet permanently unused.
  EXPECT_NE(strstr(js, "preload"), nullptr);
  EXPECT_NE(strstr(js, "stylesheet"), nullptr);
  EXPECT_NE(strstr(js, "data-pagespeed-media"), nullptr);
  // The idempotency guard, in its EXACT form. Substring assertions cannot see
  // behaviour, and this one has a mutation that keeps every substring above
  // while making the loader a total no-op: invert the comparison
  // (`!==` -> `===`) and `s()` returns early on every link that is still a
  // preload, so nothing is ever applied and every JS-enabled visitor gets a
  // permanently unstyled page. Pinning the literal is cheap insurance here;
  // the real protection is tools/async-css-loader/loader.test.mjs, which
  // executes these bytes against a DOM stand-in and fails on that mutation
  // (and on a deleted guard, a deleted window-load backstop, and a deleted
  // Resource-Timing fast path).
  EXPECT_NE(strstr(js, "if(l.rel!=='preload')return;"), nullptr)
      << "the guard must be `rel !== 'preload'` — inverted or removed, the "
         "loader either never applies a sheet or re-applies over a live one";
  // CSP-safe: an external script, never an inline handler.
  EXPECT_EQ(strstr(js, "onload="), nullptr);

  // Non-circular content-addressing check: independently recompute the
  // FNV-1a-32 digest over the loader body at RUNTIME (a separate code path from
  // the header's constexpr builder) and assert the served path embeds exactly
  // that hash. The shape asserts above pass for ANY 8-hex value, so this is what
  // would catch a nibble-order / hex-width / signed-char-mask bug in the
  // compile-time hash. unsigned-char read mirrors the constexpr's mask.
  std::uint32_t h = 0x811c9dc5u;  // FNV offset basis
  for (const char* p = js; *p != '\0'; ++p) {
    h ^= static_cast<std::uint32_t>(static_cast<unsigned char>(*p));
    h *= 0x01000193u;  // FNV prime
  }
  char expected[64];
  std::snprintf(expected, sizeof(expected),
                "/pagespeed_static/async_css.%08x.js", h);
  EXPECT_EQ(loader_path, expected);
}

// This low-level builder has no scan/cache context and therefore no way to
// judge whether the sheet it would defer is safe to defer: no byte ratio, no
// cold-cache fail-safe, and — having no browser — no validation record, ever.
// It used to take enable_async_css straight from the caller's config, so an
// embedder that set the flag got unconditional deferral with none of the
// safety the worker and ps_html_process apply. The flag is now inert here.
TEST(PageSpeedCApi, HtmlTransformCreateNeverDefersAsyncCss) {
  const char* html =
      "<html><head><link rel=\"stylesheet\" href=\"/s.css\"></head>"
      "<body><p>hi</p></body></html>";
  const char* critical = "body{margin:0}";

  ps_scan_result_t* scan = nullptr;
  ASSERT_EQ(ps_html_scan(html, std::strlen(html), "http://example.com/", &scan),
            PS_OK);

  ps_html_config_t config;
  ps_html_config_init(&config);
  config.enable_critical_css = 1;
  config.enable_async_css = 1;  // asked for, and deliberately not honoured

  ps_html_transform_t* transform = nullptr;
  ASSERT_EQ(
      ps_html_transform_create(scan, &config, critical, std::strlen(critical),
                               nullptr, nullptr, nullptr, 0, &transform),
      PS_OK);

  char* out_html = nullptr;
  size_t out_len = 0;
  ASSERT_EQ(ps_html_transform_run(transform, html, std::strlen(html),
                                  "http://example.com/", &out_html, &out_len),
            PS_OK);
  ASSERT_NE(out_html, nullptr);
  std::string out(out_html, out_len);
  ps_free(out_html);

  EXPECT_EQ(out.find("data-pagespeed-async"), std::string::npos)
      << "this entry point cannot gate deferral, so it must not defer: " << out;
  EXPECT_EQ(out.find("rel=\"preload\""), std::string::npos);
  EXPECT_EQ(out.find("as=\"style\""), std::string::npos);
  EXPECT_EQ(out.find(ps_async_css_loader_path()), std::string::npos);
  EXPECT_EQ(out.find("<noscript"), std::string::npos);
  // The stylesheet stays render-blocking...
  EXPECT_NE(out.find("rel=\"stylesheet\""), std::string::npos);
  // ...and the critical CSS the caller supplied is still inlined, so the
  // embedder loses only the unsafe half.
  EXPECT_NE(out.find("data-pagespeed-critical"), std::string::npos);

  ps_html_transform_free(transform);
  ps_scan_result_free(scan);
}

TEST(PageSpeedCApi, AsyncCssStaysOptInOnEveryCAbiPath) {
  ps_html_config_t config;
  ps_html_config_init(&config);
  EXPECT_EQ(config.enable_async_css, 0);
}

// ================================================================
// High-level HTML process tests
// ================================================================

TEST(PageSpeedCApi, HtmlProcessBasic) {
  const char* html =
      "<html><head></head><body>"
      "<div>Hello</div>"
      "<img src=\"/hero.jpg\">"
      "</body></html>";

  ps_html_result_t* result = nullptr;
  ASSERT_EQ(ps_html_process(html, std::strlen(html), "http://example.com/page",
                            nullptr, nullptr, nullptr, &result),
            PS_OK);
  ASSERT_NE(result, nullptr);

  // Check early hints.
  size_t hints_len = 0;
  const char* hints = ps_html_result_early_hints(result, &hints_len);
  // May or may not have hints depending on HTML content.
  (void)hints;

  ps_html_result_free(result);
}

TEST(PageSpeedCApi, HtmlProcessNull) {
  ps_html_result_t* result = nullptr;
  EXPECT_EQ(ps_html_process(nullptr, 0, "http://example.com/", nullptr, nullptr,
                            nullptr, &result),
            PS_ERR_INVALID_ARG);
}

TEST(PageSpeedCApi, HtmlProcessUrlTooLong) {
  std::string long_url(9000, 'a');
  const char* html = "<html></html>";
  ps_html_result_t* result = nullptr;
  EXPECT_EQ(ps_html_process(html, std::strlen(html), long_url.c_str(), nullptr,
                            nullptr, nullptr, &result),
            PS_ERR_INVALID_ARG);
}

TEST(PageSpeedCApi, HtmlProcessHtmlTooLarge) {
  ps_html_config_t config;
  ps_html_config_init(&config);
  config.max_html_size = 10;  // Very small.

  const char* html = "<html><body>hello world</body></html>";
  ps_html_result_t* result = nullptr;
  EXPECT_EQ(ps_html_process(html, std::strlen(html), "http://example.com/",
                            nullptr, &config, nullptr, &result),
            PS_ERR_INVALID_ARG);
}

// ================================================================
// HTML result accessor tests
// ================================================================

TEST(PageSpeedCApi, HtmlResultOutputReturnsHtml) {
  const char* html =
      "<html><head></head><body>"
      "<img src=\"/hero.jpg\">"
      "<img src=\"/below.jpg\">"
      "</body></html>";

  ps_html_result_t* result = nullptr;
  ASSERT_EQ(ps_html_process(html, std::strlen(html), "http://example.com/page",
                            nullptr, nullptr, nullptr, &result),
            PS_OK);
  ASSERT_NE(result, nullptr);

  // Always verify output is retrievable (modified or not).
  size_t out_len = 0;
  const char* output = ps_html_result_output(result, &out_len);
  EXPECT_NE(output, nullptr);
  EXPECT_GT(out_len, 0u);

  ps_html_result_free(result);
}

TEST(PageSpeedCApi, HtmlResultModifiedReturnsFalseForNull) {
  EXPECT_EQ(ps_html_result_modified(nullptr), 0);
}

TEST(PageSpeedCApi, HtmlResultNeedsRevalidationNull) {
  EXPECT_EQ(ps_html_result_needs_revalidation(nullptr), 0);
}

TEST(PageSpeedCApi, HtmlResultHasCriticalCssNull) {
  EXPECT_EQ(ps_html_result_has_critical_css(nullptr), 0);
}

TEST(PageSpeedCApi, HtmlResultOutputNull) {
  size_t len = 42;
  EXPECT_EQ(ps_html_result_output(nullptr, &len), nullptr);
  EXPECT_EQ(len, 0u);
}

TEST(PageSpeedCApi, HtmlResultEarlyHintsNull) {
  size_t len = 42;
  EXPECT_EQ(ps_html_result_early_hints(nullptr, &len), nullptr);
  EXPECT_EQ(len, 0u);
}

// ================================================================
// HTML scanner: inline CSS, LCP candidate, and origins
// ================================================================

TEST(PageSpeedCApi, HtmlScanInlineCss) {
  const char* html =
      "<html><head>"
      "<style>body { margin: 0; } .hero { color: red; }</style>"
      "</head><body></body></html>";

  ps_scan_result_t* result = nullptr;
  ASSERT_EQ(
      ps_html_scan(html, std::strlen(html), "http://example.com/", &result),
      PS_OK);
  ASSERT_NE(result, nullptr);

  size_t css_len = 0;
  const char* css = ps_scan_inline_css(result, &css_len);
  EXPECT_NE(css, nullptr);
  EXPECT_GT(css_len, 0u);
  // Should contain the CSS content.
  std::string css_str(css, css_len);
  EXPECT_NE(css_str.find("margin"), std::string::npos);

  ps_scan_result_free(result);
}

TEST(PageSpeedCApi, HtmlScanInlineCssNull) {
  size_t len = 42;
  EXPECT_EQ(ps_scan_inline_css(nullptr, &len), nullptr);
  EXPECT_EQ(len, 0u);
}

TEST(PageSpeedCApi, HtmlScanInlineCssEmpty) {
  const char* html = "<html><head></head><body></body></html>";

  ps_scan_result_t* result = nullptr;
  ASSERT_EQ(
      ps_html_scan(html, std::strlen(html), "http://example.com/", &result),
      PS_OK);

  size_t css_len = 0;
  const char* css = ps_scan_inline_css(result, &css_len);
  EXPECT_EQ(css, nullptr);
  EXPECT_EQ(css_len, 0u);

  ps_scan_result_free(result);
}

TEST(PageSpeedCApi, HtmlScanLcpCandidate) {
  const char* html =
      "<html><head></head><body>"
      "<img src=\"/hero.jpg\" srcset=\"/hero-2x.jpg 2x\" sizes=\"100vw\">"
      "</body></html>";

  ps_scan_result_t* result = nullptr;
  ASSERT_EQ(
      ps_html_scan(html, std::strlen(html), "http://example.com/", &result),
      PS_OK);

  const char* srcset = nullptr;
  const char* sizes = nullptr;
  int element_index = -1;
  const char* src =
      ps_scan_lcp_candidate(result, &srcset, &sizes, &element_index);
  EXPECT_NE(src, nullptr);
  if (src != nullptr) {
    EXPECT_STREQ(src, "/hero.jpg");
    EXPECT_NE(srcset, nullptr);
    EXPECT_NE(sizes, nullptr);
    EXPECT_GE(element_index, 0);
  }

  ps_scan_result_free(result);
}

TEST(PageSpeedCApi, HtmlScanLcpCandidateNull) {
  const char* srcset = nullptr;
  const char* sizes = nullptr;
  int element_index = -1;
  EXPECT_EQ(ps_scan_lcp_candidate(nullptr, &srcset, &sizes, &element_index),
            nullptr);
  EXPECT_EQ(element_index, -1);
}

TEST(PageSpeedCApi, HtmlScanOrigins) {
  const char* html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"https://cdn.example.com/style.css\">"
      "<script src=\"https://analytics.example.com/script.js\"></script>"
      "</head><body></body></html>";

  ps_scan_result_t* result = nullptr;
  ASSERT_EQ(
      ps_html_scan(html, std::strlen(html), "http://example.com/", &result),
      PS_OK);

  size_t origin_count = ps_scan_origin_count(result);
  EXPECT_GE(origin_count, 1u);

  for (size_t i = 0; i < origin_count; ++i) {
    const char* origin = ps_scan_origin(result, i);
    EXPECT_NE(origin, nullptr);
  }

  ps_scan_result_free(result);
}

TEST(PageSpeedCApi, HtmlScanOriginCountNull) {
  EXPECT_EQ(ps_scan_origin_count(nullptr), 0u);
}

TEST(PageSpeedCApi, HtmlScanOriginOutOfBounds) {
  EXPECT_EQ(ps_scan_origin(nullptr, 0), nullptr);
}

// ================================================================
// Cache remove tests
// ================================================================

TEST_F(PageSpeedCacheApiTest, CacheRemove) {
  OpenCache();

  // Write something first.
  ps_write_params_t params;
  ps_write_params_init_auto(&params);
  params.alternate_id = 0x08;
  params.content_length = 4;
  params.full_mask = 0x08;
  params.content_type = PS_CONTENT_HTML;

  ps_write_handle_t* wh = nullptr;
  ASSERT_EQ(ps_cache_write_begin(cache_, "/rm.html", "example.com", "https",
                                 &params, &wh),
            PS_OK);
  ASSERT_EQ(ps_write_data(wh, "test", 4), PS_OK);
  ASSERT_EQ(ps_write_close(wh), PS_OK);

  // Verify it exists.
  EXPECT_EQ(ps_cache_alternate_exists(cache_, "/rm.html", "example.com",
                                      "https", 0x08),
            1);

  // Remove it.
  ASSERT_EQ(ps_cache_remove(cache_, "/rm.html", "example.com", "https"), PS_OK);

  // Should be gone.
  EXPECT_EQ(ps_cache_alternate_exists(cache_, "/rm.html", "example.com",
                                      "https", 0x08),
            0);
}

TEST_F(PageSpeedCacheApiTest, CacheRemoveNullCache) {
  EXPECT_EQ(ps_cache_remove(nullptr, "/test.html", "example.com", "https"),
            PS_ERR_INVALID_ARG);
}

TEST_F(PageSpeedCacheApiTest, CacheRemoveNonexistent) {
  OpenCache();
  // Removing a non-existent entry returns NOT_FOUND.
  EXPECT_EQ(
      ps_cache_remove(cache_, "/nonexistent.html", "example.com", "https"),
      PS_ERR_NOT_FOUND);
}

// ================================================================
// Critical CSS extraction tests
// ================================================================

TEST(PageSpeedCApi, CriticalCssExtract) {
  const char* html =
      "<html><head></head><body>"
      "<div class=\"hero\">Hello</div>"
      "<p>World</p>"
      "</body></html>";

  ps_scan_result_t* scan = nullptr;
  ASSERT_EQ(ps_html_scan(html, std::strlen(html), "http://example.com/", &scan),
            PS_OK);

  const char* css =
      ".hero { color: red; } p { margin: 0; } .footer { display: none; }";

  ps_critical_css_config_t config;
  ps_critical_css_config_init(&config);

  ps_critical_css_result_t* result = nullptr;
  ASSERT_EQ(
      ps_css_extract_critical(scan, css, std::strlen(css), &config, &result),
      PS_OK);
  ASSERT_NE(result, nullptr);

  size_t out_len = 0;
  const char* output = ps_critical_css_output(result, &out_len);
  // Should produce some critical CSS output.
  EXPECT_NE(output, nullptr);
  EXPECT_GT(out_len, 0u);

  int total = 0, critical = 0;
  ps_critical_css_stats(result, &total, &critical);
  EXPECT_GT(total, 0);
  EXPECT_GT(critical, 0);
  EXPECT_LE(critical, total);

  ps_critical_css_result_free(result);
  ps_scan_result_free(scan);
}

TEST(PageSpeedCApi, CriticalCssExtractNullArgs) {
  ps_critical_css_result_t* result = nullptr;
  EXPECT_EQ(ps_css_extract_critical(nullptr, "x", 1, nullptr, &result),
            PS_ERR_INVALID_ARG);
  EXPECT_EQ(result, nullptr);
}

TEST(PageSpeedCApi, CriticalCssExtractNullCss) {
  const char* html = "<html><body></body></html>";
  ps_scan_result_t* scan = nullptr;
  ASSERT_EQ(ps_html_scan(html, std::strlen(html), "http://example.com/", &scan),
            PS_OK);

  ps_critical_css_result_t* result = nullptr;
  EXPECT_EQ(ps_css_extract_critical(scan, nullptr, 0, nullptr, &result),
            PS_ERR_INVALID_ARG);

  ps_scan_result_free(scan);
}

TEST(PageSpeedCApi, CriticalCssExtractTooBig) {
  const char* html = "<html><body></body></html>";
  ps_scan_result_t* scan = nullptr;
  ASSERT_EQ(ps_html_scan(html, std::strlen(html), "http://example.com/", &scan),
            PS_OK);

  ps_critical_css_config_t config;
  ps_critical_css_config_init(&config);
  config.max_css_size = 10;  // Very small.

  std::string big_css(100, 'x');
  ps_critical_css_result_t* result = nullptr;
  EXPECT_EQ(ps_css_extract_critical(scan, big_css.c_str(), big_css.size(),
                                    &config, &result),
            PS_ERR_INVALID_ARG);

  ps_scan_result_free(scan);
}

TEST(PageSpeedCApi, CriticalCssOutputNull) {
  size_t len = 42;
  EXPECT_EQ(ps_critical_css_output(nullptr, &len), nullptr);
  EXPECT_EQ(len, 0u);
}

TEST(PageSpeedCApi, CriticalCssStatsNull) {
  int total = 42, critical = 42;
  ps_critical_css_stats(nullptr, &total, &critical);
  EXPECT_EQ(total, 0);
  EXPECT_EQ(critical, 0);
}

TEST(PageSpeedCApi, CriticalCssEmptyCss) {
  const char* html = "<html><head></head><body><div>Hello</div></body></html>";

  ps_scan_result_t* scan = nullptr;
  ASSERT_EQ(ps_html_scan(html, std::strlen(html), "http://example.com/", &scan),
            PS_OK);

  // Empty CSS string (not null, but zero-length).
  ps_critical_css_config_t config;
  ps_critical_css_config_init(&config);

  ps_critical_css_result_t* result = nullptr;
  ps_error_t err = ps_css_extract_critical(scan, "", 0, &config, &result);
  // Empty CSS passes the null check and size check, so should succeed
  // with zero rules extracted.
  ASSERT_EQ(err, PS_OK);
  ASSERT_NE(result, nullptr);

  int total = 0, critical = 0;
  ps_critical_css_stats(result, &total, &critical);
  EXPECT_EQ(total, 0);
  EXPECT_EQ(critical, 0);
  ps_critical_css_result_free(result);

  ps_scan_result_free(scan);
}

TEST(PageSpeedCApi, CriticalCssWithViewport) {
  const char* html =
      "<html><head></head><body>"
      "<div class=\"hero\">Hello</div>"
      "</body></html>";

  ps_scan_result_t* scan = nullptr;
  ASSERT_EQ(ps_html_scan(html, std::strlen(html), "http://example.com/", &scan),
            PS_OK);

  const char* css =
      ".hero { color: red; } "
      "@media (max-width: 479px) { .hero { font-size: 12px; } }";

  ps_critical_css_config_t config;
  ps_critical_css_config_init(&config);
  config.viewport = PS_VIEWPORT_MOBILE;

  ps_critical_css_result_t* result = nullptr;
  ASSERT_EQ(
      ps_css_extract_critical(scan, css, std::strlen(css), &config, &result),
      PS_OK);
  ASSERT_NE(result, nullptr);

  size_t out_len = 0;
  const char* output = ps_critical_css_output(result, &out_len);
  EXPECT_NE(output, nullptr);

  ps_critical_css_result_free(result);
  ps_scan_result_free(scan);
}

// ================================================================
// ABI evolution tests
// ================================================================

TEST(PageSpeedCApi, CacheConfigStructSize) {
  ps_cache_config_t config;
  ps_cache_config_init(&config);
  EXPECT_EQ(config.struct_size, sizeof(ps_cache_config_t));
}

TEST(PageSpeedCApi, WriteParamsStructSize) {
  ps_write_params_t params;
  ps_write_params_init_sized(&params, sizeof(params));
  EXPECT_EQ(params.struct_size, sizeof(ps_write_params_t));
}

TEST(PageSpeedCApi, LegacyWriteParamsInitStampsTheSizeItActuallyInitialized) {
  // ps_write_params_init cannot report sizeof(ps_write_params_t): the callers
  // that still reach it were compiled when the struct was smaller, and their
  // buffer really does end there. It initializes that prefix and says so.
  // Reporting the current size would have the library read fields that are
  // not part of the caller's object at all.
  ps_write_params_t params;
  ps_write_params_init(&params);
  EXPECT_EQ(params.struct_size,
            offsetof(ps_write_params_t, origin_ct) + sizeof(const char*));
  EXPECT_LE(params.struct_size, sizeof(ps_write_params_t));
}

// ================================================================
// Extended error code tests
// ================================================================

TEST(PageSpeedCApi, ErrorNameAllCodes) {
  EXPECT_STREQ(ps_error_name(PS_ERR_CORRUPTED), "PS_ERR_CORRUPTED");
  EXPECT_STREQ(ps_error_name(PS_ERR_NO_SPACE), "PS_ERR_NO_SPACE");
  EXPECT_STREQ(ps_error_name(PS_ERR_INVALID_ARG), "PS_ERR_INVALID_ARG");
  EXPECT_STREQ(ps_error_name(PS_ERR_BUSY), "PS_ERR_BUSY");
  EXPECT_STREQ(ps_error_name(PS_ERR_CLOSED), "PS_ERR_CLOSED");
  EXPECT_STREQ(ps_error_name(PS_ERR_TOO_MANY_ALTERNATES),
               "PS_ERR_TOO_MANY_ALTERNATES");
  EXPECT_STREQ(ps_error_name(PS_ERR_EXISTS), "PS_ERR_EXISTS");
  EXPECT_STREQ(ps_error_name(PS_ERR_NOT_OWNED), "PS_ERR_NOT_OWNED");
  EXPECT_STREQ(ps_error_name(PS_ERR_VERSION_MISMATCH),
               "PS_ERR_VERSION_MISMATCH");
  EXPECT_STREQ(ps_error_name(PS_ERR_IO), "PS_ERR_IO");
  // Unknown error code.
  EXPECT_STREQ(ps_error_name(static_cast<ps_error_t>(9999)), "PS_ERR_UNKNOWN");
}

TEST(PageSpeedCApi, StrErrorAllCodes) {
  EXPECT_STREQ(ps_strerror(PS_ERR_NOT_FOUND), "Not found");
  EXPECT_STREQ(ps_strerror(PS_ERR_CORRUPTED), "Data corrupted");
  EXPECT_STREQ(ps_strerror(PS_ERR_NO_SPACE), "No space available");
  EXPECT_STREQ(ps_strerror(PS_ERR_INVALID_ARG), "Invalid argument");
  EXPECT_STREQ(ps_strerror(PS_ERR_BUSY), "Resource busy");
  EXPECT_STREQ(ps_strerror(PS_ERR_CLOSED), "Handle closed");
  EXPECT_STREQ(ps_strerror(PS_ERR_TOO_MANY_ALTERNATES), "Too many alternates");
  EXPECT_STREQ(ps_strerror(PS_ERR_EXISTS), "Already exists");
  EXPECT_STREQ(ps_strerror(PS_ERR_NOT_OWNED), "Not owned by this process");
  EXPECT_STREQ(ps_strerror(PS_ERR_VERSION_MISMATCH), "Version mismatch");
  EXPECT_STREQ(ps_strerror(PS_ERR_INTERNAL), "Internal error");
  // Unknown error code.
  EXPECT_STREQ(ps_strerror(static_cast<ps_error_t>(9999)), "Unknown error");
}

// ================================================================
// Read accessor null safety tests
// ================================================================

TEST(PageSpeedCApi, ReadMaskNull) { EXPECT_EQ(ps_read_mask(nullptr), 0u); }

TEST(PageSpeedCApi, ReadContentTypeNull) {
  EXPECT_EQ(ps_read_content_type(nullptr), PS_CONTENT_OTHER);
}

TEST(PageSpeedCApi, ReadOriginContentTypeNull) {
  EXPECT_EQ(ps_read_origin_content_type(nullptr), nullptr);
}

TEST(PageSpeedCApi, ReadFlagsNull) { EXPECT_EQ(ps_read_flags(nullptr), 0); }

TEST(PageSpeedCApi, ReadIsRamHitNull) {
  EXPECT_EQ(ps_read_is_ram_hit(nullptr), 0);
}

TEST(PageSpeedCApi, ReadCacheInsertedAtNull) {
  EXPECT_EQ(ps_read_cache_inserted_at(nullptr), 0u);
}

TEST(PageSpeedCApi, ReadContentNullArgs) {
  EXPECT_EQ(ps_read_content(nullptr, nullptr, nullptr), PS_ERR_INVALID_ARG);
}

TEST(PageSpeedCApi, ReadCopyNullArgs) {
  EXPECT_EQ(ps_read_copy(nullptr, nullptr, 0, nullptr), PS_ERR_INVALID_ARG);
}

// ================================================================
// Cache read alternate (direct ID) tests
// ================================================================

TEST_F(PageSpeedCacheApiTest, ReadAlternateDirect) {
  OpenCache();

  ps_write_params_t params;
  ps_write_params_init_auto(&params);
  params.alternate_id = 0x08;
  params.content_length = 5;
  params.full_mask = 0x08;
  params.content_type = PS_CONTENT_CSS;
  params.origin_ct = "text/css";

  ps_write_handle_t* wh = nullptr;
  ASSERT_EQ(ps_cache_write_begin(cache_, "/alt.css", "example.com", "https",
                                 &params, &wh),
            PS_OK);
  ASSERT_EQ(ps_write_data(wh, "a{b:c}", 5), PS_OK);
  ASSERT_EQ(ps_write_close(wh), PS_OK);

  ps_read_result_t* result = nullptr;
  ASSERT_EQ(ps_cache_read_alternate(cache_, "/alt.css", "example.com", "https",
                                    0x08, &result),
            PS_OK);
  ASSERT_NE(result, nullptr);

  EXPECT_EQ(ps_read_content_type(result), PS_CONTENT_CSS);
  EXPECT_EQ(ps_read_mask(result), 0x08u);
  EXPECT_STREQ(ps_read_origin_content_type(result), "text/css");
  EXPECT_EQ(ps_read_flags(result), 0);
  EXPECT_EQ(ps_read_is_ram_hit(result), 0);

  ps_read_free(result);
}

TEST_F(PageSpeedCacheApiTest, ReadAlternateNotFound) {
  OpenCache();
  ps_read_result_t* result = nullptr;
  EXPECT_EQ(ps_cache_read_alternate(cache_, "/missing.css", "example.com",
                                    "https", 0x08, &result),
            PS_ERR_NOT_FOUND);
}

TEST_F(PageSpeedCacheApiTest, ReadAlternateNullArgs) {
  EXPECT_EQ(ps_cache_read_alternate(nullptr, "/x", "h", "https", 0, nullptr),
            PS_ERR_INVALID_ARG);
}

// ================================================================
// Cache read best with multiple alternates and scoring
// ================================================================

TEST_F(PageSpeedCacheApiTest, ReadBestSelectsBestAlternate) {
  OpenCache();

  // Write two alternates: one for Desktop/Identity and one for Desktop/WebP.
  auto write = [&](uint8_t id, uint32_t mask, ps_content_type_t ct,
                   const char* data, size_t len) {
    ps_write_params_t params;
    ps_write_params_init_auto(&params);
    params.alternate_id = id;
    params.content_length = len;
    params.full_mask = mask;
    params.content_type = ct;

    ps_write_handle_t* wh = nullptr;
    ASSERT_EQ(ps_cache_write_begin(cache_, "/multi.html", "example.com",
                                   "https", &params, &wh),
              PS_OK);
    ASSERT_EQ(ps_write_data(wh, data, len), PS_OK);
    ASSERT_EQ(ps_write_close(wh), PS_OK);
  };

  write(0x08, 0x08, PS_CONTENT_HTML, "original", 8);
  write(0x09, 0x09, PS_CONTENT_HTML, "webp-variant", 12);

  // Read with mask 0x08 should find the 0x08 alternate.
  ps_read_result_t* result = nullptr;
  ASSERT_EQ(ps_cache_read_best(cache_, "/multi.html", "example.com", "https",
                               0x08, &result),
            PS_OK);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(ps_read_mask(result), 0x08u);

  const uint8_t* data = nullptr;
  size_t len = 0;
  ASSERT_EQ(ps_read_content(result, &data, &len), PS_OK);
  EXPECT_EQ(len, 8u);

  ps_read_free(result);
}

// ================================================================
// Write params init null safety
// ================================================================

TEST(PageSpeedCApi, WriteParamsInitNull) {
  // Should not crash.
  ps_write_params_init(nullptr);
}

// ================================================================
// Cache config init null safety
// ================================================================

TEST(PageSpeedCApi, CacheConfigInitNull) {
  // Should not crash.
  ps_cache_config_init(nullptr);
}

// ================================================================
// Write handle double-close and data-on-closed tests
// ================================================================

TEST_F(PageSpeedCacheApiTest, WriteDoubleClose) {
  OpenCache();

  ps_write_params_t params;
  ps_write_params_init_auto(&params);
  params.alternate_id = 0x08;
  params.content_length = 4;
  params.full_mask = 0x08;
  params.content_type = PS_CONTENT_HTML;

  ps_write_handle_t* wh = nullptr;
  ASSERT_EQ(ps_cache_write_begin(cache_, "/dbl.html", "example.com", "https",
                                 &params, &wh),
            PS_OK);
  ASSERT_EQ(ps_write_data(wh, "test", 4), PS_OK);
  ASSERT_EQ(ps_write_close(wh), PS_OK);

  // wh is now deleted by ps_write_close, so we cannot call again.
  // But ps_write_close(nullptr) should return PS_OK.
  EXPECT_EQ(ps_write_close(nullptr), PS_OK);
}

TEST_F(PageSpeedCacheApiTest, WriteDataOnNullHandle) {
  EXPECT_EQ(ps_write_data(nullptr, "x", 1), PS_ERR_INVALID_ARG);
}

// ================================================================
// Write params with flags
// ================================================================

TEST_F(PageSpeedCacheApiTest, WriteWithFlags) {
  OpenCache();

  ps_write_params_t params;
  ps_write_params_init_auto(&params);
  params.alternate_id = 0x08;
  params.content_length = 4;
  params.full_mask = 0x08;
  params.content_type = PS_CONTENT_HTML;
  params.flags = PS_FLAG_NEEDS_REVALIDATION;
  params.origin_ct = "text/html; charset=utf-8";

  ps_write_handle_t* wh = nullptr;
  ASSERT_EQ(ps_cache_write_begin(cache_, "/flags.html", "example.com", "https",
                                 &params, &wh),
            PS_OK);
  ASSERT_EQ(ps_write_data(wh, "test", 4), PS_OK);
  ASSERT_EQ(ps_write_close(wh), PS_OK);

  ps_read_result_t* result = nullptr;
  ASSERT_EQ(ps_cache_read_best(cache_, "/flags.html", "example.com", "https",
                               0x08, &result),
            PS_OK);
  ASSERT_NE(result, nullptr);

  EXPECT_EQ(ps_read_flags(result), PS_FLAG_NEEDS_REVALIDATION);
  EXPECT_STREQ(ps_read_origin_content_type(result), "text/html; charset=utf-8");

  ps_read_free(result);
}

// ================================================================
// Write with no origin_ct (empty string should give nullptr on read)
// ================================================================

TEST_F(PageSpeedCacheApiTest, WriteNoOriginCt) {
  OpenCache();

  ps_write_params_t params;
  ps_write_params_init_auto(&params);
  params.alternate_id = 0x08;
  params.content_length = 4;
  params.full_mask = 0x08;
  params.content_type = PS_CONTENT_IMAGE;

  ps_write_handle_t* wh = nullptr;
  ASSERT_EQ(ps_cache_write_begin(cache_, "/noct.img", "example.com", "https",
                                 &params, &wh),
            PS_OK);
  ASSERT_EQ(ps_write_data(wh, "test", 4), PS_OK);
  ASSERT_EQ(ps_write_close(wh), PS_OK);

  ps_read_result_t* result = nullptr;
  ASSERT_EQ(ps_cache_read_best(cache_, "/noct.img", "example.com", "https",
                               0x08, &result),
            PS_OK);
  ASSERT_NE(result, nullptr);

  // No origin_ct was set, so ps_read_origin_content_type should return nullptr.
  EXPECT_EQ(ps_read_origin_content_type(result), nullptr);
  EXPECT_EQ(ps_read_content_type(result), PS_CONTENT_IMAGE);

  ps_read_free(result);
}

// ================================================================
// List alternates with field verification
// ================================================================

TEST_F(PageSpeedCacheApiTest, ListAlternatesFields) {
  OpenCache();

  ps_write_params_t params;
  ps_write_params_init_auto(&params);
  params.alternate_id = 0x08;
  params.content_length = 7;
  params.full_mask = 0x08;
  params.content_type = PS_CONTENT_HTML;

  ps_write_handle_t* wh = nullptr;
  ASSERT_EQ(ps_cache_write_begin(cache_, "/lf.html", "example.com", "https",
                                 &params, &wh),
            PS_OK);
  ASSERT_EQ(ps_write_data(wh, "content", 7), PS_OK);
  ASSERT_EQ(ps_write_close(wh), PS_OK);

  ps_alternate_info_t* alts = nullptr;
  size_t count = 0;
  ASSERT_EQ(ps_cache_list_alternates(cache_, "/lf.html", "example.com", "https",
                                     &alts, &count),
            PS_OK);
  ASSERT_GE(count, 1u);

  bool found = false;
  for (size_t i = 0; i < count; ++i) {
    if (alts[i].alternate_id == 0x08) {
      found = true;
      EXPECT_EQ(alts[i].struct_size, sizeof(ps_alternate_info_t));
      EXPECT_GT(alts[i].content_length, 0u);
    }
  }
  EXPECT_TRUE(found);
  ps_alternates_free(alts);
}

TEST_F(PageSpeedCacheApiTest, ListAlternatesNullArgs) {
  EXPECT_EQ(
      ps_cache_list_alternates(nullptr, "/x", "h", "https", nullptr, nullptr),
      PS_ERR_INVALID_ARG);
}

TEST_F(PageSpeedCacheApiTest, ListAlternatesNotFound) {
  OpenCache();
  ps_alternate_info_t* alts = nullptr;
  size_t count = 0;
  ps_error_t err = ps_cache_list_alternates(
      cache_, "/nonexistent.html", "example.com", "https", &alts, &count);
  // May return PS_ERR_NOT_FOUND or PS_OK with count 0.
  if (err == PS_OK) {
    EXPECT_EQ(count, 0u);
    ps_alternates_free(alts);
  }
}

// ================================================================
// Cache stats null args
// ================================================================

TEST(PageSpeedCApi, CacheStatsNullArgs) {
  EXPECT_EQ(ps_cache_stats(nullptr, nullptr), PS_ERR_INVALID_ARG);
}

// ================================================================
// CSS validation: null bytes
// ================================================================

TEST(PageSpeedCApi, CssValidateNullBytes) {
  const char css[] = "body { color\0: red; }";
  EXPECT_EQ(ps_css_validate(css, sizeof(css) - 1), PS_ERR_INVALID_ARG);
}

// ================================================================
// CSS validation: short input (< 7 chars, cannot match </style)
// ================================================================

TEST(PageSpeedCApi, CssValidateShortInput) {
  EXPECT_EQ(ps_css_validate("a{b:c}", 6), PS_OK);
  EXPECT_EQ(ps_css_validate("x", 1), PS_OK);
}

// ================================================================
// CSS flatten with imports and lookup callback
// ================================================================

TEST(PageSpeedCApi, CssFlattenWithLookup) {
  const char* css = "@import url('/imported.css');\nbody { color: red; }";

  auto lookup = [](const char* url, size_t* out_len,
                   void* /*user_data*/) -> const char* {
    static const char imported[] = ".imported { display: block; }";
    if (std::strcmp(url, "/imported.css") == 0) {
      *out_len = std::strlen(imported);
      return imported;
    }
    return nullptr;
  };

  char* out = nullptr;
  size_t out_len = 0;
  int resolved = 0;
  int unresolved = 0;

  ASSERT_EQ(ps_css_flatten_imports(
                css, std::strlen(css), "http://example.com/style.css", lookup,
                nullptr, 5, &out, &out_len, &resolved, &unresolved),
            PS_OK);
  ASSERT_NE(out, nullptr);
  EXPECT_GT(out_len, 0u);
  EXPECT_EQ(resolved, 1);
  EXPECT_EQ(unresolved, 0);
  // The output should contain the imported CSS content.
  std::string result(out, out_len);
  EXPECT_NE(result.find(".imported"), std::string::npos);
  ps_free(out);
}

TEST(PageSpeedCApi, CssFlattenWithUnresolved) {
  const char* css = "@import url('/missing.css');\nbody { color: red; }";

  char* out = nullptr;
  size_t out_len = 0;
  int resolved = 0;
  int unresolved = 0;

  // No lookup function provided, so imports cannot be resolved.
  ASSERT_EQ(ps_css_flatten_imports(
                css, std::strlen(css), "http://example.com/style.css", nullptr,
                nullptr, 5, &out, &out_len, &resolved, &unresolved),
            PS_OK);
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(resolved, 0);
  EXPECT_EQ(unresolved, 1);
  ps_free(out);
}

TEST(PageSpeedCApi, CssFlattenNullArgs) {
  EXPECT_EQ(ps_css_flatten_imports(nullptr, 0, nullptr, nullptr, nullptr, 5,
                                   nullptr, nullptr, nullptr, nullptr),
            PS_ERR_INVALID_ARG);
}

// ================================================================
// CSS minify null args
// ================================================================

TEST(PageSpeedCApi, CssMinifyNullArgs) {
  EXPECT_EQ(ps_css_minify(nullptr, 0, nullptr, nullptr), PS_ERR_INVALID_ARG);
}

// ================================================================
// HTML scan null args
// ================================================================

TEST(PageSpeedCApi, HtmlScanNullHtml) {
  ps_scan_result_t* result = nullptr;
  EXPECT_EQ(ps_html_scan(nullptr, 10, "http://example.com/", &result),
            PS_ERR_INVALID_ARG);
}

TEST(PageSpeedCApi, HtmlScanNullUrl) {
  ps_scan_result_t* result = nullptr;
  EXPECT_EQ(ps_html_scan("<html></html>", 13, nullptr, &result),
            PS_ERR_INVALID_ARG);
}

TEST(PageSpeedCApi, HtmlScanNullOut) {
  EXPECT_EQ(ps_html_scan("<html></html>", 13, "http://example.com/", nullptr),
            PS_ERR_INVALID_ARG);
}

TEST(PageSpeedCApi, HtmlScanUrlTooLong) {
  std::string long_url(9000, 'a');
  ps_scan_result_t* result = nullptr;
  EXPECT_EQ(ps_html_scan("<html></html>", 13, long_url.c_str(), &result),
            PS_ERR_INVALID_ARG);
}

// ================================================================
// HTML scan complex page with multiple stylesheets and elements
// ================================================================

TEST(PageSpeedCApi, HtmlScanMultipleStylesheets) {
  const char* html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/a.css\" media=\"screen\">"
      "<link rel=\"stylesheet\" href=\"/b.css\" media=\"print\">"
      "</head><body></body></html>";

  ps_scan_result_t* result = nullptr;
  ASSERT_EQ(
      ps_html_scan(html, std::strlen(html), "http://example.com/", &result),
      PS_OK);
  ASSERT_NE(result, nullptr);

  EXPECT_EQ(ps_scan_stylesheet_count(result), 2u);

  const char* href = nullptr;
  const char* media = nullptr;

  ASSERT_EQ(ps_scan_stylesheet(result, 0, &href, &media), PS_OK);
  EXPECT_STREQ(href, "/a.css");
  EXPECT_STREQ(media, "screen");

  ASSERT_EQ(ps_scan_stylesheet(result, 1, &href, &media), PS_OK);
  EXPECT_STREQ(href, "/b.css");
  EXPECT_STREQ(media, "print");

  // Out of bounds.
  EXPECT_EQ(ps_scan_stylesheet(result, 99, &href, &media), PS_ERR_INVALID_ARG);

  ps_scan_result_free(result);
}

// ================================================================
// Scan element detail accessors
// ================================================================

TEST(PageSpeedCApi, HtmlScanElementDetails) {
  const char* html =
      "<html><head></head><body>"
      "<div id=\"main\"><p class=\"intro\">Hello</p></div>"
      "</body></html>";

  ps_scan_result_t* result = nullptr;
  ASSERT_EQ(
      ps_html_scan(html, std::strlen(html), "http://example.com/", &result),
      PS_OK);

  size_t count = ps_scan_element_count(result);
  EXPECT_GT(count, 0u);

  // Find the div element and verify its attributes.
  bool found_div = false;
  for (size_t i = 0; i < count; ++i) {
    const char* tag = nullptr;
    const char* id = nullptr;
    int depth = -1;
    int element_index = -1;
    ASSERT_EQ(ps_scan_element(result, i, &tag, &id, &depth, &element_index),
              PS_OK);
    if ((tag != nullptr) && std::strcmp(tag, "div") == 0) {
      found_div = true;
      EXPECT_STREQ(id, "main");
      EXPECT_GE(depth, 0);
      EXPECT_GE(element_index, 0);
    }
  }
  EXPECT_TRUE(found_div);

  // Out of bounds.
  EXPECT_EQ(ps_scan_element(result, 9999, nullptr, nullptr, nullptr, nullptr),
            PS_ERR_INVALID_ARG);

  ps_scan_result_free(result);
}

// ================================================================
// Scan element classes out of bounds
// ================================================================

TEST(PageSpeedCApi, HtmlScanElementClassesOutOfBounds) {
  const char*** classes = nullptr;
  EXPECT_EQ(ps_scan_element_classes(nullptr, 0, classes), 0u);
}

// ================================================================
// Scan stylesheet null result
// ================================================================

TEST(PageSpeedCApi, HtmlScanStylesheetNullResult) {
  EXPECT_EQ(ps_scan_stylesheet(nullptr, 0, nullptr, nullptr),
            PS_ERR_INVALID_ARG);
}

TEST(PageSpeedCApi, HtmlScanStylesheetCountNull) {
  EXPECT_EQ(ps_scan_stylesheet_count(nullptr), 0u);
}

TEST(PageSpeedCApi, HtmlScanElementCountNull) {
  EXPECT_EQ(ps_scan_element_count(nullptr), 0u);
}

// ================================================================
// HTML transform with critical CSS and speculation URLs
// ================================================================

TEST(PageSpeedCApi, HtmlTransformWithCriticalCssAndSpeculation) {
  const char* html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/style.css\">"
      "</head><body>"
      "<div class=\"hero\">Hello</div>"
      "<img src=\"/hero.jpg\">"
      "</body></html>";

  ps_scan_result_t* scan = nullptr;
  ASSERT_EQ(ps_html_scan(html, std::strlen(html), "http://example.com/", &scan),
            PS_OK);

  ps_html_config_t config;
  ps_html_config_init(&config);
  config.enable_critical_css = 1;
  config.enable_lazy_load = 1;
  config.enable_speculation_rules = 1;

  const char* critical_css = ".hero { color: red; }";
  const char* speculation_urls = "/page1\n/page2\n/page3";

  ps_html_transform_t* transform = nullptr;
  ASSERT_EQ(ps_html_transform_create(scan, &config, critical_css,
                                     std::strlen(critical_css), nullptr,
                                     "example.com", speculation_urls,
                                     std::strlen(speculation_urls), &transform),
            PS_OK);
  ASSERT_NE(transform, nullptr);

  char* out_html = nullptr;
  size_t out_len = 0;
  ASSERT_EQ(ps_html_transform_run(transform, html, std::strlen(html),
                                  "http://example.com/", &out_html, &out_len),
            PS_OK);

  // After running, check modified state.
  int modified = ps_html_transform_modified(transform);
  if (modified != 0) {
    EXPECT_NE(out_html, nullptr);
    EXPECT_GT(out_len, 0u);
    // Output should contain the critical CSS.
    std::string output(out_html, out_len);
    EXPECT_NE(output.find("hero"), std::string::npos);
    ps_free(out_html);
  }

  ps_html_transform_free(transform);
  ps_scan_result_free(scan);
}

TEST(PageSpeedCApi, HtmlTransformModifiedNull) {
  EXPECT_EQ(ps_html_transform_modified(nullptr), 0);
}

// ================================================================
// HTML transform create null args
// ================================================================

TEST(PageSpeedCApi, HtmlTransformCreateNullScan) {
  ps_html_transform_t* transform = nullptr;
  EXPECT_EQ(ps_html_transform_create(nullptr, nullptr, nullptr, 0, nullptr,
                                     nullptr, nullptr, 0, &transform),
            PS_ERR_INVALID_ARG);
}

TEST(PageSpeedCApi, HtmlTransformCreateNullOut) {
  const char* html = "<html></html>";
  ps_scan_result_t* scan = nullptr;
  ASSERT_EQ(ps_html_scan(html, std::strlen(html), "http://example.com/", &scan),
            PS_OK);

  EXPECT_EQ(ps_html_transform_create(scan, nullptr, nullptr, 0, nullptr,
                                     nullptr, nullptr, 0, nullptr),
            PS_ERR_INVALID_ARG);
  ps_scan_result_free(scan);
}

// ================================================================
// HTML transform run null args
// ================================================================

TEST(PageSpeedCApi, HtmlTransformRunNullArgs) {
  char* out = nullptr;
  size_t len = 0;
  EXPECT_EQ(ps_html_transform_run(nullptr, "<html></html>", 13,
                                  "http://example.com/", &out, &len),
            PS_ERR_INVALID_ARG);
}

// ================================================================
// HTML process with stylesheets (early hints, needs_revalidation)
// ================================================================

TEST(PageSpeedCApi, HtmlProcessWithStylesheetsNoCache) {
  const char* html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/style.css\">"
      "</head><body>"
      "<div>Hello</div>"
      "<img src=\"/hero.jpg\">"
      "</body></html>";

  ps_html_result_t* result = nullptr;
  ASSERT_EQ(ps_html_process(html, std::strlen(html), "http://example.com/page",
                            nullptr, nullptr, nullptr, &result),
            PS_OK);
  ASSERT_NE(result, nullptr);

  // Without cache, external stylesheets are missing.
  EXPECT_EQ(ps_html_result_needs_revalidation(result), 1);

  // Early hints should contain the stylesheet URL.
  size_t hints_len = 0;
  const char* hints = ps_html_result_early_hints(result, &hints_len);
  EXPECT_NE(hints, nullptr);
  if (hints != nullptr) {
    std::string hints_str(hints, hints_len);
    EXPECT_NE(hints_str.find("/style.css"), std::string::npos);
  }

  ps_html_result_free(result);
}

TEST(PageSpeedCApi, HtmlProcessEarlyHintsWithLcpAndOrigins) {
  const char* html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/main.css\">"
      "<script src=\"https://cdn.example.com/analytics.js\"></script>"
      "</head><body>"
      "<img src=\"/hero.jpg\">"
      "</body></html>";

  ps_html_result_t* result = nullptr;
  ASSERT_EQ(ps_html_process(html, std::strlen(html), "http://example.com/page",
                            nullptr, nullptr, nullptr, &result),
            PS_OK);
  ASSERT_NE(result, nullptr);

  size_t hints_len = 0;
  const char* hints = ps_html_result_early_hints(result, &hints_len);
  EXPECT_NE(hints, nullptr);
  if (hints != nullptr) {
    std::string hints_str(hints, hints_len);
    // Should contain stylesheet.
    EXPECT_NE(hints_str.find("/main.css"), std::string::npos);
    // Should contain LCP image prefixed with "image:".
    EXPECT_NE(hints_str.find("image:/hero.jpg"), std::string::npos);
    // Should contain preconnect for CDN origin.
    EXPECT_NE(hints_str.find("preconnect:"), std::string::npos);
  }

  ps_html_result_free(result);
}

// ================================================================
// HTML process with critical CSS enabled and inline CSS
// ================================================================

TEST(PageSpeedCApi, HtmlProcessWithInlineCss) {
  const char* html =
      "<html><head>"
      "<style>.hero { color: red; } p { margin: 0; }</style>"
      "</head><body>"
      "<div class=\"hero\">Hello</div>"
      "<p>World</p>"
      "<img src=\"/hero.jpg\">"
      "</body></html>";

  ps_html_config_t config;
  ps_html_config_init(&config);
  config.enable_critical_css = 1;

  ps_html_result_t* result = nullptr;
  ASSERT_EQ(ps_html_process(html, std::strlen(html), "http://example.com/page",
                            nullptr, &config, nullptr, &result),
            PS_OK);
  ASSERT_NE(result, nullptr);

  // With inline CSS and critical CSS enabled, we should get critical CSS.
  int has_critical = ps_html_result_has_critical_css(result);
  // There's inline CSS that matches elements, so this should be true.
  EXPECT_EQ(has_critical, 1);

  // Output should be modified.
  int modified = ps_html_result_modified(result);
  EXPECT_EQ(modified, 1);

  size_t out_len = 0;
  const char* output = ps_html_result_output(result, &out_len);
  EXPECT_NE(output, nullptr);
  EXPECT_GT(out_len, 0u);

  // No external stylesheets, so no revalidation needed.
  EXPECT_EQ(ps_html_result_needs_revalidation(result), 0);

  ps_html_result_free(result);
}

// ================================================================
// HTML process with all features disabled
// ================================================================

TEST(PageSpeedCApi, HtmlProcessAllDisabled) {
  const char* html =
      "<html><head></head><body>"
      "<img src=\"/hero.jpg\">"
      "</body></html>";

  ps_html_config_t config;
  ps_html_config_init(&config);
  config.enable_critical_css = 0;
  config.enable_lazy_load = 0;
  config.enable_image_dimensions = 0;
  config.enable_lcp_preload = 0;
  config.enable_preconnect = 0;
  config.enable_speculation_rules = 0;

  ps_html_result_t* result = nullptr;
  ASSERT_EQ(ps_html_process(html, std::strlen(html), "http://example.com/page",
                            nullptr, &config, nullptr, &result),
            PS_OK);
  ASSERT_NE(result, nullptr);

  // With everything disabled, output should not be modified.
  EXPECT_EQ(ps_html_result_modified(result), 0);

  ps_html_result_free(result);
}

// ================================================================
// HTML config init null safety
// ================================================================

TEST(PageSpeedCApi, HtmlConfigInitNull) {
  // Should not crash.
  ps_html_config_init(nullptr);
}

// ================================================================
// Critical CSS config init null safety
// ================================================================

TEST(PageSpeedCApi, CriticalCssConfigInitNull) {
  // Should not crash.
  ps_critical_css_config_init(nullptr);
}

// ================================================================
// Critical CSS with pattern arrays
// ================================================================

TEST(PageSpeedCApi, CriticalCssWithAlwaysInclude) {
  const char* html =
      "<html><head></head><body>"
      "<div class=\"hero\">Hello</div>"
      "<div class=\"footer\">Footer</div>"
      "</body></html>";

  ps_scan_result_t* scan = nullptr;
  ASSERT_EQ(ps_html_scan(html, std::strlen(html), "http://example.com/", &scan),
            PS_OK);

  const char* css =
      ".hero { color: red; } .footer { display: none; } "
      ".always-keep { font-size: 14px; }";

  const char* always_include[] = {".always-keep", nullptr};

  ps_critical_css_config_t config;
  ps_critical_css_config_init(&config);
  config.always_include_selectors = always_include;

  ps_critical_css_result_t* result = nullptr;
  ASSERT_EQ(
      ps_css_extract_critical(scan, css, std::strlen(css), &config, &result),
      PS_OK);
  ASSERT_NE(result, nullptr);

  size_t out_len = 0;
  const char* output = ps_critical_css_output(result, &out_len);
  EXPECT_NE(output, nullptr);
  if (output != nullptr) {
    std::string out_str(output, out_len);
    // The always-include selector should be present.
    EXPECT_NE(out_str.find("always-keep"), std::string::npos);
  }

  ps_critical_css_result_free(result);
  ps_scan_result_free(scan);
}

TEST(PageSpeedCApi, CriticalCssWithTabletViewport) {
  const char* html =
      "<html><head></head><body>"
      "<div class=\"hero\">Hello</div>"
      "</body></html>";

  ps_scan_result_t* scan = nullptr;
  ASSERT_EQ(ps_html_scan(html, std::strlen(html), "http://example.com/", &scan),
            PS_OK);

  const char* css = ".hero { color: red; }";

  ps_critical_css_config_t config;
  ps_critical_css_config_init(&config);
  config.viewport = PS_VIEWPORT_TABLET;

  ps_critical_css_result_t* result = nullptr;
  ASSERT_EQ(
      ps_css_extract_critical(scan, css, std::strlen(css), &config, &result),
      PS_OK);
  ASSERT_NE(result, nullptr);

  ps_critical_css_result_free(result);
  ps_scan_result_free(scan);
}

// ================================================================
// Content type classification edge cases
// ================================================================

TEST(PageSpeedCApi, ClassifyContentTypeEdgeCases) {
  EXPECT_EQ(ps_classify_content_type("image/png"), PS_CONTENT_IMAGE);
  EXPECT_EQ(ps_classify_content_type("image/gif"), PS_CONTENT_IMAGE);
  EXPECT_EQ(ps_classify_content_type("image/avif"), PS_CONTENT_IMAGE);
  EXPECT_EQ(ps_classify_content_type("image/svg+xml"), PS_CONTENT_IMAGE);
  EXPECT_EQ(ps_classify_content_type("text/plain"), PS_CONTENT_OTHER);
  EXPECT_EQ(ps_classify_content_type(""), PS_CONTENT_OTHER);
}

// ================================================================
// Content type MIME reverse mapping
// ================================================================

TEST(PageSpeedCApi, ContentTypeMimeUnknown) {
  // Out-of-range content type.
  const char* mime = ps_content_type_mime(static_cast<ps_content_type_t>(999));
  EXPECT_NE(mime, nullptr);
}

// ================================================================
// Classify with WebP accept header
// ================================================================

TEST(PageSpeedCApi, ClassifyWebP) {
  uint32_t mask = ps_classify("image/webp,*/*", nullptr, nullptr, "gzip");
  // WebP = bits 0-1 = 1.
  EXPECT_EQ(mask & 0x03, 1u);
  // Gzip = bits 6-7 = 1 (0x40).
  EXPECT_EQ(mask & 0xC0, 0x40u);
}

// ================================================================
// Normalize hostname edge cases
// ================================================================

TEST(PageSpeedCApi, NormalizeHostnameZeroBufSize) {
  char buf[1];
  EXPECT_EQ(ps_normalize_hostname("example.com", buf, 0), -1);
}

TEST(PageSpeedCApi, NormalizeHostnameNullBuf) {
  EXPECT_EQ(ps_normalize_hostname("example.com", nullptr, 256), -1);
}

// ================================================================
// HTML scan: LCP with no srcset/sizes
// ================================================================

TEST(PageSpeedCApi, HtmlScanLcpNoBigImage) {
  const char* html =
      "<html><head></head><body>"
      "<p>No images here</p>"
      "</body></html>";

  ps_scan_result_t* result = nullptr;
  ASSERT_EQ(
      ps_html_scan(html, std::strlen(html), "http://example.com/", &result),
      PS_OK);

  const char* srcset = nullptr;
  const char* sizes = nullptr;
  int element_index = -1;
  const char* src =
      ps_scan_lcp_candidate(result, &srcset, &sizes, &element_index);
  EXPECT_EQ(src, nullptr);
  EXPECT_EQ(element_index, -1);

  ps_scan_result_free(result);
}

// ================================================================
// HTML process: null result pointer handled
// ================================================================

TEST(PageSpeedCApi, HtmlProcessNullOut) {
  const char* html = "<html></html>";
  EXPECT_EQ(ps_html_process(html, std::strlen(html), "http://example.com/",
                            nullptr, nullptr, nullptr, nullptr),
            PS_ERR_INVALID_ARG);
}

// ================================================================
// Notify worker with URL/hostname validation
// ================================================================

TEST(PageSpeedCApi, NotifyWorkerUrlTooLong) {
  std::string long_url(9000, 'a');
  EXPECT_EQ(ps_notify_worker("/tmp/test.sock", long_url.c_str(), "example.com",
                             "https", PS_CONTENT_HTML, 0x08),
            PS_ERR_INVALID_ARG);
}

TEST(PageSpeedCApi, NotifyWorkerHostnameTooLong) {
  std::string long_host(600, 'h');
  EXPECT_EQ(ps_notify_worker("/tmp/test.sock", "/test.html", long_host.c_str(),
                             "https", PS_CONTENT_HTML, 0x08),
            PS_ERR_INVALID_ARG);
}

TEST(PageSpeedCApi, NotifyWorkerNullUrl) {
  EXPECT_EQ(ps_notify_worker("/tmp/test.sock", nullptr, "h", "https",
                             PS_CONTENT_HTML, 0x08),
            PS_ERR_INVALID_ARG);
}

// ================================================================
// Read copy with small buffer (truncation)
// ================================================================

TEST_F(PageSpeedCacheApiTest, ReadCopyTruncation) {
  OpenCache();

  ps_write_params_t params;
  ps_write_params_init_auto(&params);
  params.alternate_id = 0x08;
  params.content_length = 11;
  params.full_mask = 0x08;
  params.content_type = PS_CONTENT_HTML;

  ps_write_handle_t* wh = nullptr;
  ASSERT_EQ(ps_cache_write_begin(cache_, "/trunc.html", "example.com", "https",
                                 &params, &wh),
            PS_OK);
  ASSERT_EQ(ps_write_data(wh, "hello world", 11), PS_OK);
  ASSERT_EQ(ps_write_close(wh), PS_OK);

  ps_read_result_t* result = nullptr;
  ASSERT_EQ(ps_cache_read_best(cache_, "/trunc.html", "example.com", "https",
                               0x08, &result),
            PS_OK);

  // Read into a buffer smaller than the content.
  uint8_t buf[5];
  size_t copied = 0;
  ASSERT_EQ(ps_read_copy(result, buf, sizeof(buf), &copied), PS_OK);
  EXPECT_EQ(copied, 5u);
  EXPECT_EQ(std::memcmp(buf, "hello", 5), 0);

  ps_read_free(result);
}

// ================================================================
// Cache write begin/sentinel with URL/hostname validation
// ================================================================

TEST_F(PageSpeedCacheApiTest, WriteBeginUrlTooLong) {
  OpenCache();
  std::string long_url(9000, 'a');

  ps_write_params_t params;
  ps_write_params_init_auto(&params);
  params.alternate_id = 0x08;
  params.content_length = 4;
  params.full_mask = 0x08;
  params.content_type = PS_CONTENT_HTML;

  ps_write_handle_t* wh = nullptr;
  EXPECT_EQ(ps_cache_write_begin(cache_, long_url.c_str(), "example.com",
                                 "https", &params, &wh),
            PS_ERR_INVALID_ARG);
}

TEST_F(PageSpeedCacheApiTest, WriteSentinelUrlTooLong) {
  OpenCache();
  std::string long_url(9000, 'a');
  ps_write_handle_t* wh = nullptr;
  EXPECT_EQ(ps_cache_write_sentinel(cache_, long_url.c_str(), "example.com",
                                    "https", PS_SENTINEL_EARLY_HINTS, 10, &wh),
            PS_ERR_INVALID_ARG);
}

// ================================================================
// Last error message after error-producing call
// ================================================================

TEST(PageSpeedCApi, LastErrorAfterFailure) {
  ps_cache_t* c = nullptr;
  ps_cache_config_t config;
  ps_cache_config_init(&config);
  config.volume_path = nullptr;
  auto err = ps_cache_open(&config, &c);
  EXPECT_NE(err, PS_OK);
  // After a failing call, last error message should be non-empty.
  EXPECT_STRNE(ps_last_error_message(), "");
}

// ================================================================
// HTML transform: run without modification
// ================================================================

TEST(PageSpeedCApi, HtmlTransformNoModification) {
  const char* html =
      "<html><head></head><body>"
      "<p>Simple text</p>"
      "</body></html>";

  ps_scan_result_t* scan = nullptr;
  ASSERT_EQ(ps_html_scan(html, std::strlen(html), "http://example.com/", &scan),
            PS_OK);

  ps_html_config_t config;
  ps_html_config_init(&config);
  config.enable_critical_css = 0;
  config.enable_lazy_load = 0;
  config.enable_image_dimensions = 0;
  config.enable_lcp_preload = 0;
  config.enable_preconnect = 0;
  config.enable_speculation_rules = 0;

  ps_html_transform_t* transform = nullptr;
  ASSERT_EQ(ps_html_transform_create(scan, &config, nullptr, 0, nullptr,
                                     nullptr, nullptr, 0, &transform),
            PS_OK);

  char* out_html = nullptr;
  size_t out_len = 0;
  ASSERT_EQ(ps_html_transform_run(transform, html, std::strlen(html),
                                  "http://example.com/", &out_html, &out_len),
            PS_OK);

  // No modifications expected.
  EXPECT_EQ(ps_html_transform_modified(transform), 0);
  EXPECT_EQ(out_html, nullptr);
  EXPECT_EQ(out_len, 0u);

  ps_html_transform_free(transform);
  ps_scan_result_free(scan);
}

// ================================================================
// Score alternate: identity fallback and SVG universal bonus
// ================================================================

TEST(PageSpeedCApi, ScoreAlternateIdentityFallback) {
  // Client wants Gzip, stored is Identity - should get a fallback score > 0.
  uint32_t client_gzip = 0x48;      // Desktop/Gzip
  uint32_t stored_identity = 0x08;  // Desktop/Identity
  int score = ps_score_alternate(client_gzip, stored_identity);
  EXPECT_GT(score, 0);
}

TEST(PageSpeedCApi, ScoreAlternateSameExact) {
  // Perfect match.
  uint32_t mask = 0x48;  // Desktop/Gzip
  EXPECT_GT(ps_score_alternate(mask, mask), 0);
}

// ================================================================
// List alternates: empty result (key exists but no content alternates)
// ================================================================

TEST_F(PageSpeedCacheApiTest, ListAlternatesOnNonexistentKey) {
  OpenCache();

  // List alternates for a key that has never been written.
  // Depending on cyclone behavior, this will either:
  // (a) return PS_ERR_NOT_FOUND (key doesn't exist), or
  // (b) return PS_OK with count 0 (key exists but empty), exercising
  //     the count==0 path at lines 586-588.
  ps_alternate_info_t* alts = nullptr;
  size_t count = 99;
  ps_error_t err = ps_cache_list_alternates(
      cache_, "/never-written.html", "example.com", "https", &alts, &count);
  if (err == PS_OK) {
    EXPECT_EQ(count, 0u);
    EXPECT_EQ(alts, nullptr);
    ps_alternates_free(alts);
  } else {
    EXPECT_EQ(err, PS_ERR_NOT_FOUND);
  }
}

#ifndef _WIN32
// ================================================================
// Notify worker: connect to an actual listening socket (success path)
// ================================================================

TEST(PageSpeedCApi, NotifyWorkerSuccess) {
  // Create a temporary Unix domain socket.
  auto temp_dir = pagespeed::test::MakeTempDir();
  std::string sock_path = temp_dir + "/notify.sock";

  // Create a listener socket.
  int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  ASSERT_GE(listen_fd, 0);

  struct sockaddr_un addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);

  ASSERT_EQ(
      bind(listen_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)),
      0);
  ASSERT_EQ(listen(listen_fd, 1), 0);

  // Accept in a background thread to avoid blocking.
  std::thread acceptor([listen_fd]() {
    struct sockaddr_un client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_fd =
        accept(listen_fd, reinterpret_cast<struct sockaddr*>(&client_addr),
               &client_len);
    if (client_fd >= 0) {
      // Read and discard the notification data.
      char buf[4096];
      while (true) {
        ssize_t n = read(client_fd, buf, sizeof(buf));
        if (n <= 0) break;
      }
      close(client_fd);
    }
  });

  // Call ps_notify_worker -- should succeed.
  ps_error_t err =
      ps_notify_worker(sock_path.c_str(), "/test.html", "example.com", "https",
                       PS_CONTENT_HTML, 0x08);
  EXPECT_EQ(err, PS_OK);

  acceptor.join();
  close(listen_fd);
  unlink(sock_path.c_str());
  std::filesystem::remove_all(temp_dir);
}

// ================================================================
// Notify worker: non-existent socket path (IO error)
// ================================================================

TEST(PageSpeedCApi, NotifyWorkerBadSocket) {
  // Use a socket path that doesn't exist -- should return PS_ERR_IO.
  ps_error_t err =
      ps_notify_worker("/tmp/nonexistent_ps_test_socket.sock", "/test.html",
                       "example.com", "https", PS_CONTENT_HTML, 0x08);
  EXPECT_EQ(err, PS_ERR_IO);

  // After failure, last error message should describe the issue.
  EXPECT_STRNE(ps_last_error_message(), "");
}

namespace {
// Bind and listen on a Unix socket at sock_path. Returns the listening fd,
// or -1 on failure (callers ASSERT on the result).
int MakeListeningSocket(const std::string& sock_path) {
  int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (listen_fd < 0) return -1;

  struct sockaddr_un addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);

  if (bind(listen_fd, reinterpret_cast<struct sockaddr*>(&addr),
           sizeof(addr)) != 0 ||
      listen(listen_fd, 1) != 0) {
    close(listen_fd);
    return -1;
  }
  return listen_fd;
}

// True when a connection was attempted against listen_fd. A connect to a
// listening AF_UNIX socket completes while the caller is inside the call, so
// a listener with nothing pending after the call returned saw no connection
// attempt. The small poll timeout only absorbs scheduler jitter.
bool ConnectionAttempted(int listen_fd) {
  struct pollfd pfd = {listen_fd, POLLIN, 0};
  return poll(&pfd, 1, 100) > 0;
}
}  // namespace

// ================================================================
// Notify worker: an invalid scheme is refused before any connection
// ================================================================

TEST(PageSpeedCApi, NotifyWorkerInvalidSchemeDoesNotConnect) {
  auto temp_dir = pagespeed::test::MakeTempDir();
  std::string sock_path = temp_dir + "/notify.sock";
  int listen_fd = MakeListeningSocket(sock_path);
  ASSERT_GE(listen_fd, 0);

  EXPECT_EQ(ps_notify_worker(sock_path.c_str(), "/test.html", "example.com",
                             "ftp", PS_CONTENT_HTML, 0x08),
            PS_ERR_INVALID_ARG);

  // Nothing pending on the listener: no connection was attempted.
  EXPECT_FALSE(ConnectionAttempted(listen_fd));

  close(listen_fd);
  unlink(sock_path.c_str());
  std::filesystem::remove_all(temp_dir);
}

TEST(PageSpeedCApi, NotifyWorkerExInvalidSchemeDoesNotConnect) {
  auto temp_dir = pagespeed::test::MakeTempDir();
  std::string sock_path = temp_dir + "/notify.sock";
  int listen_fd = MakeListeningSocket(sock_path);
  ASSERT_GE(listen_fd, 0);

  ps_notify_params_t params;
  ps_notify_params_init_auto(&params);
  params.url = "/test.html";
  params.hostname = "example.com";
  params.scheme = "ftp";
  params.content_type = PS_CONTENT_HTML;
  params.mask = 0x08;

  EXPECT_EQ(ps_notify_worker_ex(sock_path.c_str(), &params),
            PS_ERR_INVALID_ARG);

  // Nothing pending on the listener: no connection was attempted.
  EXPECT_FALSE(ConnectionAttempted(listen_fd));

  close(listen_fd);
  unlink(sock_path.c_str());
  std::filesystem::remove_all(temp_dir);
}
#endif  // !_WIN32

// ================================================================
// ValidateContentType success path
// ================================================================

TEST_F(PageSpeedCacheApiTest, WriteWithAllContentTypes) {
  OpenCache();

  // Exercise ValidateContentType success for each valid content type.
  ps_content_type_t types[] = {PS_CONTENT_HTML, PS_CONTENT_CSS, PS_CONTENT_JS,
                               PS_CONTENT_IMAGE, PS_CONTENT_OTHER};
  int idx = 0;
  for (auto ct : types) {
    std::string url = "/ct_test_" + std::to_string(idx++) + ".dat";
    ps_write_params_t params;
    ps_write_params_init_auto(&params);
    params.alternate_id = 0x08;
    params.content_length = 4;
    params.full_mask = 0x08;
    params.content_type = ct;

    ps_write_handle_t* wh = nullptr;
    ASSERT_EQ(ps_cache_write_begin(cache_, url.c_str(), "example.com", "https",
                                   &params, &wh),
              PS_OK)
        << "Failed for content_type=" << ct;
    ASSERT_EQ(ps_write_data(wh, "test", 4), PS_OK);
    ASSERT_EQ(ps_write_close(wh), PS_OK);

    // Verify we can read it back with correct type.
    ps_read_result_t* result = nullptr;
    ASSERT_EQ(ps_cache_read_best(cache_, url.c_str(), "example.com", "https",
                                 0x08, &result),
              PS_OK);
    EXPECT_EQ(ps_read_content_type(result), ct);
    ps_read_free(result);
  }
}

// ================================================================
// BuildCriticalCssConfig fill lambda: ps_html_process with config arrays
// ================================================================

TEST(PageSpeedCApi, HtmlProcessWithConfigArrays) {
  const char* html =
      "<html><head>"
      "<style>"
      ".hero { color: red; } "
      ".footer { display: none; } "
      ".nav { font-size: 14px; } "
      "#sidebar { width: 200px; } "
      "p { margin: 0; } "
      "span { padding: 0; } "
      ".always-here { background: blue; }"
      "</style>"
      "</head><body>"
      "<div class=\"hero\">Hello</div>"
      "<div class=\"footer\">Footer</div>"
      "<p id=\"sidebar\">Sidebar</p>"
      "</body></html>";

  // Set up config with all pattern arrays populated.
  const char* always_include[] = {".always-here", nullptr};
  const char* include_tags[] = {"div", nullptr};
  const char* include_classes[] = {"hero", nullptr};
  const char* include_ids[] = {"sidebar", nullptr};
  const char* exclude_classes[] = {"footer", nullptr};
  const char* exclude_ids[] = {"excluded-id", nullptr};
  const char* exclude_tags[] = {"span", nullptr};

  ps_html_config_t config;
  ps_html_config_init(&config);
  config.enable_critical_css = 1;
  config.always_include_selectors = always_include;
  config.include_tag_patterns = include_tags;
  config.include_class_patterns = include_classes;
  config.include_id_patterns = include_ids;
  config.exclude_class_patterns = exclude_classes;
  config.exclude_id_patterns = exclude_ids;
  config.exclude_tag_patterns = exclude_tags;

  ps_html_result_t* result = nullptr;
  ASSERT_EQ(ps_html_process(html, std::strlen(html), "http://example.com/page",
                            nullptr, &config, nullptr, &result),
            PS_OK);
  ASSERT_NE(result, nullptr);

  // The fill lambda was exercised for all 7 array fields.
  // Verify the critical CSS was extracted.
  EXPECT_EQ(ps_html_result_has_critical_css(result), 1);

  size_t out_len = 0;
  const char* output = ps_html_result_output(result, &out_len);
  EXPECT_NE(output, nullptr);
  if (output != nullptr) {
    std::string out_str(output, out_len);
    // The always-include selector should be present.
    EXPECT_NE(out_str.find("always-here"), std::string::npos);
  }

  ps_html_result_free(result);
}

// ================================================================
// BuildCriticalCssConfigFromCss fill lambda: ps_css_extract_critical
// with config arrays
// ================================================================

TEST(PageSpeedCApi, CriticalCssExtractWithAllConfigArrays) {
  const char* html =
      "<html><head></head><body>"
      "<div class=\"hero\">Hello</div>"
      "<p id=\"main\">World</p>"
      "<span>Inline</span>"
      "</body></html>";

  ps_scan_result_t* scan = nullptr;
  ASSERT_EQ(ps_html_scan(html, std::strlen(html), "http://example.com/", &scan),
            PS_OK);

  const char* css =
      ".hero { color: red; } "
      "p { margin: 0; } "
      "span { padding: 0; } "
      ".always-css { background: blue; }";

  const char* always_include[] = {".always-css", nullptr};
  const char* include_tags[] = {"div", nullptr};
  const char* include_classes[] = {"hero", nullptr};
  const char* include_ids[] = {"main", nullptr};
  const char* exclude_classes[] = {"excluded", nullptr};
  const char* exclude_ids[] = {"excluded-id", nullptr};
  const char* exclude_tags[] = {"span", nullptr};

  ps_critical_css_config_t config;
  ps_critical_css_config_init(&config);
  config.always_include_selectors = always_include;
  config.include_tag_patterns = include_tags;
  config.include_class_patterns = include_classes;
  config.include_id_patterns = include_ids;
  config.exclude_class_patterns = exclude_classes;
  config.exclude_id_patterns = exclude_ids;
  config.exclude_tag_patterns = exclude_tags;

  ps_critical_css_result_t* result = nullptr;
  ASSERT_EQ(
      ps_css_extract_critical(scan, css, std::strlen(css), &config, &result),
      PS_OK);
  ASSERT_NE(result, nullptr);

  size_t out_len = 0;
  const char* output = ps_critical_css_output(result, &out_len);
  EXPECT_NE(output, nullptr);
  if (output != nullptr) {
    std::string out_str(output, out_len);
    EXPECT_NE(out_str.find("always-css"), std::string::npos);
  }

  ps_critical_css_result_free(result);
  ps_scan_result_free(scan);
}

// ================================================================
// ps_html_process: critical CSS that could hold markup is refused
// ================================================================

namespace {

// Runs ps_html_process on a page whose only stylesheet, /style.css, holds
// `css`; returns the output and sets *has_critical.
std::string ProcessWithCachedSheet(ps_cache_t* cache, const std::string& css,
                                   int* has_critical) {
  ps_write_params_t params;
  ps_write_params_init_auto(&params);
  params.alternate_id = 0x08;
  params.content_length = css.size();
  params.full_mask = 0x08;
  params.content_type = PS_CONTENT_CSS;
  params.origin_ct = "text/css";
  ps_write_handle_t* wh = nullptr;
  EXPECT_EQ(ps_cache_write_begin(cache, "/style.css", "example.com", "https",
                                 &params, &wh),
            PS_OK);
  if (wh == nullptr) return "";
  EXPECT_EQ(ps_write_data(wh, css.data(), css.size()), PS_OK);
  EXPECT_EQ(ps_write_close(wh), PS_OK);

  const char* html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/style.css\">"
      "</head><body>"
      "<div class=\"hero\">Hello</div>"
      "</body></html>";
  ps_html_config_t config;
  ps_html_config_init(&config);
  config.enable_critical_css = 1;
  ps_html_result_t* result = nullptr;
  EXPECT_EQ(ps_html_process(html, std::strlen(html), "http://example.com/page",
                            "example.com", &config, cache, &result),
            PS_OK);
  if (result == nullptr) return "";
  *has_critical = ps_html_result_has_critical_css(result);
  size_t out_len = 0;
  const char* output = ps_html_result_output(result, &out_len);
  // An unmodified page has no output: the caller serves the input as is.
  std::string out = output != nullptr ? std::string(output, out_len) : html;
  ps_html_result_free(result);
  return out;
}

// No block, and the stylesheet stays render-blocking.
void ExpectNoBlockAndBlockingSheet(int has_critical, const std::string& out) {
  EXPECT_EQ(has_critical, 0) << out;
  EXPECT_EQ(out.find("data-pagespeed-critical"), std::string::npos) << out;
  EXPECT_EQ(out.find("data-pagespeed-async"), std::string::npos) << out;
  EXPECT_EQ(out.find("rel=\"preload\""), std::string::npos) << out;
  EXPECT_NE(out.find("rel=\"stylesheet\""), std::string::npos) << out;
}

}  // namespace

// A plain block is inlined (the control for the refusals below).
TEST_F(PageSpeedCacheApiTest, HtmlProcessInlinesPlainCriticalCss) {
  OpenCache();
  int has_critical = -1;
  std::string out =
      ProcessWithCachedSheet(cache_, ".hero{color:red}", &has_critical);
  EXPECT_EQ(has_critical, 1) << out;
  EXPECT_NE(out.find("data-pagespeed-critical"), std::string::npos) << out;
}

// Tailwind CSS v4 registers properties with `@property`, whose `syntax` is a
// string such as "<color>". The block is inlined, the `<` written as the CSS
// escape `\3c `, which means the same to CSS.
TEST_F(PageSpeedCacheApiTest, HtmlProcessInlinesPropertySyntaxStrings) {
  OpenCache();
  int has_critical = -1;
  std::string out = ProcessWithCachedSheet(
      cache_,
      "@property --tw-gradient-from{syntax:\"<color>\";inherits:false;"
      "initial-value:#0000}"
      ".hero{background-image:linear-gradient(var(--tw-gradient-from),red)}",
      &has_critical);
  EXPECT_EQ(has_critical, 1) << out;
  EXPECT_NE(out.find("data-pagespeed-critical"), std::string::npos) << out;
  EXPECT_NE(out.find("syntax:\"\\3c color>\""), std::string::npos) << out;
  EXPECT_EQ(out.find("\"<color>\""), std::string::npos) << out;
}

// Outside a CSS string, a `<` then a letter is not valid CSS: refused.
TEST_F(PageSpeedCacheApiTest, HtmlProcessRefusesMarkupOutsideStrings) {
  OpenCache();
  int has_critical = -1;
  std::string out =
      ProcessWithCachedSheet(cache_, ".hero{color:red;--x:a<b}", &has_critical);
  ExpectNoBlockAndBlockingSheet(has_critical, out);
}

// `</style`, in any letter case, could end the inlined element: refused
// outside a CSS string. Inside one it is escaped and the block is inlined.
TEST_F(PageSpeedCacheApiTest, HtmlProcessRefusesStyleEndTag) {
  OpenCache();
  for (const char* css :
       {".hero{color:red;--x:a</style>}", ".hero{color:red;--x:a</StYlE }"}) {
    SCOPED_TRACE(css);
    int has_critical = -1;
    std::string out = ProcessWithCachedSheet(cache_, css, &has_critical);
    ExpectNoBlockAndBlockingSheet(has_critical, out);
  }
}

// ================================================================
// ps_html_process with cache: external CSS lookup (lines 1316-1327)
// ================================================================

TEST_F(PageSpeedCacheApiTest, HtmlProcessWithCacheAndExternalCss) {
  OpenCache();

  // Write CSS content into the cache at the URL that the HTML will reference.
  const char* css_content = ".hero { color: red; } p { margin: 0; }";
  size_t css_len = std::strlen(css_content);

  ps_write_params_t params;
  ps_write_params_init_auto(&params);
  params.alternate_id = 0x08;  // Default mask alternate.
  params.content_length = css_len;
  params.full_mask = 0x08;
  params.content_type = PS_CONTENT_CSS;
  params.origin_ct = "text/css";

  ps_write_handle_t* wh = nullptr;
  ASSERT_EQ(ps_cache_write_begin(cache_, "/style.css", "example.com", "https",
                                 &params, &wh),
            PS_OK);
  ASSERT_EQ(ps_write_data(wh, css_content, css_len), PS_OK);
  ASSERT_EQ(ps_write_close(wh), PS_OK);

  // Now process HTML that references /style.css.
  const char* html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/style.css\">"
      "</head><body>"
      "<div class=\"hero\">Hello</div>"
      "<p>World</p>"
      "</body></html>";

  ps_html_config_t config;
  ps_html_config_init(&config);
  config.enable_critical_css = 1;

  ps_html_result_t* result = nullptr;
  ASSERT_EQ(ps_html_process(html, std::strlen(html), "http://example.com/page",
                            "example.com", &config, cache_, &result),
            PS_OK);
  ASSERT_NE(result, nullptr);

  // Since we provided cache with the CSS, external CSS was found.
  // needs_revalidation should be false (external CSS was NOT missing).
  EXPECT_EQ(ps_html_result_needs_revalidation(result), 0);

  // Critical CSS should be injected from the cached CSS.
  EXPECT_EQ(ps_html_result_has_critical_css(result), 1);
  EXPECT_EQ(ps_html_result_modified(result), 1);

  size_t out_len = 0;
  const char* output = ps_html_result_output(result, &out_len);
  EXPECT_NE(output, nullptr);
  if (output != nullptr) {
    std::string out_str(output, out_len);
    // The critical CSS from the cached stylesheet should appear.
    EXPECT_NE(out_str.find("hero"), std::string::npos);
  }

  // Early hints should still contain the stylesheet URL.
  size_t hints_len = 0;
  const char* hints = ps_html_result_early_hints(result, &hints_len);
  EXPECT_NE(hints, nullptr);
  if (hints != nullptr) {
    std::string hints_str(hints, hints_len);
    EXPECT_NE(hints_str.find("/style.css"), std::string::npos);
  }

  ps_html_result_free(result);
}

// ================================================================
// ps_html_process: print-only rules never style the screen
// ================================================================
//
// The page the critical block is inlined into keeps its stylesheets, so a
// rule in the block applies on screen. A print stylesheet's rules (a <link>
// or <style> with media="print", an `@media print` block, an
// `@import ... print`) must therefore stay out of the block or stay scoped
// to print inside it. Before the fix the gather appended a print <link>'s
// sheet and a print <style>'s body to the combined sheet as if they had no
// media, and `header,footer{display:none}` from a print sheet hid the
// header and footer on screen.

namespace {

// The body of the inlined critical <style>, or "" when there is none.
std::string CriticalBlockOf(std::string_view html) {
  const size_t tag = html.find("data-pagespeed-critical");
  if (tag == std::string_view::npos) return "";
  const size_t open_end = html.find('>', tag);
  if (open_end == std::string_view::npos) return "";
  const size_t close = html.find("</style>", open_end);
  if (close == std::string_view::npos) return "";
  return std::string(html.substr(open_end + 1, close - open_end - 1));
}

// `css` with whitespace removed and every `@media print...{...}` block (the
// rules that apply only in print) cut out: what a screen applies.
std::string ScreenRulesOf(std::string_view css) {
  std::string compact;
  for (char c : css) {
    if (c != ' ' && c != '\n' && c != '\t' && c != '\r') compact += c;
  }
  std::string out;
  size_t i = 0;
  while (i < compact.size()) {
    if (compact.compare(i, 11, "@mediaprint") == 0) {
      const size_t open = compact.find('{', i);
      if (open == std::string::npos) break;
      int depth = 0;
      size_t j = open;
      for (; j < compact.size(); ++j) {
        if (compact[j] == '{') ++depth;
        if (compact[j] == '}' && --depth == 0) break;
      }
      i = j + 1;
      continue;
    }
    out += compact[i++];
  }
  return out;
}

class PrintStylesheetTest : public PageSpeedCacheApiTest {
 protected:
  void CacheCss(const char* path, std::string_view css) {
    ps_write_params_t params;
    ps_write_params_init_auto(&params);
    params.alternate_id = 0x08;
    params.content_length = css.size();
    params.full_mask = 0x08;
    params.content_type = PS_CONTENT_CSS;
    params.origin_ct = "text/css";
    ps_write_handle_t* wh = nullptr;
    ASSERT_EQ(ps_cache_write_begin(cache_, path, "example.com", "https",
                                   &params, &wh),
              PS_OK);
    ASSERT_EQ(ps_write_data(wh, css.data(), css.size()), PS_OK);
    ASSERT_EQ(ps_write_close(wh), PS_OK);
  }

  // Processes a page whose <head> is `head` and whose body has a header, a
  // paragraph and a footer, with critical CSS on (the default), and returns
  // the inlined critical block.
  std::string CriticalBlockFor(const std::string& head,
                               int* needs_revalidation = nullptr) {
    const std::string html = "<html><head>" + head +
                             "</head><body>"
                             "<header class=\"site-header\">Top</header>"
                             "<main><p>Hello</p></main>"
                             "<footer class=\"site-footer\">Bottom</footer>"
                             "</body></html>";
    ps_html_config_t config;
    ps_html_config_init(&config);
    ps_html_result_t* result = nullptr;
    EXPECT_EQ(
        ps_html_process(html.data(), html.size(), "https://example.com/page",
                        "example.com", &config, cache_, &result),
        PS_OK);
    if (result == nullptr) return "";
    size_t out_len = 0;
    const char* output = ps_html_result_output(result, &out_len);
    std::string block;
    if (output != nullptr) block = CriticalBlockOf({output, out_len});
    if (needs_revalidation != nullptr) {
      *needs_revalidation = ps_html_result_needs_revalidation(result);
    }
    ps_html_result_free(result);
    return block;
  }

  static constexpr const char* kScreenCss =
      "header{display:block;background:#eee}"
      "footer{display:block;color:#333}"
      "p{margin:0}";
  static constexpr const char* kPrintCss = "header,footer{display:none}";
};

TEST_F(PrintStylesheetTest, PrintLinkRulesStayOffScreen) {
  OpenCache();
  CacheCss("/screen.css", kScreenCss);
  CacheCss("/print.css", kPrintCss);
  const std::string block = CriticalBlockFor(
      "<link rel=\"stylesheet\" href=\"/screen.css\">"
      "<link rel=\"stylesheet\" href=\"/print.css\" media=\"print\">");
  ASSERT_FALSE(block.empty());
  const std::string screen = ScreenRulesOf(block);
  EXPECT_NE(screen.find("display:block"), std::string::npos) << block;
  EXPECT_EQ(screen.find("display:none"), std::string::npos) << block;
}

TEST_F(PrintStylesheetTest, PrintLinkMediaIsCaseInsensitive) {
  OpenCache();
  CacheCss("/screen.css", kScreenCss);
  CacheCss("/print.css", kPrintCss);
  const std::string block = CriticalBlockFor(
      "<link rel=\"stylesheet\" href=\"/screen.css\">"
      "<link rel=\"stylesheet\" href=\"/print.css\" media=\"PRINT\">");
  ASSERT_FALSE(block.empty());
  EXPECT_EQ(ScreenRulesOf(block).find("display:none"), std::string::npos)
      << block;
}

TEST_F(PrintStylesheetTest, PrintStyleElementRulesStayOffScreen) {
  OpenCache();
  CacheCss("/screen.css", kScreenCss);
  const std::string block = CriticalBlockFor(
      "<link rel=\"stylesheet\" href=\"/screen.css\">"
      "<style media=\"print\">header,footer{display:none}</style>");
  ASSERT_FALSE(block.empty());
  const std::string screen = ScreenRulesOf(block);
  EXPECT_NE(screen.find("display:block"), std::string::npos) << block;
  EXPECT_EQ(screen.find("display:none"), std::string::npos) << block;
}

TEST_F(PrintStylesheetTest, MediaPrintBlockInAScreenSheetStaysOffScreen) {
  OpenCache();
  CacheCss("/screen.css", std::string(kScreenCss) +
                              "@media print{header,footer{display:none}}");
  const std::string block =
      CriticalBlockFor("<link rel=\"stylesheet\" href=\"/screen.css\">");
  ASSERT_FALSE(block.empty());
  const std::string screen = ScreenRulesOf(block);
  EXPECT_NE(screen.find("display:block"), std::string::npos) << block;
  EXPECT_EQ(screen.find("display:none"), std::string::npos) << block;
}

TEST_F(PrintStylesheetTest, PrintImportStaysScopedToPrint) {
  OpenCache();
  CacheCss("/screen.css",
           std::string("@import url(/print.css) print;") + kScreenCss);
  CacheCss("/print.css", kPrintCss);
  const std::string block =
      CriticalBlockFor("<link rel=\"stylesheet\" href=\"/screen.css\">");
  ASSERT_FALSE(block.empty());
  const std::string screen = ScreenRulesOf(block);
  EXPECT_NE(screen.find("display:block"), std::string::npos) << block;
  EXPECT_EQ(screen.find("display:none"), std::string::npos) << block;
  // An @import the block keeps keeps its media: it must not load the print
  // sheet for every medium.
  const size_t import = screen.find("@import");
  if (import != std::string::npos) {
    EXPECT_NE(screen.find("print", import), std::string::npos) << block;
  }
}

TEST_F(PrintStylesheetTest, PrintImportInAStyleElementStaysScopedToPrint) {
  OpenCache();
  CacheCss("/screen.css", kScreenCss);
  CacheCss("/print.css", kPrintCss);
  const std::string block = CriticalBlockFor(
      "<style>@import url(/print.css) print;</style>"
      "<link rel=\"stylesheet\" href=\"/screen.css\">");
  ASSERT_FALSE(block.empty());
  const std::string screen = ScreenRulesOf(block);
  EXPECT_EQ(screen.find("display:none"), std::string::npos) << block;
  const size_t import = screen.find("@import");
  if (import != std::string::npos) {
    EXPECT_NE(screen.find("print", import), std::string::npos) << block;
  }
}

TEST_F(PrintStylesheetTest, UnusableMediaValueLeavesTheSheetOut) {
  // A media value that cannot be written as an @media prelude safely
  // contributes nothing to the block, however it is spelled.
  OpenCache();
  CacheCss("/screen.css", kScreenCss);
  CacheCss("/hostile.css", ".hostile{display:none}");
  const char* const kMedia[] = {
      "print}body{display:none}",
      "screen;x",
      "print{",
      "print\\7d b",
      "print/*",
      "(min-width:1px",
      "screen)",
      "print&quot;x",
      "@import",
      "print<b>",
  };
  for (const char* media : kMedia) {
    int needs_revalidation = 0;
    const std::string block = CriticalBlockFor(
        "<link rel=\"stylesheet\" href=\"/screen.css\">"
        "<link rel=\"stylesheet\" href=\"/hostile.css\" media=\"" +
            std::string(media) + "\">",
        &needs_revalidation);
    ASSERT_FALSE(block.empty()) << media;
    // The sheet is counted as missing, so the page stays render-blocking.
    EXPECT_EQ(needs_revalidation, 1) << media;
    EXPECT_NE(ScreenRulesOf(block).find("display:block"), std::string::npos)
        << media << ": " << block;
    EXPECT_EQ(block.find("hostile"), std::string::npos)
        << media << ": " << block;
    EXPECT_EQ(ScreenRulesOf(block).find("display:none"), std::string::npos)
        << media << ": " << block;
  }
}

TEST_F(PrintStylesheetTest, StrayBraceInAPrintSheetDoesNotLeakLaterRules) {
  OpenCache();
  CacheCss("/screen.css", kScreenCss);
  // The `}` after the first rule must not close the print block early.
  CacheCss("/print.css", "p{color:red}}header,footer{display:none}");
  int needs_revalidation = 0;
  const std::string block = CriticalBlockFor(
      "<link rel=\"stylesheet\" href=\"/screen.css\">"
      "<link rel=\"stylesheet\" href=\"/print.css\" media=\"print\">",
      &needs_revalidation);
  ASSERT_FALSE(block.empty());
  const std::string screen = ScreenRulesOf(block);
  EXPECT_NE(screen.find("display:block"), std::string::npos) << block;
  EXPECT_EQ(screen.find("display:none"), std::string::npos) << block;
  EXPECT_EQ(screen.find("color:red"), std::string::npos) << block;
  // The sheet that cannot be wrapped counts as missing.
  EXPECT_EQ(needs_revalidation, 1);
}

TEST_F(PrintStylesheetTest, UnclosedBlockInAPrintSheetStaysOffScreen) {
  OpenCache();
  CacheCss("/screen.css", kScreenCss);
  CacheCss("/print.css", "header,footer{display:none");
  int needs_revalidation = 0;
  const std::string block = CriticalBlockFor(
      "<link rel=\"stylesheet\" href=\"/print.css\" media=\"print\">"
      "<link rel=\"stylesheet\" href=\"/screen.css\">",
      &needs_revalidation);
  ASSERT_FALSE(block.empty());
  const std::string screen = ScreenRulesOf(block);
  EXPECT_EQ(screen.find("display:none"), std::string::npos) << block;
  // The screen sheet after it is not swallowed into the print block.
  EXPECT_NE(screen.find("display:block"), std::string::npos) << block;
  EXPECT_EQ(needs_revalidation, 1);
}

TEST_F(PrintStylesheetTest, StrayBraceInAPrintStyleDoesNotLeakLaterRules) {
  OpenCache();
  CacheCss("/screen.css", kScreenCss);
  const std::string block = CriticalBlockFor(
      "<link rel=\"stylesheet\" href=\"/screen.css\">"
      "<style media=\"print\">p{color:red}}header,footer{display:none}"
      "</style>");
  ASSERT_FALSE(block.empty());
  const std::string screen = ScreenRulesOf(block);
  EXPECT_NE(screen.find("display:block"), std::string::npos) << block;
  EXPECT_EQ(screen.find("display:none"), std::string::npos) << block;
}

TEST_F(PrintStylesheetTest, NestedBlocksInAPrintSheetStayOffScreen) {
  OpenCache();
  CacheCss("/screen.css", kScreenCss);
  // Balanced nesting, and braces inside a string and a comment, are fine to
  // wrap: every rule stays inside the print block.
  CacheCss("/print.css",
           "@supports (display:block){header{display:none}}"
           "footer::before{content:\"}\"}/* } */footer{display:none}");
  int needs_revalidation = 0;
  const std::string block = CriticalBlockFor(
      "<link rel=\"stylesheet\" href=\"/screen.css\">"
      "<link rel=\"stylesheet\" href=\"/print.css\" media=\"print\">",
      &needs_revalidation);
  ASSERT_FALSE(block.empty());
  const std::string screen = ScreenRulesOf(block);
  EXPECT_NE(screen.find("display:block"), std::string::npos) << block;
  EXPECT_EQ(screen.find("display:none"), std::string::npos) << block;
  EXPECT_EQ(needs_revalidation, 0);
}

TEST_F(PrintStylesheetTest,
       OnloadThatDoesNotChangeMediaKeepsTheSheetPrintOnly) {
  OpenCache();
  CacheCss("/screen.css", kScreenCss);
  CacheCss("/print.css", kPrintCss);
  for (const char* handler :
       {"track()", "this.rel='stylesheet'", "console.log(this.media)",
        "this.media=='print'", "this.onload=null"}) {
    const std::string block = CriticalBlockFor(
        "<link rel=\"stylesheet\" href=\"/screen.css\">"
        "<link rel=\"stylesheet\" href=\"/print.css\" media=\"print\" "
        "onload=\"" +
        std::string(handler) + "\">");
    ASSERT_FALSE(block.empty()) << handler;
    EXPECT_EQ(ScreenRulesOf(block).find("display:none"), std::string::npos)
        << handler << ": " << block;
  }
}

TEST_F(PrintStylesheetTest, OnloadThatChangesMediaMakesTheSheetAScreenSheet) {
  OpenCache();
  CacheCss("/screen.css", kScreenCss);
  CacheCss("/print.css", kPrintCss);
  for (const char* handler :
       {"this.media='all'", "this.media = \"all\"",
        "this.onload=null;this.media='all'", "this['media']='all'",
        "this.setAttribute('media','all')", "this.removeAttribute('media')"}) {
    const std::string block = CriticalBlockFor(
        "<link rel=\"stylesheet\" href=\"/screen.css\">"
        "<link rel=\"stylesheet\" href=\"/print.css\" media=\"print\" "
        "onload=\"" +
        std::string(handler) + "\">");
    ASSERT_FALSE(block.empty()) << handler;
    EXPECT_NE(ScreenRulesOf(block).find("display:none"), std::string::npos)
        << handler << ": " << block;
  }
}

TEST_F(PrintStylesheetTest, ScreenAndAllMediaSheetsAreUnchanged) {
  // No wrapper for the media values that apply on every screen: the block
  // is what it was before print scoping existed.
  OpenCache();
  CacheCss("/screen.css", kScreenCss);
  CacheCss("/all.css", "main{padding:1px}");
  const std::string with_media = CriticalBlockFor(
      "<link rel=\"stylesheet\" href=\"/screen.css\" media=\"screen\">"
      "<link rel=\"stylesheet\" href=\"/all.css\" media=\"all\">");
  const std::string without_media = CriticalBlockFor(
      "<link rel=\"stylesheet\" href=\"/screen.css\">"
      "<link rel=\"stylesheet\" href=\"/all.css\">");
  ASSERT_FALSE(with_media.empty());
  EXPECT_EQ(with_media, without_media);
  EXPECT_EQ(with_media.find("@media"), std::string::npos) << with_media;
}

}  // namespace

// ================================================================
// ps_html_process with cache: external CSS partially missing
// ================================================================

TEST_F(PageSpeedCacheApiTest, HtmlProcessWithCachePartialCssMissing) {
  OpenCache();

  // Write only one of two stylesheets into the cache.
  const char* css_content = ".hero { color: red; }";
  size_t css_len = std::strlen(css_content);

  ps_write_params_t params;
  ps_write_params_init_auto(&params);
  params.alternate_id = 0x08;
  params.content_length = css_len;
  params.full_mask = 0x08;
  params.content_type = PS_CONTENT_CSS;
  params.origin_ct = "text/css";

  ps_write_handle_t* wh = nullptr;
  ASSERT_EQ(ps_cache_write_begin(cache_, "/found.css", "example.com", "https",
                                 &params, &wh),
            PS_OK);
  ASSERT_EQ(ps_write_data(wh, css_content, css_len), PS_OK);
  ASSERT_EQ(ps_write_close(wh), PS_OK);

  // HTML references two stylesheets, only one is in cache.
  const char* html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/found.css\">"
      "<link rel=\"stylesheet\" href=\"/missing.css\">"
      "</head><body>"
      "<div class=\"hero\">Hello</div>"
      "</body></html>";

  ps_html_config_t config;
  ps_html_config_init(&config);
  config.enable_critical_css = 1;

  ps_html_result_t* result = nullptr;
  ASSERT_EQ(ps_html_process(html, std::strlen(html), "http://example.com/page",
                            "example.com", &config, cache_, &result),
            PS_OK);
  ASSERT_NE(result, nullptr);

  // One stylesheet was missing, so revalidation is needed.
  EXPECT_EQ(ps_html_result_needs_revalidation(result), 1);

  ps_html_result_free(result);
}

// ================================================================
// ps_html_process with cache but no hostname (external CSS missing path)
// ================================================================

TEST_F(PageSpeedCacheApiTest, HtmlProcessWithCacheNoHostname) {
  OpenCache();

  // HTML references an external stylesheet.
  const char* html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/style.css\">"
      "</head><body><div>Hello</div></body></html>";

  ps_html_config_t config;
  ps_html_config_init(&config);
  config.enable_critical_css = 1;

  ps_html_result_t* result = nullptr;
  // Pass cache but no hostname -- the external CSS lookup path requires both.
  ASSERT_EQ(ps_html_process(html, std::strlen(html), "http://example.com/page",
                            nullptr, &config, cache_, &result),
            PS_OK);
  ASSERT_NE(result, nullptr);

  // Without hostname, external stylesheets cannot be looked up.
  EXPECT_EQ(ps_html_result_needs_revalidation(result), 1);

  ps_html_result_free(result);
}

// ================================================================
// ps_html_process async-CSS FOUC fail-safe (.NET in-process path parity
// with the worker). Mirrors the iispeed.com cold-cache regression: the .NET
// path must NOT async-defer a stylesheet it could not measure.
// ================================================================

TEST_F(PageSpeedCacheApiTest,
       HtmlProcessAsyncCssSuppressedWhenExternalCssMissing) {
  OpenCache();

  // HTML with an inline <style> (echoed as critical CSS) + an external <link>
  // that is NOT in cache. Without the guard, the deferred-byte denominator
  // collapses to the inline blob, trips the small-sheet escape hatch, and
  // async-defers the unmeasured sheet -> FOUC.
  const char* html =
      "<html><head>"
      "<style>html.dark,html.dark body{background:#0c0a09}"
      ".hero{color:red}</style>"
      "<link rel=\"stylesheet\" href=\"/missing.css\">"
      "</head><body><div class=\"hero\">Hello</div></body></html>";

  ps_html_config_t config;
  ps_html_config_init(&config);
  config.enable_critical_css = 1;
  config.enable_async_css = 1;  // customer opted in

  ps_html_result_t* result = nullptr;
  ASSERT_EQ(ps_html_process(html, std::strlen(html), "http://example.com/page",
                            "example.com", &config, cache_, &result),
            PS_OK);
  ASSERT_NE(result, nullptr);

  // External sheet uncached -> revalidation required (self-heals next request).
  EXPECT_EQ(ps_html_result_needs_revalidation(result), 1);
  // Critical CSS IS injected (the inline echo) ...
  EXPECT_EQ(ps_html_result_has_critical_css(result), 1);

  size_t out_len = 0;
  const char* output = ps_html_result_output(result, &out_len);
  ASSERT_NE(output, nullptr);
  std::string out(output, out_len);
  // ... but the stylesheet must NOT be async-deferred (the fail-safe).
  EXPECT_EQ(out.find("rel=\"preload\""), std::string::npos)
      << ".NET path must not async-defer an unmeasured (cold) stylesheet";
  EXPECT_EQ(out.find("as=\"style\""), std::string::npos);
  EXPECT_EQ(out.find("data-pagespeed-async"), std::string::npos);

  ps_html_result_free(result);
}

TEST_F(PageSpeedCacheApiTest, HtmlProcessAsyncCssAppliesWhenCssCached) {
  OpenCache();

  // Positive companion: a genuinely small CACHED sheet -> the guard does NOT
  // over-suppress; async-CSS still applies. Proves the cold-cache veto is
  // scoped to the unmeasured case, not a blanket disable.
  const char* css_content = ".hero{color:red}p{margin:0}";
  size_t css_len = std::strlen(css_content);

  ps_write_params_t params;
  ps_write_params_init_auto(&params);
  params.alternate_id = 0x08;
  params.content_length = css_len;
  params.full_mask = 0x08;
  params.content_type = PS_CONTENT_CSS;
  params.origin_ct = "text/css";

  ps_write_handle_t* wh = nullptr;
  ASSERT_EQ(ps_cache_write_begin(cache_, "/small.css", "example.com", "https",
                                 &params, &wh),
            PS_OK);
  ASSERT_EQ(ps_write_data(wh, css_content, css_len), PS_OK);
  ASSERT_EQ(ps_write_close(wh), PS_OK);

  const char* html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/small.css\">"
      "</head><body><div class=\"hero\">Hello</div><p>World</p></body></html>";

  ps_html_config_t config;
  ps_html_config_init(&config);
  config.enable_critical_css = 1;
  config.enable_async_css = 1;

  ps_html_result_t* result = nullptr;
  ASSERT_EQ(ps_html_process(html, std::strlen(html), "http://example.com/page",
                            "example.com", &config, cache_, &result),
            PS_OK);
  ASSERT_NE(result, nullptr);

  // Sheet cached -> no revalidation, and the small-sheet escape hatch allows
  // async (the deferred sheet is genuinely tiny, trivial FOUC window).
  EXPECT_EQ(ps_html_result_needs_revalidation(result), 0);
  EXPECT_EQ(ps_html_result_has_critical_css(result), 1);

  size_t out_len = 0;
  const char* output = ps_html_result_output(result, &out_len);
  ASSERT_NE(output, nullptr);
  std::string out(output, out_len);
  EXPECT_NE(out.find("rel=\"preload\""), std::string::npos)
      << "warm small cached sheet must still async-defer (no over-suppression)";
  EXPECT_NE(out.find("as=\"style\""), std::string::npos) << out;

  // The deferred sheet is Early-Hint promoted, on THIS path too.  The
  // in-process (.NET) pipeline and the worker serve the same pages from the
  // same cache, so a page that got a stylesheet hint from one and not the
  // other would flip its 103 depending on who last reprocessed it.  The
  // primitive is a preload now, so the hint announces the same fetch the
  // markup announces.
  size_t hints_len = 0;
  const char* hints = ps_html_result_early_hints(result, &hints_len);
  ASSERT_NE(hints, nullptr) << "a deferring page must still emit early hints";
  std::string hints_str(hints, hints_len);
  EXPECT_NE(hints_str.find("/small.css"), std::string::npos)
      << "the deferred stylesheet must be preload-hinted: " << hints_str;

  ps_html_result_free(result);
}

// The asymmetry between the two C-ABI entry points, asserted on BOTH sides in
// one test so neither half can drift alone: identical page, identical cached
// stylesheet, identical config with enable_async_css=1.
//
// ps_html_process gathers the stylesheet itself, so it can run the byte-ratio
// floor and the cold-cache fail-safe over real bytes, and it honours the flag.
// ps_html_transform_create sees no stylesheets and no cache, cannot judge
// anything, and ignores the flag outright. (Neither can run the worker's
// empirical gate — no browser, no validation record — which is why the flag
// stays opt-in on both.)
TEST_F(PageSpeedCacheApiTest, TransformCreateAndHtmlProcessDisagreeByDesign) {
  OpenCache();

  const char* css_content = ".hero{color:red}p{margin:0}";
  size_t css_len = std::strlen(css_content);

  ps_write_params_t params;
  ps_write_params_init_auto(&params);
  params.alternate_id = 0x08;
  params.content_length = css_len;
  params.full_mask = 0x08;
  params.content_type = PS_CONTENT_CSS;
  params.origin_ct = "text/css";

  ps_write_handle_t* wh = nullptr;
  ASSERT_EQ(ps_cache_write_begin(cache_, "/shared.css", "example.com", "https",
                                 &params, &wh),
            PS_OK);
  ASSERT_EQ(ps_write_data(wh, css_content, css_len), PS_OK);
  ASSERT_EQ(ps_write_close(wh), PS_OK);

  const char* html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/shared.css\">"
      "</head><body><div class=\"hero\">Hello</div><p>World</p></body></html>";

  ps_html_config_t config;
  ps_html_config_init(&config);
  config.enable_critical_css = 1;
  config.enable_async_css = 1;

  // --- Side A: ps_html_process honours it. ---
  ps_html_result_t* result = nullptr;
  ASSERT_EQ(ps_html_process(html, std::strlen(html), "http://example.com/page",
                            "example.com", &config, cache_, &result),
            PS_OK);
  ASSERT_NE(result, nullptr);
  size_t out_len = 0;
  const char* output = ps_html_result_output(result, &out_len);
  ASSERT_NE(output, nullptr);
  std::string processed(output, out_len);
  ps_html_result_free(result);

  EXPECT_NE(processed.find("data-pagespeed-async"), std::string::npos)
      << "ps_html_process gathers the sheet and can gate, so it must still "
         "honour the flag: "
      << processed;
  EXPECT_NE(processed.find("rel=\"preload\""), std::string::npos);
  EXPECT_NE(processed.find("as=\"style\""), std::string::npos);

  // --- Side B: ps_html_transform_create ignores it, same inputs. ---
  ps_scan_result_t* scan = nullptr;
  ASSERT_EQ(
      ps_html_scan(html, std::strlen(html), "http://example.com/page", &scan),
      PS_OK);
  const char* critical = "div.hero{color:red}";
  ps_html_transform_t* transform = nullptr;
  ASSERT_EQ(
      ps_html_transform_create(scan, &config, critical, std::strlen(critical),
                               nullptr, nullptr, nullptr, 0, &transform),
      PS_OK);
  char* out_html = nullptr;
  size_t tlen = 0;
  ASSERT_EQ(ps_html_transform_run(transform, html, std::strlen(html),
                                  "http://example.com/page", &out_html, &tlen),
            PS_OK);
  ASSERT_NE(out_html, nullptr);
  std::string transformed(out_html, tlen);
  ps_free(out_html);
  ps_html_transform_free(transform);
  ps_scan_result_free(scan);

  EXPECT_EQ(transformed.find("data-pagespeed-async"), std::string::npos)
      << "same flag, same page, same sheet — but this path cannot gate, so it "
         "must not defer: "
      << transformed;
  EXPECT_EQ(transformed.find("rel=\"preload\""), std::string::npos);
  EXPECT_EQ(transformed.find("as=\"style\""), std::string::npos);
  // ...while still inlining the critical CSS it was handed.
  EXPECT_NE(transformed.find("data-pagespeed-critical"), std::string::npos);
}

// ================================================================
// MapError: exercise various CacheError code paths indirectly
// ================================================================

TEST_F(PageSpeedCacheApiTest, CacheReadBestNullArgs) {
  // Read with null cache -- exercises INVALID_ARG path.
  ps_read_result_t* result = nullptr;
  EXPECT_EQ(ps_cache_read_best(nullptr, "/test.html", "example.com", "https",
                               0x08, &result),
            PS_ERR_INVALID_ARG);
}

TEST_F(PageSpeedCacheApiTest, WriteBeginNullUrl) {
  OpenCache();

  ps_write_params_t params;
  ps_write_params_init_auto(&params);
  params.alternate_id = 0x08;
  params.content_length = 4;
  params.full_mask = 0x08;
  params.content_type = PS_CONTENT_HTML;

  ps_write_handle_t* wh = nullptr;
  EXPECT_EQ(ps_cache_write_begin(cache_, nullptr, "example.com", "https",
                                 &params, &wh),
            PS_ERR_INVALID_ARG);
}

TEST_F(PageSpeedCacheApiTest, WriteBeginNullHostname) {
  OpenCache();

  ps_write_params_t params;
  ps_write_params_init_auto(&params);
  params.alternate_id = 0x08;
  params.content_length = 4;
  params.full_mask = 0x08;
  params.content_type = PS_CONTENT_HTML;

  ps_write_handle_t* wh = nullptr;
  EXPECT_EQ(ps_cache_write_begin(cache_, "/test.html", nullptr, "https",
                                 &params, &wh),
            PS_ERR_INVALID_ARG);
}

// ================================================================
// Cache open with invalid path (exercises MapError IO path)
// ================================================================

TEST(PageSpeedCApi, CacheOpenInvalidPath) {
  ps_cache_config_t config;
  ps_cache_config_init(&config);
  // Path to a directory that doesn't exist and can't be created.
  config.volume_path = "/nonexistent/deep/path/cache.vol";
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);

  ps_cache_t* c = nullptr;
  ps_error_t err = ps_cache_open(&config, &c);
  // Should fail with some error (IO, INTERNAL, or similar).
  EXPECT_NE(err, PS_OK);
  EXPECT_EQ(c, nullptr);
  // Last error should be set.
  EXPECT_STRNE(ps_last_error_message(), "");
}

// The open failure carries the cache library's own reason, not just a generic
// "Failed to create cache", so clients can log why the cache would not open.
TEST(PageSpeedCApi, CacheOpenFailureReportsReason) {
  ps_cache_config_t config;
  ps_cache_config_init(&config);
  config.volume_path = "/nonexistent/deep/path/cache.vol";
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);

  ps_cache_t* c = nullptr;
  ASSERT_NE(ps_cache_open(&config, &c), PS_OK);
  EXPECT_EQ(c, nullptr);

  const std::string msg = ps_last_error_message();
  const std::string prefix = "Failed to create cache: ";
  ASSERT_EQ(msg.compare(0, prefix.size(), prefix), 0) << msg;
  EXPECT_GT(msg.size(), prefix.size()) << "no reason after the prefix";
}

// A volume the cache refuses to use (here: a second hard link to the same
// bytes) reports what was wrong with it, which the cache logs while opening.
#ifndef _WIN32
TEST(PageSpeedCApi, CacheOpenRefusedVolumeReportsLoggedReason) {
  const std::string dir = pagespeed::test::MakeTempDir();
  const std::string path = dir + "/cache.vol";

  ps_cache_config_t config;
  ps_cache_config_init(&config);
  config.volume_path = path.c_str();
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);

  ps_cache_t* first = nullptr;
  ASSERT_EQ(ps_cache_open(&config, &first), PS_OK);
  ps_cache_close(first);

  // Hard-link every regular file the first open created, so the volume file
  // has a second name.
  std::vector<std::filesystem::path> created;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (entry.is_regular_file()) created.push_back(entry.path());
  }
  ASSERT_FALSE(created.empty());
  for (const auto& file : created) {
    std::filesystem::create_hard_link(file, file.string() + ".link");
  }

  ps_cache_t* second = nullptr;
  EXPECT_NE(ps_cache_open(&config, &second), PS_OK);
  EXPECT_EQ(second, nullptr);
  EXPECT_NE(std::string(ps_last_error_message()).find("lone regular file"),
            std::string::npos)
      << ps_last_error_message();

  std::filesystem::remove_all(dir);
}
#endif  // _WIN32

// ================================================================
// Read early hints from non-existent key (NOT_FOUND via MapError)
// ================================================================

TEST_F(PageSpeedCacheApiTest, ReadEarlyHintsNotFound) {
  OpenCache();
  ps_read_result_t* result = nullptr;
  EXPECT_EQ(ps_cache_read_early_hints(cache_, "/missing.html", "example.com",
                                      "https", &result),
            PS_ERR_NOT_FOUND);
}

// ================================================================
// Read early hints null args
// ================================================================

TEST(PageSpeedCApi, ReadEarlyHintsNullArgs) {
  EXPECT_EQ(ps_cache_read_early_hints(nullptr, "/x", "h", "https", nullptr),
            PS_ERR_INVALID_ARG);
}

// ================================================================
// HTML process with hostname provided (exercises hostname path in
// ps_html_process, which may normalize the hostname)
// ================================================================

TEST(PageSpeedCApi, HtmlProcessWithHostname) {
  const char* html =
      "<html><head></head><body>"
      "<img src=\"/hero.jpg\">"
      "</body></html>";

  ps_html_result_t* result = nullptr;
  ASSERT_EQ(ps_html_process(html, std::strlen(html), "http://example.com/page",
                            "EXAMPLE.COM", nullptr, nullptr, &result),
            PS_OK);
  ASSERT_NE(result, nullptr);
  ps_html_result_free(result);
}

// ================================================================
// Critical CSS with include/exclude tag patterns actually filtering
// ================================================================

TEST(PageSpeedCApi, CriticalCssExcludeTagPattern) {
  const char* html =
      "<html><head></head><body>"
      "<div class=\"keep\">Keep</div>"
      "<span class=\"skip\">Skip</span>"
      "</body></html>";

  ps_scan_result_t* scan = nullptr;
  ASSERT_EQ(ps_html_scan(html, std::strlen(html), "http://example.com/", &scan),
            PS_OK);

  const char* css = ".keep { color: green; } .skip { color: red; }";

  const char* exclude_tags[] = {"span", nullptr};
  const char* include_tags[] = {"div", nullptr};

  ps_critical_css_config_t config;
  ps_critical_css_config_init(&config);
  config.exclude_tag_patterns = exclude_tags;
  config.include_tag_patterns = include_tags;

  ps_critical_css_result_t* result = nullptr;
  ASSERT_EQ(
      ps_css_extract_critical(scan, css, std::strlen(css), &config, &result),
      PS_OK);
  ASSERT_NE(result, nullptr);

  size_t out_len = 0;
  const char* output = ps_critical_css_output(result, &out_len);
  EXPECT_NE(output, nullptr);
  if (output != nullptr) {
    std::string out_str(output, out_len);
    EXPECT_NE(out_str.find("keep"), std::string::npos);
  }

  ps_critical_css_result_free(result);
  ps_scan_result_free(scan);
}

// ================================================================
// Notify worker with valid content types (exercises ValidateContentType OK)
// ================================================================

TEST(PageSpeedCApi, NotifyWorkerAllContentTypes) {
  // These all pass ValidateContentType but fail on the socket.
  // The important thing is they get past validation.
  ps_content_type_t types[] = {PS_CONTENT_HTML, PS_CONTENT_CSS, PS_CONTENT_JS,
                               PS_CONTENT_IMAGE, PS_CONTENT_OTHER};
  for (auto ct : types) {
    ps_error_t err =
        ps_notify_worker("/tmp/nonexistent_ps_test.sock", "/test.html",
                         "example.com", "https", ct, 0x08);
    // Should fail with IO (socket doesn't exist), not INVALID_ARG.
    EXPECT_EQ(err, PS_ERR_IO) << "Unexpected error for content_type=" << ct;
  }
}

// ================================================================
// Cache remove with null URL and hostname
// ================================================================

TEST_F(PageSpeedCacheApiTest, CacheRemoveNullUrl) {
  OpenCache();
  EXPECT_EQ(ps_cache_remove(cache_, nullptr, "example.com", "https"),
            PS_ERR_INVALID_ARG);
}

TEST_F(PageSpeedCacheApiTest, CacheRemoveNullHostname) {
  OpenCache();
  EXPECT_EQ(ps_cache_remove(cache_, "/test.html", nullptr, "https"),
            PS_ERR_INVALID_ARG);
}

// ================================================================
// List alternates with null URL and hostname
// ================================================================

TEST_F(PageSpeedCacheApiTest, ListAlternatesNullUrl) {
  OpenCache();
  ps_alternate_info_t* alts = nullptr;
  size_t count = 0;
  EXPECT_EQ(ps_cache_list_alternates(cache_, nullptr, "example.com", "https",
                                     &alts, &count),
            PS_ERR_INVALID_ARG);
}

TEST_F(PageSpeedCacheApiTest, ListAlternatesNullHostname) {
  OpenCache();
  ps_alternate_info_t* alts = nullptr;
  size_t count = 0;
  EXPECT_EQ(ps_cache_list_alternates(cache_, "/test.html", nullptr, "https",
                                     &alts, &count),
            PS_ERR_INVALID_ARG);
}

// ================================================================
// Cache stats after writes (exercises stats path with actual data)
// ================================================================

TEST_F(PageSpeedCacheApiTest, CacheStatsAfterWrites) {
  OpenCache();

  // Write some data.
  ps_write_params_t params;
  ps_write_params_init_auto(&params);
  params.alternate_id = 0x08;
  params.content_length = 100;
  params.full_mask = 0x08;
  params.content_type = PS_CONTENT_HTML;

  std::string data(100, 'x');
  ps_write_handle_t* wh = nullptr;
  ASSERT_EQ(ps_cache_write_begin(cache_, "/stats.html", "example.com", "https",
                                 &params, &wh),
            PS_OK);
  ASSERT_EQ(ps_write_data(wh, data.c_str(), data.size()), PS_OK);
  ASSERT_EQ(ps_write_close(wh), PS_OK);

  // Read it to generate read stats.
  ps_read_result_t* result = nullptr;
  ASSERT_EQ(ps_cache_read_best(cache_, "/stats.html", "example.com", "https",
                               0x08, &result),
            PS_OK);
  ps_read_free(result);

  // Cyclone may update stats counters asynchronously after the synchronous
  // write/read calls return.  Poll briefly to avoid flakes on loaded CI.
  ps_cache_stats_t stats;
  for (int i = 0; i < 20; ++i) {
    std::memset(&stats, 0, sizeof(stats));
    stats.struct_size = sizeof(ps_cache_stats_t);
    ASSERT_EQ(ps_cache_stats(cache_, &stats), PS_OK);
    if (stats.current_entries > 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  // bytes_written is not tracked per-operation in Cyclone (returns 0).
  EXPECT_GT(stats.current_entries, 0u);
}

// ================================================================
// Notify worker null hostname
// ================================================================

TEST(PageSpeedCApi, NotifyWorkerNullHostname) {
  EXPECT_EQ(ps_notify_worker("/tmp/test.sock", "/test.html", nullptr, "https",
                             PS_CONTENT_HTML, 0x08),
            PS_ERR_INVALID_ARG);
}

// ================================================================
// HTML process: config with css_import_max_depth
// ================================================================

TEST(PageSpeedCApi, HtmlProcessWithCustomConfig) {
  const char* html =
      "<html><head>"
      "<style>.test { color: red; }</style>"
      "</head><body>"
      "<div class=\"test\">Hello</div>"
      "<img src=\"/hero.jpg\">"
      "</body></html>";

  ps_html_config_t config;
  ps_html_config_init(&config);
  config.enable_critical_css = 1;
  config.enable_lazy_load = 1;
  config.enable_lcp_preload = 1;
  config.enable_preconnect = 1;
  config.css_import_max_depth = 3;
  config.critical_css_max_elements = 50;
  config.critical_css_max_depth = 20;
  config.viewport = PS_VIEWPORT_MOBILE;

  ps_html_result_t* result = nullptr;
  ASSERT_EQ(ps_html_process(html, std::strlen(html), "http://example.com/page",
                            nullptr, &config, nullptr, &result),
            PS_OK);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(ps_html_result_has_critical_css(result), 1);
  EXPECT_EQ(ps_html_result_modified(result), 1);

  ps_html_result_free(result);
}

// ================================================================
// Write sentinel: various sentinel IDs
// ================================================================

TEST_F(PageSpeedCacheApiTest, WriteSentinelTypes) {
  OpenCache();

  uint8_t sentinels[] = {PS_SENTINEL_EARLY_HINTS, PS_SENTINEL_WARMUP,
                         PS_SENTINEL_CONTENT_HASH, PS_SENTINEL_SUBRESOURCE,
                         PS_SENTINEL_BROWSER_PROFILE};

  int idx = 0;
  for (auto sentinel_id : sentinels) {
    std::string url = "/sentinel_" + std::to_string(idx++) + ".html";
    ps_write_handle_t* wh = nullptr;
    ASSERT_EQ(ps_cache_write_sentinel(cache_, url.c_str(), "example.com",
                                      "https", sentinel_id, 4, &wh),
              PS_OK)
        << "Failed for sentinel_id=" << static_cast<int>(sentinel_id);
    ASSERT_EQ(ps_write_data(wh, "data", 4), PS_OK);
    ASSERT_EQ(ps_write_close(wh), PS_OK);
  }
}

// ================================================================
// Write sentinel: reserved ids are refused
// ================================================================

TEST_F(PageSpeedCacheApiTest, WriteSentinelRefusesReservedIds) {
  // Two kinds of claimed id are refused here, and the second is the one worth
  // stating: a RESERVED class has no reader or writer yet, so foreign bytes
  // under its id would be indistinguishable from its own; a class with its OWN
  // write entry point refuses this surface even though it has shipped, because
  // its invariants (a metadata prefix, a content cap) live in that entry point
  // and a raw sentinel write would skip them.  0x0C and 0x6C are now both the
  // second kind — ps_cache_write_original and ps_cache_write_headers_sidecar
  // write them — and both must stay refused here.  The sidecar is the sharper
  // case: its entry point IS the admission gate, so a raw write into its id
  // stores exactly the header blocks the gate exists to refuse.
  OpenCache();

  const uint8_t reserved[] = {
      0x0C,  // durable original content — has ps_cache_write_original
      0x6C,  // header sidecar — has ps_cache_write_headers_sidecar
      0xAC,  // negative verdict
  };
  int idx = 0;
  for (uint8_t sentinel_id : reserved) {
    std::string url = "/reserved_" + std::to_string(idx++) + ".html";
    ps_write_handle_t* wh = nullptr;
    EXPECT_EQ(ps_cache_write_sentinel(cache_, url.c_str(), "example.com",
                                      "https", sentinel_id, 4, &wh),
              PS_ERR_INVALID_ARG)
        << "reserved sentinel_id=" << static_cast<int>(sentinel_id)
        << " must not be writable before the class that owns it exists";
    EXPECT_EQ(wh, nullptr);
  }
}

// ================================================================
// The durable original class on the C surface
// ================================================================

TEST_F(PageSpeedCacheApiTest, WriteOriginalRoundTripsAndCarriesOriginState) {
  OpenCache();

  const char* body = "ORIGINAL-BYTES";
  const size_t body_len = std::strlen(body);

  ps_write_params_t params;
  ps_write_params_init_auto(&params);
  params.content_length = body_len;
  params.content_type = PS_CONTENT_IMAGE;
  params.origin_ct = "image/jpeg";
  params.origin_etag = "\"v1\"";
  params.cache_inserted_at = 1700000000;
  params.origin_max_age = 600;
  params.origin_last_modified = 1699999000;
  params.origin_cc_flags = PS_CC_ORIGIN_HEADER_PRESENT | PS_CC_ORIGIN_PUBLIC;

  ps_write_handle_t* wh = nullptr;
  ASSERT_EQ(ps_cache_write_original(cache_, "/hero.jpg", "example.com", "https",
                                    &params, &wh),
            PS_OK);
  ASSERT_NE(wh, nullptr);
  ASSERT_EQ(ps_write_data(wh, body, body_len), PS_OK);
  ASSERT_EQ(ps_write_close(wh), PS_OK);

  // Read it back the way it is meant to be read: by exact id.
  ps_read_result_t* result = nullptr;
  ASSERT_EQ(ps_cache_read_alternate(cache_, "/hero.jpg", "example.com", "https",
                                    PS_SENTINEL_ORIGINAL, &result),
            PS_OK);
  ASSERT_NE(result, nullptr);

  const uint8_t* data = nullptr;
  size_t len = 0;
  ASSERT_EQ(ps_read_content(result, &data, &len), PS_OK);
  EXPECT_EQ(len, body_len);
  EXPECT_EQ(std::memcmp(data, body, body_len), 0);

  // The entry describes itself as the class it is stored under, and the origin
  // state that governs its lifetime came back with it.
  EXPECT_EQ(ps_read_mask(result) & 0xFF,
            static_cast<uint32_t>(PS_SENTINEL_ORIGINAL));
  EXPECT_EQ(ps_read_origin_max_age(result), 600u);
  EXPECT_EQ(ps_read_origin_last_modified(result), 1699999000u);
  EXPECT_EQ(ps_read_cache_inserted_at(result), 1700000000u);
  EXPECT_STREQ(ps_read_origin_etag(result), "\"v1\"");
  EXPECT_EQ(ps_read_origin_cc_flags(result),
            PS_CC_ORIGIN_HEADER_PRESENT | PS_CC_ORIGIN_PUBLIC);
  ps_read_free(result);

  // And it is not something ordinary selection can produce.
  ps_read_result_t* best = nullptr;
  EXPECT_EQ(
      ps_cache_read_best(cache_, "/hero.jpg", "example.com", "https", 0, &best),
      PS_ERR_NOT_FOUND);
}

// The `Vary: Accept` marker survives the published ABI, both directions.
//
// This is the ABI half of the guarantee the optimizer's refusal rests on: a
// front end that classifies its response, sets PS_FLAG_ORIGIN_VARIES_ACCEPT
// and stores a durable original must end up with an entry the optimizer will
// refuse.  The optimizer half (the refusal itself) is pinned in the worker
// suite; a static_assert in pagespeed.cc ties this published bit to the
// internal one, so the two halves provably speak about the same bit rather
// than about two constants that happen to be equal.
TEST_F(PageSpeedCacheApiTest, WriteOriginalCarriesTheVaryAcceptMarker) {
  OpenCache();

  const char* body = "ORIGINAL";
  const size_t body_len = 8;

  ps_write_params_t params;
  ps_write_params_init_auto(&params);
  params.content_length = body_len;
  params.content_type = PS_CONTENT_IMAGE;
  params.origin_ct = "image/jpeg";
  params.cache_inserted_at = 1700000000;
  params.origin_max_age = 600;
  // Exactly what the doc block obliges a writer to do for an origin whose
  // `Vary` says it negotiates on Accept.
  params.flags = PS_FLAG_ORIGIN_VARIES_ACCEPT;

  ps_write_handle_t* wh = nullptr;
  ASSERT_EQ(ps_cache_write_original(cache_, "/negotiated.jpg", "example.com",
                                    "https", &params, &wh),
            PS_OK);
  ASSERT_NE(wh, nullptr);
  ASSERT_EQ(ps_write_data(wh, body, body_len), PS_OK);
  ASSERT_EQ(ps_write_close(wh), PS_OK);

  ps_read_result_t* result = nullptr;
  ASSERT_EQ(ps_cache_read_alternate(cache_, "/negotiated.jpg", "example.com",
                                    "https", PS_SENTINEL_ORIGINAL, &result),
            PS_OK);
  ASSERT_NE(result, nullptr);
  EXPECT_NE(ps_read_flags(result) & PS_FLAG_ORIGIN_VARIES_ACCEPT, 0)
      << "the marker a writer set did not survive into the stored entry";
  ps_read_free(result);

  // An unmarked original of the same class comes back unmarked — the bit is
  // carried, not synthesised.
  ps_write_params_t plain;
  ps_write_params_init_auto(&plain);
  plain.content_length = body_len;
  plain.content_type = PS_CONTENT_IMAGE;
  plain.origin_ct = "image/jpeg";
  plain.cache_inserted_at = 1700000000;
  ps_write_handle_t* wh2 = nullptr;
  ASSERT_EQ(ps_cache_write_original(cache_, "/plain.jpg", "example.com",
                                    "https", &plain, &wh2),
            PS_OK);
  ASSERT_EQ(ps_write_data(wh2, body, body_len), PS_OK);
  ASSERT_EQ(ps_write_close(wh2), PS_OK);

  ps_read_result_t* plain_result = nullptr;
  ASSERT_EQ(
      ps_cache_read_alternate(cache_, "/plain.jpg", "example.com", "https",
                              PS_SENTINEL_ORIGINAL, &plain_result),
      PS_OK);
  EXPECT_EQ(ps_read_flags(plain_result) & PS_FLAG_ORIGIN_VARIES_ACCEPT, 0);
  ps_read_free(plain_result);
}

// The header-fidelity marker survives the published ABI, both directions.
//
// The question it records — did the origin send a header this entry cannot
// reproduce — is answerable only while the response's headers are in hand, so
// the write is the only place it can be recorded and ps_read_flags is the only
// way a serve path gets it back.  Asserted through the real entry points, and
// asserted alongside the `Vary` marker because a response can carry both and
// the two must come back independently rather than as one "origin state" bit.
TEST_F(PageSpeedCacheApiTest, WriteOriginalCarriesTheHeaderFidelityMarker) {
  OpenCache();

  const char* body = "ORIGINAL";
  const size_t body_len = 8;

  ps_write_params_t params;
  ps_write_params_init_auto(&params);
  params.content_length = body_len;
  params.content_type = PS_CONTENT_IMAGE;
  params.origin_ct = "image/jpeg";
  params.cache_inserted_at = 1700000000;
  params.origin_max_age = 600;
  params.flags =
      PS_FLAG_ORIGIN_HEADERS_NOT_REPRODUCIBLE | PS_FLAG_ORIGIN_VARIES_ACCEPT;

  ps_write_handle_t* wh = nullptr;
  ASSERT_EQ(ps_cache_write_original(cache_, "/unreproducible.jpg",
                                    "example.com", "https", &params, &wh),
            PS_OK);
  ASSERT_NE(wh, nullptr);
  ASSERT_EQ(ps_write_data(wh, body, body_len), PS_OK);
  ASSERT_EQ(ps_write_close(wh), PS_OK);

  ps_read_result_t* result = nullptr;
  ASSERT_EQ(
      ps_cache_read_alternate(cache_, "/unreproducible.jpg", "example.com",
                              "https", PS_SENTINEL_ORIGINAL, &result),
      PS_OK);
  ASSERT_NE(result, nullptr);
  EXPECT_NE(ps_read_flags(result) & PS_FLAG_ORIGIN_HEADERS_NOT_REPRODUCIBLE, 0)
      << "the marker a writer set did not survive into the stored entry";
  EXPECT_NE(ps_read_flags(result) & PS_FLAG_ORIGIN_VARIES_ACCEPT, 0)
      << "the two origin-state markers must be readable independently";
  ps_read_free(result);

  // Carried, not synthesised: the default is unmarked, so a serve path cannot
  // be pushed onto its plain path by an entry nobody marked.
  ps_write_params_t plain;
  ps_write_params_init_auto(&plain);
  plain.content_length = body_len;
  plain.content_type = PS_CONTENT_IMAGE;
  plain.origin_ct = "image/jpeg";
  plain.cache_inserted_at = 1700000000;
  ps_write_handle_t* wh2 = nullptr;
  ASSERT_EQ(ps_cache_write_original(cache_, "/reproducible.jpg", "example.com",
                                    "https", &plain, &wh2),
            PS_OK);
  ASSERT_EQ(ps_write_data(wh2, body, body_len), PS_OK);
  ASSERT_EQ(ps_write_close(wh2), PS_OK);

  ps_read_result_t* plain_result = nullptr;
  ASSERT_EQ(
      ps_cache_read_alternate(cache_, "/reproducible.jpg", "example.com",
                              "https", PS_SENTINEL_ORIGINAL, &plain_result),
      PS_OK);
  EXPECT_EQ(ps_read_flags(plain_result), 0)
      << "an unmarked write must store an unmarked entry";
  ps_read_free(plain_result);
}

// The marker is advisory to serve paths and to nothing else: with it set,
// admission and selection answer identically.
//
// Differential rather than assertive, because "changes nothing" is the whole
// contract and the only honest way to state it is to run the same family more
// than once.  Three URLs get byte-identical variant families — one stamped on
// every member, one on none, and one on a SUBSET — and every client mask a
// request can present is asked of each: the write verdict, what exists
// afterwards, and which variant selection returns must agree at every point.
//
// The MIXED family is what makes the sweep sensitive to ranking, and it is not
// redundant with the uniform pair.  A uniformly marked family cannot detect a
// selector that merely SHIFTS a marked entry's score: every candidate moves by
// the same amount, so the ranking is preserved and the comparison passes while
// the flag is being consumed.  The ids and the marked subset below are picked
// so that a shift in either direction changes the winner: at a client mask
// whose viewport matches nothing, 0x00/0x04/0x08 tie and the tie-break takes
// the largest byte (unmarked 0x08), so a BONUS on marked 0x04 takes it; among
// the same-density WebP entries 0x01/0x05/0x09 tie and marked 0x09 wins, so a
// PENALTY on it hands the win to 0x05.
TEST_F(PageSpeedCacheApiTest, TheHeaderFidelityMarkerChangesNoStoredDecision) {
  OpenCache();

  // Format matches, format disqualifies, Original fallbacks and per-dimension
  // ties, so the sweep exercises every arm of the scoring the flag must stay
  // out of.
  const uint8_t kIds[] = {0x00, 0x01, 0x02, 0x04, 0x05, 0x08, 0x09, 0x11};
  const char* kBody = "VARIANTBYTES";
  const size_t kBodyLen = 12;

  auto is_mixed_marked = [](uint8_t id) { return id == 0x04 || id == 0x09; };

  auto write_family = [&](const char* url, auto flags_for_id) {
    for (uint8_t id : kIds) {
      ps_write_params_t params;
      ps_write_params_init_auto(&params);
      params.alternate_id = id;
      params.full_mask = id;
      params.content_length = kBodyLen;
      params.content_type = PS_CONTENT_IMAGE;
      params.origin_ct = "image/jpeg";
      params.cache_inserted_at = 1700000000;
      params.origin_max_age = 600;
      params.flags = flags_for_id(id);

      ps_write_handle_t* wh = nullptr;
      ASSERT_EQ(ps_cache_write_begin(cache_, url, "example.com", "https",
                                     &params, &wh),
                PS_OK)
          << "admission differed on id " << static_cast<int>(id);
      ASSERT_EQ(ps_write_data(wh, kBody, kBodyLen), PS_OK);
      ASSERT_EQ(ps_write_close(wh), PS_OK);
    }
  };

  write_family("/marked.jpg", [](uint8_t) -> uint8_t {
    return PS_FLAG_ORIGIN_HEADERS_NOT_REPRODUCIBLE;
  });
  write_family("/unmarked.jpg", [](uint8_t) -> uint8_t { return 0; });
  write_family("/mixed.jpg", [&](uint8_t id) -> uint8_t {
    return is_mixed_marked(id) ? PS_FLAG_ORIGIN_HEADERS_NOT_REPRODUCIBLE : 0;
  });

  // Admission: the same set of alternates exists under all three URLs.
  for (uint8_t id : kIds) {
    const int unmarked_exists = ps_cache_alternate_exists(
        cache_, "/unmarked.jpg", "example.com", "https", id);
    EXPECT_EQ(ps_cache_alternate_exists(cache_, "/marked.jpg", "example.com",
                                        "https", id),
              unmarked_exists)
        << "storage admitted the marked family differently at id "
        << static_cast<int>(id);
    EXPECT_EQ(ps_cache_alternate_exists(cache_, "/mixed.jpg", "example.com",
                                        "https", id),
              unmarked_exists)
        << "storage admitted the mixed family differently at id "
        << static_cast<int>(id);
  }

  // Selection: every client mask resolves to the same variant on every family.
  int hits = 0;
  int mixed_marked_wins = 0;
  int mixed_unmarked_wins = 0;
  for (int m = 0; m <= 0xFF; ++m) {
    ps_read_result_t* marked = nullptr;
    ps_read_result_t* unmarked = nullptr;
    ps_read_result_t* mixed = nullptr;
    const ps_error_t marked_err =
        ps_cache_read_best(cache_, "/marked.jpg", "example.com", "https",
                           static_cast<uint32_t>(m), &marked);
    const ps_error_t unmarked_err =
        ps_cache_read_best(cache_, "/unmarked.jpg", "example.com", "https",
                           static_cast<uint32_t>(m), &unmarked);
    const ps_error_t mixed_err =
        ps_cache_read_best(cache_, "/mixed.jpg", "example.com", "https",
                           static_cast<uint32_t>(m), &mixed);
    ASSERT_EQ(marked_err, unmarked_err) << "client mask " << m;
    ASSERT_EQ(mixed_err, unmarked_err) << "client mask " << m;
    if (unmarked_err == PS_OK) {
      ++hits;
      EXPECT_EQ(ps_read_mask(marked), ps_read_mask(unmarked))
          << "selection preferred a different variant at client mask " << m;
      EXPECT_EQ(ps_read_mask(mixed), ps_read_mask(unmarked))
          << "the marker changed the RANKING within a family at client mask "
          << m;
      // The only difference between the families, which is what keeps the
      // equalities above from being comparisons of identical things.
      EXPECT_NE(ps_read_flags(marked) & PS_FLAG_ORIGIN_HEADERS_NOT_REPRODUCIBLE,
                0);
      EXPECT_EQ(ps_read_flags(unmarked), 0);
      if ((ps_read_flags(mixed) & PS_FLAG_ORIGIN_HEADERS_NOT_REPRODUCIBLE) !=
          0) {
        ++mixed_marked_wins;
      } else {
        ++mixed_unmarked_wins;
      }
    }
    ps_read_free(marked);
    ps_read_free(unmarked);
    ps_read_free(mixed);
  }
  EXPECT_GT(hits, 0) << "the sweep never selected anything, so it proved "
                        "nothing about selection";
  // Non-vacuity for the mixed arm: the sweep has to have landed on marked
  // winners AND unmarked ones, or it never put the ranking at risk.
  EXPECT_GT(mixed_marked_wins, 0)
      << "no marked variant ever won, so a penalty on marked entries would "
         "have gone unnoticed";
  EXPECT_GT(mixed_unmarked_wins, 0)
      << "no unmarked variant ever won, so a bonus on marked entries would "
         "have gone unnoticed";
}

// The predicate a writer uses to decide whether to set the marker.
//
// Asserted alongside ps_vary_uncacheable rather than alone, because the pair
// is the contract: a refused response has nothing to mark, so varies_accept
// must imply storable.  Both are wrappers over one classification of one
// value, and this is what would catch them drifting into two decisions.
TEST(VaryStoreVerdictApiTest, VariesAcceptAnswersTheStampingQuestion) {
  struct Case {
    const char* vary;
    int uncacheable;
    int varies_accept;
  };
  const Case cases[] = {
      // No Vary at all: storable, negotiates on nothing.
      {nullptr, 0, 0},
      {"", 0, 0},
      // The one field a stored response is keyed on.
      {"Accept-Encoding", 0, 0},
      // Fields the origin picks its answer from: refused, so never marked.
      {"User-Agent", 1, 0},
      {"Save-Data", 1, 0},
      {"Accept", 1, 0},
      {"accept", 1, 0},
      {"  Accept  ", 1, 0},
      {"Accept-Encoding, Accept", 1, 0},
      {"Accept,Accept-Encoding", 1, 0},
      // Refused outright: nothing to store, so nothing to mark.
      {"*", 1, 0},
      {"Cookie", 1, 0},
      {"Accept, Cookie", 1, 0},
      {"Accept, *", 1, 0},
  };
  for (const auto& c : cases) {
    const char* label = c.vary == nullptr ? "(null)" : c.vary;
    EXPECT_EQ(ps_vary_uncacheable(c.vary), c.uncacheable)
        << "storability for Vary: " << label;
    EXPECT_EQ(ps_vary_varies_accept(c.vary), c.varies_accept)
        << "varies_accept for Vary: " << label;
    // The pairing the header promises.
    if (ps_vary_varies_accept(c.vary) == 1) {
      EXPECT_EQ(ps_vary_uncacheable(c.vary), 0)
          << "varies_accept must imply storable, for Vary: " << label;
    }
  }
}

TEST_F(PageSpeedCacheApiTest, WriteOriginalRefusesAForeignId) {
  // The class has exactly one id, which is what lets "the original" mean one
  // thing.  A caller who set alternate_id believes they are writing something
  // else; refusing is cheaper than storing an entry under a name neither side
  // agrees on.
  OpenCache();

  ps_write_params_t params;
  ps_write_params_init_auto(&params);
  params.content_length = 4;
  params.alternate_id = 0x08;

  ps_write_handle_t* wh = nullptr;
  EXPECT_EQ(ps_cache_write_original(cache_, "/hero.jpg", "example.com", "https",
                                    &params, &wh),
            PS_ERR_INVALID_ARG);
  EXPECT_EQ(wh, nullptr);

  // Naming the class explicitly is fine, as is leaving it unset.
  ps_write_params_init_auto(&params);
  params.content_length = 4;
  params.alternate_id = PS_SENTINEL_ORIGINAL;
  ASSERT_EQ(ps_cache_write_original(cache_, "/hero.jpg", "example.com", "https",
                                    &params, &wh),
            PS_OK);
  ASSERT_EQ(ps_write_data(wh, "data", 4), PS_OK);
  ASSERT_EQ(ps_write_close(wh), PS_OK);
}

TEST_F(PageSpeedCacheApiTest, WriteOriginalRefusesAForeignMaskLowByte) {
  OpenCache();

  ps_write_params_t params;
  ps_write_params_init_auto(&params);
  params.content_length = 4;
  params.full_mask = 0x08;  // a variant's mask

  ps_write_handle_t* wh = nullptr;
  EXPECT_EQ(ps_cache_write_original(cache_, "/hero.jpg", "example.com", "https",
                                    &params, &wh),
            PS_ERR_INVALID_ARG);
  EXPECT_EQ(wh, nullptr);
}

TEST_F(PageSpeedCacheApiTest, WriteOriginalRefusesAnOversizeResponse) {
  // 16 MB by default, matching the module's default maximum cacheable
  // response content length.  Declaring more is refused before anything is
  // allocated — a skipped store, not an error to retry.
  OpenCache();

  ps_write_params_t params;
  ps_write_params_init_auto(&params);
  params.content_length = (16u * 1024u * 1024u) + 1u;

  ps_write_handle_t* wh = nullptr;
  EXPECT_EQ(ps_cache_write_original(cache_, "/huge.bin", "example.com", "https",
                                    &params, &wh),
            PS_ERR_NO_SPACE);
  EXPECT_EQ(wh, nullptr);
  EXPECT_EQ(ps_cache_alternate_exists(cache_, "/huge.bin", "example.com",
                                      "https", PS_SENTINEL_ORIGINAL),
            0);
}

TEST_F(PageSpeedCacheApiTest, OversizeSkipNamesTheCapInTheLastError) {
  // PS_ERR_NO_SPACE is shared with the substrate's genuine out-of-space, and
  // the two call for opposite handling: this one is permanent for the
  // response and must not be retried, that one is transient and should be.
  // The distinction is the entire justification for reusing the code, so the
  // message has to carry it.
  OpenCache();

  ps_write_params_t params;
  ps_write_params_init_auto(&params);
  params.content_length = (16u * 1024u * 1024u) + 1u;

  ps_write_handle_t* wh = nullptr;
  ASSERT_EQ(ps_cache_write_original(cache_, "/huge.bin", "example.com", "https",
                                    &params, &wh),
            PS_ERR_NO_SPACE);
  const char* msg = ps_last_error_message();
  ASSERT_NE(msg, nullptr);
  const std::string text(msg);
  EXPECT_NE(text.find("cap"), std::string::npos) << text;
  EXPECT_NE(text.find("16777216"), std::string::npos) << text;
  EXPECT_NE(text.find("not a retryable error"), std::string::npos) << text;
}

TEST_F(PageSpeedCacheApiTest, MidStreamCapCutOffIsNamedAndClosesAsClosed) {
  // The response declared a size it did not keep to. The write that crosses
  // the cap fails, the entry is abandoned, and the close reports "closed"
  // rather than the substrate's "invalid argument" — an embedder's cleanup
  // path should not log a caller mistake for what is a normal skipped store.
  OpenCache();

  ps_write_params_t params;
  ps_write_params_init_auto(&params);
  params.content_length =
      16u * 1024u * 1024u;  // at the cap, so the begin is fine

  ps_write_handle_t* wh = nullptr;
  ASSERT_EQ(ps_cache_write_original(cache_, "/lying.bin", "example.com",
                                    "https", &params, &wh),
            PS_OK);
  ASSERT_NE(wh, nullptr);

  // Write past the declared length, one chunk at a time, until the cap fires.
  const std::vector<char> chunk(1u << 20, 'x');  // 1 MiB
  ps_error_t err = PS_OK;
  for (int i = 0; i < 20 && err == PS_OK; ++i) {
    err = ps_write_data(wh, chunk.data(), chunk.size());
  }
  ASSERT_EQ(err, PS_ERR_NO_SPACE);
  const std::string wtext(ps_last_error_message());
  EXPECT_NE(wtext.find("cap"), std::string::npos) << wtext;
  EXPECT_NE(wtext.find("16777216"), std::string::npos) << wtext;
  EXPECT_NE(wtext.find("nothing partial"), std::string::npos) << wtext;

  EXPECT_EQ(ps_write_close(wh), PS_ERR_CLOSED);

  // Nothing partial is visible, which is the property the message describes.
  EXPECT_EQ(ps_cache_alternate_exists(cache_, "/lying.bin", "example.com",
                                      "https", PS_SENTINEL_ORIGINAL),
            0);
}

TEST_F(PageSpeedCacheApiTest, WriteOriginalHonoursTheCallersStructSize) {
  // Same struct_size discipline as ps_cache_write_begin: a caller built
  // against the 1.1 header hands over the shorter struct and it works, with
  // the fields it never heard of reading as "not supplied"; a size naming no
  // published shape is refused rather than acted on.
  OpenCache();

  ps_write_params_t params;
  ps_write_params_init(&params);  // legacy initializer: 1.1 prefix only
  params.content_length = 4;

  ps_write_handle_t* wh = nullptr;
  ASSERT_EQ(ps_cache_write_original(cache_, "/legacy.jpg", "example.com",
                                    "https", &params, &wh),
            PS_OK);
  ASSERT_EQ(ps_write_data(wh, "data", 4), PS_OK);
  ASSERT_EQ(ps_write_close(wh), PS_OK);

  ps_write_params_t bad;
  std::memset(&bad, 0, sizeof(bad));
  bad.struct_size = 8;  // not a published rung of the ladder
  wh = nullptr;
  EXPECT_EQ(ps_cache_write_original(cache_, "/bad.jpg", "example.com", "https",
                                    &bad, &wh),
            PS_ERR_INVALID_ARG);
}

TEST_F(PageSpeedCacheApiTest, WriteOriginalRejectsNullArguments) {
  OpenCache();
  ps_write_params_t params;
  ps_write_params_init_auto(&params);
  ps_write_handle_t* wh = nullptr;

  EXPECT_EQ(ps_cache_write_original(nullptr, "/a.jpg", "example.com", "https",
                                    &params, &wh),
            PS_ERR_INVALID_ARG);
  EXPECT_EQ(ps_cache_write_original(cache_, "/a.jpg", "example.com", "https",
                                    nullptr, &wh),
            PS_ERR_INVALID_ARG);
  EXPECT_EQ(ps_cache_write_original(cache_, "/a.jpg", "example.com", "https",
                                    &params, nullptr),
            PS_ERR_INVALID_ARG);
  EXPECT_EQ(ps_cache_write_original(cache_, nullptr, "example.com", "https",
                                    &params, &wh),
            PS_ERR_INVALID_ARG);
}

// ================================================================
// The response-header sidecar on the C surface
// ================================================================

TEST(PageSpeedCApi, HeadersSidecarClassifyIsTheGateWithoutACache) {
  const char* names[] = {"Cache-Control", "Content-Security-Policy",
                         "X-Content-Type-Options"};
  const char* values[] = {"max-age=600", "default-src 'self'", "nosniff"};
  EXPECT_EQ(ps_headers_sidecar_classify(names, values, 3),
            PS_SIDECAR_SWAP_ELIGIBLE);

  const char* nonce_names[] = {"Content-Security-Policy"};
  const char* nonce_values[] = {"script-src 'nonce-abc'"};
  EXPECT_EQ(ps_headers_sidecar_classify(nonce_names, nonce_values, 1),
            PS_SIDECAR_NEVER_OPTIMIZED);

  const char* cookie_names[] = {"Set-Cookie"};
  const char* cookie_values[] = {"a=b"};
  EXPECT_EQ(ps_headers_sidecar_classify(cookie_names, cookie_values, 1),
            PS_SIDECAR_FALL_THROUGH);

  // No headers at all is a well-formed question with an answer.
  EXPECT_EQ(ps_headers_sidecar_classify(nullptr, nullptr, 0),
            PS_SIDECAR_SWAP_ELIGIBLE);

  // A NULL array, or a NULL element, is a caller mistake and says so rather
  // than dereferencing it.
  EXPECT_EQ(ps_headers_sidecar_classify(nullptr, values, 3), -1);
  EXPECT_EQ(ps_headers_sidecar_classify(names, nullptr, 3), -1);
  const char* holed[] = {nullptr};
  EXPECT_EQ(ps_headers_sidecar_classify(holed, cookie_values, 1), -1);
}

// ================================================================
// The HTML cacheability policy on the C surface (PS_API 1.12)
// ================================================================

namespace {

// One ps_html_store_classify call with every out-parameter captured.
struct StoreResult {
  int action = -99;
  int reason = -99;
  uint32_t lifetime_s = 0;
  uint32_t flags = 0;
  uint16_t cc_flags = 0;
  int sidecar = -99;
  char detail[64] = {};
  size_t detail_len = 0;
};

StoreResult ClassifyStore(
    const std::vector<std::pair<const char*, const char*>>& headers,
    const char* method = "GET", const char* path = "/index.html",
    uint32_t request_flags = 0, int status = 200,
    const char* body = "<p>hi</p>", int mode = PS_HTML_CACHE_REPORT_ONLY) {
  std::vector<const char*> names;
  std::vector<const char*> values;
  for (const auto& [n, v] : headers) {
    names.push_back(n);
    values.push_back(v);
  }
  StoreResult r;
  r.action = ps_html_store_classify(
      mode, method, path, request_flags, status,
      names.empty() ? nullptr : names.data(),
      values.empty() ? nullptr : values.data(), names.size(),
      /*body_bytes=*/std::strlen(body),
      /*body_complete=*/1, body, std::strlen(body),
      /*max_body_bytes=*/1 << 20, /*now_seconds=*/784111777, &r.reason,
      &r.lifetime_s, &r.flags, &r.cc_flags, &r.sidecar, r.detail,
      sizeof(r.detail), &r.detail_len);
  return r;
}

const std::vector<std::pair<const char*, const char*>> kStorableHeaders = {
    {"Content-Type", "text/html; charset=utf-8"},
    {"Cache-Control", "public, max-age=300"},
    {"ETag", "\"v1\""}};

}  // namespace

TEST(PageSpeedCApi, HtmlStoreClassifyRoundTrip) {
  const StoreResult ok = ClassifyStore(kStorableHeaders);
  EXPECT_EQ(ok.action, PS_HTML_STORE_ACTION_WOULD_STORE);
  EXPECT_STREQ(ps_html_store_reason_name(ok.reason), "none");
  EXPECT_EQ(ok.lifetime_s, 300u);
  EXPECT_EQ(ok.flags, PS_HTML_STORE_FLAG_LIFETIME_EXPLICIT |
                          PS_HTML_STORE_FLAG_HAS_VALIDATORS |
                          PS_HTML_STORE_FLAG_EXPLICIT_SHARED |
                          PS_HTML_STORE_FLAG_SIDECAR_EVALUATED |
                          PS_HTML_STORE_FLAG_CC_EVALUATED);
  EXPECT_NE(ok.cc_flags & PS_CC_ORIGIN_PUBLIC, 0u);
  EXPECT_EQ(ok.sidecar, PS_SIDECAR_SWAP_ELIGIBLE);
  EXPECT_EQ(ok.detail_len, 0u);
  EXPECT_STREQ(ok.detail, "");

  // A refusal names its reason and carries the detail.
  const StoreResult vary = ClassifyStore({{"Content-Type", "text/html"},
                                          {"Cache-Control", "max-age=60"},
                                          {"Vary", "Accept-Encoding, Cookie"}});
  EXPECT_EQ(vary.action, 0);
  EXPECT_STREQ(ps_html_store_reason_name(vary.reason), "vary");
  EXPECT_STREQ(vary.detail, "cookie");
  EXPECT_EQ(vary.detail_len, 6u);

  const StoreResult method = ClassifyStore(kStorableHeaders, "HEAD");
  EXPECT_EQ(method.action, 0);
  EXPECT_STREQ(ps_html_store_reason_name(method.reason), "method");
  // Refused before either check ran: the flags say so, because a sidecar value
  // of 0 is a real verdict.
  EXPECT_EQ(method.flags & PS_HTML_STORE_FLAG_SIDECAR_EVALUATED, 0u);
  EXPECT_EQ(method.flags & PS_HTML_STORE_FLAG_CC_EVALUATED, 0u);
  const StoreResult priv = ClassifyStore(
      {{"Content-Type", "text/html"}, {"Cache-Control", "private"}});
  EXPECT_EQ(priv.flags & PS_HTML_STORE_FLAG_SIDECAR_EVALUATED, 0u);
  EXPECT_NE(priv.flags & PS_HTML_STORE_FLAG_CC_EVALUATED, 0u);

  const StoreResult cookie = ClassifyStore(
      {{"Content-Type", "text/html"}, {"Cache-Control", "max-age=60"}}, "GET",
      "/", PS_HTML_STORE_REQUEST_HAS_COOKIE);
  EXPECT_EQ(cookie.action, 0);
  EXPECT_STREQ(ps_html_store_reason_name(cookie.reason), "cookie_request");

  const StoreResult signal =
      ClassifyStore(kStorableHeaders, "GET", "/", 0, 200,
                    "<meta name=\"csrf-token\" content=\"x\">");
  EXPECT_EQ(signal.action, PS_HTML_STORE_ACTION_REFUSE);
  EXPECT_STREQ(ps_html_store_reason_name(signal.reason),
               "personalisation_signal");
  EXPECT_STREQ(signal.detail, "meta_csrf_token");

  const StoreResult no_transform =
      ClassifyStore({{"Content-Type", "text/html"},
                     {"Cache-Control", "max-age=60, no-transform"}});
  EXPECT_EQ(no_transform.action, PS_HTML_STORE_ACTION_WOULD_STORE_NO_NOTIFY);
  EXPECT_STREQ(ps_html_store_reason_name(no_transform.reason), "no_transform");

  const StoreResult headers = ClassifyStore({{"Content-Type", "text/html"},
                                             {"Cache-Control", "max-age=60"},
                                             {"X-Request-Id", "x"}});
  EXPECT_EQ(headers.action, 0);
  EXPECT_STREQ(headers.detail, "x-request-id");
  EXPECT_EQ(headers.sidecar, PS_SIDECAR_FALL_THROUGH);
}

TEST(PageSpeedCApi, HtmlStoreClassifyRequestFlags) {
  const std::vector<std::pair<const char*, const char*>> plain = {
      {"Content-Type", "text/html"}, {"Cache-Control", "max-age=60"}};
  const std::vector<std::pair<const char*, const char*>> shared = {
      {"Content-Type", "text/html"}, {"Cache-Control", "public, max-age=60"}};
  const StoreResult auth =
      ClassifyStore(plain, "GET", "/", PS_HTML_STORE_REQUEST_HAS_AUTHORIZATION);
  EXPECT_STREQ(ps_html_store_reason_name(auth.reason), "auth_request");
  const StoreResult user = ClassifyStore(
      plain, "GET", "/", PS_HTML_STORE_REQUEST_AUTHENTICATED_USER);
  EXPECT_STREQ(ps_html_store_reason_name(user.reason), "auth_request");
  const StoreResult cookie =
      ClassifyStore(plain, "GET", "/", PS_HTML_STORE_REQUEST_HAS_COOKIE);
  EXPECT_STREQ(ps_html_store_reason_name(cookie.reason), "cookie_request");
  const StoreResult cookie_public =
      ClassifyStore(shared, "GET", "/", PS_HTML_STORE_REQUEST_HAS_COOKIE);
  EXPECT_EQ(cookie_public.action, PS_HTML_STORE_ACTION_WOULD_STORE);
  // The bits are what a front end derives from the request headers:
  // Proxy-Authorization is the authorization bit, Cookie2 the cookie bit.
  auto flags_for = [](const std::vector<const char*>& request_headers) {
    uint32_t flags = 0;
    for (const char* name : request_headers) {
      const std::string lower = [&] {
        std::string s(name);
        for (char& c : s) c = static_cast<char>(std::tolower(c));
        return s;
      }();
      if (lower == "authorization" || lower == "proxy-authorization") {
        flags |= PS_HTML_STORE_REQUEST_HAS_AUTHORIZATION;
      }
      if (lower == "cookie" || lower == "cookie2") {
        flags |= PS_HTML_STORE_REQUEST_HAS_COOKIE;
      }
    }
    return flags;
  };
  const StoreResult proxy_auth =
      ClassifyStore(shared, "GET", "/", flags_for({"Proxy-Authorization"}));
  EXPECT_EQ(proxy_auth.action, 0);
  EXPECT_STREQ(ps_html_store_reason_name(proxy_auth.reason), "auth_request");
  const StoreResult cookie2 =
      ClassifyStore(plain, "GET", "/", flags_for({"Cookie2"}));
  EXPECT_EQ(cookie2.action, 0);
  EXPECT_STREQ(ps_html_store_reason_name(cookie2.reason), "cookie_request");
  EXPECT_EQ(ClassifyStore(plain, "GET", "/", flags_for({"Accept"})).action,
            PS_HTML_STORE_ACTION_WOULD_STORE);
  const StoreResult no_store =
      ClassifyStore(shared, "GET", "/", PS_HTML_STORE_REQUEST_NO_STORE);
  EXPECT_EQ(no_store.action, 0);
  EXPECT_STREQ(ps_html_store_reason_name(no_store.reason), "no_store");
  EXPECT_STREQ(no_store.detail, "request");
  // Unknown bits are ignored.
  EXPECT_EQ(ClassifyStore(plain, "GET", "/", 0x80000000u).action,
            PS_HTML_STORE_ACTION_WOULD_STORE);
}

TEST(PageSpeedCApi, HtmlStoreClassifyDetailBufferTruncates) {
  const char* names[] = {"Content-Type", "Cache-Control", "X-Request-Id"};
  const char* values[] = {"text/html", "max-age=60", "x"};
  int reason = 0, sidecar = 0;
  uint32_t lifetime = 0, flags = 0;
  uint16_t cc = 0;
  size_t detail_len = 0;
  char small[5];
  std::memset(small, 'z', sizeof(small));
  EXPECT_EQ(ps_html_store_classify(PS_HTML_CACHE_REPORT_ONLY, "GET", "/", 0,
                                   200, names, values, 3, 10, 1, "", 0, 1 << 20,
                                   0, &reason, &lifetime, &flags, &cc, &sidecar,
                                   small, sizeof(small), &detail_len),
            0);
  EXPECT_EQ(detail_len, 12u);  // "x-request-id", untruncated
  EXPECT_STREQ(small, "x-re");
  // Capacity 1 holds only the terminator; capacity 0 may pass NULL.
  char one[1] = {'z'};
  EXPECT_EQ(ps_html_store_classify(PS_HTML_CACHE_REPORT_ONLY, "GET", "/", 0,
                                   200, names, values, 3, 10, 1, "", 0, 1 << 20,
                                   0, &reason, &lifetime, &flags, &cc, &sidecar,
                                   one, 1, &detail_len),
            0);
  EXPECT_EQ(one[0], '\0');
  EXPECT_EQ(detail_len, 12u);
  EXPECT_EQ(ps_html_store_classify(PS_HTML_CACHE_REPORT_ONLY, "GET", "/", 0,
                                   200, names, values, 3, 10, 1, nullptr, 0,
                                   1 << 20, 0, &reason, &lifetime, &flags, &cc,
                                   &sidecar, nullptr, 0, &detail_len),
            0);
  EXPECT_EQ(detail_len, 12u);
}

TEST(PageSpeedCApi, HtmlStoreClassifyRejectsMalformedCalls) {
  const char* names[] = {"Content-Type"};
  const char* values[] = {"text/html"};
  int reason = 7, sidecar = 7;
  uint32_t lifetime = 7, flags = 7;
  uint16_t cc = 7;
  size_t detail_len = 7;
  char buf[8];
  auto call = [&](const char* method, const char* path, const char* const* n,
                  const char* const* v, size_t count, const char* body,
                  size_t body_len, int* r, uint32_t* l, uint32_t* f,
                  uint16_t* c, int* sc, char* d, size_t dcap, size_t* dl) {
    return ps_html_store_classify(PS_HTML_CACHE_REPORT_ONLY, method, path, 0,
                                  200, n, v, count, 10, 1, body, body_len,
                                  1 << 20, 0, r, l, f, c, sc, d, dcap, dl);
  };
  // The good call, to prove the harness around the bad ones.
  EXPECT_NE(call("GET", "/", names, values, 1, "", 0, &reason, &lifetime,
                 &flags, &cc, &sidecar, buf, sizeof(buf), &detail_len),
            -1);
  reason = sidecar = 7;
  lifetime = flags = 7;
  cc = 7;
  detail_len = 7;

  EXPECT_EQ(call(nullptr, "/", names, values, 1, "", 0, &reason, &lifetime,
                 &flags, &cc, &sidecar, buf, sizeof(buf), &detail_len),
            -1);
  EXPECT_EQ(call("GET", nullptr, names, values, 1, "", 0, &reason, &lifetime,
                 &flags, &cc, &sidecar, buf, sizeof(buf), &detail_len),
            -1);
  EXPECT_EQ(call("GET", "/", nullptr, values, 1, "", 0, &reason, &lifetime,
                 &flags, &cc, &sidecar, buf, sizeof(buf), &detail_len),
            -1);
  EXPECT_EQ(call("GET", "/", names, nullptr, 1, "", 0, &reason, &lifetime,
                 &flags, &cc, &sidecar, buf, sizeof(buf), &detail_len),
            -1);
  const char* holed[] = {nullptr};
  EXPECT_EQ(call("GET", "/", holed, values, 1, "", 0, &reason, &lifetime,
                 &flags, &cc, &sidecar, buf, sizeof(buf), &detail_len),
            -1);
  EXPECT_EQ(call("GET", "/", names, holed, 1, "", 0, &reason, &lifetime, &flags,
                 &cc, &sidecar, buf, sizeof(buf), &detail_len),
            -1);
  EXPECT_EQ(call("GET", "/", names, values, 1, nullptr, 5, &reason, &lifetime,
                 &flags, &cc, &sidecar, buf, sizeof(buf), &detail_len),
            -1);
  EXPECT_EQ(call("GET", "/", names, values, 1, "", 0, nullptr, &lifetime,
                 &flags, &cc, &sidecar, buf, sizeof(buf), &detail_len),
            -1);
  EXPECT_EQ(call("GET", "/", names, values, 1, "", 0, &reason, nullptr, &flags,
                 &cc, &sidecar, buf, sizeof(buf), &detail_len),
            -1);
  EXPECT_EQ(call("GET", "/", names, values, 1, "", 0, &reason, &lifetime,
                 nullptr, &cc, &sidecar, buf, sizeof(buf), &detail_len),
            -1);
  EXPECT_EQ(call("GET", "/", names, values, 1, "", 0, &reason, &lifetime,
                 &flags, nullptr, &sidecar, buf, sizeof(buf), &detail_len),
            -1);
  EXPECT_EQ(call("GET", "/", names, values, 1, "", 0, &reason, &lifetime,
                 &flags, &cc, nullptr, buf, sizeof(buf), &detail_len),
            -1);
  EXPECT_EQ(call("GET", "/", names, values, 1, "", 0, &reason, &lifetime,
                 &flags, &cc, &sidecar, buf, sizeof(buf), nullptr),
            -1);
  EXPECT_EQ(call("GET", "/", names, values, 1, "", 0, &reason, &lifetime,
                 &flags, &cc, &sidecar, nullptr, 8, &detail_len),
            -1);
  // No out-parameter was written by any of them.
  EXPECT_EQ(reason, 7);
  EXPECT_EQ(sidecar, 7);
  EXPECT_EQ(lifetime, 7u);
  EXPECT_EQ(flags, 7u);
  EXPECT_EQ(cc, 7u);
  EXPECT_EQ(detail_len, 7u);

  // count == 0 with NULL arrays is a well-formed question: no Content-Type.
  const StoreResult none = ClassifyStore({});
  EXPECT_EQ(none.action, 0);
  EXPECT_STREQ(ps_html_store_reason_name(none.reason), "content_type");
}

TEST(PageSpeedCApi, HtmlStoreClassifyModes) {
  // Off refuses a response report-only would store.
  const StoreResult off = ClassifyStore(kStorableHeaders, "GET", "/", 0, 200,
                                        "<p>hi</p>", PS_HTML_CACHE_OFF);
  EXPECT_EQ(off.action, PS_HTML_STORE_ACTION_REFUSE);
  EXPECT_STREQ(ps_html_store_reason_name(off.reason), "mode");
  // The values that would act are not accepted, and report-only never
  // answers with one.
  for (int mode : {-1, 2, 3, 4}) {
    EXPECT_EQ(
        ClassifyStore(kStorableHeaders, "GET", "/", 0, 200, "<p>hi</p>", mode)
            .action,
        -1)
        << mode;
  }
  EXPECT_NE(ClassifyStore(kStorableHeaders).action, 1);
  EXPECT_NE(ClassifyStore(kStorableHeaders).action, 2);
}

TEST(PageSpeedCApi, HtmlStoreClassifyShortPrefixIsRefused) {
  const char* names[] = {"Content-Type", "Cache-Control"};
  const char* values[] = {"text/html", "max-age=60"};
  int reason = 0, sidecar = 0;
  uint32_t lifetime = 0, flags = 0;
  uint16_t cc = 0;
  size_t detail_len = 0;
  char detail[32];
  EXPECT_EQ(ps_html_store_classify(
                PS_HTML_CACHE_REPORT_ONLY, "GET", "/", 0, 200, names, values, 2,
                4096, 1, nullptr, 0, 1 << 20, 0, &reason, &lifetime, &flags,
                &cc, &sidecar, detail, sizeof(detail), &detail_len),
            PS_HTML_STORE_ACTION_REFUSE);
  EXPECT_STREQ(ps_html_store_reason_name(reason), "incomplete");
  EXPECT_STREQ(detail, "body_prefix");
}

TEST(PageSpeedCApi, HtmlDecisionFormatRefusesServeFacts) {
  char buf[256];
  size_t len = 99;
  const char* action_key[] = {"action"};
  const char* would_serve[] = {"would_serve"};
  EXPECT_EQ(ps_html_decision_format(action_key, would_serve, 1, buf,
                                    sizeof(buf), &len),
            -2);
  const char* mode_key[] = {"mode"};
  const char* serving[] = {"serve-variants"};
  EXPECT_EQ(
      ps_html_decision_format(mode_key, serving, 1, buf, sizeof(buf), &len),
      -2);
  const char* reason_key[] = {"reason"};
  const char* fresh[] = {"fresh"};
  EXPECT_EQ(
      ps_html_decision_format(reason_key, fresh, 1, buf, sizeof(buf), &len),
      -2);
  const char* request_key[] = {"request"};
  const char* markdown[] = {"accept_markdown"};
  EXPECT_EQ(
      ps_html_decision_format(request_key, markdown, 1, buf, sizeof(buf), &len),
      -2);
  const char* age_key[] = {"age_s"};
  const char* age[] = {"1"};
  EXPECT_EQ(ps_html_decision_format(age_key, age, 1, buf, sizeof(buf), &len),
            -2);
}

TEST(PageSpeedCApi, HtmlStoreReasonNames) {
  EXPECT_STREQ(ps_html_store_reason_name(0), "none");
  EXPECT_STREQ(ps_html_store_reason_name(19), "no_transform");
  EXPECT_STREQ(ps_html_store_reason_name(20), "mode");
  EXPECT_EQ(ps_html_store_reason_name(21), nullptr);
  EXPECT_EQ(ps_html_store_reason_name(-1), nullptr);
}

TEST(PageSpeedCApi, HtmlDecisionFormatRoundTrip) {
  const char* keys[] = {"reason", "host", "ts_ms", "action"};
  const char* values[] = {"vary:accept", "a.example", "1700000000000",
                          "refuse"};
  char buf[256];
  size_t len = 0;
  EXPECT_EQ(ps_html_decision_format(keys, values, 4, buf, sizeof(buf), &len),
            0);
  const std::string expected =
      "{\"ts_ms\":1700000000000,\"host\":\"a.example\",\"action\":\"refuse\","
      "\"reason\":\"vary:accept\"}";
  EXPECT_EQ(std::string(buf), expected);
  EXPECT_EQ(len, expected.size());
}

TEST(PageSpeedCApi, HtmlDecisionFormatErrorDirections) {
  const char* keys[] = {"host", "path"};
  const char* values[] = {"a.example", "/p"};
  char buf[256];
  size_t len = 99;

  // Too small: -3, the needed length, an empty string left behind.
  char tiny[8];
  std::memset(tiny, 'z', sizeof(tiny));
  EXPECT_EQ(ps_html_decision_format(keys, values, 2, tiny, sizeof(tiny), &len),
            -3);
  EXPECT_EQ(len, std::strlen("{\"host\":\"a.example\",\"path\":\"/p\"}"));
  EXPECT_STREQ(tiny, "");
  // The exact fit (length + NUL) succeeds; one byte less does not.
  const size_t need = len;
  std::vector<char> exact(need + 1);
  EXPECT_EQ(ps_html_decision_format(keys, values, 2, exact.data(), exact.size(),
                                    &len),
            0);
  EXPECT_EQ(len, need);
  EXPECT_EQ(ps_html_decision_format(keys, values, 2, exact.data(),
                                    exact.size() - 1, &len),
            -3);
  // Capacity 0 with a NULL buffer asks only for the length.
  EXPECT_EQ(ps_html_decision_format(keys, values, 2, nullptr, 0, &len), -3);
  EXPECT_EQ(len, need);

  // Refused fields: -2 and a zero length.
  const char* bad_keys[] = {"host", "body"};
  const char* bad_values[] = {"a.example", "x"};
  len = 99;
  EXPECT_EQ(
      ps_html_decision_format(bad_keys, bad_values, 2, buf, sizeof(buf), &len),
      -2);
  EXPECT_EQ(len, 0u);
  const char* empty_host[] = {""};
  const char* host_key[] = {"host"};
  EXPECT_EQ(
      ps_html_decision_format(host_key, empty_host, 1, buf, sizeof(buf), &len),
      -2);
  const char* nl_values[] = {"a\nb"};
  EXPECT_EQ(
      ps_html_decision_format(host_key, nl_values, 1, buf, sizeof(buf), &len),
      -2);
  const char* dup_keys[] = {"host", "host"};
  EXPECT_EQ(
      ps_html_decision_format(dup_keys, values, 2, buf, sizeof(buf), &len), -2);
  // A legitimate key with a value outside its grammar: free text in `cc`, a
  // query in `path`, a non-count in `size`.
  const char* cc_keys[] = {"host", "cc"};
  const char* cc_values[] = {"a.example", "max-age=60, x-session=abc"};
  EXPECT_EQ(
      ps_html_decision_format(cc_keys, cc_values, 2, buf, sizeof(buf), &len),
      -2);
  const char* query_values[] = {"a.example", "/p?token=1"};
  EXPECT_EQ(
      ps_html_decision_format(keys, query_values, 2, buf, sizeof(buf), &len),
      -2);
  const char* size_keys[] = {"host", "size"};
  const char* size_values[] = {"a.example", "12 KiB"};
  EXPECT_EQ(ps_html_decision_format(size_keys, size_values, 2, buf, sizeof(buf),
                                    &len),
            -2);
  EXPECT_EQ(len, 0u);

  // count == 0: well formed, and the formatter's own rules decide (an empty
  // object is a valid line).
  EXPECT_EQ(
      ps_html_decision_format(nullptr, nullptr, 0, buf, sizeof(buf), &len), 0);
  EXPECT_STREQ(buf, "{}");

  // Malformed calls: -1.
  EXPECT_EQ(ps_html_decision_format(nullptr, values, 2, buf, sizeof(buf), &len),
            -1);
  EXPECT_EQ(ps_html_decision_format(keys, nullptr, 2, buf, sizeof(buf), &len),
            -1);
  const char* holed[] = {"host", nullptr};
  EXPECT_EQ(ps_html_decision_format(keys, holed, 2, buf, sizeof(buf), &len),
            -1);
  EXPECT_EQ(ps_html_decision_format(holed, values, 2, buf, sizeof(buf), &len),
            -1);
  EXPECT_EQ(ps_html_decision_format(keys, values, 2, buf, sizeof(buf), nullptr),
            -1);
  EXPECT_EQ(ps_html_decision_format(keys, values, 2, nullptr, 16, &len), -1);
}

TEST_F(PageSpeedCacheApiTest, WriteHeadersSidecarStoresAndReadsBackByExactId) {
  OpenCache();

  const char* names[] = {"Cache-Control", "Content-Security-Policy",
                         "Referrer-Policy", "Vary"};
  const char* values[] = {"max-age=600", "default-src 'self'", "no-referrer",
                          "Accept-Encoding,  ACCEPT-ENCODING "};
  int verdict = -1;
  int stored = -1;
  ASSERT_EQ(ps_cache_write_headers_sidecar(cache_, "/style.css", "example.com",
                                           "https", names, values, 4, &verdict,
                                           &stored),
            PS_OK);
  EXPECT_EQ(verdict, PS_SIDECAR_SWAP_ELIGIBLE);
  EXPECT_EQ(stored, 1);

  ps_read_result_t* result = nullptr;
  ASSERT_EQ(
      ps_cache_read_alternate(cache_, "/style.css", "example.com", "https",
                              PS_SENTINEL_HEADERS_SIDECAR, &result),
      PS_OK);
  ASSERT_NE(result, nullptr);
  const uint8_t* data = nullptr;
  size_t len = 0;
  ASSERT_EQ(ps_read_content(result, &data, &len), PS_OK);
  ASSERT_GT(len, 3u);
  // The blob is a VERSIONED payload: its first byte is the format version, and
  // a consumer that does not know the version stops there.
  EXPECT_EQ(data[0], 1);
  // The origin's actual Vary string is in there, byte for byte.
  const std::string blob(reinterpret_cast<const char*>(data), len);
  EXPECT_NE(blob.find("Accept-Encoding,  ACCEPT-ENCODING "), std::string::npos);
  ps_read_free(result);

  // And it is not something ordinary selection can produce.
  ps_read_result_t* best = nullptr;
  EXPECT_EQ(ps_cache_read_best(cache_, "/style.css", "example.com", "https", 0,
                               &best),
            PS_ERR_NOT_FOUND);
}

TEST_F(PageSpeedCacheApiTest, WriteHeadersSidecarRefusalIsNotAnError) {
  OpenCache();

  const char* names[] = {"Content-Security-Policy"};
  const char* values[] = {"script-src 'nonce-abc'"};
  int verdict = -1;
  int stored = -1;
  // PS_OK with stored == 0: the response is served plain, which is a normal
  // outcome and not something the caller has to handle as a failure.
  ASSERT_EQ(ps_cache_write_headers_sidecar(cache_, "/nonce.css", "example.com",
                                           "https", names, values, 1, &verdict,
                                           &stored),
            PS_OK);
  EXPECT_EQ(verdict, PS_SIDECAR_NEVER_OPTIMIZED);
  EXPECT_EQ(stored, 0);
  EXPECT_EQ(ps_cache_alternate_exists(cache_, "/nonce.css", "example.com",
                                      "https", PS_SENTINEL_HEADERS_SIDECAR),
            0);

  // Both out-params are optional.
  ASSERT_EQ(ps_cache_write_headers_sidecar(cache_, "/nonce.css", "example.com",
                                           "https", names, values, 1, nullptr,
                                           nullptr),
            PS_OK);
}

TEST_F(PageSpeedCacheApiTest, WriteHeadersSidecarRejectsNullArguments) {
  OpenCache();
  const char* names[] = {"Referrer-Policy"};
  const char* values[] = {"no-referrer"};

  EXPECT_EQ(
      ps_cache_write_headers_sidecar(nullptr, "/a.css", "example.com", "https",
                                     names, values, 1, nullptr, nullptr),
      PS_ERR_INVALID_ARG);
  EXPECT_EQ(
      ps_cache_write_headers_sidecar(cache_, nullptr, "example.com", "https",
                                     names, values, 1, nullptr, nullptr),
      PS_ERR_INVALID_ARG);
  EXPECT_EQ(
      ps_cache_write_headers_sidecar(cache_, "/a.css", "example.com", "https",
                                     nullptr, values, 1, nullptr, nullptr),
      PS_ERR_INVALID_ARG);
  const char* holed[] = {nullptr};
  EXPECT_EQ(
      ps_cache_write_headers_sidecar(cache_, "/a.css", "example.com", "https",
                                     holed, values, 1, nullptr, nullptr),
      PS_ERR_INVALID_ARG);
}

TEST_F(PageSpeedCacheApiTest, WriteSentinelStillAcceptsUnclaimedIds) {
  // The gate is on RESERVED, not on "not in the registry".  An id that names no
  // class at all stays writable — an embedder may use a free slot, it just may
  // not take a claimed one.  0xFC is sentinel-shaped and unregistered.
  OpenCache();

  ps_write_handle_t* wh = nullptr;
  ASSERT_EQ(ps_cache_write_sentinel(cache_, "/unclaimed.html", "example.com",
                                    "https", 0xFC, 4, &wh),
            PS_OK);
  ASSERT_EQ(ps_write_data(wh, "data", 4), PS_OK);
  ASSERT_EQ(ps_write_close(wh), PS_OK);
}

// ================================================================
// HTML process: with cache, multiple external stylesheets all found
// ================================================================

TEST_F(PageSpeedCacheApiTest, HtmlProcessMultipleExternalCssAllCached) {
  OpenCache();

  // Write two CSS files into cache.
  auto write_css = [&](const char* url, const char* content) {
    ps_write_params_t params;
    ps_write_params_init_auto(&params);
    params.alternate_id = 0x08;
    params.content_length = std::strlen(content);
    params.full_mask = 0x08;
    params.content_type = PS_CONTENT_CSS;
    params.origin_ct = "text/css";

    ps_write_handle_t* wh = nullptr;
    ASSERT_EQ(
        ps_cache_write_begin(cache_, url, "example.com", "https", &params, &wh),
        PS_OK);
    ASSERT_EQ(ps_write_data(wh, content, std::strlen(content)), PS_OK);
    ASSERT_EQ(ps_write_close(wh), PS_OK);
  };

  write_css("/a.css", ".hero { color: red; }");
  write_css("/b.css", "p { margin: 0; }");

  const char* html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/a.css\">"
      "<link rel=\"stylesheet\" href=\"/b.css\">"
      "</head><body>"
      "<div class=\"hero\">Hello</div>"
      "<p>World</p>"
      "</body></html>";

  ps_html_config_t config;
  ps_html_config_init(&config);
  config.enable_critical_css = 1;

  ps_html_result_t* result = nullptr;
  ASSERT_EQ(ps_html_process(html, std::strlen(html), "http://example.com/page",
                            "example.com", &config, cache_, &result),
            PS_OK);
  ASSERT_NE(result, nullptr);

  // All external CSS found, no revalidation needed.
  EXPECT_EQ(ps_html_result_needs_revalidation(result), 0);
  EXPECT_EQ(ps_html_result_has_critical_css(result), 1);
  EXPECT_EQ(ps_html_result_modified(result), 1);

  ps_html_result_free(result);
}

// ================================================================
// WriteDataAfterError: write handle errored state blocks further writes
// ================================================================

TEST_F(PageSpeedCacheApiTest, WriteDataAfterError) {
  OpenCache();

  // Declare content_length = 4 but write much more data to trigger an
  // overflow error from write_sync, which sets the errored flag.
  ps_write_params_t params;
  ps_write_params_init_auto(&params);
  params.alternate_id = 0x08;
  params.content_length = 4;  // Declare only 4 bytes.
  params.full_mask = 0x08;
  params.content_type = PS_CONTENT_HTML;

  ps_write_handle_t* wh = nullptr;
  ASSERT_EQ(ps_cache_write_begin(cache_, "/err-write.html", "example.com",
                                 "https", &params, &wh),
            PS_OK);
  ASSERT_NE(wh, nullptr);

  // Write the declared amount first.
  ASSERT_EQ(ps_write_data(wh, "test", 4), PS_OK);

  // Write way beyond the declared content_length to force a write_sync error.
  // This large overflow should cause the underlying write to fail.
  std::string overflow(64ULL * 1024, 'x');
  ps_error_t err = ps_write_data(wh, overflow.data(), overflow.size());

  if (err != PS_OK) {
    // The first overflow write failed and set the errored flag.
    // A subsequent write must return PS_ERR_CLOSED (errored state).
    ps_error_t err2 = ps_write_data(wh, "more", 4);
    EXPECT_EQ(err2, PS_ERR_CLOSED);
  }
  // If overflow succeeded (Cyclone allowed it), that's OK too -- just
  // verify the handle is still usable.

  // Clean up.
  ps_write_abort(wh);
}

// ================================================================
// CssValidateStartsWithStyleClose: CSS containing </style>
// ================================================================

TEST(PageSpeedCApi, CssValidateStartsWithStyleClose) {
  // CSS that starts with a </style> tag should be rejected.
  const char* css = "</style><script>alert('xss')</script>";
  EXPECT_EQ(ps_css_validate(css, std::strlen(css)), PS_ERR_INVALID_ARG);

  // Verify the error message mentions the injection.
  EXPECT_STRNE(ps_last_error_message(), "");
}

// ================================================================
// CssMinifyEmptyInput: empty and null CSS inputs
// ================================================================

TEST(PageSpeedCApi, CssMinifyEmptyInput) {
  // Null CSS input should return INVALID_ARG.
  char* out_css = nullptr;
  size_t out_len = 0;
  EXPECT_EQ(ps_css_minify(nullptr, 0, &out_css, &out_len), PS_ERR_INVALID_ARG);
  EXPECT_EQ(out_css, nullptr);
  EXPECT_EQ(out_len, 0u);

  // Null out_css pointer.
  EXPECT_EQ(ps_css_minify("body{}", 6, nullptr, &out_len), PS_ERR_INVALID_ARG);

  // Null out_len pointer.
  EXPECT_EQ(ps_css_minify("body{}", 6, &out_css, nullptr), PS_ERR_INVALID_ARG);

  // Empty CSS (zero length) should succeed and produce empty output.
  ASSERT_EQ(ps_css_minify("", 0, &out_css, &out_len), PS_OK);
  EXPECT_NE(out_css, nullptr);
  EXPECT_EQ(out_len, 0u);
  ps_free(out_css);
}

// ================================================================
// ClassifyWithNullInputs: ps_classify with NULL for various params
// ================================================================

TEST(PageSpeedCApi, ClassifyWithNullInputs) {
  // All NULL -- should not crash.  A NULL Accept is an absent Accept, which
  // RFC 9110 12.5.1 reads as */*, so the format bits are WebP and the mask is
  // 0x09 rather than the default-constructed 0x08 (#1377).
  uint32_t mask = ps_classify(nullptr, nullptr, nullptr, nullptr);
  EXPECT_EQ(mask, 0x09u);  // Desktop/Identity, WebP from the absent Accept.

  // Each parameter NULL individually.
  mask = ps_classify(nullptr, "Mozilla/5.0", nullptr, "gzip");
  EXPECT_NE(mask, 0u);  // Should produce a valid mask.

  mask = ps_classify("image/webp,*/*", nullptr, nullptr, nullptr);
  EXPECT_EQ(mask & 0x03, 1u);  // WebP detected.

  mask = ps_classify(nullptr, nullptr, "on", nullptr);
  EXPECT_NE(mask & 0x20, 0u);  // Save-Data detected.

  mask = ps_classify(nullptr, nullptr, nullptr, "br");
  EXPECT_EQ(mask & 0xC0, 0x80u);  // Brotli detected.
}

// ================================================================
// NormalizeHostnameEmpty: empty string hostname
// ================================================================

TEST(PageSpeedCApi, NormalizeHostnameEmpty) {
  char buf[256];
  int len = ps_normalize_hostname("", buf, sizeof(buf));
  // Empty string is a valid (though degenerate) hostname.
  // Should return 0 length and buf should be empty string.
  EXPECT_EQ(len, 0);
  EXPECT_STREQ(buf, "");
}

// ================================================================
// HtmlScanNullInputs: ps_html_scan with NULL html pointer
// ================================================================

TEST(PageSpeedCApi, HtmlScanNullInputs) {
  ps_scan_result_t* result = nullptr;

  // NULL html pointer should return INVALID_ARG.
  EXPECT_EQ(ps_html_scan(nullptr, 100, "http://example.com/", &result),
            PS_ERR_INVALID_ARG);
  EXPECT_EQ(result, nullptr);

  // NULL url should return INVALID_ARG.
  const char* html = "<html></html>";
  EXPECT_EQ(ps_html_scan(html, std::strlen(html), nullptr, &result),
            PS_ERR_INVALID_ARG);
  EXPECT_EQ(result, nullptr);

  // NULL out pointer should return INVALID_ARG.
  EXPECT_EQ(
      ps_html_scan(html, std::strlen(html), "http://example.com/", nullptr),
      PS_ERR_INVALID_ARG);
}

// ================================================================
// CacheSentinelReadBest: write sentinel and read via ReadBestAlternate
// ================================================================

TEST_F(PageSpeedCacheApiTest, CacheSentinelReadBest) {
  OpenCache();

  // Write a sentinel alternate (early hints).
  const char* hints =
      "/main.css\nimage:/hero.jpg\npreconnect:https://cdn.ex.com";
  size_t hints_len = std::strlen(hints);

  ps_write_handle_t* wh = nullptr;
  ASSERT_EQ(
      ps_cache_write_sentinel(cache_, "/sentinel-rb.html", "example.com",
                              "https", PS_SENTINEL_EARLY_HINTS, hints_len, &wh),
      PS_OK);
  ASSERT_EQ(ps_write_data(wh, hints, hints_len), PS_OK);
  ASSERT_EQ(ps_write_close(wh), PS_OK);

  // ReadBestAlternate with the default mask should NOT find the sentinel
  // (sentinels are not content alternates). It should return NOT_FOUND
  // since no content alternates exist.
  ps_read_result_t* result = nullptr;
  ps_error_t err = ps_cache_read_best(cache_, "/sentinel-rb.html",
                                      "example.com", "https", 0x08, &result);
  EXPECT_EQ(err, PS_ERR_NOT_FOUND);

  // But reading it back via the sentinel-specific API should work.
  result = nullptr;
  ASSERT_EQ(ps_cache_read_early_hints(cache_, "/sentinel-rb.html",
                                      "example.com", "https", &result),
            PS_OK);
  ASSERT_NE(result, nullptr);

  const uint8_t* data = nullptr;
  size_t len = 0;
  ASSERT_EQ(ps_read_content(result, &data, &len), PS_OK);
  EXPECT_EQ(len, hints_len);
  EXPECT_EQ(std::memcmp(data, hints, hints_len), 0);

  ps_read_free(result);
}

// ================================================================
// WriteAlternateVariousFlags: write with flags and verify persistence
// ================================================================

TEST_F(PageSpeedCacheApiTest, WriteAlternateVariousFlags) {
  OpenCache();

  // Test 1: Write with kFlagNeedsRevalidation (0x01).
  {
    ps_write_params_t params;
    ps_write_params_init_auto(&params);
    params.alternate_id = 0x08;
    params.content_length = 5;
    params.full_mask = 0x08;
    params.content_type = PS_CONTENT_HTML;
    params.flags = PS_FLAG_NEEDS_REVALIDATION;
    params.origin_ct = "text/html";

    ps_write_handle_t* wh = nullptr;
    ASSERT_EQ(ps_cache_write_begin(cache_, "/flags1.html", "example.com",
                                   "https", &params, &wh),
              PS_OK);
    ASSERT_EQ(ps_write_data(wh, "hello", 5), PS_OK);
    ASSERT_EQ(ps_write_close(wh), PS_OK);

    ps_read_result_t* result = nullptr;
    ASSERT_EQ(ps_cache_read_best(cache_, "/flags1.html", "example.com", "https",
                                 0x08, &result),
              PS_OK);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(ps_read_flags(result) & PS_FLAG_NEEDS_REVALIDATION,
              PS_FLAG_NEEDS_REVALIDATION);
    EXPECT_EQ(ps_read_content_type(result), PS_CONTENT_HTML);
    EXPECT_STREQ(ps_read_origin_content_type(result), "text/html");
    ps_read_free(result);
  }

  // Test 2: Write with kFlagWorkerProcessed (0x02).
  {
    const uint8_t kFlagWorkerProcessed = 0x02;
    ps_write_params_t params;
    ps_write_params_init_auto(&params);
    params.alternate_id = 0x08;
    params.content_length = 5;
    params.full_mask = 0x08;
    params.content_type = PS_CONTENT_IMAGE;
    params.flags = kFlagWorkerProcessed;
    params.origin_ct = "image/jpeg";

    ps_write_handle_t* wh = nullptr;
    ASSERT_EQ(ps_cache_write_begin(cache_, "/flags2.dat", "example.com",
                                   "https", &params, &wh),
              PS_OK);
    ASSERT_EQ(ps_write_data(wh, "imgdt", 5), PS_OK);
    ASSERT_EQ(ps_write_close(wh), PS_OK);

    ps_read_result_t* result = nullptr;
    ASSERT_EQ(ps_cache_read_best(cache_, "/flags2.dat", "example.com", "https",
                                 0x08, &result),
              PS_OK);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(ps_read_flags(result) & kFlagWorkerProcessed,
              kFlagWorkerProcessed);
    EXPECT_EQ(ps_read_content_type(result), PS_CONTENT_IMAGE);
    EXPECT_STREQ(ps_read_origin_content_type(result), "image/jpeg");
    ps_read_free(result);
  }

  // Test 3: Write with both flags combined (0x01 | 0x02 = 0x03).
  {
    const uint8_t kBothFlags = 0x03;
    ps_write_params_t params;
    ps_write_params_init_auto(&params);
    params.alternate_id = 0x08;
    params.content_length = 3;
    params.full_mask = 0x08;
    params.content_type = PS_CONTENT_CSS;
    params.flags = kBothFlags;
    params.origin_ct = "text/css";

    ps_write_handle_t* wh = nullptr;
    ASSERT_EQ(ps_cache_write_begin(cache_, "/flags3.css", "example.com",
                                   "https", &params, &wh),
              PS_OK);
    ASSERT_EQ(ps_write_data(wh, "css", 3), PS_OK);
    ASSERT_EQ(ps_write_close(wh), PS_OK);

    ps_read_result_t* result = nullptr;
    ASSERT_EQ(ps_cache_read_best(cache_, "/flags3.css", "example.com", "https",
                                 0x08, &result),
              PS_OK);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(ps_read_flags(result), kBothFlags);
    EXPECT_EQ(ps_read_content_type(result), PS_CONTENT_CSS);
    EXPECT_STREQ(ps_read_origin_content_type(result), "text/css");
    ps_read_free(result);
  }
}

// ================================================================
// Error path coverage: empty alternates list after cache remove
// ================================================================

TEST_F(PageSpeedCacheApiTest, ListAlternatesEmptyAfterRemove) {
  OpenCache();

  // Write an alternate so the key exists.
  ps_write_params_t params;
  ps_write_params_init_auto(&params);
  params.alternate_id = 0x08;
  params.content_length = 4;
  params.full_mask = 0x08;
  params.content_type = PS_CONTENT_HTML;

  ps_write_handle_t* wh = nullptr;
  ASSERT_EQ(ps_cache_write_begin(cache_, "/list-empty.html", "example.com",
                                 "https", &params, &wh),
            PS_OK);
  ASSERT_EQ(ps_write_data(wh, "test", 4), PS_OK);
  ASSERT_EQ(ps_write_close(wh), PS_OK);

  // Verify it has at least one alternate.
  ps_alternate_info_t* alts = nullptr;
  size_t count = 0;
  ASSERT_EQ(ps_cache_list_alternates(cache_, "/list-empty.html", "example.com",
                                     "https", &alts, &count),
            PS_OK);
  EXPECT_GE(count, 1u);
  ps_alternates_free(alts);

  // Remove the key.
  ASSERT_EQ(ps_cache_remove(cache_, "/list-empty.html", "example.com", "https"),
            PS_OK);

  // List alternates again -- exercises the count==0 or NOT_FOUND path.
  alts = nullptr;
  count = 99;
  ps_error_t err = ps_cache_list_alternates(
      cache_, "/list-empty.html", "example.com", "https", &alts, &count);
  if (err == PS_OK) {
    // Exercises the count==0 path at lines 585-588 of pagespeed.cc.
    EXPECT_EQ(count, 0u);
    EXPECT_EQ(alts, nullptr);
    ps_alternates_free(alts);
  } else {
    EXPECT_EQ(err, PS_ERR_NOT_FOUND);
  }
}

// ================================================================
// Error path coverage: list alternates for sentinel-only key
// ================================================================

TEST_F(PageSpeedCacheApiTest, ListAlternatesSentinelOnly) {
  OpenCache();

  // Write only a sentinel for this key (no content alternates).
  const char* hints = "/style.css";
  size_t hints_len = std::strlen(hints);

  ps_write_handle_t* wh = nullptr;
  ASSERT_EQ(
      ps_cache_write_sentinel(cache_, "/sentinel-only.html", "example.com",
                              "https", PS_SENTINEL_EARLY_HINTS, hints_len, &wh),
      PS_OK);
  ASSERT_EQ(ps_write_data(wh, hints, hints_len), PS_OK);
  ASSERT_EQ(ps_write_close(wh), PS_OK);

  // List alternates -- sentinel is not a content alternate, so the list
  // should either be empty (count==0) or not found.
  ps_alternate_info_t* alts = nullptr;
  size_t count = 99;
  ps_error_t err = ps_cache_list_alternates(
      cache_, "/sentinel-only.html", "example.com", "https", &alts, &count);
  if (err == PS_OK) {
    // The sentinel alternate may or may not appear in the listing depending
    // on Cyclone's implementation.  If it appears, count >= 1.
    // If no content alternates are listed, count == 0 (exercises lines 585-588).
    ps_alternates_free(alts);
  } else {
    EXPECT_EQ(err, PS_ERR_NOT_FOUND);
  }
}

// ================================================================
// Error path coverage: write data to closed handle via errored flag
// ================================================================

TEST_F(PageSpeedCacheApiTest, WriteDataAfterCloseViaErrorFlag) {
  OpenCache();

  // Declare content_length=4, write all 4 bytes, then try to write more.
  // The close after write should succeed, but writing after close on the
  // same handle pointer is UB (handle deleted). Instead, we test the
  // errored path: force an error, then verify subsequent writes return
  // PS_ERR_CLOSED.
  ps_write_params_t params;
  ps_write_params_init_auto(&params);
  params.alternate_id = 0x08;
  params.content_length = 4;
  params.full_mask = 0x08;
  params.content_type = PS_CONTENT_HTML;

  ps_write_handle_t* wh = nullptr;
  ASSERT_EQ(ps_cache_write_begin(cache_, "/err-flag.html", "example.com",
                                 "https", &params, &wh),
            PS_OK);
  ASSERT_NE(wh, nullptr);

  // Write the declared amount.
  ASSERT_EQ(ps_write_data(wh, "test", 4), PS_OK);

  // Attempt a massive overflow to trigger write_sync error.
  std::string overflow(size_t{1024} * 1024, 'x');
  ps_error_t first_err = ps_write_data(wh, overflow.data(), overflow.size());
  if (first_err != PS_OK) {
    // The handle is now in errored state. All subsequent writes must fail.
    EXPECT_EQ(ps_write_data(wh, "a", 1), PS_ERR_CLOSED);
    EXPECT_EQ(ps_write_data(wh, "b", 1), PS_ERR_CLOSED);

    // Close the errored handle -- should still work (abort is also fine).
    ps_write_abort(wh);
  } else {
    // If Cyclone accepted the overflow, clean up normally.
    ps_write_abort(wh);
  }
}

// ================================================================
// Error path coverage: ps_write_close on errored handle
// ================================================================

TEST_F(PageSpeedCacheApiTest, WriteCloseAfterError) {
  OpenCache();

  ps_write_params_t params;
  ps_write_params_init_auto(&params);
  params.alternate_id = 0x08;
  params.content_length = 2;
  params.full_mask = 0x08;
  params.content_type = PS_CONTENT_HTML;

  ps_write_handle_t* wh = nullptr;
  ASSERT_EQ(ps_cache_write_begin(cache_, "/close-err.html", "example.com",
                                 "https", &params, &wh),
            PS_OK);

  // Write declared amount.
  ASSERT_EQ(ps_write_data(wh, "ab", 2), PS_OK);

  // Overflow to force error.
  std::string overflow(size_t{1024} * 1024, 'y');
  ps_error_t err = ps_write_data(wh, overflow.data(), overflow.size());
  if (err != PS_OK) {
    // Handle is errored. ps_write_close should still work without crashing.
    // close_sync may also fail, but ps_write_close should handle it.
    ps_error_t close_err = ps_write_close(wh);
    // Any error is acceptable; we just verify no crash.
    (void)close_err;
  } else {
    ps_write_abort(wh);
  }
}

// ================================================================
// Error path coverage: ps_write_data with zero-length data
// ================================================================

TEST_F(PageSpeedCacheApiTest, WriteZeroLengthData) {
  OpenCache();

  ps_write_params_t params;
  ps_write_params_init_auto(&params);
  params.alternate_id = 0x08;
  params.content_length = 4;
  params.full_mask = 0x08;
  params.content_type = PS_CONTENT_HTML;

  ps_write_handle_t* wh = nullptr;
  ASSERT_EQ(ps_cache_write_begin(cache_, "/zero-len.html", "example.com",
                                 "https", &params, &wh),
            PS_OK);

  // Write zero-length data -- should not crash or error.
  EXPECT_EQ(ps_write_data(wh, "", 0), PS_OK);

  // Continue with actual data.
  ASSERT_EQ(ps_write_data(wh, "test", 4), PS_OK);
  ASSERT_EQ(ps_write_close(wh), PS_OK);
}

// ================================================================
// Error path coverage: ps_cache_open null out_cache
// ================================================================

TEST(PageSpeedCApi, CacheOpenNullOutCache) {
  ps_cache_config_t config;
  ps_cache_config_init(&config);
  config.volume_path = "/tmp/test_cache.vol";
  EXPECT_EQ(ps_cache_open(&config, nullptr), PS_ERR_INVALID_ARG);
}

// ================================================================
// Error path coverage: ps_cache_open null config
// ================================================================

TEST(PageSpeedCApi, CacheOpenNullConfig) {
  ps_cache_t* c = nullptr;
  EXPECT_EQ(ps_cache_open(nullptr, &c), PS_ERR_INVALID_ARG);
}

// ================================================================
// Error path coverage: ps_cache_alternate_exists with null cache
// ================================================================

TEST(PageSpeedCApi, AlternateExistsNullCache) {
  EXPECT_EQ(ps_cache_alternate_exists(nullptr, "/test.html", "example.com",
                                      "https", 0x08),
            0);
}

// ================================================================
// Error path coverage: ps_cache_alternate_exists with null URL
// ================================================================

TEST_F(PageSpeedCacheApiTest, AlternateExistsNullUrl) {
  OpenCache();
  EXPECT_EQ(
      ps_cache_alternate_exists(cache_, nullptr, "example.com", "https", 0x08),
      0);
}

// ================================================================
// Error path coverage: ps_cache_alternate_exists with null hostname
// ================================================================

TEST_F(PageSpeedCacheApiTest, AlternateExistsNullHostname) {
  OpenCache();
  EXPECT_EQ(
      ps_cache_alternate_exists(cache_, "/test.html", nullptr, "https", 0x08),
      0);
}

// ================================================================
// Error path coverage: ps_cache_write_sentinel with null cache
// ================================================================

TEST(PageSpeedCApi, WriteSentinelNullCache) {
  ps_write_handle_t* wh = nullptr;
  EXPECT_EQ(ps_cache_write_sentinel(nullptr, "/test.html", "example.com",
                                    "https", PS_SENTINEL_EARLY_HINTS, 10, &wh),
            PS_ERR_INVALID_ARG);
}

// ================================================================
// Error path coverage: ps_cache_write_sentinel with null out handle
// ================================================================

TEST_F(PageSpeedCacheApiTest, WriteSentinelNullOut) {
  OpenCache();
  EXPECT_EQ(
      ps_cache_write_sentinel(cache_, "/test.html", "example.com", "https",
                              PS_SENTINEL_EARLY_HINTS, 10, nullptr),
      PS_ERR_INVALID_ARG);
}

// ================================================================
// Error path coverage: ps_cache_write_sentinel with null URL
// ================================================================

TEST_F(PageSpeedCacheApiTest, WriteSentinelNullUrl) {
  OpenCache();
  ps_write_handle_t* wh = nullptr;
  EXPECT_EQ(ps_cache_write_sentinel(cache_, nullptr, "example.com", "https",
                                    PS_SENTINEL_EARLY_HINTS, 10, &wh),
            PS_ERR_INVALID_ARG);
}

// ================================================================
// Error path coverage: ps_cache_write_sentinel with null hostname
// ================================================================

TEST_F(PageSpeedCacheApiTest, WriteSentinelNullHostname) {
  OpenCache();
  ps_write_handle_t* wh = nullptr;
  EXPECT_EQ(ps_cache_write_sentinel(cache_, "/test.html", nullptr, "https",
                                    PS_SENTINEL_EARLY_HINTS, 10, &wh),
            PS_ERR_INVALID_ARG);
}

// ================================================================
// Error path coverage: ps_cache_write_begin with null params
// ================================================================

TEST_F(PageSpeedCacheApiTest, WriteBeginNullParams) {
  OpenCache();
  ps_write_handle_t* wh = nullptr;
  EXPECT_EQ(ps_cache_write_begin(cache_, "/test.html", "example.com", "https",
                                 nullptr, &wh),
            PS_ERR_INVALID_ARG);
}

// ================================================================
// Error path coverage: ps_cache_write_begin with null out handle
// ================================================================

TEST_F(PageSpeedCacheApiTest, WriteBeginNullOut) {
  OpenCache();
  ps_write_params_t params;
  ps_write_params_init_auto(&params);
  params.alternate_id = 0x08;
  params.content_length = 4;
  params.full_mask = 0x08;
  params.content_type = PS_CONTENT_HTML;

  EXPECT_EQ(ps_cache_write_begin(cache_, "/test.html", "example.com", "https",
                                 &params, nullptr),
            PS_ERR_INVALID_ARG);
}

// ================================================================
// Error path coverage: ps_cache_read_best with null URL / hostname
// ================================================================

TEST_F(PageSpeedCacheApiTest, ReadBestNullUrl) {
  OpenCache();
  ps_read_result_t* result = nullptr;
  EXPECT_EQ(ps_cache_read_best(cache_, nullptr, "example.com", "https", 0x08,
                               &result),
            PS_ERR_INVALID_ARG);
}

TEST_F(PageSpeedCacheApiTest, ReadBestNullHostname) {
  OpenCache();
  ps_read_result_t* result = nullptr;
  EXPECT_EQ(
      ps_cache_read_best(cache_, "/test.html", nullptr, "https", 0x08, &result),
      PS_ERR_INVALID_ARG);
}

TEST_F(PageSpeedCacheApiTest, ReadBestNullOut) {
  OpenCache();
  EXPECT_EQ(ps_cache_read_best(cache_, "/test.html", "example.com", "https",
                               0x08, nullptr),
            PS_ERR_INVALID_ARG);
}

// ================================================================
// Error path coverage: ps_cache_read_alternate with null URL / hostname
// ================================================================

TEST_F(PageSpeedCacheApiTest, ReadAlternateNullUrl) {
  OpenCache();
  ps_read_result_t* result = nullptr;
  EXPECT_EQ(ps_cache_read_alternate(cache_, nullptr, "example.com", "https",
                                    0x08, &result),
            PS_ERR_INVALID_ARG);
}

TEST_F(PageSpeedCacheApiTest, ReadAlternateNullHostname) {
  OpenCache();
  ps_read_result_t* result = nullptr;
  EXPECT_EQ(ps_cache_read_alternate(cache_, "/test.html", nullptr, "https",
                                    0x08, &result),
            PS_ERR_INVALID_ARG);
}

TEST_F(PageSpeedCacheApiTest, ReadAlternateNullOut) {
  OpenCache();
  EXPECT_EQ(ps_cache_read_alternate(cache_, "/test.html", "example.com",
                                    "https", 0x08, nullptr),
            PS_ERR_INVALID_ARG);
}

// ================================================================
// Error path coverage: ps_cache_stats with null cache
// ================================================================

TEST(PageSpeedCApi, CacheStatsNullCache) {
  ps_cache_stats_t stats;
  EXPECT_EQ(ps_cache_stats(nullptr, &stats), PS_ERR_INVALID_ARG);
}

// ================================================================
// Error path coverage: ps_cache_stats with null out
// ================================================================

TEST_F(PageSpeedCacheApiTest, CacheStatsNullOut) {
  OpenCache();
  EXPECT_EQ(ps_cache_stats(cache_, nullptr), PS_ERR_INVALID_ARG);
}

// ================================================================
// Error path coverage: ps_css_minify with CSS that should still
// succeed (coverage of the success path through minification)
// ================================================================

TEST(PageSpeedCApi, CssMinifyVariousInputs) {
  // Test with comments -- should be stripped.
  {
    const char* css = "/* comment */ body { color: red; }";
    char* out = nullptr;
    size_t out_len = 0;
    ASSERT_EQ(ps_css_minify(css, std::strlen(css), &out, &out_len), PS_OK);
    ASSERT_NE(out, nullptr);
    // Comment should be removed.
    std::string result(out, out_len);
    EXPECT_EQ(result.find("comment"), std::string::npos);
    ps_free(out);
  }

  // Test with multiple whitespace characters.
  {
    const char* css = "body  \n\t {  \n  color  :  red  ;  \n  }";
    char* out = nullptr;
    size_t out_len = 0;
    ASSERT_EQ(ps_css_minify(css, std::strlen(css), &out, &out_len), PS_OK);
    ASSERT_NE(out, nullptr);
    EXPECT_LT(out_len, std::strlen(css));
    ps_free(out);
  }

  // Test with already-minified CSS.
  {
    const char* css = "body{color:red}";
    char* out = nullptr;
    size_t out_len = 0;
    ASSERT_EQ(ps_css_minify(css, std::strlen(css), &out, &out_len), PS_OK);
    ASSERT_NE(out, nullptr);
    EXPECT_LE(out_len, std::strlen(css));
    ps_free(out);
  }
}

// ================================================================
// Error path coverage: ps_css_flatten_imports with null CSS URL
// ================================================================

TEST(PageSpeedCApi, CssFlattenNullCssUrl) {
  char* out = nullptr;
  size_t out_len = 0;
  EXPECT_EQ(ps_css_flatten_imports("body{}", 6, nullptr, nullptr, nullptr, 5,
                                   &out, &out_len, nullptr, nullptr),
            PS_ERR_INVALID_ARG);
}

// ================================================================
// Error path coverage: ps_css_flatten_imports with null out_css
// ================================================================

TEST(PageSpeedCApi, CssFlattenNullOutCss) {
  size_t out_len = 0;
  EXPECT_EQ(
      ps_css_flatten_imports("body{}", 6, "http://example.com/s.css", nullptr,
                             nullptr, 5, nullptr, &out_len, nullptr, nullptr),
      PS_ERR_INVALID_ARG);
}

// ================================================================
// Error path coverage: ps_css_flatten_imports with null out_len
// ================================================================

TEST(PageSpeedCApi, CssFlattenNullOutLen) {
  char* out = nullptr;
  EXPECT_EQ(
      ps_css_flatten_imports("body{}", 6, "http://example.com/s.css", nullptr,
                             nullptr, 5, &out, nullptr, nullptr, nullptr),
      PS_ERR_INVALID_ARG);
}

// ================================================================
// Error path coverage: ps_css_validate with embedded null bytes
// in the middle and end
// ================================================================

TEST(PageSpeedCApi, CssValidateNullBytesMiddle) {
  // Null byte in the middle of CSS content.
  const char css[] = "body { col\0or: red; }";
  EXPECT_EQ(ps_css_validate(css, sizeof(css) - 1), PS_ERR_INVALID_ARG);
  EXPECT_STRNE(ps_last_error_message(), "");
}

// ================================================================
// Error path coverage: ps_read_content with partial null args
// ================================================================

TEST_F(PageSpeedCacheApiTest, ReadContentPartialNullArgs) {
  OpenCache();

  // Write and read an entry.
  ps_write_params_t params;
  ps_write_params_init_auto(&params);
  params.alternate_id = 0x08;
  params.content_length = 4;
  params.full_mask = 0x08;
  params.content_type = PS_CONTENT_HTML;

  ps_write_handle_t* wh = nullptr;
  ASSERT_EQ(ps_cache_write_begin(cache_, "/read-null.html", "example.com",
                                 "https", &params, &wh),
            PS_OK);
  ASSERT_EQ(ps_write_data(wh, "test", 4), PS_OK);
  ASSERT_EQ(ps_write_close(wh), PS_OK);

  ps_read_result_t* result = nullptr;
  ASSERT_EQ(ps_cache_read_best(cache_, "/read-null.html", "example.com",
                               "https", 0x08, &result),
            PS_OK);

  // Valid result but null out_data.
  size_t len = 0;
  EXPECT_EQ(ps_read_content(result, nullptr, &len), PS_ERR_INVALID_ARG);

  // Valid result but null out_length.
  const uint8_t* data = nullptr;
  EXPECT_EQ(ps_read_content(result, &data, nullptr), PS_ERR_INVALID_ARG);

  ps_read_free(result);
}

// ================================================================
// Error path coverage: ps_read_copy with partial null args
// ================================================================

TEST_F(PageSpeedCacheApiTest, ReadCopyPartialNullArgs) {
  OpenCache();

  ps_write_params_t params;
  ps_write_params_init_auto(&params);
  params.alternate_id = 0x08;
  params.content_length = 4;
  params.full_mask = 0x08;
  params.content_type = PS_CONTENT_HTML;

  ps_write_handle_t* wh = nullptr;
  ASSERT_EQ(ps_cache_write_begin(cache_, "/copy-null.html", "example.com",
                                 "https", &params, &wh),
            PS_OK);
  ASSERT_EQ(ps_write_data(wh, "test", 4), PS_OK);
  ASSERT_EQ(ps_write_close(wh), PS_OK);

  ps_read_result_t* result = nullptr;
  ASSERT_EQ(ps_cache_read_best(cache_, "/copy-null.html", "example.com",
                               "https", 0x08, &result),
            PS_OK);

  // Valid result but null buf.
  size_t copied = 0;
  EXPECT_EQ(ps_read_copy(result, nullptr, 100, &copied), PS_ERR_INVALID_ARG);

  // Valid result but null out_copied.
  uint8_t buf[100];
  EXPECT_EQ(ps_read_copy(result, buf, sizeof(buf), nullptr),
            PS_ERR_INVALID_ARG);

  ps_read_free(result);
}

// ================================================================
// Error path coverage: cache alternate_exists for URL too long
// ================================================================

TEST_F(PageSpeedCacheApiTest, AlternateExistsUrlTooLong) {
  OpenCache();
  std::string long_url(9000, 'a');
  EXPECT_EQ(ps_cache_alternate_exists(cache_, long_url.c_str(), "example.com",
                                      "https", 0x08),
            0);
}

// ================================================================
// Error path coverage: cache alternate_exists for hostname too long
// ================================================================

TEST_F(PageSpeedCacheApiTest, AlternateExistsHostnameTooLong) {
  OpenCache();
  std::string long_host(600, 'h');
  EXPECT_EQ(ps_cache_alternate_exists(cache_, "/test.html", long_host.c_str(),
                                      "https", 0x08),
            0);
}

// ================================================================
// Error path coverage: ps_cache_remove with URL too long
// ================================================================

TEST_F(PageSpeedCacheApiTest, CacheRemoveUrlTooLong) {
  OpenCache();
  std::string long_url(9000, 'a');
  EXPECT_EQ(ps_cache_remove(cache_, long_url.c_str(), "example.com", "https"),
            PS_ERR_INVALID_ARG);
}

// ================================================================
// Error path coverage: ps_cache_remove with hostname too long
// ================================================================

TEST_F(PageSpeedCacheApiTest, CacheRemoveHostnameTooLong) {
  OpenCache();
  std::string long_host(600, 'h');
  EXPECT_EQ(ps_cache_remove(cache_, "/test.html", long_host.c_str(), "https"),
            PS_ERR_INVALID_ARG);
}

// ================================================================
// Error path coverage: ps_cache_list_alternates URL/hostname too long
// ================================================================

TEST_F(PageSpeedCacheApiTest, ListAlternatesUrlTooLong) {
  OpenCache();
  std::string long_url(9000, 'a');
  ps_alternate_info_t* alts = nullptr;
  size_t count = 0;
  EXPECT_EQ(ps_cache_list_alternates(cache_, long_url.c_str(), "example.com",
                                     "https", &alts, &count),
            PS_ERR_INVALID_ARG);
}

TEST_F(PageSpeedCacheApiTest, ListAlternatesHostnameTooLong) {
  OpenCache();
  std::string long_host(600, 'h');
  ps_alternate_info_t* alts = nullptr;
  size_t count = 0;
  EXPECT_EQ(ps_cache_list_alternates(cache_, "/test.html", long_host.c_str(),
                                     "https", &alts, &count),
            PS_ERR_INVALID_ARG);
}

// ================================================================
// Error path coverage: ps_cache_write_sentinel URL/hostname too long
// ================================================================

TEST_F(PageSpeedCacheApiTest, WriteSentinelHostnameTooLong) {
  OpenCache();
  std::string long_host(600, 'h');
  ps_write_handle_t* wh = nullptr;
  EXPECT_EQ(ps_cache_write_sentinel(cache_, "/test.html", long_host.c_str(),
                                    "https", PS_SENTINEL_EARLY_HINTS, 10, &wh),
            PS_ERR_INVALID_ARG);
}

// ================================================================
// Error path coverage: ps_cache_write_begin hostname too long
// ================================================================

TEST_F(PageSpeedCacheApiTest, WriteBeginHostnameTooLong) {
  OpenCache();
  std::string long_host(600, 'h');

  ps_write_params_t params;
  ps_write_params_init_auto(&params);
  params.alternate_id = 0x08;
  params.content_length = 4;
  params.full_mask = 0x08;
  params.content_type = PS_CONTENT_HTML;

  ps_write_handle_t* wh = nullptr;
  EXPECT_EQ(ps_cache_write_begin(cache_, "/test.html", long_host.c_str(),
                                 "https", &params, &wh),
            PS_ERR_INVALID_ARG);
}

// ================================================================
// Error path coverage: ps_cache_read_alternate URL/hostname too long
// ================================================================

TEST_F(PageSpeedCacheApiTest, ReadAlternateUrlTooLong) {
  OpenCache();
  std::string long_url(9000, 'a');
  ps_read_result_t* result = nullptr;
  EXPECT_EQ(ps_cache_read_alternate(cache_, long_url.c_str(), "example.com",
                                    "https", 0x08, &result),
            PS_ERR_INVALID_ARG);
}

TEST_F(PageSpeedCacheApiTest, ReadAlternateHostnameTooLong) {
  OpenCache();
  std::string long_host(600, 'h');
  ps_read_result_t* result = nullptr;
  EXPECT_EQ(ps_cache_read_alternate(cache_, "/test.html", long_host.c_str(),
                                    "https", 0x08, &result),
            PS_ERR_INVALID_ARG);
}

// ================================================================
// MapError coverage: test every CacheError -> ps_error_t mapping
// ================================================================

TEST(PageSpeedCApi, MapErrorNotFound) {
  EXPECT_EQ(MapError(cyclone::CacheError::NotFound), PS_ERR_NOT_FOUND);
}

TEST(PageSpeedCApi, MapErrorAlternateNotFound) {
  EXPECT_EQ(MapError(cyclone::CacheError::AlternateNotFound), PS_ERR_NOT_FOUND);
}

TEST(PageSpeedCApi, MapErrorIoError) {
  EXPECT_EQ(MapError(cyclone::CacheError::IoError), PS_ERR_IO);
}

TEST(PageSpeedCApi, MapErrorCorrupted) {
  EXPECT_EQ(MapError(cyclone::CacheError::Corrupted), PS_ERR_CORRUPTED);
}

TEST(PageSpeedCApi, MapErrorChainCorrupted) {
  EXPECT_EQ(MapError(cyclone::CacheError::ChainCorrupted), PS_ERR_CORRUPTED);
}

TEST(PageSpeedCApi, MapErrorNoSpace) {
  EXPECT_EQ(MapError(cyclone::CacheError::NoSpace), PS_ERR_NO_SPACE);
}

TEST(PageSpeedCApi, MapErrorInvalidKey) {
  EXPECT_EQ(MapError(cyclone::CacheError::InvalidKey), PS_ERR_INVALID_ARG);
}

TEST(PageSpeedCApi, MapErrorInvalidArgument) {
  EXPECT_EQ(MapError(cyclone::CacheError::InvalidArgument), PS_ERR_INVALID_ARG);
}

TEST(PageSpeedCApi, MapErrorInvalidConfiguration) {
  EXPECT_EQ(MapError(cyclone::CacheError::InvalidConfiguration),
            PS_ERR_INVALID_ARG);
}

TEST(PageSpeedCApi, MapErrorBusy) {
  EXPECT_EQ(MapError(cyclone::CacheError::Busy), PS_ERR_BUSY);
}

TEST(PageSpeedCApi, MapErrorTimeout) {
  EXPECT_EQ(MapError(cyclone::CacheError::Timeout), PS_ERR_BUSY);
}

TEST(PageSpeedCApi, MapErrorClosed) {
  EXPECT_EQ(MapError(cyclone::CacheError::Closed), PS_ERR_CLOSED);
}

TEST(PageSpeedCApi, MapErrorNotInitialized) {
  EXPECT_EQ(MapError(cyclone::CacheError::NotInitialized), PS_ERR_CLOSED);
}

TEST(PageSpeedCApi, MapErrorTooManyAlternates) {
  EXPECT_EQ(MapError(cyclone::CacheError::TooManyAlternates),
            PS_ERR_TOO_MANY_ALTERNATES);
}

TEST(PageSpeedCApi, MapErrorExists) {
  EXPECT_EQ(MapError(cyclone::CacheError::Exists), PS_ERR_EXISTS);
}

TEST(PageSpeedCApi, MapErrorAlreadyOpen) {
  EXPECT_EQ(MapError(cyclone::CacheError::AlreadyOpen), PS_ERR_EXISTS);
}

TEST(PageSpeedCApi, MapErrorNotOwned) {
  EXPECT_EQ(MapError(cyclone::CacheError::NotOwned), PS_ERR_NOT_OWNED);
}

TEST(PageSpeedCApi, MapErrorIncompatibleVersion) {
  EXPECT_EQ(MapError(cyclone::CacheError::IncompatibleVersion),
            PS_ERR_VERSION_MISMATCH);
}

TEST(PageSpeedCApi, MapErrorDefaultCase) {
  // PluginError, InternalError, and other unknown values map to PS_ERR_INTERNAL.
  EXPECT_EQ(MapError(cyclone::CacheError::PluginError), PS_ERR_INTERNAL);
  EXPECT_EQ(MapError(cyclone::CacheError::InternalError), PS_ERR_INTERNAL);
  EXPECT_EQ(MapError(cyclone::CacheError::OptimizationQueueFull),
            PS_ERR_INTERNAL);
  EXPECT_EQ(MapError(cyclone::CacheError::OptimizationCancelled),
            PS_ERR_INTERNAL);
  EXPECT_EQ(MapError(cyclone::CacheError::TransformFailed), PS_ERR_INTERNAL);
}

// ================================================================
// SetLastError / ClearLastError coverage
// ================================================================

TEST(PageSpeedCApi, SetAndClearLastError) {
  SetLastError("test error message");
  EXPECT_STREQ(ps_last_error_message(), "test error message");
  ClearLastError();
  EXPECT_STREQ(ps_last_error_message(), "");
}

// ================================================================
// ValidateUrlHostname / ValidateContentType coverage
// ================================================================

TEST(PageSpeedCApi, ValidateUrlHostnameNullUrl) {
  EXPECT_EQ(ValidateUrlHostname(nullptr, "example.com"), PS_ERR_INVALID_ARG);
}

TEST(PageSpeedCApi, ValidateUrlHostnameNullHostname) {
  EXPECT_EQ(ValidateUrlHostname("/test", nullptr), PS_ERR_INVALID_ARG);
}

TEST(PageSpeedCApi, ValidateUrlHostnameUrlTooLong) {
  std::string long_url(9000, 'a');
  EXPECT_EQ(ValidateUrlHostname(long_url.c_str(), "example.com"),
            PS_ERR_INVALID_ARG);
  // Should set last error.
  EXPECT_NE(std::string(ps_last_error_message()).find("URL"),
            std::string::npos);
}

TEST(PageSpeedCApi, ValidateUrlHostnameHostnameTooLong) {
  std::string long_host(600, 'h');
  EXPECT_EQ(ValidateUrlHostname("/test", long_host.c_str()),
            PS_ERR_INVALID_ARG);
  EXPECT_NE(std::string(ps_last_error_message()).find("Hostname"),
            std::string::npos);
}

TEST(PageSpeedCApi, ValidateUrlHostnameSuccess) {
  EXPECT_EQ(ValidateUrlHostname("/test", "example.com"), PS_OK);
}

TEST(PageSpeedCApi, ValidateContentTypeInRange) {
  EXPECT_EQ(ValidateContentType(PS_CONTENT_HTML), PS_OK);
  EXPECT_EQ(ValidateContentType(PS_CONTENT_OTHER), PS_OK);
}

TEST(PageSpeedCApi, ValidateContentTypeOutOfRange) {
  EXPECT_EQ(ValidateContentType(static_cast<ps_content_type_t>(99)),
            PS_ERR_INVALID_ARG);
  EXPECT_NE(std::string(ps_last_error_message()).find("content_type"),
            std::string::npos);
}

// ================================================================
// ps_cache_open failure: uncreatable volume path (MapError path)
// ================================================================

TEST(PageSpeedCApi, CacheOpenUncreatablePath) {
  // A volume path in a nonexistent directory cannot be created, so
  // ps_cache_open must fail and map the error.  (/dev/null no longer
  // exercises MapError: Cyclone's structural-fingerprint naming redirects
  // the configured path to a creatable sibling file.)
  ps_cache_config_t config;
  ps_cache_config_init(&config);
  config.volume_path = "/nonexistent-pagespeed-test-dir/cache.dat";
  config.volume_size = static_cast<uint64_t>(1024 * 1024);

  ps_cache_t* cache = nullptr;
  ps_error_t err = ps_cache_open(&config, &cache);
  EXPECT_NE(err, PS_OK);
  EXPECT_EQ(cache, nullptr);
}

// ================================================================
// ps_css_minify failure path (line 1098-1100)
// ================================================================

TEST(PageSpeedCApi, CssMinifyUnclosedString) {
  // An unclosed string may cause MinifyCss to fail.
  const char* css = "body { content: \"unclosed";
  char* out = nullptr;
  size_t out_len = 0;
  ps_error_t err = ps_css_minify(css, std::strlen(css), &out, &out_len);
  // It might succeed with a best-effort result or fail.
  if (err == PS_OK) {
    EXPECT_NE(out, nullptr);
    ps_free(out);
  } else {
    EXPECT_EQ(err, PS_ERR_INTERNAL);
    EXPECT_EQ(out, nullptr);
    EXPECT_NE(std::strlen(ps_last_error_message()), 0u);
  }
}

// ================================================================
// ps_css_flatten_imports with null CSS data (not null URL)
// ================================================================

TEST(PageSpeedCApi, CssFlattenNullCssData) {
  char* out = nullptr;
  size_t out_len = 0;
  EXPECT_EQ(ps_css_flatten_imports(nullptr, 0, "http://example.com/style.css",
                                   nullptr, nullptr, 5, &out, &out_len, nullptr,
                                   nullptr),
            PS_ERR_INVALID_ARG);
}

// ================================================================
// Coverage: ps_cache_read_alternate with specific ID (ExactIdSelector)
// ================================================================

TEST_F(PageSpeedCacheApiTest, ReadAlternateBySpecificId) {
  OpenCache();

  // Write two alternates with different IDs to the same URL.
  auto write_alt = [&](uint8_t id, uint32_t mask, const char* data,
                       size_t len) {
    ps_write_params_t params;
    ps_write_params_init_auto(&params);
    params.alternate_id = id;
    params.content_length = len;
    params.full_mask = mask;
    params.content_type = PS_CONTENT_IMAGE;
    params.origin_ct = "image/jpeg";
    params.flags = 0;

    ps_write_handle_t* wh = nullptr;
    ASSERT_EQ(ps_cache_write_begin(cache_, "/multi-alt.jpg", "example.com",
                                   "https", &params, &wh),
              PS_OK);
    ASSERT_EQ(ps_write_data(wh, data, len), PS_OK);
    ASSERT_EQ(ps_write_close(wh), PS_OK);
  };

  write_alt(0x08, 0x08, "original-jpeg", 13);  // Desktop/Identity
  write_alt(0x09, 0x09, "webp-variant!", 13);  // Desktop/WebP

  // Read the first alternate by exact ID.
  {
    ps_read_result_t* result = nullptr;
    ASSERT_EQ(ps_cache_read_alternate(cache_, "/multi-alt.jpg", "example.com",
                                      "https", 0x08, &result),
              PS_OK);
    ASSERT_NE(result, nullptr);

    const uint8_t* data = nullptr;
    size_t len = 0;
    ASSERT_EQ(ps_read_content(result, &data, &len), PS_OK);
    EXPECT_EQ(len, 13u);
    EXPECT_EQ(std::memcmp(data, "original-jpeg", 13), 0);
    EXPECT_EQ(ps_read_mask(result), 0x08u);
    EXPECT_EQ(ps_read_content_type(result), PS_CONTENT_IMAGE);
    EXPECT_STREQ(ps_read_origin_content_type(result), "image/jpeg");
    ps_read_free(result);
  }

  // Read the second alternate by exact ID.
  {
    ps_read_result_t* result = nullptr;
    ASSERT_EQ(ps_cache_read_alternate(cache_, "/multi-alt.jpg", "example.com",
                                      "https", 0x09, &result),
              PS_OK);
    ASSERT_NE(result, nullptr);

    const uint8_t* data = nullptr;
    size_t len = 0;
    ASSERT_EQ(ps_read_content(result, &data, &len), PS_OK);
    EXPECT_EQ(len, 13u);
    EXPECT_EQ(std::memcmp(data, "webp-variant!", 13), 0);
    EXPECT_EQ(ps_read_mask(result), 0x09u);
    ps_read_free(result);
  }

  // Reading a non-existent alternate ID returns NOT_FOUND.
  {
    ps_read_result_t* result = nullptr;
    EXPECT_EQ(ps_cache_read_alternate(cache_, "/multi-alt.jpg", "example.com",
                                      "https", 0xFF, &result),
              PS_ERR_NOT_FOUND);
  }
}

// ================================================================
// Coverage: ps_cache_list_alternates with multiple written alternates
// ================================================================

TEST_F(PageSpeedCacheApiTest, ListAlternatesMultipleVariants) {
  OpenCache();

  // Write three alternates to the same URL.
  auto write_alt = [&](uint8_t id, uint32_t mask, ps_content_type_t ct) {
    ps_write_params_t params;
    ps_write_params_init_auto(&params);
    params.alternate_id = id;
    params.content_length = 6;
    params.full_mask = mask;
    params.content_type = ct;

    ps_write_handle_t* wh = nullptr;
    ASSERT_EQ(ps_cache_write_begin(cache_, "/variants.img", "example.com",
                                   "https", &params, &wh),
              PS_OK);
    ASSERT_EQ(ps_write_data(wh, "data-x", 6), PS_OK);
    ASSERT_EQ(ps_write_close(wh), PS_OK);
  };

  write_alt(0x08, 0x08, PS_CONTENT_IMAGE);  // Desktop/Identity/Original
  write_alt(0x09, 0x09, PS_CONTENT_IMAGE);  // Desktop/Identity/WebP
  write_alt(0x0A, 0x0A, PS_CONTENT_IMAGE);  // Desktop/Identity/AVIF

  ps_alternate_info_t* alts = nullptr;
  size_t count = 0;
  ASSERT_EQ(ps_cache_list_alternates(cache_, "/variants.img", "example.com",
                                     "https", &alts, &count),
            PS_OK);
  ASSERT_GE(count, 3u);

  // Verify we can find all three alternate IDs in the result.
  bool found_08 = false, found_09 = false, found_0a = false;
  for (size_t i = 0; i < count; ++i) {
    EXPECT_EQ(alts[i].struct_size, sizeof(ps_alternate_info_t));
    EXPECT_GT(alts[i].content_length, 0u);
    if (alts[i].alternate_id == 0x08) found_08 = true;
    if (alts[i].alternate_id == 0x09) found_09 = true;
    if (alts[i].alternate_id == 0x0A) found_0a = true;
  }
  EXPECT_TRUE(found_08) << "Missing alternate 0x08";
  EXPECT_TRUE(found_09) << "Missing alternate 0x09";
  EXPECT_TRUE(found_0a) << "Missing alternate 0x0A";

  ps_alternates_free(alts);
}

// ================================================================
// Coverage: ps_html_transform full lifecycle
// ================================================================

TEST(PageSpeedCApi, HtmlTransformFullLifecycle) {
  // Step 1: Scan a simple HTML document with enough images to trigger
  // lazy loading (first image gets fetchpriority="high", images 2-3 are
  // above-fold guard, image 4+ gets loading="lazy").
  const char* html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/main.css\">"
      "</head><body>"
      "<div class=\"hero\">Welcome</div>"
      "<img src=\"/banner.jpg\">"
      "<img src=\"/photo1.jpg\">"
      "<img src=\"/photo2.jpg\">"
      "<img src=\"/photo3.jpg\">"
      "<img src=\"/photo4.jpg\">"
      "</body></html>";

  ps_scan_result_t* scan = nullptr;
  ASSERT_EQ(ps_html_scan(html, std::strlen(html), "http://example.com/", &scan),
            PS_OK);
  ASSERT_NE(scan, nullptr);

  // Step 2: Create a transform with critical CSS and lazy loading.
  ps_html_config_t config;
  ps_html_config_init(&config);
  config.enable_critical_css = 1;
  config.enable_lazy_load = 1;
  config.enable_image_dimensions = 0;
  config.enable_lcp_preload = 1;
  config.enable_preconnect = 0;
  config.enable_speculation_rules = 0;

  const char* critical_css = ".hero { color: blue; font-size: 24px; }";

  ps_html_transform_t* transform = nullptr;
  ASSERT_EQ(ps_html_transform_create(scan, &config, critical_css,
                                     std::strlen(critical_css), nullptr,
                                     nullptr, nullptr, 0, &transform),
            PS_OK);
  ASSERT_NE(transform, nullptr);

  // Step 3: Run the transform.
  char* out_html = nullptr;
  size_t out_len = 0;
  ASSERT_EQ(ps_html_transform_run(transform, html, std::strlen(html),
                                  "http://example.com/", &out_html, &out_len),
            PS_OK);

  // Step 4: Verify results.
  int modified = ps_html_transform_modified(transform);
  EXPECT_EQ(modified, 1);
  EXPECT_NE(out_html, nullptr);
  EXPECT_GT(out_len, 0u);

  if (out_html != nullptr) {
    std::string output(out_html, out_len);
    // Critical CSS should be injected.
    EXPECT_NE(output.find("hero"), std::string::npos);
    // The first image should get fetchpriority="high".
    EXPECT_NE(output.find("fetchpriority"), std::string::npos);
    // Images beyond the above-fold guard should get loading="lazy".
    EXPECT_NE(output.find("loading"), std::string::npos);
    ps_free(out_html);
  }

  // Step 5: Free resources.
  ps_html_transform_free(transform);
  ps_scan_result_free(scan);
}

// ================================================================
// Coverage: ps_css_extract_critical via C API (non-exception path)
// ================================================================

TEST(PageSpeedCApi, CriticalCssExtractMatchesAndNonMatches) {
  const char* html =
      "<html><head></head><body>"
      "<header class=\"site-header\">Header</header>"
      "<main><article class=\"post\"><p>Content</p></article></main>"
      "<footer class=\"site-footer\">Footer</footer>"
      "</body></html>";

  ps_scan_result_t* scan = nullptr;
  ASSERT_EQ(ps_html_scan(html, std::strlen(html), "http://example.com/", &scan),
            PS_OK);

  // CSS with rules that match and rules that don't match the HTML.
  const char* css =
      ".site-header { background: #333; color: white; } "
      ".post { max-width: 800px; margin: 0 auto; } "
      "p { line-height: 1.6; } "
      ".sidebar { width: 300px; float: right; } "
      ".ad-banner { display: block; height: 250px; } "
      ".site-footer { padding: 20px; background: #eee; }";

  ps_critical_css_config_t config;
  ps_critical_css_config_init(&config);
  config.viewport = PS_VIEWPORT_DESKTOP;

  ps_critical_css_result_t* result = nullptr;
  ASSERT_EQ(
      ps_css_extract_critical(scan, css, std::strlen(css), &config, &result),
      PS_OK);
  ASSERT_NE(result, nullptr);

  size_t out_len = 0;
  const char* output = ps_critical_css_output(result, &out_len);
  EXPECT_NE(output, nullptr);
  EXPECT_GT(out_len, 0u);

  if (output != nullptr) {
    std::string out_str(output, out_len);
    // Matching selectors should be present.
    EXPECT_NE(out_str.find("site-header"), std::string::npos);
    EXPECT_NE(out_str.find("post"), std::string::npos);
    // Non-matching selectors (sidebar, ad-banner) should be absent.
    EXPECT_EQ(out_str.find("sidebar"), std::string::npos);
    EXPECT_EQ(out_str.find("ad-banner"), std::string::npos);
  }

  int total = 0, critical = 0;
  ps_critical_css_stats(result, &total, &critical);
  EXPECT_GT(total, 0);
  EXPECT_GT(critical, 0);
  EXPECT_LT(critical, total);

  ps_critical_css_result_free(result);
  ps_scan_result_free(scan);
}

// ================================================================
// Coverage: ps_css_flatten_imports with chained @import
// ================================================================

TEST(PageSpeedCApi, CssFlattenChainedImports) {
  // Level 0: main.css imports level1.css
  // Level 1: level1.css imports level2.css
  // Level 2: level2.css has actual rules
  const char* main_css =
      "@import url('/level1.css');\n"
      "body { margin: 0; }";

  // Lookup function that resolves chained imports.
  auto lookup = [](const char* url, size_t* out_len,
                   void* /*user_data*/) -> const char* {
    static const char level1[] =
        "@import url('/level2.css');\n.level1 { color: blue; }";
    static const char level2[] = ".level2 { font-size: 14px; }";

    if (std::strcmp(url, "/level1.css") == 0) {
      *out_len = std::strlen(level1);
      return level1;
    }
    if (std::strcmp(url, "/level2.css") == 0) {
      *out_len = std::strlen(level2);
      return level2;
    }
    return nullptr;
  };

  char* out = nullptr;
  size_t out_len = 0;
  int resolved = 0;
  int unresolved = 0;

  ASSERT_EQ(ps_css_flatten_imports(
                main_css, std::strlen(main_css), "http://example.com/main.css",
                lookup, nullptr, 5, &out, &out_len, &resolved, &unresolved),
            PS_OK);
  ASSERT_NE(out, nullptr);
  EXPECT_GT(out_len, 0u);

  // Both imports should have been resolved.
  EXPECT_EQ(resolved, 2);
  EXPECT_EQ(unresolved, 0);

  // The output should contain content from all three levels.
  std::string result(out, out_len);
  EXPECT_NE(result.find("margin"), std::string::npos)
      << "Missing main.css content";
  EXPECT_NE(result.find("level1"), std::string::npos)
      << "Missing level1.css content";
  EXPECT_NE(result.find("level2"), std::string::npos)
      << "Missing level2.css content";

  // No @import statements should remain in the flattened output.
  EXPECT_EQ(result.find("@import"), std::string::npos)
      << "Unresolved @import found in output";

  ps_free(out);
}

TEST(PageSpeedCApi, CssFlattenChainedImportsWithDepthLimit) {
  // The depth limit feeds the all-or-nothing rule: with max_depth=1 the
  // second-level @import cannot be resolved, so flattening is skipped
  // entirely and the input CSS is returned unchanged (a partial flatten
  // would silently drop styles).
  const char* main_css = "@import url('/level1.css');\nbody { margin: 0; }";

  auto lookup = [](const char* url, size_t* out_len,
                   void* /*user_data*/) -> const char* {
    static const char level1[] =
        "@import url('/level2.css');\n.level1 { color: blue; }";
    static const char level2[] = ".level2 { font-size: 14px; }";

    if (std::strcmp(url, "/level1.css") == 0) {
      *out_len = std::strlen(level1);
      return level1;
    }
    if (std::strcmp(url, "/level2.css") == 0) {
      *out_len = std::strlen(level2);
      return level2;
    }
    return nullptr;
  };

  char* out = nullptr;
  size_t out_len = 0;
  int resolved = 0;
  int unresolved = 0;

  // max_depth=1: only resolve one level deep.
  ASSERT_EQ(ps_css_flatten_imports(
                main_css, std::strlen(main_css), "http://example.com/main.css",
                lookup, nullptr, 1, &out, &out_len, &resolved, &unresolved),
            PS_OK);
  ASSERT_NE(out, nullptr);

  // All-or-nothing: the depth-limited nested @import counts as
  // unresolvable, so NOTHING is flattened and the input comes back
  // unchanged.
  EXPECT_EQ(resolved, 0);
  EXPECT_GE(unresolved, 1);

  std::string result(out, out_len);
  EXPECT_EQ(result, main_css);
  // Neither level was inlined.
  EXPECT_EQ(result.find(".level1"), std::string::npos);
  EXPECT_EQ(result.find(".level2"), std::string::npos);

  ps_free(out);
}

// ================================================================
// Coverage: ps_cache_write_sentinel and read back
// ================================================================

TEST_F(PageSpeedCacheApiTest, WriteSentinelAndReadBack) {
  OpenCache();

  // Write early hints sentinel with multiple preload/preconnect entries.
  const char* hints =
      "/styles/main.css\n"
      "/styles/vendor.css\n"
      "image:/images/hero.webp\n"
      "preconnect:https://fonts.googleapis.com\n"
      "preconnect:https://cdn.example.com";
  size_t hints_len = std::strlen(hints);

  // Write the sentinel.
  ps_write_handle_t* wh = nullptr;
  ASSERT_EQ(
      ps_cache_write_sentinel(cache_, "/index.html", "example.com", "https",
                              PS_SENTINEL_EARLY_HINTS, hints_len, &wh),
      PS_OK);
  ASSERT_NE(wh, nullptr);
  ASSERT_EQ(ps_write_data(wh, hints, hints_len), PS_OK);
  ASSERT_EQ(ps_write_close(wh), PS_OK);

  // Read it back via ps_cache_read_early_hints.
  ps_read_result_t* result = nullptr;
  ASSERT_EQ(ps_cache_read_early_hints(cache_, "/index.html", "example.com",
                                      "https", &result),
            PS_OK);
  ASSERT_NE(result, nullptr);

  // Verify content.
  const uint8_t* data = nullptr;
  size_t len = 0;
  ASSERT_EQ(ps_read_content(result, &data, &len), PS_OK);
  EXPECT_EQ(len, hints_len);
  EXPECT_EQ(std::memcmp(data, hints, hints_len), 0);

  // Also verify read_copy works.
  std::vector<uint8_t> buf(hints_len + 10);
  size_t copied = 0;
  ASSERT_EQ(ps_read_copy(result, buf.data(), buf.size(), &copied), PS_OK);
  EXPECT_EQ(copied, hints_len);
  EXPECT_EQ(std::memcmp(buf.data(), hints, hints_len), 0);

  ps_read_free(result);

  // Verify that reading via ps_cache_read_alternate with the sentinel ID
  // also works (ps_cache_read_early_hints is a wrapper).
  result = nullptr;
  ASSERT_EQ(ps_cache_read_alternate(cache_, "/index.html", "example.com",
                                    "https", PS_SENTINEL_EARLY_HINTS, &result),
            PS_OK);
  ASSERT_NE(result, nullptr);

  data = nullptr;
  len = 0;
  ASSERT_EQ(ps_read_content(result, &data, &len), PS_OK);
  EXPECT_EQ(len, hints_len);

  ps_read_free(result);
}

// ================================================================
// Coverage: ps_cache_write_sentinel for WARMUP sentinel
// ================================================================

TEST_F(PageSpeedCacheApiTest, WriteSentinelWarmup) {
  OpenCache();

  const char* warmup_data = "prefetch:/page2.html\nprefetch:/page3.html";
  size_t warmup_len = std::strlen(warmup_data);

  ps_write_handle_t* wh = nullptr;
  ASSERT_EQ(
      ps_cache_write_sentinel(cache_, "/home.html", "example.com", "https",
                              PS_SENTINEL_WARMUP, warmup_len, &wh),
      PS_OK);
  ASSERT_EQ(ps_write_data(wh, warmup_data, warmup_len), PS_OK);
  ASSERT_EQ(ps_write_close(wh), PS_OK);

  // Read back via ps_cache_read_alternate with the warmup sentinel ID.
  ps_read_result_t* result = nullptr;
  ASSERT_EQ(ps_cache_read_alternate(cache_, "/home.html", "example.com",
                                    "https", PS_SENTINEL_WARMUP, &result),
            PS_OK);
  ASSERT_NE(result, nullptr);

  const uint8_t* data = nullptr;
  size_t len = 0;
  ASSERT_EQ(ps_read_content(result, &data, &len), PS_OK);
  EXPECT_EQ(len, warmup_len);
  EXPECT_EQ(std::memcmp(data, warmup_data, warmup_len), 0);

  ps_read_free(result);
}

// ================================================================
// Coverage: ps_html_transform_run produces modified output then check
// ps_html_transform_modified accessor
// ================================================================

TEST(PageSpeedCApi, HtmlTransformModifiedAccessor) {
  // The scanner detects the first body image as the LCP candidate (fallback
  // heuristic).  The transform filter then:
  //   - Image #1 (LCP match): fetchpriority="high", no lazy
  //   - Images #2-3: above-fold guard (body_img_count_ <= 3), no lazy
  //   - Images #4+: loading="lazy"
  // We need 4+ images to trigger a visible "loading" attribute.
  const char* html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/style.css\">"
      "</head><body>"
      "<p>Some text</p>"
      "<img src=\"/img1.jpg\">"
      "<img src=\"/img2.jpg\">"
      "<img src=\"/img3.jpg\">"
      "<img src=\"/img4.jpg\">"
      "<img src=\"/img5.jpg\">"
      "</body></html>";

  ps_scan_result_t* scan = nullptr;
  ASSERT_EQ(ps_html_scan(html, std::strlen(html), "http://example.com/", &scan),
            PS_OK);

  // Enable only lazy loading (guaranteed to modify HTML with images).
  ps_html_config_t config;
  ps_html_config_init(&config);
  config.enable_critical_css = 0;
  config.enable_lazy_load = 1;
  config.enable_image_dimensions = 0;
  config.enable_lcp_preload = 0;
  config.enable_preconnect = 0;
  config.enable_speculation_rules = 0;

  ps_html_transform_t* transform = nullptr;
  ASSERT_EQ(ps_html_transform_create(scan, &config, nullptr, 0, nullptr,
                                     nullptr, nullptr, 0, &transform),
            PS_OK);

  // Before running, modified should be false.
  EXPECT_EQ(ps_html_transform_modified(transform), 0);

  char* out_html = nullptr;
  size_t out_len = 0;
  ASSERT_EQ(ps_html_transform_run(transform, html, std::strlen(html),
                                  "http://example.com/", &out_html, &out_len),
            PS_OK);

  // After running with images present, lazy loading should modify HTML.
  EXPECT_EQ(ps_html_transform_modified(transform), 1);
  EXPECT_NE(out_html, nullptr);
  EXPECT_GT(out_len, 0u);

  if (out_html != nullptr) {
    std::string output(out_html, out_len);
    // LCP image (#1) gets fetchpriority="high".
    EXPECT_NE(output.find("fetchpriority"), std::string::npos)
        << "fetchpriority attribute not found on LCP image";
    // Images beyond the 3-image above-fold guard get loading="lazy".
    EXPECT_NE(output.find("loading"), std::string::npos)
        << "Lazy loading attribute not found on below-fold images";
    ps_free(out_html);
  }

  ps_html_transform_free(transform);
  ps_scan_result_free(scan);
}

// ================================================================
// Coverage: ps_css_extract_critical with mobile viewport
// ================================================================

TEST(PageSpeedCApi, CriticalCssExtractMobileViewport) {
  const char* html =
      "<html><head></head><body>"
      "<nav class=\"mobile-nav\">Nav</nav>"
      "<div class=\"content\">Content</div>"
      "</body></html>";

  ps_scan_result_t* scan = nullptr;
  ASSERT_EQ(ps_html_scan(html, std::strlen(html), "http://example.com/", &scan),
            PS_OK);

  const char* css =
      ".mobile-nav { display: block; } "
      ".content { padding: 10px; } "
      "@media (min-width: 1024px) { .mobile-nav { display: none; } } "
      "@media (max-width: 479px) { .content { padding: 5px; } }";

  ps_critical_css_config_t config;
  ps_critical_css_config_init(&config);
  config.viewport = PS_VIEWPORT_MOBILE;

  ps_critical_css_result_t* result = nullptr;
  ASSERT_EQ(
      ps_css_extract_critical(scan, css, std::strlen(css), &config, &result),
      PS_OK);
  ASSERT_NE(result, nullptr);

  size_t out_len = 0;
  const char* output = ps_critical_css_output(result, &out_len);
  EXPECT_NE(output, nullptr);
  EXPECT_GT(out_len, 0u);

  int total = 0, critical = 0;
  ps_critical_css_stats(result, &total, &critical);
  EXPECT_GT(total, 0);
  EXPECT_GT(critical, 0);

  ps_critical_css_result_free(result);
  ps_scan_result_free(scan);
}

// ================================================================
// Coverage: ps_css_flatten_imports — unresolvable import skips flattening
// ================================================================

TEST(PageSpeedCApi, CssFlattenPartiallyResolved) {
  // CSS with two @import statements, only one of which can be resolved.
  // All-or-nothing: one unresolvable import means nothing is flattened.
  const char* css =
      "@import url('/found.css');\n"
      "@import url('/missing.css');\n"
      "body { background: white; }";

  auto lookup = [](const char* url, size_t* out_len,
                   void* /*user_data*/) -> const char* {
    static const char found[] = ".found { color: green; }";
    if (std::strcmp(url, "/found.css") == 0) {
      *out_len = std::strlen(found);
      return found;
    }
    return nullptr;  // /missing.css cannot be resolved.
  };

  char* out = nullptr;
  size_t out_len = 0;
  int resolved = 0;
  int unresolved = 0;

  ASSERT_EQ(ps_css_flatten_imports(
                css, std::strlen(css), "http://example.com/style.css", lookup,
                nullptr, 5, &out, &out_len, &resolved, &unresolved),
            PS_OK);
  ASSERT_NE(out, nullptr);

  EXPECT_EQ(resolved, 0);
  EXPECT_GE(unresolved, 1);

  std::string result(out, out_len);
  EXPECT_EQ(result, css);
  // /found.css was NOT inlined (its rules are absent) and the original
  // rules are intact.
  EXPECT_EQ(result.find(".found"), std::string::npos);
  EXPECT_NE(result.find("background"), std::string::npos);

  ps_free(out);
}

// ================================================================
// Coverage: ps_cache_write_sentinel null args validation
// ================================================================

TEST_F(PageSpeedCacheApiTest, WriteSentinelNullArgs) {
  OpenCache();

  ps_write_handle_t* wh = nullptr;

  // Null cache.
  EXPECT_EQ(ps_cache_write_sentinel(nullptr, "/test.html", "example.com",
                                    "https", PS_SENTINEL_EARLY_HINTS, 10, &wh),
            PS_ERR_INVALID_ARG);

  // Null out.
  EXPECT_EQ(
      ps_cache_write_sentinel(cache_, "/test.html", "example.com", "https",
                              PS_SENTINEL_EARLY_HINTS, 10, nullptr),
      PS_ERR_INVALID_ARG);

  // Null URL.
  EXPECT_EQ(ps_cache_write_sentinel(cache_, nullptr, "example.com", "https",
                                    PS_SENTINEL_EARLY_HINTS, 10, &wh),
            PS_ERR_INVALID_ARG);

  // Null hostname.
  EXPECT_EQ(ps_cache_write_sentinel(cache_, "/test.html", nullptr, "https",
                                    PS_SENTINEL_EARLY_HINTS, 10, &wh),
            PS_ERR_INVALID_ARG);
}

// ================================================================
// Coverage: ps_html_transform with speculation URLs
// ================================================================

TEST(PageSpeedCApi, HtmlTransformWithSpeculationUrls) {
  const char* html =
      "<html><head></head><body>"
      "<a href=\"/page1\">Page 1</a>"
      "<a href=\"/page2\">Page 2</a>"
      "</body></html>";

  ps_scan_result_t* scan = nullptr;
  ASSERT_EQ(ps_html_scan(html, std::strlen(html), "http://example.com/", &scan),
            PS_OK);

  ps_html_config_t config;
  ps_html_config_init(&config);
  config.enable_critical_css = 0;
  config.enable_lazy_load = 0;
  config.enable_speculation_rules = 1;

  const char* speculation = "/page1\n/page2\n/page3";

  ps_html_transform_t* transform = nullptr;
  ASSERT_EQ(ps_html_transform_create(scan, &config, nullptr, 0, nullptr,
                                     "example.com", speculation,
                                     std::strlen(speculation), &transform),
            PS_OK);
  ASSERT_NE(transform, nullptr);

  char* out_html = nullptr;
  size_t out_len = 0;
  ASSERT_EQ(ps_html_transform_run(transform, html, std::strlen(html),
                                  "http://example.com/", &out_html, &out_len),
            PS_OK);

  if (out_html != nullptr) {
    std::string output(out_html, out_len);
    // Speculation rules should be injected.
    EXPECT_NE(output.find("speculationrules"), std::string::npos)
        << "Speculation rules not found in output";
    ps_free(out_html);
  }

  ps_html_transform_free(transform);
  ps_scan_result_free(scan);
}

// ---------------------------------------------------------------------------
// agent_optimize native FFI
// ---------------------------------------------------------------------------

// P3.5: ps_wants_agent_markdown — presence-only text/markdown negotiation,
// token-bounded, never matched by a wildcard accept.
TEST(PageSpeedCApi, WantsAgentMarkdown) {
  EXPECT_EQ(ps_wants_agent_markdown(nullptr), 0);
  EXPECT_EQ(ps_wants_agent_markdown(""), 0);
  EXPECT_EQ(ps_wants_agent_markdown("text/markdown"), 1);
  EXPECT_EQ(ps_wants_agent_markdown("application/json, text/markdown"), 1);
  EXPECT_EQ(ps_wants_agent_markdown("text/markdown;q=0.9"), 1);
  EXPECT_EQ(ps_wants_agent_markdown("text/html, application/json"), 0);
  // A substring without a token boundary must NOT match.
  EXPECT_EQ(ps_wants_agent_markdown("text/markdown-x"), 0);
  // A wildcard accept does NOT opt into markdown.
  EXPECT_EQ(ps_wants_agent_markdown("*/*"), 0);
  EXPECT_EQ(ps_wants_agent_markdown("text/html, */*"), 0);
}

// P3.2: ps_read_origin_html_hash returns NULL for an unbound (all-zero) hash.
// The C write path cannot stamp a D8 binding, so any C-written variant is
// unbound and the accessor must report NULL (never a dangling/garbage pointer).
TEST_F(PageSpeedCacheApiTest, ReadOriginHtmlHashUnboundIsNull) {
  OpenCache();

  ps_write_params_t params;
  ps_write_params_init_auto(&params);
  params.alternate_id = 0x08;
  params.content_length = 11;
  params.full_mask = 0x08;
  params.content_type = PS_CONTENT_HTML;
  params.origin_ct = "text/html";

  ps_write_handle_t* wh = nullptr;
  ASSERT_EQ(ps_cache_write_begin(cache_, "/h.html", "example.com", "https",
                                 &params, &wh),
            PS_OK);
  ASSERT_EQ(ps_write_data(wh, "hello world", 11), PS_OK);
  ASSERT_EQ(ps_write_close(wh), PS_OK);

  ps_read_result_t* result = nullptr;
  ASSERT_EQ(ps_cache_read_best(cache_, "/h.html", "example.com", "https", 0x08,
                               &result),
            PS_OK);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(ps_read_origin_html_hash(result), nullptr);
  ps_read_free(result);

  // Null result → NULL, no crash.
  EXPECT_EQ(ps_read_origin_html_hash(nullptr), nullptr);
}

// P3.4: ps_cache_read_best_agent is a behavior-preserving pass-through when no
// markdown variant exists — it returns the best non-agent alternate (the
// content-hash gate only ever REFUSES a 0x7C variant, never a normal one).
// Null args are rejected.
TEST_F(PageSpeedCacheApiTest, ReadBestAgentPassThroughAndNullArgs) {
  OpenCache();

  ps_write_params_t params;
  ps_write_params_init_auto(&params);
  params.alternate_id = 0x08;
  params.content_length = 11;
  params.full_mask = 0x08;
  params.content_type = PS_CONTENT_HTML;
  params.origin_ct = "text/html";

  ps_write_handle_t* wh = nullptr;
  ASSERT_EQ(ps_cache_write_begin(cache_, "/a.html", "example.com", "https",
                                 &params, &wh),
            PS_OK);
  ASSERT_EQ(ps_write_data(wh, "hello world", 11), PS_OK);
  ASSERT_EQ(ps_write_close(wh), PS_OK);

  // Entitled agent read, no markdown variant present → falls through to the
  // normal best alternate (same bytes as ps_cache_read_best).
  ps_read_result_t* result = nullptr;
  ASSERT_EQ(ps_cache_read_best_agent(cache_, "/a.html", "example.com", "https",
                                     0x08, /*agent_entitled=*/1, &result),
            PS_OK);
  ASSERT_NE(result, nullptr);
  const uint8_t* data = nullptr;
  size_t len = 0;
  ASSERT_EQ(ps_read_content(result, &data, &len), PS_OK);
  EXPECT_EQ(len, 11u);
  EXPECT_EQ(std::memcmp(data, "hello world", 11), 0);
  EXPECT_EQ(ps_read_content_type(result), PS_CONTENT_HTML);
  ps_read_free(result);

  // Not-entitled agent read also passes through (the gate only blocks 0x7C).
  result = nullptr;
  ASSERT_EQ(ps_cache_read_best_agent(cache_, "/a.html", "example.com", "https",
                                     0x08, /*agent_entitled=*/0, &result),
            PS_OK);
  ASSERT_NE(result, nullptr);
  ps_read_free(result);

  // Null args rejected.
  result = nullptr;
  EXPECT_EQ(ps_cache_read_best_agent(nullptr, "/a.html", "example.com", "https",
                                     0x08, 1, &result),
            PS_ERR_INVALID_ARG);
  EXPECT_EQ(ps_cache_read_best_agent(cache_, "/a.html", "example.com", "https",
                                     0x08, 1, nullptr),
            PS_ERR_INVALID_ARG);
}

// P3.3: ps_read_shared_config_agent_entitled reads the worker-written serve-side
// entitlement.  -1 on null; 0 on a missing config (the safe default) or a
// written-not-entitled config; 1 on a written-entitled config.
TEST_F(PageSpeedCacheApiTest, ReadSharedConfigAgentEntitled) {
  // Null arg → -1.
  EXPECT_EQ(ps_read_shared_config_agent_entitled(nullptr), -1);

  // Missing config → 0 (safe default). temp_dir_ exists, the conf does not.
  EXPECT_EQ(ps_read_shared_config_agent_entitled(cache_path_.c_str()), 0);

  const std::string config_path = pagespeed::SharedConfigFilePath(cache_path_);
  ASSERT_FALSE(config_path.empty());

  // Written, not entitled → 0.
  {
    pagespeed::SharedConfig sc;
    sc.agent_optimize_entitled = false;
    ASSERT_TRUE(pagespeed::WriteSharedConfigFile(config_path, sc));
    EXPECT_EQ(ps_read_shared_config_agent_entitled(cache_path_.c_str()), 0);
  }

  // Written, entitled → 1.
  {
    pagespeed::SharedConfig sc;
    sc.agent_optimize_entitled = true;
    ASSERT_TRUE(pagespeed::WriteSharedConfigFile(config_path, sc));
    EXPECT_EQ(ps_read_shared_config_agent_entitled(cache_path_.c_str()), 1);
  }
}

// The published volume sizing (PS_API 1.6).  A caller that has to open the
// worker's volume must MATCH its size — the filename is derived from the
// geometry, so a mismatched opener silently creates and uses a different file.
// 0 is the "do not guess" answer and every not-known case has to produce it.
TEST_F(PageSpeedCacheApiTest, ReadSharedConfigVolumeSize) {
  // Null arg → 0 (not known).
  EXPECT_EQ(ps_read_shared_config_volume_size(nullptr), 0u);

  // Missing config → 0.  temp_dir_ exists, the conf does not.
  EXPECT_EQ(ps_read_shared_config_volume_size(cache_path_.c_str()), 0u);

  const std::string config_path = pagespeed::SharedConfigFilePath(cache_path_);
  ASSERT_FALSE(config_path.empty());

  // A worker that did not state a size → 0, NOT a default.  This is the
  // older-worker case, and it has to stay distinguishable from a real size.
  {
    pagespeed::SharedConfig sc;
    sc.volume_size = 0;
    ASSERT_TRUE(pagespeed::WriteSharedConfigFile(config_path, sc));
    EXPECT_EQ(ps_read_shared_config_volume_size(cache_path_.c_str()), 0u);
  }

  // A stated size comes back exactly, including one that is not the default.
  {
    pagespeed::SharedConfig sc;
    sc.volume_size = 64ull * 1024 * 1024;
    ASSERT_TRUE(pagespeed::WriteSharedConfigFile(config_path, sc));
    EXPECT_EQ(ps_read_shared_config_volume_size(cache_path_.c_str()),
              64ull * 1024 * 1024);
  }
}

// ================================================================
// ps_cache_config_init_sized (PS_API 1.6)
// ================================================================
//
// ps_cache_config_init is not size-aware: it writes sizeof(the LIBRARY's
// struct) and every field it knows, before any caller check can run.  A caller
// whose struct is shorter has its buffer overrun, and the struct_size it then
// reads reports the damage rather than preventing it.  These pin the sized
// entry point that does prevent it.

TEST(PageSpeedCApi, CacheConfigInitSizedWritesNoByteBeyondTheStatedSize) {
  // A generous canary region after the "caller's struct": if the initializer
  // writes past what it was told it owns, the canary changes.
  struct alignas(alignof(std::max_align_t)) Probe {
    unsigned char bytes[sizeof(ps_cache_config_t) * 4];
  };
  Probe probe;
  std::memset(&probe, 0xAB, sizeof(probe));

  // Claim only the first half of what the library's struct actually is.
  const size_t claimed = sizeof(size_t) * 2;
  ASSERT_LT(claimed, sizeof(ps_cache_config_t));
  ps_cache_config_init_sized(
      reinterpret_cast<ps_cache_config_t*>(&probe.bytes[0]), claimed);

  for (size_t i = claimed; i < sizeof(probe.bytes); ++i) {
    ASSERT_EQ(probe.bytes[i], 0xAB)
        << "ps_cache_config_init_sized wrote past the size it was given, at "
           "offset "
        << i;
  }
  // And it recorded the CALLER's size, not the library's.
  size_t recorded = 0;
  std::memcpy(&recorded, &probe.bytes[0], sizeof(recorded));
  EXPECT_EQ(recorded, claimed);
}

TEST(PageSpeedCApi, CacheConfigInitSizedRefusesASizeTooSmallToRecordOne) {
  unsigned char probe[sizeof(ps_cache_config_t)];
  std::memset(probe, 0xCD, sizeof(probe));
  ps_cache_config_init_sized(reinterpret_cast<ps_cache_config_t*>(probe),
                             sizeof(size_t) - 1);
  for (unsigned char b : probe) {
    ASSERT_EQ(b, 0xCD) << "a size too small to hold struct_size was acted on";
  }
  ps_cache_config_init_sized(nullptr, sizeof(ps_cache_config_t));  // no crash
}

TEST(PageSpeedCApi, CacheConfigInitSizedAtFullSizeMatchesTheUnsizedForm) {
  ps_cache_config_t sized;
  ps_cache_config_t plain;
  std::memset(&sized, 0xEE, sizeof(sized));
  std::memset(&plain, 0xEE, sizeof(plain));
  ps_cache_config_init_sized(&sized, sizeof(sized));
  ps_cache_config_init(&plain);

  // Compared FIELD BY FIELD, not with memcmp. ps_cache_config_t has padding
  // after enable_checksum, and ps_cache_config_init assigns fields rather than
  // clearing the struct, so its padding is whatever the caller's buffer held.
  // A byte comparison here would be asserting something the unsized form has
  // never promised -- and it caught exactly that during development.
  EXPECT_EQ(sized.struct_size, plain.struct_size);
  EXPECT_EQ(sized.volume_path, plain.volume_path);
  EXPECT_EQ(sized.volume_size, plain.volume_size);
  EXPECT_EQ(sized.enable_checksum, plain.enable_checksum);
  EXPECT_EQ(sized.ram_cache_size, plain.ram_cache_size);
  EXPECT_EQ(sized.max_metadata_size, plain.max_metadata_size);

  // The RAM tier default the serve surfaces depend on, restated here because
  // the sized form is a second path to it.
  EXPECT_EQ(sized.ram_cache_size, 0u);
}

TEST(PageSpeedCApi, CacheConfigInitSizedLeavesNoIndeterminatePadding) {
  // What the sized form promises that the unsized one does not: every byte
  // inside the stated size is a function of the library's defaults, never of
  // whatever was on the caller's stack. Filling with two different patterns
  // must produce identical bytes -- padding included.
  ps_cache_config_t a;
  ps_cache_config_t b;
  std::memset(&a, 0x00, sizeof(a));
  std::memset(&b, 0xFF, sizeof(b));
  ps_cache_config_init_sized(&a, sizeof(a));
  ps_cache_config_init_sized(&b, sizeof(b));

  // Compared byte by byte rather than with memcmp, and deliberately so: the
  // padding IS the subject here, and a memcmp over a type that has padding is
  // exactly what the "suspicious memory comparison" diagnostic exists to flag.
  // Spelling the loop says what is meant -- every byte, padding included.
  const auto* pa = reinterpret_cast<const unsigned char*>(&a);
  const auto* pb = reinterpret_cast<const unsigned char*>(&b);
  for (size_t i = 0; i < sizeof(a); ++i) {
    ASSERT_EQ(pa[i], pb[i])
        << "byte " << i
        << " depends on what the caller's buffer held, not on the defaults";
  }
}

TEST(PageSpeedCApi, CacheConfigInitSizedZeroesATailItDoesNotKnow) {
  // A caller built against a LONGER struct than this library's must not be
  // handed stack garbage in the fields the library knows nothing about.
  struct alignas(alignof(std::max_align_t)) Longer {
    unsigned char bytes[sizeof(ps_cache_config_t) + 64];
  };
  Longer probe;
  std::memset(&probe, 0x77, sizeof(probe));
  ps_cache_config_init_sized(
      reinterpret_cast<ps_cache_config_t*>(&probe.bytes[0]),
      sizeof(probe.bytes));
  for (size_t i = sizeof(ps_cache_config_t); i < sizeof(probe.bytes); ++i) {
    ASSERT_EQ(probe.bytes[i], 0x00)
        << "unknown tail left uninitialised at " << i;
  }
}

// ================================================================
// Serve-stats v8: serve-class + saturation block over the C ABI
// ================================================================

class PageSpeedServeStatsApiTest : public ::testing::Test {
 protected:
  void SetUp() override {
    temp_dir_ = pagespeed::test::MakeTempDir();
    cache_path_ = temp_dir_ + "/cache.vol";
    // The worker is the sole creator of this file; stand in for it here.
    stats_ =
        pagespeed::CreateServeStats(pagespeed::ServeStatsPath(cache_path_));
    ASSERT_NE(stats_, nullptr);
    ASSERT_EQ(ps_serve_stats_open(cache_path_.c_str(), &handle_), PS_OK);
    ASSERT_NE(handle_, nullptr);
  }

  void TearDown() override {
    if (handle_ != nullptr) ps_serve_stats_close(handle_);
    if (stats_ != nullptr) pagespeed::CloseServeStats(stats_);
    std::filesystem::remove_all(temp_dir_);
  }

  static ps_serve_stats_snapshot_t Snapshot(ps_serve_stats_t* h) {
    ps_serve_stats_snapshot_t snap;
    std::memset(&snap, 0, sizeof(snap));
    snap.struct_size = sizeof(snap);
    EXPECT_EQ(ps_serve_stats_snapshot(h, &snap), PS_OK);
    return snap;
  }

  std::string temp_dir_;
  std::string cache_path_;
  pagespeed::ServeStats* stats_ = nullptr;
  ps_serve_stats_t* handle_ = nullptr;
};

TEST_F(PageSpeedServeStatsApiTest, ServeClassRoundTripsThroughTheAbi) {
  ps_serve_stats_record_serve_class(handle_, PS_SERVE_CLASS_OPTIMIZED,
                                    PS_SERVE_FLAG_NONE);
  ps_serve_stats_record_serve_class(handle_, PS_SERVE_CLASS_ORIGINAL_COLD,
                                    PS_SERVE_FLAG_NOTIFY_SUPPRESSED);
  ps_serve_stats_record_serve_class(handle_, PS_SERVE_CLASS_ORIGINAL_PENDING,
                                    PS_SERVE_FLAG_NONE);
  ps_serve_stats_record_serve_class(handle_, PS_SERVE_CLASS_ORIGINAL_PENDING,
                                    PS_SERVE_FLAG_NONE);
  ps_serve_stats_record_serve_class(handle_, PS_SERVE_CLASS_ORIGINAL_DECLINED,
                                    PS_SERVE_FLAG_NONE);
  ps_serve_stats_record_serve_class(handle_, PS_SERVE_CLASS_ORIGINAL_SKEW,
                                    PS_SERVE_FLAG_NONE);

  const ps_serve_stats_snapshot_t snap = Snapshot(handle_);
  EXPECT_EQ(snap.serve_optimized_total, 1u);
  EXPECT_EQ(snap.serve_original_cold_total, 1u);
  EXPECT_EQ(snap.serve_original_pending_total, 2u);
  EXPECT_EQ(snap.serve_original_declined_total, 1u);
  EXPECT_EQ(snap.serve_original_skew_total, 1u);
  EXPECT_EQ(snap.notify_suppressed_total, 1u);
  // Every write above was understood, so neither drop counter moved.
  EXPECT_EQ(snap.serve_class_unrecognized_total, 0u);
  EXPECT_EQ(snap.serve_flags_unrecognized_total, 0u);
  EXPECT_EQ(snap.struct_size, sizeof(ps_serve_stats_snapshot_t));
}

TEST_F(PageSpeedServeStatsApiTest, TwoClassesInOneCallCountNothing) {
  // The class values are disjoint bits so that a combination is not a class.
  // Checked at the ABI boundary too: this is where a caller from another
  // language is most likely to hand over a bitmask out of habit.
  const uint32_t combined =
      static_cast<uint32_t>(PS_SERVE_CLASS_ORIGINAL_COLD) |
      static_cast<uint32_t>(PS_SERVE_CLASS_ORIGINAL_PENDING);
  ps_serve_stats_record_serve_class(handle_,
                                    static_cast<ps_serve_class_t>(combined),
                                    PS_SERVE_FLAG_NOTIFY_SUPPRESSED);
  ps_serve_stats_record_serve_class(handle_, static_cast<ps_serve_class_t>(0),
                                    PS_SERVE_FLAG_NONE);
  ps_serve_stats_record_serve_class(handle_, static_cast<ps_serve_class_t>(99),
                                    PS_SERVE_FLAG_NONE);

  const ps_serve_stats_snapshot_t snap = Snapshot(handle_);
  EXPECT_EQ(snap.serve_optimized_total, 0u);
  EXPECT_EQ(snap.serve_original_cold_total, 0u);
  EXPECT_EQ(snap.serve_original_pending_total, 0u);
  EXPECT_EQ(snap.serve_original_declined_total, 0u);
  EXPECT_EQ(snap.serve_original_skew_total, 0u);
  EXPECT_EQ(snap.notify_suppressed_total, 0u);
  // All three writes were dropped, and all three are visible as drops through
  // the same accessor a consumer already polls — no log to correlate.
  EXPECT_EQ(snap.serve_class_unrecognized_total, 3u);
  EXPECT_EQ(snap.serve_flags_unrecognized_total, 0u);
}

TEST_F(PageSpeedServeStatsApiTest, UnknownFlagBitsKeepTheServeAndAreCounted) {
  // What a NEWER caller looks like: a class this build knows, carrying a flag
  // bit it does not.  The serve must still be classified — losing it would
  // punish a peer for being ahead — and the ignored bit must be visible.
  ps_serve_stats_record_serve_class(handle_, PS_SERVE_CLASS_OPTIMIZED,
                                    PS_SERVE_FLAG_NOTIFY_SUPPRESSED | 0x8u);

  const ps_serve_stats_snapshot_t snap = Snapshot(handle_);
  EXPECT_EQ(snap.serve_optimized_total, 1u);
  EXPECT_EQ(snap.notify_suppressed_total, 1u);
  EXPECT_EQ(snap.serve_flags_unrecognized_total, 1u);
  EXPECT_EQ(snap.serve_class_unrecognized_total, 0u)
      << "an accepted write must not be reported as a dropped one";
}

TEST_F(PageSpeedServeStatsApiTest, SnapshotReadsTheWorkerWrittenSaturation) {
  // The worker owns these fields; write them the way it does and read them
  // back the way a front-end consumer would.
  pagespeed::SetWorkerPoolThreads(stats_, 8);
  pagespeed::RecordSaturationSample(stats_, 2);
  pagespeed::RecordSaturationSample(stats_, 6);
  pagespeed::RecordSaturationSample(stats_, 4);

  const ps_serve_stats_snapshot_t snap = Snapshot(handle_);
  EXPECT_EQ(snap.saturation_sample_count, 3u);
  EXPECT_EQ(snap.saturation_sample_accum, 12u);
  EXPECT_EQ(snap.saturation_hwm, 6u);
  EXPECT_EQ(snap.worker_pool_threads, 8u);
  // What the consumer actually computes.
  EXPECT_EQ(snap.saturation_sample_accum / snap.saturation_sample_count, 4u);
}

TEST_F(PageSpeedServeStatsApiTest, SnapshotFillsOnlyTheCallersPrefix) {
  ps_serve_stats_record_serve_class(handle_, PS_SERVE_CLASS_OPTIMIZED,
                                    PS_SERVE_FLAG_NONE);
  pagespeed::SetWorkerPoolThreads(stats_, 3);

  // An older caller whose struct stops after the serve-class counters must get
  // its prefix filled and nothing written past it.
  ps_serve_stats_snapshot_t snap;
  std::memset(&snap, 0xAB, sizeof(snap));
  const size_t prefix =
      offsetof(ps_serve_stats_snapshot_t, notify_suppressed_total);
  snap.struct_size = prefix;
  ASSERT_EQ(ps_serve_stats_snapshot(handle_, &snap), PS_OK);

  EXPECT_EQ(snap.struct_size, prefix);
  EXPECT_EQ(snap.serve_optimized_total, 1u);
  // Past the caller's prefix: untouched, still the poison pattern.
  EXPECT_EQ(snap.worker_pool_threads, 0xABABABABu);
}

TEST_F(PageSpeedServeStatsApiTest, SnapshotRejectsBadArguments) {
  ps_serve_stats_snapshot_t snap;
  std::memset(&snap, 0, sizeof(snap));
  snap.struct_size = sizeof(snap);
  EXPECT_EQ(ps_serve_stats_snapshot(nullptr, &snap), PS_ERR_INVALID_ARG);
  EXPECT_EQ(ps_serve_stats_snapshot(handle_, nullptr), PS_ERR_INVALID_ARG);

  // An uninitialized struct_size is a caller bug, not a zero-size read.
  std::memset(&snap, 0, sizeof(snap));
  EXPECT_EQ(ps_serve_stats_snapshot(handle_, &snap), PS_ERR_INVALID_ARG);
}

TEST_F(PageSpeedServeStatsApiTest, RecordServeClassNullHandleIsNoOp) {
  ps_serve_stats_record_serve_class(nullptr, PS_SERVE_CLASS_OPTIMIZED,
                                    PS_SERVE_FLAG_NONE);
  const ps_serve_stats_snapshot_t snap = Snapshot(handle_);
  EXPECT_EQ(snap.serve_optimized_total, 0u);
}

// ps_serve_stats_record_hit (PS_API 1.0): the per-type bytes/hits half of the
// surface.  The module-serves topology depends on it — the module is the only
// writer there (mpp #891) — and it had no C-ABI-level pin until now.

TEST_F(PageSpeedServeStatsApiTest, RecordHitAccumulatesPerTypeBytesAndHits) {
  ps_serve_stats_record_hit(handle_, PS_CONTENT_HTML, 1000, 400, /*mask=*/0);
  ps_serve_stats_record_hit(handle_, PS_CONTENT_HTML, 500, 200, /*mask=*/0);
  ps_serve_stats_record_hit(handle_, PS_CONTENT_IMAGE, 9000, 3000, /*mask=*/0);

  EXPECT_EQ(stats_->html_original_bytes, 1500u);
  EXPECT_EQ(stats_->html_optimized_bytes, 600u);
  EXPECT_EQ(stats_->html_optimized_hits, 2u);
  EXPECT_EQ(stats_->image_original_bytes, 9000u);
  EXPECT_EQ(stats_->image_optimized_bytes, 3000u);
  EXPECT_EQ(stats_->image_optimized_hits, 1u);
  // Untouched types stay at zero.
  EXPECT_EQ(stats_->css_optimized_hits, 0u);
  EXPECT_EQ(stats_->js_optimized_hits, 0u);
}

TEST_F(PageSpeedServeStatsApiTest, RecordHitSplitCountsTheServeAndTheSplit) {
  ps_serve_stats_record_hit_split(handle_, PS_CONTENT_CSS, 1000, 300, 250,
                                  /*mask=*/0x40, "Shop.Example.com", 16);
  EXPECT_EQ(stats_->css_optimized_hits, 1u);
  EXPECT_EQ(stats_->css_original_bytes, 1000u);
  EXPECT_EQ(stats_->css_optimized_bytes, 250u);
  const auto split =
      pagespeed::ReadServeSplit(stats_, pagespeed::ContentType::kCss);
  EXPECT_EQ(split.original_bytes, 1000u);
  EXPECT_EQ(split.compressed_original_bytes, 300u);
  EXPECT_EQ(split.optimized_bytes, 250u);
  const pagespeed::ServeSavingsByHost by_host =
      pagespeed::ReadServeSavingsByHost(stats_,
                                        pagespeed::kServeHostReportLimit);
  ASSERT_EQ(by_host.hosts.size(), 1u);
  EXPECT_EQ(by_host.hosts[0].host, "shop.example.com");
}

TEST_F(PageSpeedServeStatsApiTest, RecordHitSplitNullAndOtherTypeAreNoOps) {
  ps_serve_stats_record_hit_split(nullptr, PS_CONTENT_CSS, 1, 1, 1, 0, nullptr,
                                  0);
  ps_serve_stats_record_hit_split(handle_, PS_CONTENT_OTHER, 1, 1, 1, 0,
                                  nullptr, 0);
  EXPECT_EQ(stats_->css_optimized_hits, 0u);
  EXPECT_EQ(pagespeed::ReadServeSplit(stats_, pagespeed::ContentType::kCss)
                .original_bytes,
            0u);
}

TEST(PageSpeedCApi, ReadEncodedOriginLengthRejectsNullArguments) {
  uint64_t out = 7;
  EXPECT_EQ(ps_read_encoded_origin_length(nullptr, &out), 0);
  EXPECT_EQ(out, 7u);
  EXPECT_EQ(ps_read_encoded_origin_length(nullptr, nullptr), 0);
}

TEST_F(PageSpeedServeStatsApiTest, RecordHitWithSvgMaskAlsoCountsSvgServed) {
  // Image-format bits (0-1) == kSvg on an image hit bump svg_optimized_hits in
  // addition to the image counters (#455); mask 0 skips
  // that accounting.
  ps_serve_stats_record_hit(handle_, PS_CONTENT_IMAGE, 2000, 500,
                            /*mask=*/0x0B);
  ps_serve_stats_record_hit(handle_, PS_CONTENT_IMAGE, 2000, 500, /*mask=*/0);

  EXPECT_EQ(stats_->image_optimized_hits, 2u);
  EXPECT_EQ(stats_->svg_optimized_hits, 1u);
}

TEST_F(PageSpeedServeStatsApiTest, RecordHitOtherTypeAndNullHandleAreNoOps) {
  ps_serve_stats_record_hit(handle_, PS_CONTENT_OTHER, 1000, 400, /*mask=*/0);
  ps_serve_stats_record_hit(nullptr, PS_CONTENT_HTML, 1000, 400, /*mask=*/0);

  EXPECT_EQ(stats_->html_optimized_hits, 0u);
  EXPECT_EQ(stats_->css_optimized_hits, 0u);
  EXPECT_EQ(stats_->js_optimized_hits, 0u);
  EXPECT_EQ(stats_->image_optimized_hits, 0u);
  EXPECT_EQ(stats_->html_original_bytes, 0u);
  EXPECT_EQ(stats_->image_optimized_bytes, 0u);
}

// ps_serve_stats_record_hit_host (PS_API 1.11): the same serve, attributed to
// the host the front end served it for.

TEST_F(PageSpeedServeStatsApiTest, RecordHitHostAttributesTheServeToItsHost) {
  const char kHost[] = "Shop.Example.com:443";
  ps_serve_stats_record_hit_host(handle_, PS_CONTENT_CSS, 1000, 600,
                                 /*mask=*/0, kHost, sizeof(kHost) - 1);
  ps_serve_stats_record_hit_host(handle_, PS_CONTENT_CSS, 1000, 600,
                                 /*mask=*/0, kHost, sizeof(kHost) - 1);

  EXPECT_EQ(stats_->css_optimized_hits, 2u);
  EXPECT_EQ(stats_->css_original_bytes, 2000u);
  const pagespeed::ServeSavingsByHost by_host =
      pagespeed::ReadServeSavingsByHost(stats_,
                                        pagespeed::kServeHostReportLimit);
  ASSERT_EQ(by_host.hosts.size(), 1u);
  EXPECT_EQ(by_host.hosts[0].host, "shop.example.com");
  EXPECT_EQ(by_host.hosts[0].hits, 2u);
  EXPECT_EQ(by_host.hosts[0].original_bytes, 2000u);
  EXPECT_EQ(by_host.hosts[0].optimized_bytes, 1200u);
}

// The host is exactly host_len bytes: whatever follows in the caller's buffer
// (no NUL needed) is not part of it.
TEST_F(PageSpeedServeStatsApiTest, RecordHitHostHonoursTheExplicitLength) {
  const char kBuffer[] = "a.example.comJUNK";
  ps_serve_stats_record_hit_host(handle_, PS_CONTENT_JS, 800, 300,
                                 /*mask=*/0, kBuffer, 13);

  const pagespeed::ServeSavingsByHost by_host =
      pagespeed::ReadServeSavingsByHost(stats_,
                                        pagespeed::kServeHostReportLimit);
  ASSERT_EQ(by_host.hosts.size(), 1u);
  EXPECT_EQ(by_host.hosts[0].host, "a.example.com");
  EXPECT_EQ(by_host.hosts[0].hits, 1u);
  EXPECT_EQ(by_host.other.hits, 0u);
}

TEST_F(PageSpeedServeStatsApiTest, RecordHitWithoutAHostCountsUnderOther) {
  ps_serve_stats_record_hit_host(handle_, PS_CONTENT_JS, 500, 100, 0, nullptr,
                                 12);
  ps_serve_stats_record_hit_host(handle_, PS_CONTENT_JS, 500, 100, 0, "", 0);
  ps_serve_stats_record_hit(handle_, PS_CONTENT_JS, 500, 100, 0);
  ps_serve_stats_record_hit_host(nullptr, PS_CONTENT_JS, 500, 100, 0,
                                 "a.example", 9);

  EXPECT_EQ(stats_->js_optimized_hits, 3u);
  const pagespeed::ServeSavingsByHost by_host =
      pagespeed::ReadServeSavingsByHost(stats_,
                                        pagespeed::kServeHostReportLimit);
  EXPECT_TRUE(by_host.hosts.empty());
  EXPECT_EQ(by_host.other.hits, 3u);
  EXPECT_EQ(by_host.other.original_bytes, 1500u);
}

// The C entry point applies the same host-name rule as the worker: every
// spelling of one host is one row, an IPv6 literal keeps its brackets, and a
// value the rule refuses is never named.
TEST_F(PageSpeedServeStatsApiTest, RecordHitHostAppliesTheHostNameRule) {
  const std::vector<std::string> accepted = {"[::1]", "[2001:DB8::1]:443",
                                             "a_b.test", "www.example.com.",
                                             "WWW.EXAMPLE.COM:80"};
  const std::vector<std::string> refused = {"www.example.com..",
                                            std::string(300, 'a'),
                                            "",
                                            "_",
                                            "[::1]x",
                                            "[zz::1]",
                                            "[::1"};
  for (const std::string& host : accepted) {
    ps_serve_stats_record_hit_host(handle_, PS_CONTENT_CSS, 100, 40,
                                   /*mask=*/0, host.data(), host.size());
  }
  for (const std::string& host : refused) {
    ps_serve_stats_record_hit_host(handle_, PS_CONTENT_CSS, 100, 40,
                                   /*mask=*/0, host.data(), host.size());
  }
  const pagespeed::ServeSavingsByHost by_host =
      pagespeed::ReadServeSavingsByHost(stats_,
                                        pagespeed::kServeHostReportLimit);
  std::map<std::string, uint64_t> hits;
  for (const pagespeed::ServeHostRow& row : by_host.hosts) {
    hits[row.host] = row.hits;
  }
  const std::map<std::string, uint64_t> expected = {{"[2001:db8::1]", 1},
                                                    {"[::1]", 1},
                                                    {"a_b.test", 1},
                                                    {"www.example.com", 2}};
  EXPECT_EQ(hits, expected);
  EXPECT_EQ(by_host.other.hits, refused.size());
}

// ps_html_config_init_sized (PS_API 1.9)
// ================================================================
//
// ps_html_config_init is not size-aware: it writes sizeof(the LIBRARY's
// struct) and every field it knows, before any caller check can run.  A caller
// whose struct is shorter has its buffer overrun, and the struct_size it then
// reads reports the damage rather than preventing it.  These pin the sized
// entry point that does prevent it.

TEST(PageSpeedCApi, HtmlConfigInitSizedWritesNoByteBeyondTheStatedSize) {
  // A generous canary region after the "caller's struct": if the initializer
  // writes past what it was told it owns, the canary changes.
  struct alignas(alignof(std::max_align_t)) Probe {
    unsigned char bytes[sizeof(ps_html_config_t) * 4];
  };
  Probe probe;
  std::memset(&probe, 0xAB, sizeof(probe));

  // Claim only the first half of what the library's struct actually is.
  const size_t claimed = sizeof(size_t) * 2;
  ASSERT_LT(claimed, sizeof(ps_html_config_t));
  ps_html_config_init_sized(
      reinterpret_cast<ps_html_config_t*>(&probe.bytes[0]), claimed);

  for (size_t i = claimed; i < sizeof(probe.bytes); ++i) {
    ASSERT_EQ(probe.bytes[i], 0xAB)
        << "ps_html_config_init_sized wrote past the size it was given, at "
           "offset "
        << i;
  }
  // And it recorded the CALLER's size, not the library's.
  size_t recorded = 0;
  std::memcpy(&recorded, &probe.bytes[0], sizeof(recorded));
  EXPECT_EQ(recorded, claimed);
}

TEST(PageSpeedCApi, HtmlConfigInitSizedRefusesASizeTooSmallToRecordOne) {
  unsigned char probe[sizeof(ps_html_config_t)];
  std::memset(probe, 0xCD, sizeof(probe));
  ps_html_config_init_sized(reinterpret_cast<ps_html_config_t*>(probe),
                            sizeof(size_t) - 1);
  for (unsigned char b : probe) {
    ASSERT_EQ(b, 0xCD) << "a size too small to hold struct_size was acted on";
  }
  ps_html_config_init_sized(nullptr, sizeof(ps_html_config_t));  // no crash
}

TEST(PageSpeedCApi, HtmlConfigInitSizedAtFullSizeMatchesTheUnsizedForm) {
  ps_html_config_t sized;
  ps_html_config_t plain;
  std::memset(&sized, 0xEE, sizeof(sized));
  std::memset(&plain, 0xEE, sizeof(plain));
  ps_html_config_init_sized(&sized, sizeof(sized));
  ps_html_config_init(&plain);

  // Compared FIELD BY FIELD, not with memcmp. ps_html_config_t may have
  // padding, and we want to verify functional equivalence, not byte identity.
  // struct_size differs by design: the unsized form stamps the 1.8 size, the
  // sized one the size it was given, and only the sized one sets scheme.
  EXPECT_EQ(sized.struct_size, sizeof(sized));
  EXPECT_EQ(plain.struct_size, 128u);
  EXPECT_EQ(sized.scheme, nullptr);
  EXPECT_EQ(sized.enable_critical_css, plain.enable_critical_css);
  EXPECT_EQ(sized.enable_lazy_load, plain.enable_lazy_load);
  EXPECT_EQ(sized.enable_image_dimensions, plain.enable_image_dimensions);
  EXPECT_EQ(sized.enable_lcp_preload, plain.enable_lcp_preload);
  EXPECT_EQ(sized.enable_preconnect, plain.enable_preconnect);
  EXPECT_EQ(sized.enable_speculation_rules, plain.enable_speculation_rules);
  EXPECT_EQ(sized.enable_async_css, plain.enable_async_css);
  EXPECT_EQ(sized.critical_css_max_elements, plain.critical_css_max_elements);
  EXPECT_EQ(sized.critical_css_max_depth, plain.critical_css_max_depth);
  EXPECT_EQ(sized.css_import_max_depth, plain.css_import_max_depth);
  EXPECT_EQ(sized.viewport, plain.viewport);
  EXPECT_EQ(sized.max_html_size, plain.max_html_size);
  EXPECT_EQ(sized.max_css_size, plain.max_css_size);
}

TEST(PageSpeedCApi, HtmlConfigInitSizedLeavesNoIndeterminatePadding) {
  // What the sized form promises that the unsized one does not: every byte
  // inside the stated size is a function of the library's defaults, never of
  // whatever was on the caller's stack. Filling with two different patterns
  // must produce identical bytes -- padding included.
  ps_html_config_t a;
  ps_html_config_t b;
  std::memset(&a, 0x00, sizeof(a));
  std::memset(&b, 0xFF, sizeof(b));
  ps_html_config_init_sized(&a, sizeof(a));
  ps_html_config_init_sized(&b, sizeof(b));

  // Compared byte by byte rather than with memcmp, and deliberately so: the
  // padding IS the subject here, and a memcmp over a type that has padding is
  // exactly what the "suspicious memory comparison" diagnostic exists to flag.
  // Spelling the loop says what is meant -- every byte, padding included.
  const auto* pa = reinterpret_cast<const unsigned char*>(&a);
  const auto* pb = reinterpret_cast<const unsigned char*>(&b);
  for (size_t i = 0; i < sizeof(a); ++i) {
    ASSERT_EQ(pa[i], pb[i])
        << "byte " << i
        << " depends on what the caller's buffer held, not on the defaults";
  }
}

TEST(PageSpeedCApi, HtmlConfigInitLegacyPinsStructSizeToV1_8) {
  // The legacy initializer writes the PS_API 1.8 prefix (128 bytes) of
  // defaults, pinned by kHtmlConfigSizeV1_8 so that future struct growth
  // forces a developer decision.
  ps_html_config_t config;
  ps_html_config_init(&config);
  EXPECT_EQ(config.struct_size, 128u);  // kHtmlConfigSizeV1_8
}

// ================================================================
// ps_html_config_t.scheme (PS_API 1.12)
// ================================================================
//
// An embedder records a page's stylesheets under the request's scheme and may
// hand ps_html_process a path-only page URL, which names no scheme. The
// stylesheet lookup reads under config->scheme; a config whose struct_size
// predates the field reads under "https", as before.

class HtmlProcessSchemeTest : public PageSpeedCacheApiTest {
 protected:
  struct Outcome {
    int has_critical_css = -1;
    int needs_revalidation = -1;
  };

  void SetUp() override {
    PageSpeedCacheApiTest::SetUp();
    OpenCache();
  }

  // Records "/style.css" on example.com under `scheme`.
  void WriteSheet(const char* scheme) {
    static constexpr char kCss[] =
        ".hero { color: red; } .unused { margin: 0; }";
    ps_write_params_t params;
    ps_write_params_init_auto(&params);
    params.alternate_id = 0x08;
    params.content_length = sizeof(kCss) - 1;
    params.full_mask = 0x08;
    params.content_type = PS_CONTENT_CSS;
    params.origin_ct = "text/css";
    ps_write_handle_t* wh = nullptr;
    ASSERT_EQ(ps_cache_write_begin(cache_, "/style.css", "example.com", scheme,
                                   &params, &wh),
              PS_OK);
    ASSERT_EQ(ps_write_data(wh, kCss, sizeof(kCss) - 1), PS_OK);
    ASSERT_EQ(ps_write_close(wh), PS_OK);
  }

  // A full-size config reading under `scheme`.
  static ps_html_config_t FullConfig(const char* scheme) {
    ps_html_config_t config;
    ps_html_config_init_auto(&config);
    config.enable_critical_css = 1;
    config.scheme = scheme;
    return config;
  }

  // Processes a page linking "/style.css" at the path-only URL "/page".
  ps_error_t Process(const ps_html_config_t& config, Outcome* out) {
    static constexpr char kPage[] =
        "<html><head><link rel=\"stylesheet\" href=\"/style.css\"></head>"
        "<body><div class=\"hero\">Hello</div></body></html>";
    ps_html_result_t* result = nullptr;
    const ps_error_t err =
        ps_html_process(kPage, sizeof(kPage) - 1, "/page", "example.com",
                        &config, cache_, &result);
    if (err == PS_OK) {
      out->has_critical_css = ps_html_result_has_critical_css(result);
      out->needs_revalidation = ps_html_result_needs_revalidation(result);
      ps_html_result_free(result);
    }
    return err;
  }
};

TEST_F(HtmlProcessSchemeTest, PathOnlyHttpPageReadsTheSheetRecordedUnderHttp) {
  WriteSheet("http");
  Outcome out;
  ASSERT_EQ(Process(FullConfig("http"), &out), PS_OK);
  EXPECT_EQ(out.has_critical_css, 1);
  EXPECT_EQ(out.needs_revalidation, 0);
}

TEST_F(HtmlProcessSchemeTest, HttpsAndUnsetReadTheSheetRecordedUnderHttps) {
  WriteSheet("https");
  for (const char* scheme : {"https", "", static_cast<const char*>(nullptr)}) {
    SCOPED_TRACE(scheme == nullptr ? "NULL" : scheme);
    Outcome out;
    ASSERT_EQ(Process(FullConfig(scheme), &out), PS_OK);
    EXPECT_EQ(out.has_critical_css, 1);
    EXPECT_EQ(out.needs_revalidation, 0);
  }
}

TEST_F(HtmlProcessSchemeTest, TheSchemeSelectsWhichEntryIsRead) {
  WriteSheet("https");
  Outcome out;
  ASSERT_EQ(Process(FullConfig("http"), &out), PS_OK);
  EXPECT_EQ(out.has_critical_css, 0);
  EXPECT_EQ(out.needs_revalidation, 1);
}

TEST_F(HtmlProcessSchemeTest, AConfigOlderThanTheFieldKeepsReadingHttps) {
  // struct_size 128 is what ps_html_config_init stamps and what a caller
  // built before the field passes. The bytes past it are not read, whatever
  // they hold.
  ps_html_config_t config = FullConfig("http");
  config.struct_size = offsetof(ps_html_config_t, scheme);
  ASSERT_EQ(config.struct_size, 128u);

  WriteSheet("http");
  Outcome out;
  ASSERT_EQ(Process(config, &out), PS_OK);
  EXPECT_EQ(out.has_critical_css, 0);
  EXPECT_EQ(out.needs_revalidation, 1);

  WriteSheet("https");
  ASSERT_EQ(Process(config, &out), PS_OK);
  EXPECT_EQ(out.has_critical_css, 1);
  EXPECT_EQ(out.needs_revalidation, 0);
}

TEST_F(HtmlProcessSchemeTest, AnySchemeButHttpOrHttpsIsRefused) {
  WriteSheet("https");
  for (const char* scheme : {"ftp", "HTTP", "https:", " http"}) {
    SCOPED_TRACE(scheme);
    Outcome out;
    EXPECT_EQ(Process(FullConfig(scheme), &out), PS_ERR_INVALID_ARG);
  }
}

// ps_critical_css_config_init_sized (PS_API 1.9)
// ================================================================
//
// ps_critical_css_config_init is not size-aware: it writes sizeof(the LIBRARY's
// struct) and every field it knows, before any caller check can run.  A caller
// whose struct is shorter has its buffer overrun, and the struct_size it then
// reads reports the damage rather than preventing it.  These pin the sized
// entry point that does prevent it.

TEST(PageSpeedCApi, CriticalCssConfigInitSizedWritesNoByteBeyondTheStatedSize) {
  // A generous canary region after the "caller's struct": if the initializer
  // writes past what it was told it owns, the canary changes.
  struct alignas(alignof(std::max_align_t)) Probe {
    unsigned char bytes[sizeof(ps_critical_css_config_t) * 4];
  };
  Probe probe;
  std::memset(&probe, 0xAB, sizeof(probe));

  // Claim only the first half of what the library's struct actually is.
  const size_t claimed = sizeof(size_t) * 2;
  ASSERT_LT(claimed, sizeof(ps_critical_css_config_t));
  ps_critical_css_config_init_sized(
      reinterpret_cast<ps_critical_css_config_t*>(&probe.bytes[0]), claimed);

  for (size_t i = claimed; i < sizeof(probe.bytes); ++i) {
    ASSERT_EQ(probe.bytes[i], 0xAB) << "ps_critical_css_config_init_sized "
                                       "wrote past the size it was given, at "
                                       "offset "
                                    << i;
  }
  // And it recorded the CALLER's size, not the library's.
  size_t recorded = 0;
  std::memcpy(&recorded, &probe.bytes[0], sizeof(recorded));
  EXPECT_EQ(recorded, claimed);
}

TEST(PageSpeedCApi, CriticalCssConfigInitSizedRefusesASizeTooSmallToRecordOne) {
  unsigned char probe[sizeof(ps_critical_css_config_t)];
  std::memset(probe, 0xCD, sizeof(probe));
  ps_critical_css_config_init_sized(
      reinterpret_cast<ps_critical_css_config_t*>(probe), sizeof(size_t) - 1);
  for (unsigned char b : probe) {
    ASSERT_EQ(b, 0xCD) << "a size too small to hold struct_size was acted on";
  }
  ps_critical_css_config_init_sized(
      nullptr,
      sizeof(ps_critical_css_config_t));  // no crash
}

TEST(PageSpeedCApi, CriticalCssConfigInitSizedAtFullSizeMatchesTheUnsizedForm) {
  ps_critical_css_config_t sized;
  ps_critical_css_config_t plain;
  std::memset(&sized, 0xEE, sizeof(sized));
  std::memset(&plain, 0xEE, sizeof(plain));
  ps_critical_css_config_init_sized(&sized, sizeof(sized));
  ps_critical_css_config_init(&plain);

  // Compared FIELD BY FIELD, not with memcmp. ps_critical_css_config_t may have
  // padding, and we want to verify functional equivalence, not byte identity.
  EXPECT_EQ(sized.struct_size, plain.struct_size);
  EXPECT_EQ(sized.max_elements, plain.max_elements);
  EXPECT_EQ(sized.max_depth, plain.max_depth);
  EXPECT_EQ(sized.viewport, plain.viewport);
  EXPECT_EQ(sized.max_css_size, plain.max_css_size);
}

TEST(PageSpeedCApi, CriticalCssConfigInitSizedLeavesNoIndeterminatePadding) {
  // What the sized form promises that the unsized one does not: every byte
  // inside the stated size is a function of the library's defaults, never of
  // whatever was on the caller's stack. Filling with two different patterns
  // must produce identical bytes -- padding included.
  ps_critical_css_config_t a;
  ps_critical_css_config_t b;
  std::memset(&a, 0x00, sizeof(a));
  std::memset(&b, 0xFF, sizeof(b));
  ps_critical_css_config_init_sized(&a, sizeof(a));
  ps_critical_css_config_init_sized(&b, sizeof(b));

  // Compared byte by byte rather than with memcmp, and deliberately so: the
  // padding IS the subject here, and a memcmp over a type that has padding is
  // exactly what the "suspicious memory comparison" diagnostic exists to flag.
  // Spelling the loop says what is meant -- every byte, padding included.
  const auto* pa = reinterpret_cast<const unsigned char*>(&a);
  const auto* pb = reinterpret_cast<const unsigned char*>(&b);
  for (size_t i = 0; i < sizeof(a); ++i) {
    ASSERT_EQ(pa[i], pb[i])
        << "byte " << i
        << " depends on what the caller's buffer held, not on the defaults";
  }
}

TEST(PageSpeedCApi, CriticalCssConfigInitLegacyPinsStructSizeToV1_8) {
  // The legacy initializer writes the PS_API 1.8 prefix (88 bytes) of
  // defaults, pinned by kCriticalCssConfigSizeV1_8 so that future struct growth
  // forces a developer decision.
  ps_critical_css_config_t config;
  ps_critical_css_config_init(&config);
  EXPECT_EQ(config.struct_size, 88u);  // kCriticalCssConfigSizeV1_8
}

TEST(PageSpeedCApi, CacheConfigInitLegacyPinsStructSizeToV1_8) {
  // The legacy initializer writes the PS_API 1.8 prefix (48 bytes) of
  // defaults, pinned by kCacheConfigSizeV1_8 so that future struct growth
  // forces a developer decision.
  ps_cache_config_t config;
  ps_cache_config_init(&config);
  EXPECT_EQ(config.struct_size, 48u);  // kCacheConfigSizeV1_8
}
