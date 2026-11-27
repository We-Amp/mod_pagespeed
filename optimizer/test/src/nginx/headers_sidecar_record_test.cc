// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Store-side response-header sidecar decisions for the nginx front stage
// (issue #208): the gate's verdicts on origin header sets, the
// sidecar-first record and its re-record rules against a REAL cache on a temp
// volume (the restamp_apply_test seam), and the 304 merge plan.

#include "src/nginx/headers_sidecar_record.h"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

#include "gtest/gtest.h"
#include "lib/cache/cache.h"
#include "lib/cache/headers_sidecar.h"
#include "lib/classify/alternate_id.h"
#include "lib/classify/alternate_metadata.h"
#include "test/test_util/temp_dir.h"

namespace pagespeed {
namespace {

constexpr std::string_view kUrl = "/sidecar/asset.css";
constexpr std::string_view kHost = "example.com";
constexpr std::string_view kScheme = "https";
constexpr std::string_view kBody = "body{color:red}";
constexpr uint8_t kIdentityMask = 0x08;
constexpr uint8_t kVariantMask = 0x09;

constexpr uint8_t kClassified = AlternateMetadata::kFlagHeadersClassified;
constexpr uint8_t kRequired = AlternateMetadata::kFlagHeadersSidecarRequired;

// The verification origin's reproducible set: every line is either carried
// by the sidecar, reproduced by entry metadata, or stamped per response.
std::vector<HeaderField> SecurityHeaders() {
  return {
      {"Cache-Control", "public, max-age=600"},
      {"ETag", "\"v1\""},
      {"Access-Control-Allow-Origin", "*"},
      {"Content-Security-Policy", "default-src 'self'"},
      {"X-Content-Type-Options", "nosniff"},
      {"Referrer-Policy", "no-referrer"},
  };
}

std::vector<HeaderField> NothingToCarry() {
  return {
      {"Cache-Control", "public, max-age=600"},
      {"ETag", "\"v1\""},
      {"Last-Modified", "Tue, 13 Oct 2026 08:00:00 GMT"},
      {"Age", "12"},
      {"Accept-Ranges", "bytes"},
  };
}

std::string PayloadOf(const std::vector<HeaderField>& fields) {
  return ClassifyForHeadersSidecar(fields).payload;
}

// --- The gate on origin header sets -----------------------------------------

TEST(HeadersSidecarGateTest, SecurityHeadersAreEligibleWithABlock) {
  auto c = ClassifyForHeadersSidecar(SecurityHeaders());
  EXPECT_EQ(c.verdict, SidecarVerdict::kSwapEligible);
  EXPECT_FALSE(c.payload.empty());
  EXPECT_EQ(c.fields, 4u);
}

TEST(HeadersSidecarGateTest, VaryAloneIsCarried) {
  std::vector<HeaderField> h = {{"Cache-Control", "max-age=60"},
                                {"Vary", "Accept-Encoding"}};
  auto c = ClassifyForHeadersSidecar(h);
  EXPECT_EQ(c.verdict, SidecarVerdict::kSwapEligible);
  EXPECT_EQ(c.fields, 1u);
}

TEST(HeadersSidecarGateTest, AgeAndAcceptRangesAloneCarryNothing) {
  auto c = ClassifyForHeadersSidecar(NothingToCarry());
  EXPECT_EQ(c.verdict, SidecarVerdict::kSwapEligible);
  EXPECT_TRUE(c.payload.empty());
}

TEST(HeadersSidecarGateTest, CustomHeaderFallsThroughAndIsNamed) {
  auto h = SecurityHeaders();
  h.push_back({"X-Custom-Foo", "bar"});
  auto c = ClassifyForHeadersSidecar(h);
  EXPECT_EQ(c.verdict, SidecarVerdict::kFallThrough);
  EXPECT_EQ(FirstUnreproducibleHeader(c), "x-custom-foo");
}

TEST(HeadersSidecarGateTest, RequestIdFallsThrough) {
  std::vector<HeaderField> h = {{"Cache-Control", "max-age=60"},
                                {"X-Request-Id", "abc123"}};
  auto c = ClassifyForHeadersSidecar(h);
  EXPECT_EQ(c.verdict, SidecarVerdict::kFallThrough);
  EXPECT_EQ(FirstUnreproducibleHeader(c), "x-request-id");
}

TEST(HeadersSidecarGateTest, NonceCspIsNeverOptimized) {
  std::vector<HeaderField> h = {
      {"Cache-Control", "max-age=60"},
      {"Content-Security-Policy", "script-src 'nonce-abc'"}};
  auto c = ClassifyForHeadersSidecar(h);
  EXPECT_EQ(c.verdict, SidecarVerdict::kNeverOptimized);
  EXPECT_TRUE(FirstUnreproducibleHeader(c).empty());
}

// Why the record arm classifies a SNAPSHOT taken before the module adds its
// own lines: the module's names are not in any of the gate's sets, so
// classifying the live list at record time would refuse every response.
TEST(HeadersSidecarGateTest, ModuleAddedNamesWouldRefuseEverything) {
  auto origin = SecurityHeaders();
  ASSERT_TRUE(ClassifyForHeadersSidecar(origin).swap_eligible());
  for (std::string_view module_name :
       {"X-PageSpeed", "Accept-CH", "x-verified-bot",
        "X-PageSpeed-Revalidation"}) {
    auto live = origin;
    live.push_back({module_name, "x"});
    EXPECT_FALSE(ClassifyForHeadersSidecar(live).swap_eligible())
        << module_name;
  }
}

TEST(HeadersSidecarGateTest, HeaderNameEqualsIsCaseInsensitive) {
  EXPECT_TRUE(HeaderNameEquals("Referrer-Policy", "referrer-policy"));
  EXPECT_FALSE(HeaderNameEquals("Vary", "Varys"));
  EXPECT_FALSE(HeaderNameEquals("Vary", "Vari"));
}

// --- Record against a real cache ---------------------------------------------

class HeadersSidecarRecordTest : public ::testing::Test {
 protected:
  void SetUp() override {
    cache_dir_ = pagespeed::test::MakeTempDir();
    PageSpeedCacheConfig config;
    config.volume_path = cache_dir_ + "/cache.vol";
    config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
    auto result = PageSpeedCache::Create(config);
    ASSERT_TRUE(result.has_value()) << "failed to open cache";
    cache_ = std::move(*result);
  }

  void TearDown() override {
    cache_.reset();
    if (!cache_dir_.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(cache_dir_, ec);
    }
  }

  static AlternateMetadata Meta(uint8_t mask) {
    AlternateMetadata meta;
    meta.full_mask = mask;
    meta.content_type = ContentType::kCss;
    meta.cache_inserted_at = 1000;
    return meta;
  }

  void Seed(uint8_t mask, uint8_t flags, std::string_view body = kBody) {
    AlternateMetadata meta = Meta(mask);
    meta.flags = flags;
    auto wh = cache_->WriteAlternate(
        kUrl, kHost, kScheme, MaskToAlternateId(mask), body.size(), meta);
    ASSERT_TRUE(wh.has_value());
    ASSERT_TRUE(wh->write_sync(std::as_bytes(std::span(body))).has_value());
    ASSERT_TRUE(wh->close_sync().has_value());
  }

  void SeedBlock(const std::vector<HeaderField>& headers) {
    auto w = cache_->WriteHeadersSidecar(kUrl, kHost, kScheme, headers);
    ASSERT_TRUE(w.has_value());
    ASSERT_TRUE(w->stored);
  }

  bool Exists(uint8_t mask) {
    return cache_->AlternateExists(kUrl, kHost, kScheme,
                                   MaskToAlternateId(mask));
  }

  std::optional<uint8_t> Flags(uint8_t mask) {
    auto r =
        cache_->ReadAlternate(kUrl, kHost, kScheme, MaskToAlternateId(mask));
    if (!r.has_value() || !r->is_valid()) return std::nullopt;
    return r->metadata.flags;
  }

  std::optional<std::string> Block() {
    auto r = cache_->ReadHeadersSidecar(kUrl, kHost, kScheme);
    if (!r.has_value() || !r->is_valid()) return std::nullopt;
    return std::string(PageSpeedCache::HeadersSidecarPayload(*r));
  }

  StoredIdentityState Stored() {
    StoredIdentityState s;
    auto f = Flags(kIdentityMask);
    s.exists = f.has_value();
    s.flags = f.value_or(0);
    return s;
  }

  template <typename Cache = PageSpeedCache>
  SidecarRecordResult Record(const std::vector<HeaderField>& headers,
                             bool force = false, bool slot = true,
                             int* slot_calls = nullptr,
                             Cache* cache = nullptr) {
    auto acquire = [slot, slot_calls] {
      if (slot_calls != nullptr) ++*slot_calls;
      return slot;
    };
    if constexpr (std::is_same_v<Cache, PageSpeedCache>) {
      return RecordEntryWithHeadersSidecar(
          *cache_, kUrl, kHost, kScheme, MaskToAlternateId(kIdentityMask),
          Meta(kIdentityMask), std::as_bytes(std::span(kBody)), headers,
          Stored(), force, acquire);
    } else {
      return RecordEntryWithHeadersSidecar(
          *cache, kUrl, kHost, kScheme, MaskToAlternateId(kIdentityMask),
          Meta(kIdentityMask), std::as_bytes(std::span(kBody)), headers,
          Stored(), force, acquire);
    }
  }

  std::string cache_dir_;
  std::unique_ptr<PageSpeedCache> cache_;
};

TEST_F(HeadersSidecarRecordTest, FreshRecordWritesBlockThenFlaggedEntry) {
  auto r = Record(SecurityHeaders());
  EXPECT_EQ(r.outcome, SidecarRecordOutcome::kWritten);
  EXPECT_EQ(r.reason, SidecarRecordReason::kFresh);
  EXPECT_TRUE(r.block_written);
  EXPECT_EQ(Flags(kIdentityMask),
            std::optional<uint8_t>(kClassified | kRequired));
  EXPECT_EQ(Block(), std::optional<std::string>(PayloadOf(SecurityHeaders())));
}

TEST_F(HeadersSidecarRecordTest, NothingToCarryFlagsClassifiedOnly) {
  auto r = Record(NothingToCarry());
  EXPECT_EQ(r.outcome, SidecarRecordOutcome::kWritten);
  EXPECT_FALSE(r.block_written);
  EXPECT_EQ(Flags(kIdentityMask), std::optional<uint8_t>(kClassified));
  EXPECT_FALSE(Block().has_value());
}

TEST_F(HeadersSidecarRecordTest, NothingToCarryRemovesAStaleBlock) {
  // An orphan block (an earlier record whose entry write failed, say) must
  // not survive an entry that says there is nothing to carry.
  SeedBlock(SecurityHeaders());
  ASSERT_TRUE(Block().has_value());
  auto r = Record(NothingToCarry());
  EXPECT_EQ(r.outcome, SidecarRecordOutcome::kWritten);
  EXPECT_FALSE(Block().has_value());
}

TEST_F(HeadersSidecarRecordTest, NotEligibleStoresNothing) {
  auto h = SecurityHeaders();
  h.push_back({"X-Custom-Foo", "bar"});
  auto r = Record(h);
  EXPECT_EQ(r.outcome, SidecarRecordOutcome::kNotEligible);
  EXPECT_FALSE(Exists(kIdentityMask));
  EXPECT_FALSE(Block().has_value());
}

TEST_F(HeadersSidecarRecordTest, CurrentEntryIsKept) {
  ASSERT_EQ(Record(SecurityHeaders()).outcome, SidecarRecordOutcome::kWritten);
  int slot_calls = 0;
  auto r = Record(SecurityHeaders(), false, true, &slot_calls);
  EXPECT_EQ(r.outcome, SidecarRecordOutcome::kSkipped);
  EXPECT_EQ(r.reason, SidecarRecordReason::kCurrent);
  EXPECT_EQ(slot_calls, 0);
}

TEST_F(HeadersSidecarRecordTest, ForcedRewriteOfACurrentEntryWrites) {
  ASSERT_EQ(Record(SecurityHeaders()).outcome, SidecarRecordOutcome::kWritten);
  auto r = Record(SecurityHeaders(), /*force=*/true);
  EXPECT_EQ(r.outcome, SidecarRecordOutcome::kWritten);
  EXPECT_EQ(r.reason, SidecarRecordReason::kForced);
  EXPECT_EQ(Flags(kIdentityMask),
            std::optional<uint8_t>(kClassified | kRequired));
}

TEST_F(HeadersSidecarRecordTest, LegacyEntryIsUpgraded) {
  Seed(kIdentityMask, /*flags=*/0);
  auto r = Record(SecurityHeaders());
  EXPECT_EQ(r.outcome, SidecarRecordOutcome::kWritten);
  EXPECT_EQ(r.reason, SidecarRecordReason::kUpgrade);
  EXPECT_EQ(Flags(kIdentityMask),
            std::optional<uint8_t>(kClassified | kRequired));
  EXPECT_TRUE(Block().has_value());
}

TEST_F(HeadersSidecarRecordTest, LegacyBitsBesideTheSidecarBitsAreKept) {
  // Bits the caller set on the metadata (the Accept marker) survive.
  AlternateMetadata meta = Meta(kIdentityMask);
  meta.flags = AlternateMetadata::kFlagOriginVariesAccept;
  auto r = RecordEntryWithHeadersSidecar(
      *cache_, kUrl, kHost, kScheme, MaskToAlternateId(kIdentityMask), meta,
      std::as_bytes(std::span(kBody)), SecurityHeaders(), Stored(), false,
      [] { return true; });
  EXPECT_EQ(r.outcome, SidecarRecordOutcome::kWritten);
  EXPECT_EQ(Flags(kIdentityMask),
            std::optional<uint8_t>(kClassified | kRequired |
                                   AlternateMetadata::kFlagOriginVariesAccept));
}

TEST_F(HeadersSidecarRecordTest, FlaggedEntryWithMissingBlockSelfHeals) {
  Seed(kIdentityMask, kClassified | kRequired);
  ASSERT_FALSE(Block().has_value());
  int slot_calls = 0;
  auto r = Record(SecurityHeaders(), false, true, &slot_calls);
  EXPECT_EQ(r.outcome, SidecarRecordOutcome::kWritten);
  EXPECT_EQ(r.reason, SidecarRecordReason::kSelfHeal);
  EXPECT_EQ(slot_calls, 1);
  EXPECT_EQ(Block(), std::optional<std::string>(PayloadOf(SecurityHeaders())));
}

TEST_F(HeadersSidecarRecordTest, SelfHealIsSingleFlighted) {
  // Two requests race to heal the same URL: only the slot holder writes;
  // the other records nothing (no block, no entry rewrite).
  Seed(kIdentityMask, kClassified | kRequired, "old body");
  auto loser = Record(SecurityHeaders(), false, /*slot=*/false);
  EXPECT_EQ(loser.outcome, SidecarRecordOutcome::kNotSlotHolder);
  EXPECT_FALSE(Block().has_value());

  auto winner = Record(SecurityHeaders(), false, /*slot=*/true);
  EXPECT_EQ(winner.outcome, SidecarRecordOutcome::kWritten);
  EXPECT_TRUE(Block().has_value());

  // Once healed, a third request finds it current and asks for no slot.
  int slot_calls = 0;
  auto third = Record(SecurityHeaders(), false, false, &slot_calls);
  EXPECT_EQ(third.outcome, SidecarRecordOutcome::kSkipped);
  EXPECT_EQ(slot_calls, 0);
}

TEST_F(HeadersSidecarRecordTest, ChangedBlockIsRewritten) {
  ASSERT_EQ(Record(SecurityHeaders()).outcome, SidecarRecordOutcome::kWritten);
  auto changed = SecurityHeaders();
  changed[5].value = "same-origin";  // Referrer-Policy
  auto r = Record(changed);
  EXPECT_EQ(r.outcome, SidecarRecordOutcome::kWritten);
  EXPECT_EQ(r.reason, SidecarRecordReason::kPayloadChanged);
  EXPECT_EQ(Block(), std::optional<std::string>(PayloadOf(changed)));
}

TEST_F(HeadersSidecarRecordTest, ChangedBlockRewriteIsSingleFlighted) {
  ASSERT_EQ(Record(SecurityHeaders()).outcome, SidecarRecordOutcome::kWritten);
  auto changed = SecurityHeaders();
  changed[5].value = "same-origin";
  auto r = Record(changed, false, /*slot=*/false);
  EXPECT_EQ(r.outcome, SidecarRecordOutcome::kNotSlotHolder);
  EXPECT_EQ(Block(), std::optional<std::string>(PayloadOf(SecurityHeaders())));
}

TEST_F(HeadersSidecarRecordTest, CarryFlipRemovesTheUrlThenRecords) {
  // Recorded with a block, a worker variant derived from it; the origin
  // then stops sending the carried headers.
  ASSERT_EQ(Record(SecurityHeaders()).outcome, SidecarRecordOutcome::kWritten);
  Seed(kVariantMask, kClassified | kRequired);
  ASSERT_TRUE(Exists(kVariantMask));

  auto r = Record(NothingToCarry());
  EXPECT_EQ(r.outcome, SidecarRecordOutcome::kRemovedAndWritten);
  EXPECT_EQ(r.reason, SidecarRecordReason::kCarryFlip);
  EXPECT_FALSE(Exists(kVariantMask))
      << "a variant carrying the old bits survived a carry-state switch";
  EXPECT_FALSE(Block().has_value());
  EXPECT_EQ(Flags(kIdentityMask), std::optional<uint8_t>(kClassified));
}

TEST_F(HeadersSidecarRecordTest, CarryFlipTheOtherWay) {
  ASSERT_EQ(Record(NothingToCarry()).outcome, SidecarRecordOutcome::kWritten);
  Seed(kVariantMask, kClassified);
  auto r = Record(SecurityHeaders());
  EXPECT_EQ(r.outcome, SidecarRecordOutcome::kRemovedAndWritten);
  EXPECT_FALSE(Exists(kVariantMask));
  EXPECT_EQ(Flags(kIdentityMask),
            std::optional<uint8_t>(kClassified | kRequired));
  EXPECT_TRUE(Block().has_value());
}

TEST_F(HeadersSidecarRecordTest, CarryFlipIsSingleFlighted) {
  ASSERT_EQ(Record(SecurityHeaders()).outcome, SidecarRecordOutcome::kWritten);
  auto r = Record(NothingToCarry(), false, /*slot=*/false);
  EXPECT_EQ(r.outcome, SidecarRecordOutcome::kNotSlotHolder);
  EXPECT_EQ(Flags(kIdentityMask),
            std::optional<uint8_t>(kClassified | kRequired));
}

// A cache whose sidecar write always fails; everything else forwards to the
// real cache.
struct FailingSidecarCache {
  PageSpeedCache* real;
  auto ReadHeadersSidecar(std::string_view u, std::string_view h,
                          std::string_view s) {
    return real->ReadHeadersSidecar(u, h, s);
  }
  static std::string_view HeadersSidecarPayload(const ReadResult& r) {
    return PageSpeedCache::HeadersSidecarPayload(r);
  }
  auto Remove(std::string_view u, std::string_view h, std::string_view s) {
    return real->Remove(u, h, s);
  }
  std::expected<PageSpeedCache::HeadersSidecarWrite, cyclone::CacheError>
  WriteHeadersSidecar(std::string_view, std::string_view, std::string_view,
                      std::span<const HeaderField>) {
    return std::unexpected(cyclone::CacheError::IoError);
  }
  auto WriteAlternate(std::string_view u, std::string_view h,
                      std::string_view s, AlternateId id, size_t n,
                      const AlternateMetadata& m) {
    return real->WriteAlternate(u, h, s, id, n, m);
  }
  void EvictAlternateFromRamCache(std::string_view u, std::string_view h,
                                  std::string_view s, AlternateId id) {
    real->EvictAlternateFromRamCache(u, h, s, id);
  }
  auto RemoveAlternate(std::string_view u, std::string_view h,
                       std::string_view s, AlternateId id) {
    return real->RemoveAlternate(u, h, s, id);
  }
};

TEST_F(HeadersSidecarRecordTest, FailedBlockWriteStoresNoEntry) {
  FailingSidecarCache failing{cache_.get()};
  auto r = Record(SecurityHeaders(), false, true, nullptr, &failing);
  EXPECT_EQ(r.outcome, SidecarRecordOutcome::kSidecarWriteFailed);
  EXPECT_FALSE(r.block_written);
  EXPECT_FALSE(Exists(kIdentityMask))
      << "an entry was stored without the block it requires";
}

TEST_F(HeadersSidecarRecordTest,
       FailedBlockWriteNeedsNoBlockWhenNothingToCarry) {
  FailingSidecarCache failing{cache_.get()};
  auto r = Record(NothingToCarry(), false, true, nullptr, &failing);
  EXPECT_EQ(r.outcome, SidecarRecordOutcome::kWritten);
  EXPECT_EQ(Flags(kIdentityMask), std::optional<uint8_t>(kClassified));
}

// --- Refusal eviction --------------------------------------------------------

TEST_F(HeadersSidecarRecordTest, RefusalEvictsTheWholeUrl) {
  ASSERT_EQ(Record(SecurityHeaders()).outcome, SidecarRecordOutcome::kWritten);
  Seed(kVariantMask, kClassified | kRequired);
  EXPECT_TRUE(EvictOnSidecarRefusal(*cache_, kUrl, kHost, kScheme,
                                    MaskToAlternateId(kIdentityMask)));
  EXPECT_FALSE(Exists(kIdentityMask));
  EXPECT_FALSE(Exists(kVariantMask));
  EXPECT_FALSE(Block().has_value());
}

TEST_F(HeadersSidecarRecordTest, RefusalWithNothingStoredRemovesNothing) {
  EXPECT_FALSE(EvictOnSidecarRefusal(*cache_, kUrl, kHost, kScheme,
                                     MaskToAlternateId(kIdentityMask)));
}

// --- 304 merge plan -----------------------------------------------------

TEST(RestampSidecarPlanTest, LegacyStashRestampsAsBefore) {
  std::vector<HeaderField> r304 = {{"X-Request-Id", "abc"}};
  auto plan = PlanRestampSidecar(/*stored_flags=*/0, std::nullopt, r304);
  EXPECT_EQ(plan.action, RestampSidecarAction::kLegacy);
  EXPECT_TRUE(plan.stored_not_overridden.empty());
}

TEST(RestampSidecarPlanTest, ValidatorOnly304KeepsTheBlock) {
  const std::string stored = PayloadOf(SecurityHeaders());
  std::vector<HeaderField> r304 = {{"ETag", "\"v1\""},
                                   {"Cache-Control", "public, max-age=600"}};
  auto plan = PlanRestampSidecar(kClassified | kRequired, stored, r304);
  EXPECT_EQ(plan.action, RestampSidecarAction::kRestampOnly);
  EXPECT_EQ(plan.stored_not_overridden.size(), 4u);
}

TEST(RestampSidecarPlanTest, Repeated304LinesKeepTheBlock) {
  const std::string stored = PayloadOf(SecurityHeaders());
  auto plan =
      PlanRestampSidecar(kClassified | kRequired, stored, SecurityHeaders());
  EXPECT_EQ(plan.action, RestampSidecarAction::kRestampOnly);
  EXPECT_TRUE(plan.stored_not_overridden.empty());
}

TEST(RestampSidecarPlanTest, ChangedCarriedHeaderRewritesTheBlock) {
  const std::string stored = PayloadOf(SecurityHeaders());
  std::vector<HeaderField> r304 = {{"ETag", "\"v1\""},
                                   {"Referrer-Policy", "same-origin"}};
  auto plan = PlanRestampSidecar(kClassified | kRequired, stored, r304);
  ASSERT_EQ(plan.action, RestampSidecarAction::kRewriteThenRestamp);
  // The merged set carries the new value and every other stored line.
  bool new_value = false;
  for (const auto& f : plan.merged) {
    if (HeaderNameEquals(f.name, "referrer-policy")) {
      EXPECT_EQ(f.value, "same-origin");
      new_value = true;
    }
  }
  EXPECT_TRUE(new_value);
  EXPECT_EQ(plan.stored_not_overridden.size(), 3u);
  EXPECT_EQ(ClassifyForHeadersSidecar(plan.merged).fields, 4u);
}

TEST(RestampSidecarPlanTest, NothingToCarryStaysNothingToCarry) {
  std::vector<HeaderField> r304 = {{"ETag", "\"v1\""}, {"Age", "3"}};
  auto plan = PlanRestampSidecar(kClassified, std::nullopt, r304);
  EXPECT_EQ(plan.action, RestampSidecarAction::kRestampOnly);
}

TEST(RestampSidecarPlanTest, CarryStateSwitchRemoves) {
  // Stored with nothing to carry; the 304 starts sending ACAO.
  std::vector<HeaderField> r304 = {{"Access-Control-Allow-Origin", "*"}};
  auto plan = PlanRestampSidecar(kClassified, std::nullopt, r304);
  EXPECT_EQ(plan.action, RestampSidecarAction::kRemove);
}

TEST(RestampSidecarPlanTest, RequiredBlockMissingRemoves) {
  std::vector<HeaderField> r304 = {{"ETag", "\"v1\""}};
  auto plan = PlanRestampSidecar(kClassified | kRequired, std::nullopt, r304);
  EXPECT_EQ(plan.action, RestampSidecarAction::kRemove);
  EXPECT_TRUE(plan.stored_not_overridden.empty());
}

TEST(RestampSidecarPlanTest, UnparseableBlockRemoves) {
  std::vector<HeaderField> r304 = {{"ETag", "\"v1\""}};
  auto plan = PlanRestampSidecar(kClassified | kRequired,
                                 std::string_view("\xff garbage"), r304);
  EXPECT_EQ(plan.action, RestampSidecarAction::kRemove);
}

TEST(RestampSidecarPlanTest, UnreproducibleHeaderOn304Removes) {
  const std::string stored = PayloadOf(SecurityHeaders());
  std::vector<HeaderField> r304 = {{"X-Request-Id", "abc123"}};
  auto plan = PlanRestampSidecar(kClassified | kRequired, stored, r304);
  EXPECT_EQ(plan.action, RestampSidecarAction::kRemove);
  // The current response is still served with the stored lines it did not
  // override.
  EXPECT_EQ(plan.stored_not_overridden.size(), 4u);
}

TEST(RestampSidecarPlanTest, NonceCspOn304Removes) {
  const std::string stored = PayloadOf(SecurityHeaders());
  std::vector<HeaderField> r304 = {
      {"Content-Security-Policy", "script-src 'nonce-xyz'"}};
  auto plan = PlanRestampSidecar(kClassified | kRequired, stored, r304);
  EXPECT_EQ(plan.action, RestampSidecarAction::kRemove);
}

}  // namespace
}  // namespace pagespeed
