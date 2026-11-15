// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The PS_API 1.2 surface: origin state on the write path, its read accessors,
// and the three RFC 9111 policy entry points.
//
// Two things here are worth more attention than the rest.
//
// The struct_size cases exercise the version skew that actually happens in the
// field: a consumer compiled against an older header calling a newer library,
// and the reverse. Both are silent when they go wrong -- one reads uninitialized
// memory past the end of a caller's struct, the other reports fields the caller
// never set. Neither crashes reliably enough to be found any other way.
//
// The Age cases pin a contract that is invisible when broken. A response that
// arrived through an upstream cache carrying `Age: N` was already N seconds
// old; storing the local clock instead grants it N extra seconds of apparent
// freshness. Behind a CDN that is not an edge case, it is every response, and
// the symptom is stale bytes served as fresh -- with nothing in any log.

#include <stddef.h>

#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>

#include "gtest/gtest.h"
#include "lib/cache/cache_control_header.h"
#include "lib/cache/freshness.h"
#include "lib/classify/alternate_metadata.h"
#include "lib/pagespeed/pagespeed.h"
#include "test/test_util/temp_dir.h"

namespace {

using pagespeed::AlternateMetadata;

class AbiOriginStateTest : public ::testing::Test {
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
    config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
    ASSERT_EQ(ps_cache_open(&config, &cache_), PS_OK);
    ASSERT_NE(cache_, nullptr);
  }

  void Write(const char* url, const ps_write_params_t* params,
             const char* body) {
    ps_write_handle_t* wh = nullptr;
    ASSERT_EQ(
        ps_cache_write_begin(cache_, url, "example.com", "https", params, &wh),
        PS_OK);
    ASSERT_NE(wh, nullptr);
    ASSERT_EQ(ps_write_data(wh, body, std::strlen(body)), PS_OK);
    ASSERT_EQ(ps_write_close(wh), PS_OK);
  }

  ps_read_result_t* Read(const char* url, uint32_t mask) {
    ps_read_result_t* result = nullptr;
    EXPECT_EQ(
        ps_cache_read_best(cache_, url, "example.com", "https", mask, &result),
        PS_OK);
    return result;
  }

  std::string temp_dir_;
  std::string cache_path_;
  ps_cache_t* cache_ = nullptr;
};

// ===========================================================================
// Origin state: written through the ABI, read back through the ABI
// ===========================================================================

TEST_F(AbiOriginStateTest, OriginStateRoundTrips) {
  OpenCache();

  ps_write_params_t params;
  ps_write_params_init_sized(&params, sizeof(params));
  params.alternate_id = 0x08;
  params.content_length = 5;
  params.full_mask = 0x08;
  params.content_type = PS_CONTENT_CSS;
  params.origin_ct = "text/css";
  params.origin_etag = "W/\"abc123\"";
  params.cache_inserted_at = 1000000;
  params.origin_max_age = 600;
  params.origin_s_maxage = 1200;
  params.origin_last_modified = 999000;
  params.origin_cc_flags = PS_CC_ORIGIN_HEADER_PRESENT | PS_CC_ORIGIN_PUBLIC |
                           PS_CC_ORIGIN_S_MAXAGE_PRESENT;
  Write("/a.css", &params, "body{");

  ps_read_result_t* r = Read("/a.css", 0x08);
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(ps_read_cache_inserted_at(r), 1000000u);
  EXPECT_EQ(ps_read_origin_max_age(r), 600u);
  EXPECT_EQ(ps_read_origin_s_maxage(r), 1200u);
  EXPECT_EQ(ps_read_origin_last_modified(r), 999000u);
  EXPECT_EQ(ps_read_origin_cc_flags(r), PS_CC_ORIGIN_HEADER_PRESENT |
                                            PS_CC_ORIGIN_PUBLIC |
                                            PS_CC_ORIGIN_S_MAXAGE_PRESENT);
  // Verbatim: quotes and the W/ prefix survive, so it can go straight into an
  // If-None-Match.
  ASSERT_NE(ps_read_origin_etag(r), nullptr);
  EXPECT_STREQ(ps_read_origin_etag(r), "W/\"abc123\"");
  ps_read_free(r);
}

TEST_F(AbiOriginStateTest, UnsetOriginStateReadsBackAsAbsent) {
  OpenCache();

  ps_write_params_t params;
  ps_write_params_init_sized(&params, sizeof(params));
  params.alternate_id = 0x08;
  params.content_length = 2;
  params.full_mask = 0x08;
  params.content_type = PS_CONTENT_HTML;
  Write("/b.html", &params, "hi");

  ps_read_result_t* r = Read("/b.html", 0x08);
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(ps_read_origin_max_age(r), 0u);
  EXPECT_EQ(ps_read_origin_s_maxage(r), 0u);
  EXPECT_EQ(ps_read_origin_cc_flags(r), 0u);
  EXPECT_EQ(ps_read_origin_last_modified(r), 0u);
  EXPECT_EQ(ps_read_origin_etag(r), nullptr);
  ps_read_free(r);
}

TEST_F(AbiOriginStateTest, AccessorsTolerateANullResult) {
  EXPECT_EQ(ps_read_origin_max_age(nullptr), 0u);
  EXPECT_EQ(ps_read_origin_s_maxage(nullptr), 0u);
  EXPECT_EQ(ps_read_origin_cc_flags(nullptr), 0u);
  EXPECT_EQ(ps_read_origin_last_modified(nullptr), 0u);
  EXPECT_EQ(ps_read_origin_etag(nullptr), nullptr);
}

// ===========================================================================
// The Age contract
// ===========================================================================

TEST(AbiAgeAdjustment, AgeIsSubtractedFromNow) {
  EXPECT_EQ(ps_age_adjusted_insert_time(1000, 120), 880u);
  EXPECT_EQ(ps_age_adjusted_insert_time(1000, 1), 999u);
}

TEST(AbiAgeAdjustment, NoAgeLeavesNowAlone) {
  EXPECT_EQ(ps_age_adjusted_insert_time(1000, 0), 1000u);
}

TEST(AbiAgeAdjustment, AnAgeBeyondNowIsRefusedRatherThanUnderflowed) {
  // A response claiming to be older than the epoch-relative clock is a broken
  // origin or a broken clock. Subtracting would land the entry in a different
  // era; keeping `now` leaves it merely over-fresh, which is recoverable.
  EXPECT_EQ(ps_age_adjusted_insert_time(1000, 1001), 1000u);
  EXPECT_EQ(ps_age_adjusted_insert_time(1000, 0xFFFFFFFF), 1000u);
  EXPECT_EQ(ps_age_adjusted_insert_time(0, 5), 0u);
}

TEST(AbiAgeAdjustment, ExactlyNowIsAccepted) {
  EXPECT_EQ(ps_age_adjusted_insert_time(1000, 1000), 0u);
}

TEST_F(AbiOriginStateTest, InboundAgeYieldsAnEntryStoredNowMinusAge) {
  // The regression this exists for: a writer that folds `Age: N` into the
  // insertion time stores now-N, and the entry is therefore already N seconds
  // old the moment it lands -- exactly as the origin's own clock says.
  OpenCache();
  const uint32_t now = static_cast<uint32_t>(std::time(nullptr));
  const uint32_t inbound_age = 300;

  ps_write_params_t params;
  ps_write_params_init_sized(&params, sizeof(params));
  params.alternate_id = 0x08;
  params.content_length = 2;
  params.full_mask = 0x08;
  params.content_type = PS_CONTENT_CSS;
  params.cache_inserted_at = ps_age_adjusted_insert_time(now, inbound_age);
  Write("/aged.css", &params, "hi");

  ps_read_result_t* r = Read("/aged.css", 0x08);
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(ps_read_cache_inserted_at(r), now - inbound_age);
  ps_read_free(r);
}

TEST_F(AbiOriginStateTest, AnAgedEntryIsAlreadyStaleWhereAnUnadjustedOneIsNot) {
  // Same response, same max-age, same moment -- the only difference is whether
  // the writer honoured the Age it arrived with. One verdict is FRESH and the
  // other is REVALIDATE, and that gap is precisely what the contract buys.
  OpenCache();
  const uint32_t now = static_cast<uint32_t>(std::time(nullptr));
  const uint32_t inbound_age = 300;
  const uint32_t origin_max_age = 120;

  ps_write_params_t adjusted;
  ps_write_params_init_sized(&adjusted, sizeof(adjusted));
  adjusted.alternate_id = 0x08;
  adjusted.content_length = 2;
  adjusted.full_mask = 0x08;
  adjusted.content_type = PS_CONTENT_CSS;
  adjusted.origin_max_age = origin_max_age;
  adjusted.origin_cc_flags = PS_CC_ORIGIN_HEADER_PRESENT;
  adjusted.cache_inserted_at = ps_age_adjusted_insert_time(now, inbound_age);
  Write("/adjusted.css", &adjusted, "hi");

  ps_write_params_t unadjusted = adjusted;
  unadjusted.cache_inserted_at = now;
  Write("/unadjusted.css", &unadjusted, "hi");

  auto verdict = [&](const char* url) {
    ps_read_result_t* r = Read(url, 0x08);
    EXPECT_NE(r, nullptr);
    ps_freshness_input_t in;
    std::memset(&in, 0, sizeof(in));
    in.struct_size = sizeof(in);
    in.now_seconds = now;
    in.cache_inserted_at = ps_read_cache_inserted_at(r);
    in.origin_max_age = ps_read_origin_max_age(r);
    in.origin_cc_flags = ps_read_origin_cc_flags(r);
    in.content_type = PS_CONTENT_CSS;
    in.cache_scope = PS_CACHE_SCOPE_SHARED;
    ps_freshness_result_t out;
    std::memset(&out, 0, sizeof(out));
    out.struct_size = sizeof(out);
    EXPECT_EQ(ps_evaluate_freshness(&in, nullptr, &out), PS_OK);
    ps_read_free(r);
    return out;
  };

  const ps_freshness_result_t aged = verdict("/adjusted.css");
  EXPECT_EQ(aged.verdict, PS_FRESHNESS_REVALIDATE);
  EXPECT_EQ(aged.age_seconds, inbound_age);
  EXPECT_TRUE(aged.expired_by_age);

  const ps_freshness_result_t fresh = verdict("/unadjusted.css");
  EXPECT_EQ(fresh.verdict, PS_FRESHNESS_FRESH);
  EXPECT_EQ(fresh.age_seconds, 0u);
}

TEST_F(AbiOriginStateTest, ZeroInsertionTimeFallsBackToTheLocalClock) {
  // The pre-1.2 behaviour, and what a caller that never sets the field gets.
  OpenCache();
  ps_write_params_t params;
  ps_write_params_init_sized(&params, sizeof(params));
  params.alternate_id = 0x08;
  params.content_length = 2;
  params.full_mask = 0x08;
  params.content_type = PS_CONTENT_HTML;
  params.cache_inserted_at = 0;
  Write("/clock.html", &params, "hi");

  const uint32_t now = static_cast<uint32_t>(std::time(nullptr));
  ps_read_result_t* r = Read("/clock.html", 0x08);
  ASSERT_NE(r, nullptr);
  const uint32_t inserted = ps_read_cache_inserted_at(r);
  EXPECT_GT(inserted, 0u);
  EXPECT_LE(inserted, now);
  EXPECT_GE(inserted, now - 5);
  ps_read_free(r);
}

// ===========================================================================
// struct_size skew, both directions
// ===========================================================================

TEST_F(AbiOriginStateTest, LegacyInitializerLeavesTheStructAtItsOldSize) {
  // ps_write_params_init is what a pre-1.2 consumer links against, and its
  // struct is only the old prefix. Clearing more than that would run off the
  // end of the caller's buffer -- so it must not, and it must report the size
  // it actually initialized.
  ps_write_params_t params;
  std::memset(&params, 0xAB, sizeof(params));
  ps_write_params_init(&params);
  EXPECT_EQ(params.struct_size,
            offsetof(ps_write_params_t, origin_ct) + sizeof(const char*));
  EXPECT_EQ(params.struct_size, 48u);
  // The prefix is cleared...
  EXPECT_EQ(params.alternate_id, 0);
  EXPECT_EQ(params.origin_ct, nullptr);
  // ...and the bytes past it are untouched, which is the whole point: they
  // may not belong to this struct at all.
  EXPECT_EQ(params.cache_inserted_at, 0xABABABABu);
}

TEST_F(AbiOriginStateTest, SizedInitializerClearsTheWholeStruct) {
  ps_write_params_t params;
  std::memset(&params, 0xAB, sizeof(params));
  ps_write_params_init_sized(&params, sizeof(params));
  EXPECT_EQ(params.struct_size, sizeof(ps_write_params_t));
  EXPECT_EQ(params.cache_inserted_at, 0u);
  EXPECT_EQ(params.origin_etag, nullptr);
  EXPECT_EQ(params.origin_cc_flags, 0);
}

TEST_F(AbiOriginStateTest, OldCallerNewLibrary) {
  // Simulate a consumer whose header stopped at 1.1: it declares struct_size
  // as the old size, and its buffer genuinely ends there. The library must
  // read only that prefix -- anything more is a live overread of memory that
  // is not part of the struct.
  OpenCache();
  const size_t kOldSize =
      offsetof(ps_write_params_t, origin_ct) + sizeof(const char*);

  // Allocate exactly the old size, with a guard page-ish canary behind it so a
  // read past the declared prefix would at least be visible under a sanitizer.
  alignas(ps_write_params_t) unsigned char storage[sizeof(ps_write_params_t)];
  std::memset(storage, 0xEE, sizeof(storage));
  ps_write_params_t* params = reinterpret_cast<ps_write_params_t*>(storage);
  std::memset(params, 0, kOldSize);
  params->struct_size = kOldSize;
  params->alternate_id = 0x08;
  params->content_length = 2;
  params->full_mask = 0x08;
  params->content_type = PS_CONTENT_HTML;
  params->origin_ct = "text/html";

  Write("/old.html", params, "hi");

  ps_read_result_t* r = Read("/old.html", 0x08);
  ASSERT_NE(r, nullptr);
  // The 1.1 fields arrived...
  EXPECT_STREQ(ps_read_origin_content_type(r), "text/html");
  // ...and the 1.2 fields, which the caller's struct does not contain, read
  // back as absent rather than as whatever 0xEE happened to spell.
  EXPECT_EQ(ps_read_origin_max_age(r), 0u);
  EXPECT_EQ(ps_read_origin_cc_flags(r), 0u);
  EXPECT_EQ(ps_read_origin_etag(r), nullptr);
  // cache_inserted_at was not supplied, so the library stamped its own clock.
  EXPECT_GT(ps_read_cache_inserted_at(r), 0u);
  ps_read_free(r);
}

TEST_F(AbiOriginStateTest, NewCallerOldLibrary) {
  // The mirror case, as far as it can be simulated in-process: a caller that
  // declares a LARGER struct than the library knows. The library must clamp to
  // its own size and ignore the tail rather than interpreting bytes it has no
  // definition for.
  OpenCache();
  struct Bigger {
    ps_write_params_t base;
    uint64_t future_field;
  };
  Bigger big;
  std::memset(&big, 0, sizeof(big));
  big.base.struct_size = sizeof(big);  // larger than sizeof(ps_write_params_t)
  big.base.alternate_id = 0x08;
  big.base.content_length = 2;
  big.base.full_mask = 0x08;
  big.base.content_type = PS_CONTENT_HTML;
  big.base.origin_max_age = 42;
  big.future_field = 0xDEADBEEFCAFEF00DULL;

  Write("/new.html", &big.base, "hi");

  ps_read_result_t* r = Read("/new.html", 0x08);
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(ps_read_origin_max_age(r), 42u);
  ps_read_free(r);
}

TEST_F(AbiOriginStateTest, AStructSizeBetweenPublishedShapesIsSnappedDown) {
  // struct_size selects how many of the caller's bytes we read, so a value
  // that is not a shape this library ever published must not be taken
  // literally. 52 lands mid-origin_etag; copying 52 bytes would leave half a
  // POINTER in the local, which the write path then dereferences. Snapping to
  // the 1.1 rung means only whole, real shapes are ever read.
  OpenCache();
  const size_t kOldSize =
      offsetof(ps_write_params_t, origin_ct) + sizeof(const char*);

  for (size_t declared : {kOldSize + 1, kOldSize + 4,
                          offsetof(ps_write_params_t, origin_etag) + 4,
                          sizeof(ps_write_params_t) - 1}) {
    SCOPED_TRACE(declared);
    ps_write_params_t params;
    // Poison the tail: if the clamp were not applied, these bytes would be
    // copied into the local and read as a pointer.
    std::memset(&params, 0x7F, sizeof(params));
    std::memset(&params, 0, kOldSize);
    params.struct_size = declared;
    params.alternate_id = 0x08;
    params.content_length = 2;
    params.full_mask = 0x08;
    params.content_type = PS_CONTENT_HTML;

    const std::string url = "/ladder-" + std::to_string(declared) + ".html";
    ps_write_handle_t* wh = nullptr;
    ASSERT_EQ(ps_cache_write_begin(cache_, url.c_str(), "example.com", "https",
                                   &params, &wh),
              PS_OK);
    ASSERT_EQ(ps_write_data(wh, "hi", 2), PS_OK);
    ASSERT_EQ(ps_write_close(wh), PS_OK);

    ps_read_result_t* r = Read(url.c_str(), 0x08);
    ASSERT_NE(r, nullptr);
    // Everything above the 1.1 rung was discarded rather than half-read.
    EXPECT_EQ(ps_read_origin_etag(r), nullptr);
    EXPECT_EQ(ps_read_origin_max_age(r), 0u);
    ps_read_free(r);
  }
}

TEST_F(AbiOriginStateTest, AStructSizeBelowEveryPublishedShapeIsRefused) {
  // There has never been a ps_write_params_t smaller than the 1.1 struct, so a
  // caller claiming one is telling us something we cannot act on. Guessing
  // would mean reading a shape that never existed.
  OpenCache();
  const size_t kOldSize =
      offsetof(ps_write_params_t, origin_ct) + sizeof(const char*);
  for (size_t declared : {sizeof(size_t), kOldSize - 1, size_t{16}}) {
    SCOPED_TRACE(declared);
    ps_write_params_t params;
    std::memset(&params, 0, sizeof(params));
    params.struct_size = declared;
    params.alternate_id = 0x08;
    ps_write_handle_t* wh = nullptr;
    EXPECT_EQ(ps_cache_write_begin(cache_, "/short.html", "example.com",
                                   "https", &params, &wh),
              PS_ERR_INVALID_ARG);
    EXPECT_EQ(wh, nullptr);
  }
}

TEST_F(AbiOriginStateTest, UninitializedStructSizeIsRefused) {
  // struct_size is the only thing standing between the library and a
  // misinterpreted buffer, so an unset one is an error, not a guess.
  OpenCache();
  ps_write_params_t params;
  std::memset(&params, 0, sizeof(params));
  params.alternate_id = 0x08;
  ps_write_handle_t* wh = nullptr;
  EXPECT_EQ(ps_cache_write_begin(cache_, "/x.html", "example.com", "https",
                                 &params, &wh),
            PS_ERR_INVALID_ARG);
  EXPECT_EQ(wh, nullptr);
}

TEST(AbiStructSize, PolicyEntryPointsRefuseAnUnsetStructSize) {
  ps_freshness_input_t in;
  std::memset(&in, 0, sizeof(in));
  ps_freshness_result_t out;
  std::memset(&out, 0, sizeof(out));
  out.struct_size = sizeof(out);
  EXPECT_EQ(ps_evaluate_freshness(&in, nullptr, &out), PS_ERR_INVALID_ARG);

  in.struct_size = sizeof(in);
  std::memset(&out, 0, sizeof(out));
  EXPECT_EQ(ps_evaluate_freshness(&in, nullptr, &out), PS_ERR_INVALID_ARG);

  ps_cache_control_input_t cci;
  std::memset(&cci, 0, sizeof(cci));
  char buf[256];
  size_t len = 0;
  EXPECT_EQ(ps_build_cache_control(&cci, buf, sizeof(buf), &len, nullptr),
            PS_ERR_INVALID_ARG);

  ps_cache_control_t cc;
  std::memset(&cc, 0, sizeof(cc));
  EXPECT_EQ(ps_parse_cache_control("public", &cc), PS_ERR_INVALID_ARG);
}

TEST(AbiStructSize, AShorterResultStructIsFilledOnlyAsFarAsItGoes) {
  // An older caller's result struct ends early; the library must fill its
  // prefix and stop, not write past it.
  ps_freshness_input_t in;
  std::memset(&in, 0, sizeof(in));
  in.struct_size = sizeof(in);
  in.now_seconds = 1000;
  in.cache_inserted_at = 900;
  in.origin_max_age = 600;
  in.origin_cc_flags = PS_CC_ORIGIN_HEADER_PRESENT;
  in.content_type = PS_CONTENT_CSS;
  in.cache_scope = PS_CACHE_SCOPE_SHARED;

  alignas(ps_freshness_result_t) unsigned char
      storage[sizeof(ps_freshness_result_t)];
  std::memset(storage, 0x5A, sizeof(storage));
  ps_freshness_result_t* out =
      reinterpret_cast<ps_freshness_result_t*>(storage);
  const size_t kShort =
      offsetof(ps_freshness_result_t, age_seconds) + sizeof(uint32_t);
  std::memset(out, 0, kShort);
  out->struct_size = kShort;

  ASSERT_EQ(ps_evaluate_freshness(&in, nullptr, out), PS_OK);
  EXPECT_EQ(out->verdict, PS_FRESHNESS_FRESH);
  EXPECT_EQ(out->age_seconds, 100u);
  // Past the caller's declared end, the canary is intact.
  EXPECT_EQ(storage[kShort], 0x5A);
}

// ===========================================================================
// The exported policy functions answer exactly what the engine answers
// ===========================================================================

// The vectors below are the shapes the freshness evaluator is specified on:
// each one is a distinct branch, not a variation on a theme.
struct FreshnessVector {
  const char* name;
  uint32_t now;
  uint32_t inserted;
  uint32_t max_age;
  uint32_t s_maxage;
  uint16_t cc_flags;
  ps_content_type_t ct;
  ps_cache_scope_t scope;
  int force;
};

const FreshnessVector kVectors[] = {
    {"fresh within max-age", 1000, 900, 600, 0, PS_CC_ORIGIN_HEADER_PRESENT,
     PS_CONTENT_CSS, PS_CACHE_SCOPE_SHARED, 0},
    {"stale past max-age", 1000, 100, 600, 0, PS_CC_ORIGIN_HEADER_PRESENT,
     PS_CONTENT_CSS, PS_CACHE_SCOPE_SHARED, 0},
    {"exactly at max-age", 1000, 400, 600, 0, PS_CC_ORIGIN_HEADER_PRESENT,
     PS_CONTENT_CSS, PS_CACHE_SCOPE_SHARED, 0},
    {"no-store", 1000, 990, 600, 0,
     PS_CC_ORIGIN_HEADER_PRESENT | PS_CC_ORIGIN_NO_STORE, PS_CONTENT_CSS,
     PS_CACHE_SCOPE_SHARED, 0},
    {"private, shared cache", 1000, 990, 600, 0,
     PS_CC_ORIGIN_HEADER_PRESENT | PS_CC_ORIGIN_PRIVATE, PS_CONTENT_CSS,
     PS_CACHE_SCOPE_SHARED, 0},
    {"private, private cache", 1000, 990, 600, 0,
     PS_CC_ORIGIN_HEADER_PRESENT | PS_CC_ORIGIN_PRIVATE, PS_CONTENT_CSS,
     PS_CACHE_SCOPE_PRIVATE, 0},
    {"no-cache is permanently stale", 1000, 999, 600, 0,
     PS_CC_ORIGIN_HEADER_PRESENT | PS_CC_ORIGIN_NO_CACHE, PS_CONTENT_CSS,
     PS_CACHE_SCOPE_SHARED, 0},
    {"s-maxage wins for a shared cache", 1000, 700, 100, 600,
     PS_CC_ORIGIN_HEADER_PRESENT | PS_CC_ORIGIN_S_MAXAGE_PRESENT,
     PS_CONTENT_CSS, PS_CACHE_SCOPE_SHARED, 0},
    {"s-maxage ignored by a private cache", 1000, 700, 100, 600,
     PS_CC_ORIGIN_HEADER_PRESENT | PS_CC_ORIGIN_S_MAXAGE_PRESENT,
     PS_CONTENT_CSS, PS_CACHE_SCOPE_PRIVATE, 0},
    {"css default when origin sent no CC", 1000, 900, 0, 0, 0, PS_CONTENT_CSS,
     PS_CACHE_SCOPE_SHARED, 0},
    {"image default when origin sent no CC", 1000, 900, 0, 0, 0,
     PS_CONTENT_IMAGE, PS_CACHE_SCOPE_SHARED, 0},
    {"html with no CC at all", 1000, 999, 0, 0, 0, PS_CONTENT_HTML,
     PS_CACHE_SCOPE_SHARED, 0},
    {"max-age cap applies", 1000, 900, 999999, 0, PS_CC_ORIGIN_HEADER_PRESENT,
     PS_CONTENT_IMAGE, PS_CACHE_SCOPE_SHARED, 0},
    {"immutable gets the longer cap", 1000, 900, 999999, 0,
     PS_CC_ORIGIN_HEADER_PRESENT | PS_CC_ORIGIN_IMMUTABLE, PS_CONTENT_IMAGE,
     PS_CACHE_SCOPE_SHARED, 0},
    {"client force-refresh", 1000, 999, 600, 0, PS_CC_ORIGIN_HEADER_PRESENT,
     PS_CONTENT_CSS, PS_CACHE_SCOPE_SHARED, 1},
    {"clock skew: inserted in the future", 1000, 1010, 600, 0,
     PS_CC_ORIGIN_HEADER_PRESENT, PS_CONTENT_CSS, PS_CACHE_SCOPE_SHARED, 0},
    {"corrupt far-future timestamp", 1000, 900000, 600, 0,
     PS_CC_ORIGIN_HEADER_PRESENT, PS_CONTENT_CSS, PS_CACHE_SCOPE_SHARED, 0},
    // The JS arm of the CSS/JS default. The header claims css_max_age is
    // "also used for JS"; only PS_CONTENT_CSS was exercised, so the claim
    // rested on reading the code rather than on running it.
    {"js shares the css default", 1000, 900, 0, 0, 0, PS_CONTENT_JS,
     PS_CACHE_SCOPE_SHARED, 0},
    // No per-type default applies at all: effective lifetime 0, so the entry
    // is stale on arrival. This is the fall-through nothing covered.
    {"other type, no CC, no default", 1000, 999, 0, 0, 0, PS_CONTENT_OTHER,
     PS_CACHE_SCOPE_SHARED, 0},
    // no-store on a PRIVATE cache -- the second operand of the
    // `is_shared_cache || no-store` disjunction, and the branch that decides
    // whether the scope polarity can leak a no-store response.
    {"no-store on a private cache", 1000, 990, 600, 0,
     PS_CC_ORIGIN_HEADER_PRESENT | PS_CC_ORIGIN_NO_STORE, PS_CONTENT_CSS,
     PS_CACHE_SCOPE_PRIVATE, 0},
};

TEST(AbiFreshness, MatchesTheEngineOnEveryVector) {
  const pagespeed::FreshnessConfig cfg;
  for (const FreshnessVector& v : kVectors) {
    SCOPED_TRACE(v.name);

    pagespeed::FreshnessInput fi;
    fi.now_seconds = v.now;
    fi.cache_inserted_at = v.inserted;
    fi.origin_max_age = v.max_age;
    fi.origin_s_maxage = v.s_maxage;
    fi.origin_cc_flags = v.cc_flags;
    fi.content_type = static_cast<pagespeed::ContentType>(v.ct);
    fi.is_shared_cache = (v.scope != PS_CACHE_SCOPE_PRIVATE);
    fi.force_revalidate = v.force != 0;
    const pagespeed::FreshnessResult want =
        pagespeed::EvaluateFreshness(fi, cfg);

    ps_freshness_input_t in;
    std::memset(&in, 0, sizeof(in));
    in.struct_size = sizeof(in);
    in.now_seconds = v.now;
    in.cache_inserted_at = v.inserted;
    in.origin_max_age = v.max_age;
    in.origin_s_maxage = v.s_maxage;
    in.origin_cc_flags = v.cc_flags;
    in.content_type = v.ct;
    in.cache_scope = v.scope;
    in.force_revalidate = v.force;

    ps_freshness_result_t got;
    std::memset(&got, 0, sizeof(got));
    got.struct_size = sizeof(got);
    ASSERT_EQ(ps_evaluate_freshness(&in, nullptr, &got), PS_OK);

    EXPECT_EQ(static_cast<int>(got.verdict), static_cast<int>(want.verdict));
    EXPECT_EQ(got.age_seconds, want.age_seconds);
    EXPECT_EQ(got.effective_max_age, want.effective_max_age);
    EXPECT_EQ(got.remaining_ttl, want.remaining_ttl);
    EXPECT_EQ(got.is_stale != 0, want.is_stale);
    EXPECT_EQ(got.expired_by_age != 0, want.expired_by_age);
  }
}

// ===========================================================================
// The zero value of every field has to be the safe one
// ===========================================================================

TEST(AbiCacheScope, ZeroIsSharedWhichIsTheFailSafeDirection) {
  // The regression this exists for. An embedder that zero-fills its input and
  // never sets the scope is, overwhelmingly, a proxy -- a shared cache. If
  // zero meant "private", that embedder would serve an origin's `private`
  // response to the next user, and would silently discard `s-maxage`. Neither
  // failure is visible from the outside, which is why the polarity is pinned
  // here rather than left to a comment telling people to set the field.
  ps_freshness_input_t in;
  std::memset(&in, 0, sizeof(in));
  in.struct_size = sizeof(in);
  in.now_seconds = 1000;
  in.cache_inserted_at = 990;
  in.origin_max_age = 600;
  in.origin_cc_flags = PS_CC_ORIGIN_HEADER_PRESENT | PS_CC_ORIGIN_PRIVATE;
  in.content_type = PS_CONTENT_CSS;
  // cache_scope deliberately left at its zero value.

  ps_freshness_result_t out;
  std::memset(&out, 0, sizeof(out));
  out.struct_size = sizeof(out);
  ASSERT_EQ(ps_evaluate_freshness(&in, nullptr, &out), PS_OK);
  EXPECT_EQ(out.verdict, PS_FRESHNESS_SERVE_NO_CACHE)
      << "a zero-filled input must be read as a SHARED cache; reading it as "
         "private serves one user's private response to the next";

  // And the opposite value really does reach the private-cache relaxation, so
  // the field is not simply being ignored.
  in.cache_scope = PS_CACHE_SCOPE_PRIVATE;
  std::memset(&out, 0, sizeof(out));
  out.struct_size = sizeof(out);
  ASSERT_EQ(ps_evaluate_freshness(&in, nullptr, &out), PS_OK);
  EXPECT_EQ(out.verdict, PS_FRESHNESS_FRESH);
}

TEST(AbiCacheScope, ZeroIsSharedForSMaxageToo) {
  ps_freshness_input_t in;
  std::memset(&in, 0, sizeof(in));
  in.struct_size = sizeof(in);
  in.now_seconds = 1000;
  in.cache_inserted_at = 700;
  in.origin_max_age = 100;
  in.origin_s_maxage = 600;
  in.origin_cc_flags =
      PS_CC_ORIGIN_HEADER_PRESENT | PS_CC_ORIGIN_S_MAXAGE_PRESENT;
  in.content_type = PS_CONTENT_CSS;

  ps_freshness_result_t out;
  std::memset(&out, 0, sizeof(out));
  out.struct_size = sizeof(out);
  ASSERT_EQ(ps_evaluate_freshness(&in, nullptr, &out), PS_OK);
  // s-maxage honoured => the shared lifetime, not max-age.
  EXPECT_EQ(out.effective_max_age, 600u);
}

TEST(AbiCacheScope, AnUnrecognisedScopeValueIsTreatedAsShared) {
  // A value from a newer header must not land on the permissive side either.
  ps_freshness_input_t in;
  std::memset(&in, 0, sizeof(in));
  in.struct_size = sizeof(in);
  in.now_seconds = 1000;
  in.cache_inserted_at = 990;
  in.origin_max_age = 600;
  in.origin_cc_flags = PS_CC_ORIGIN_HEADER_PRESENT | PS_CC_ORIGIN_PRIVATE;
  in.content_type = PS_CONTENT_CSS;
  in.cache_scope = static_cast<ps_cache_scope_t>(77);

  ps_freshness_result_t out;
  std::memset(&out, 0, sizeof(out));
  out.struct_size = sizeof(out);
  ASSERT_EQ(ps_evaluate_freshness(&in, nullptr, &out), PS_OK);
  EXPECT_EQ(out.verdict, PS_FRESHNESS_SERVE_NO_CACHE);
}

TEST(AbiCacheScope, TheBuilderTakesTheSameZeroSafeDefault) {
  ps_cache_control_input_t in;
  std::memset(&in, 0, sizeof(in));
  in.struct_size = sizeof(in);
  in.mode = PS_CACHE_MODE_SAFE;
  in.origin_cc_flags =
      PS_CC_ORIGIN_HEADER_PRESENT | PS_CC_ORIGIN_S_MAXAGE_PRESENT;
  in.effective_max_age = 600;
  in.origin_max_age = 60;
  in.content_type = PS_CONTENT_CSS;
  // cache_scope left zero.

  char buf[256];
  size_t len = 0;
  ASSERT_EQ(ps_build_cache_control(&in, buf, sizeof(buf), &len, nullptr),
            PS_OK);
  const std::string got(buf, len);
  EXPECT_NE(got.find("s-maxage="), std::string::npos)
      << "zero-filled input must build a SHARED cache's header; got: " << got;
}

// ===========================================================================
// Config initialization
// ===========================================================================

TEST(AbiFreshnessConfig, SizedInitializerClearsOnlyWhatTheCallerOwns) {
  // The whole reason there is no size-less form: this initializer must never
  // write past the caller's buffer, however much bigger the library's own
  // struct becomes.
  alignas(ps_freshness_config_t) unsigned char
      storage[sizeof(ps_freshness_config_t) + 16];
  std::memset(storage, 0x5A, sizeof(storage));
  ps_freshness_config_t* c = reinterpret_cast<ps_freshness_config_t*>(storage);

  ps_freshness_config_init_sized(c, sizeof(ps_freshness_config_t));
  EXPECT_EQ(c->struct_size, sizeof(ps_freshness_config_t));
  EXPECT_EQ(storage[sizeof(ps_freshness_config_t)], 0x5A)
      << "the initializer wrote past the size it was given";
}

TEST(AbiFreshnessConfig, AShortBufferIsInitializedOnlyAsFarAsItGoes) {
  // What a caller compiled against a future, smaller-prefix header looks like.
  alignas(ps_freshness_config_t) unsigned char
      storage[sizeof(ps_freshness_config_t)];
  std::memset(storage, 0x5A, sizeof(storage));
  ps_freshness_config_t* c = reinterpret_cast<ps_freshness_config_t*>(storage);
  const size_t kShort =
      offsetof(ps_freshness_config_t, html_max_age) + sizeof(uint32_t);

  ps_freshness_config_init_sized(c, kShort);
  EXPECT_EQ(c->struct_size, kShort);
  EXPECT_EQ(c->max_age_cap, 86400u);
  EXPECT_EQ(c->html_max_age, 0u);
  // Past the declared end, untouched.
  EXPECT_EQ(storage[kShort], 0x5A);
}

TEST(AbiFreshnessConfig, TooSmallToHoldStructSizeIsLeftAlone) {
  // There is nowhere to record the size, so writing anything would be the
  // overrun this entry point exists to prevent.
  unsigned char storage[sizeof(ps_freshness_config_t)];
  std::memset(storage, 0x5A, sizeof(storage));
  ps_freshness_config_init_sized(
      reinterpret_cast<ps_freshness_config_t*>(storage), 4);
  for (size_t i = 0; i < sizeof(storage); ++i) {
    ASSERT_EQ(storage[i], 0x5A) << "byte " << i << " was written";
  }
}

TEST(AbiFreshnessConfig, TheAutoMacroTakesTheCallersOwnSize) {
  ps_freshness_config_t c;
  std::memset(&c, 0xAB, sizeof(c));
  ps_freshness_config_init_auto(&c);
  EXPECT_EQ(c.struct_size, sizeof(ps_freshness_config_t));
  EXPECT_EQ(c.css_max_age, 300u);
}

TEST(AbiWriteParams, TheAutoMacroTakesTheCallersOwnSize) {
  ps_write_params_t p;
  std::memset(&p, 0xAB, sizeof(p));
  ps_write_params_init_auto(&p);
  EXPECT_EQ(p.struct_size, sizeof(ps_write_params_t));
  EXPECT_EQ(p.origin_etag, nullptr);
  EXPECT_EQ(p.cache_inserted_at, 0u);
}

TEST(AbiFreshness, DefaultConfigMatchesTheEngineDefaults) {
  ps_freshness_config_t c;
  ps_freshness_config_init_auto(&c);
  const pagespeed::FreshnessConfig want;
  EXPECT_EQ(c.struct_size, sizeof(ps_freshness_config_t));
  EXPECT_EQ(c.max_age_cap, want.max_age_cap);
  EXPECT_EQ(c.immutable_max_age_cap, want.immutable_max_age_cap);
  EXPECT_EQ(c.html_max_age, want.html_max_age);
  EXPECT_EQ(c.css_max_age, want.css_max_age);
  EXPECT_EQ(c.image_max_age, want.image_max_age);
}

TEST(AbiFreshness, AnExplicitConfigIsHonoured) {
  ps_freshness_config_t c;
  ps_freshness_config_init_auto(&c);
  c.max_age_cap = 60;

  ps_freshness_input_t in;
  std::memset(&in, 0, sizeof(in));
  in.struct_size = sizeof(in);
  in.now_seconds = 1000;
  in.cache_inserted_at = 990;
  in.origin_max_age = 3600;
  in.origin_cc_flags = PS_CC_ORIGIN_HEADER_PRESENT;
  in.content_type = PS_CONTENT_CSS;
  in.cache_scope = PS_CACHE_SCOPE_SHARED;

  ps_freshness_result_t out;
  std::memset(&out, 0, sizeof(out));
  out.struct_size = sizeof(out);
  ASSERT_EQ(ps_evaluate_freshness(&in, &c, &out), PS_OK);
  EXPECT_EQ(out.effective_max_age, 60u);
}

TEST(AbiCacheControl, MatchesTheEngineOnRepresentativeInputs) {
  struct Case {
    const char* name;
    ps_cache_mode_t mode;
    uint16_t flags;
    uint32_t effective;
    uint32_t origin_max_age;
    uint8_t swr;
    ps_cache_scope_t scope;
    uint8_t relay_no_cache;
    uint8_t forward;
  };
  const Case cases[] = {
      {"safe, plain", PS_CACHE_MODE_SAFE, PS_CC_ORIGIN_HEADER_PRESENT, 300, 300,
       0, PS_CACHE_SCOPE_SHARED, 0, 0},
      {"aggressive, public", PS_CACHE_MODE_AGGRESSIVE,
       PS_CC_ORIGIN_HEADER_PRESENT | PS_CC_ORIGIN_PUBLIC, 300, 300, 1,
       PS_CACHE_SCOPE_SHARED, 0, 0},
      {"aggressive, must-revalidate blocks SWR", PS_CACHE_MODE_AGGRESSIVE,
       PS_CC_ORIGIN_HEADER_PRESENT | PS_CC_ORIGIN_MUST_REVALIDATE, 300, 300, 1,
       PS_CACHE_SCOPE_SHARED, 0, 0},
      {"s-maxage split", PS_CACHE_MODE_SAFE,
       PS_CC_ORIGIN_HEADER_PRESENT | PS_CC_ORIGIN_S_MAXAGE_PRESENT, 600, 60, 0,
       PS_CACHE_SCOPE_SHARED, 0, 0},
      {"relayed no-cache", PS_CACHE_MODE_SAFE,
       PS_CC_ORIGIN_HEADER_PRESENT | PS_CC_ORIGIN_NO_CACHE |
           PS_CC_ORIGIN_NO_CACHE_BARE,
       300, 300, 0, PS_CACHE_SCOPE_SHARED, 1, 0},
      {"forwarded restrictions", PS_CACHE_MODE_SAFE,
       PS_CC_ORIGIN_HEADER_PRESENT | PS_CC_ORIGIN_NO_STORE, 300, 300, 0,
       PS_CACHE_SCOPE_SHARED, 0, 1},
      {"no-transform preserved", PS_CACHE_MODE_SAFE,
       PS_CC_ORIGIN_HEADER_PRESENT | PS_CC_ORIGIN_NO_TRANSFORM, 300, 300, 0,
       PS_CACHE_SCOPE_SHARED, 0, 0},
  };

  for (const Case& c : cases) {
    SCOPED_TRACE(c.name);

    pagespeed::CacheControlInput want_in;
    want_in.mode = static_cast<pagespeed::CacheMode>(c.mode);
    want_in.origin_cc_flags = c.flags;
    want_in.effective_max_age = c.effective;
    want_in.origin_max_age = c.origin_max_age;
    want_in.synthesize_swr = c.swr != 0;
    want_in.is_shared_cache = (c.scope != PS_CACHE_SCOPE_PRIVATE);
    want_in.content_type = pagespeed::ContentType::kCss;
    want_in.relay_origin_no_cache = c.relay_no_cache != 0;
    want_in.forward_origin_restrictions = c.forward != 0;
    char want_buf[256];
    const pagespeed::CacheControlOutput want =
        pagespeed::BuildCacheControlHeader(want_in, want_buf, sizeof(want_buf));

    ps_cache_control_input_t in;
    std::memset(&in, 0, sizeof(in));
    in.struct_size = sizeof(in);
    in.mode = c.mode;
    in.origin_cc_flags = c.flags;
    in.effective_max_age = c.effective;
    in.origin_max_age = c.origin_max_age;
    in.synthesize_swr = c.swr;
    in.cache_scope = c.scope;
    in.content_type = PS_CONTENT_CSS;
    in.relay_origin_no_cache = c.relay_no_cache;
    in.forward_origin_restrictions = c.forward;

    char got_buf[256];
    size_t got_len = 0;
    uint32_t got_max_age = 0;
    ASSERT_EQ(ps_build_cache_control(&in, got_buf, sizeof(got_buf), &got_len,
                                     &got_max_age),
              PS_OK);
    ASSERT_EQ(got_len, want.len);
    EXPECT_EQ(std::memcmp(got_buf, want_buf, want.len), 0)
        << "got: " << std::string(got_buf, got_len)
        << " want: " << std::string(want_buf, want.len);
    EXPECT_EQ(got_max_age, want.final_max_age);
  }
}

TEST(AbiCacheControl, RejectsNullOutputsAndAZeroBuffer) {
  ps_cache_control_input_t in;
  std::memset(&in, 0, sizeof(in));
  in.struct_size = sizeof(in);
  char buf[16];
  size_t len = 0;
  EXPECT_EQ(ps_build_cache_control(nullptr, buf, sizeof(buf), &len, nullptr),
            PS_ERR_INVALID_ARG);
  EXPECT_EQ(ps_build_cache_control(&in, nullptr, sizeof(buf), &len, nullptr),
            PS_ERR_INVALID_ARG);
  EXPECT_EQ(ps_build_cache_control(&in, buf, 0, &len, nullptr),
            PS_ERR_INVALID_ARG);
  EXPECT_EQ(ps_build_cache_control(&in, buf, sizeof(buf), nullptr, nullptr),
            PS_ERR_INVALID_ARG);
}

// ===========================================================================
// The storability predicate and the flag derivation, across the boundary
// ===========================================================================

TEST(AbiVary, MirrorsTheSharedPredicate) {
  EXPECT_EQ(ps_vary_uncacheable(nullptr), 0);
  EXPECT_EQ(ps_vary_uncacheable(""), 0);
  EXPECT_EQ(ps_vary_uncacheable("Accept-Encoding"), 0);
  EXPECT_EQ(ps_vary_uncacheable("accept-encoding"), 0);
  // The request fields an origin negotiates on are refused: one original is
  // kept per URL, so it would reach clients that asked for something else.
  EXPECT_EQ(ps_vary_uncacheable("Accept"), 1);
  EXPECT_EQ(ps_vary_uncacheable("User-Agent"), 1);
  EXPECT_EQ(ps_vary_uncacheable("Save-Data"), 1);
  EXPECT_EQ(ps_vary_uncacheable("Accept, User-Agent, Save-Data"), 1);
  EXPECT_EQ(ps_vary_uncacheable("*"), 1);
  EXPECT_EQ(ps_vary_uncacheable("Cookie"), 1);
  EXPECT_EQ(ps_vary_uncacheable("Accept-Encoding, Accept-Language"), 1);
}

TEST(AbiCacheControlParse, DerivesTheStoredFlags) {
  ps_cache_control_t cc;
  std::memset(&cc, 0, sizeof(cc));
  cc.struct_size = sizeof(cc);
  ASSERT_EQ(ps_parse_cache_control("public, max-age=600, s-maxage=1200", &cc),
            PS_OK);
  EXPECT_EQ(cc.max_age, 600u);
  EXPECT_EQ(cc.s_maxage, 1200u);
  EXPECT_TRUE(cc.cc_flags & PS_CC_ORIGIN_PUBLIC);
  EXPECT_TRUE(cc.cc_flags & PS_CC_ORIGIN_S_MAXAGE_PRESENT);
  EXPECT_TRUE(cc.cc_flags & PS_CC_ORIGIN_HEADER_PRESENT);
}

TEST(AbiCacheControlParse, AccumulatesAcrossHeaderLines) {
  ps_cache_control_t cc;
  std::memset(&cc, 0, sizeof(cc));
  cc.struct_size = sizeof(cc);
  ASSERT_EQ(ps_parse_cache_control("private=\"Set-Cookie\"", &cc), PS_OK);
  ASSERT_EQ(ps_parse_cache_control("max-age=60", &cc), PS_OK);
  EXPECT_EQ(cc.max_age, 60u);
  EXPECT_TRUE(cc.cc_flags & PS_CC_ORIGIN_PRIVATE);
  EXPECT_TRUE(cc.cc_flags & PS_CC_ORIGIN_PRIVATE_QUALIFIED);
  EXPECT_FALSE(cc.cc_flags & PS_CC_ORIGIN_PRIVATE_BARE);
}

TEST(AbiCacheControlParse, TheDerivedFlagsFeedTheOtherTwoEntryPointsDirectly) {
  // The point of exporting the derivation: what comes out of the parser is
  // what goes into the stored entry and into the freshness evaluation, with
  // no per-port translation step in between to get wrong.
  ps_cache_control_t cc;
  std::memset(&cc, 0, sizeof(cc));
  cc.struct_size = sizeof(cc);
  ASSERT_EQ(ps_parse_cache_control("public, max-age=300", &cc), PS_OK);

  ps_freshness_input_t in;
  std::memset(&in, 0, sizeof(in));
  in.struct_size = sizeof(in);
  in.now_seconds = 1000;
  in.cache_inserted_at = 900;
  in.origin_max_age = cc.max_age;
  in.origin_s_maxage = cc.s_maxage;
  in.origin_cc_flags = cc.cc_flags;
  in.content_type = PS_CONTENT_CSS;
  in.cache_scope = PS_CACHE_SCOPE_SHARED;

  ps_freshness_result_t out;
  std::memset(&out, 0, sizeof(out));
  out.struct_size = sizeof(out);
  ASSERT_EQ(ps_evaluate_freshness(&in, nullptr, &out), PS_OK);
  EXPECT_EQ(out.verdict, PS_FRESHNESS_FRESH);
  EXPECT_EQ(out.remaining_ttl, 200u);
}

TEST(AbiCacheControlParse, RejectsNullArguments) {
  ps_cache_control_t cc;
  std::memset(&cc, 0, sizeof(cc));
  cc.struct_size = sizeof(cc);
  EXPECT_EQ(ps_parse_cache_control(nullptr, &cc), PS_ERR_INVALID_ARG);
  EXPECT_EQ(ps_parse_cache_control("public", nullptr), PS_ERR_INVALID_ARG);
}

// The published constants are the stored bits. A consumer that hard-codes the
// header's value and a stored entry written by the engine have to agree.
TEST(AbiConstants, MatchTheStoredFlagBits) {
  EXPECT_EQ(PS_CC_ORIGIN_NO_CACHE, AlternateMetadata::kCCOriginNoCache);
  EXPECT_EQ(PS_CC_ORIGIN_NO_STORE, AlternateMetadata::kCCOriginNoStore);
  EXPECT_EQ(PS_CC_ORIGIN_PRIVATE, AlternateMetadata::kCCOriginPrivate);
  EXPECT_EQ(PS_CC_ORIGIN_PUBLIC, AlternateMetadata::kCCOriginPublic);
  EXPECT_EQ(PS_CC_ORIGIN_IMMUTABLE, AlternateMetadata::kCCOriginImmutable);
  EXPECT_EQ(PS_CC_ORIGIN_S_MAXAGE_PRESENT,
            AlternateMetadata::kCCOriginSMaxagePresent);
  EXPECT_EQ(PS_CC_ORIGIN_HEADER_PRESENT,
            AlternateMetadata::kCCOriginHeaderPresent);
  EXPECT_EQ(PS_FLAG_NEEDS_REVALIDATION,
            AlternateMetadata::kFlagNeedsRevalidation);
  EXPECT_EQ(PS_FLAG_WORKER_PROCESSED, AlternateMetadata::kFlagWorkerProcessed);
}

}  // namespace
