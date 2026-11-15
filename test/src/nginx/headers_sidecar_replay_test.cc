// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Serve-side response-header sidecar decisions for the nginx front stage
// (issue #208): which hits read the stored block, the replay plan (order,
// Vary precedence, missing block → miss), the legacy-entry migration rule,
// the stale-if-error rules, and a record → serve round trip against a REAL
// cache on a temp volume.

#include "src/nginx/headers_sidecar_replay.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "gtest/gtest.h"
#include "lib/cache/cache.h"
#include "lib/cache/headers_sidecar.h"
#include "lib/classify/alternate_id.h"
#include "lib/classify/alternate_metadata.h"
#include "src/nginx/headers_sidecar_record.h"
#include "test/test_util/temp_dir.h"

namespace pagespeed {
namespace {

constexpr uint8_t kClassified = AlternateMetadata::kFlagHeadersClassified;
constexpr uint8_t kRequired = AlternateMetadata::kFlagHeadersSidecarRequired;
constexpr uint8_t kBoth = kClassified | kRequired;
// Unrelated bits that must not change any answer here.
constexpr uint8_t kOther = AlternateMetadata::kFlagWorkerProcessed |
                           AlternateMetadata::kFlagOriginVariesAccept;

// The verification origin's header set: four carried security lines and a
// Vary, interleaved with lines the entry metadata reproduces.
std::vector<HeaderField> OriginHeaders() {
  return {
      {"Cache-Control", "public, max-age=600"},
      {"Access-Control-Allow-Origin", "*"},
      {"ETag", "\"v1\""},
      {"Content-Security-Policy", "default-src 'self'"},
      {"X-Content-Type-Options", "nosniff"},
      {"Vary", "Accept-Encoding"},
      {"Referrer-Policy", "no-referrer"},
  };
}

// What the MISS relays of the carried names, in received order.
std::vector<HeaderField> CarriedOnMiss(const std::vector<HeaderField>& origin) {
  std::vector<HeaderField> out;
  for (const auto& f : origin) {
    if (IsSidecarCarriedName(f.name)) out.push_back(f);
  }
  return out;
}

std::string PayloadOf(const std::vector<HeaderField>& fields) {
  return ClassifyForHeadersSidecar(fields).payload;
}

// A block in the stored format that the gate would never produce (for the
// "did not come from the gate" checks).  The version byte is taken from a
// real block so this does not hard-code the format version.
std::string EncodeUnchecked(const std::vector<HeaderField>& fields) {
  const std::string real = PayloadOf({{"Referrer-Policy", "no-referrer"}});
  std::string out;
  out.push_back(real[0]);
  out.push_back(static_cast<char>((fields.size() >> 8) & 0xFF));
  out.push_back(static_cast<char>(fields.size() & 0xFF));
  for (const auto& f : fields) {
    out.push_back(static_cast<char>(f.name.size()));
    out.push_back(static_cast<char>((f.value.size() >> 8) & 0xFF));
    out.push_back(static_cast<char>(f.value.size() & 0xFF));
    out.append(f.name);
    out.append(f.value);
  }
  return out;
}

void ExpectSameFields(const std::vector<HeaderField>& got,
                      const std::vector<HeaderField>& want) {
  ASSERT_EQ(got.size(), want.size());
  for (size_t i = 0; i < got.size(); ++i) {
    // Verbatim: name case and value bytes as the origin sent them.
    EXPECT_EQ(got[i].name, want[i].name) << i;
    EXPECT_EQ(got[i].value, want[i].value) << i;
  }
}

// --- Which hits read the block -------------------------------------------

TEST(HeadersSidecarReplayTest, OnlyClassifiedAndRequiredEntriesRead) {
  EXPECT_FALSE(SidecarReadRequired(0, false));
  EXPECT_FALSE(SidecarReadRequired(kOther, false));
  EXPECT_FALSE(SidecarReadRequired(kClassified, false));
  EXPECT_FALSE(SidecarReadRequired(kClassified | kOther, false));
  // Required without classified is not a state the record arm writes; it is
  // treated like an unmarked entry rather than trusted.
  EXPECT_FALSE(SidecarReadRequired(kRequired, false));
  EXPECT_TRUE(SidecarReadRequired(kBoth, false));
  EXPECT_TRUE(SidecarReadRequired(kBoth | kOther, false));
}

TEST(HeadersSidecarReplayTest, AgentMarkdownIsNeverReplayedOnto) {
  EXPECT_FALSE(SidecarReadRequired(kBoth, true));
  const std::string payload = PayloadOf(OriginHeaders());
  auto plan = PlanSidecarServe(kBoth, true, payload);
  EXPECT_EQ(plan.outcome, SidecarServeOutcome::kNotRequired);
  EXPECT_TRUE(plan.fields.empty());
  // Not even a missing block turns it into a miss.
  EXPECT_EQ(PlanSidecarServe(kBoth, true, std::nullopt).outcome,
            SidecarServeOutcome::kNotRequired);
}

// --- The replay plan -----------------------------------------------------

TEST(HeadersSidecarReplayTest, ReplaysStoredLinesVerbatimInOrder) {
  const auto origin = OriginHeaders();
  const std::string payload = PayloadOf(origin);
  auto plan = PlanSidecarServe(kBoth, false, payload);
  ASSERT_EQ(plan.outcome, SidecarServeOutcome::kReplay);
  ExpectSameFields(plan.fields, CarriedOnMiss(origin));
  EXPECT_TRUE(plan.carried_vary);
  EXPECT_TRUE(plan.carried_nosniff);
}

TEST(HeadersSidecarReplayTest, RepeatedLinesStaySeparateAndOrdered) {
  const std::vector<HeaderField> origin = {
      {"content-security-policy", "default-src 'self'"},
      {"Cache-Control", "max-age=60"},
      {"Content-Security-Policy", "img-src https:"},
      {"Vary", "Accept-Encoding"},
      {"vary", "accept-encoding"},
  };
  const std::string payload = PayloadOf(origin);
  auto plan = PlanSidecarServe(kBoth, false, payload);
  ASSERT_EQ(plan.outcome, SidecarServeOutcome::kReplay);
  ExpectSameFields(plan.fields, CarriedOnMiss(origin));
  EXPECT_TRUE(plan.carried_vary);
  EXPECT_FALSE(plan.carried_nosniff);
}

TEST(HeadersSidecarReplayTest, MissingBlockIsAMiss) {
  auto plan = PlanSidecarServe(kBoth, false, std::nullopt);
  EXPECT_EQ(plan.outcome, SidecarServeOutcome::kMissing);
  EXPECT_TRUE(plan.fields.empty());
}

TEST(HeadersSidecarReplayTest, UnparseableOrEmptyBlockIsAMiss) {
  EXPECT_EQ(PlanSidecarServe(kBoth, false, std::string_view()).outcome,
            SidecarServeOutcome::kMissing);
  EXPECT_EQ(PlanSidecarServe(kBoth, false, std::string_view("\x7f\x00\x01", 3))
                .outcome,
            SidecarServeOutcome::kMissing);
  std::string truncated = PayloadOf(OriginHeaders());
  truncated.pop_back();
  EXPECT_EQ(PlanSidecarServe(kBoth, false, truncated).outcome,
            SidecarServeOutcome::kMissing);
  // A well-formed block with no lines is not "nothing to carry" (that is the
  // classified-only state, which stores no block): it cannot be served with.
  EXPECT_EQ(PlanSidecarServe(kBoth, false, EncodeUnchecked({})).outcome,
            SidecarServeOutcome::kMissing);
}

TEST(HeadersSidecarReplayTest, UnclassifiedAndNothingToCarryEntriesUntouched) {
  const std::string payload = PayloadOf(OriginHeaders());
  for (uint8_t flags : {uint8_t{0}, kOther, kClassified,
                        static_cast<uint8_t>(kClassified | kOther)}) {
    // Whatever is (or is not) stored for the URL, these serve as before.
    auto with_block = PlanSidecarServe(flags, false, payload);
    EXPECT_EQ(with_block.outcome, SidecarServeOutcome::kNotRequired) << +flags;
    EXPECT_TRUE(with_block.fields.empty());
    EXPECT_FALSE(with_block.carried_vary);
    EXPECT_FALSE(with_block.carried_nosniff);
    EXPECT_EQ(PlanSidecarServe(flags, false, std::nullopt).outcome,
              SidecarServeOutcome::kNotRequired)
        << +flags;
  }
}

// The gate never stores a name the serve path builds; a block that does
// was not written by it.  Every build refuses it (miss, logged by the caller,
// and the record arm rewrites the block) — never an abort.
TEST(HeadersSidecarReplayTest, ServePathNamesNeverComeFromTheBlock) {
  for (std::string_view name :
       {"Cache-Control", "etag", "Last-Modified", "Expires", "Content-Type",
        "Content-Length", "Date", "Age", "Server", "Accept-Ranges"}) {
    EXPECT_TRUE(SidecarFieldCollidesWithServePath(name)) << name;
  }
  for (std::string_view name :
       {"Vary", "Access-Control-Allow-Origin", "Content-Security-Policy",
        "X-Content-Type-Options", "Referrer-Policy"}) {
    EXPECT_FALSE(SidecarFieldCollidesWithServePath(name)) << name;
  }
  // The gate's own output never collides.
  // The plan's fields are views into the payload, so the payload must outlive
  // every use of the plan: keep it in a named object, not a temporary.
  const std::string payload = PayloadOf(OriginHeaders());
  auto plan = PlanSidecarServe(kBoth, false, payload);
  ASSERT_EQ(plan.outcome, SidecarServeOutcome::kReplay);
  for (const auto& f : plan.fields) {
    EXPECT_FALSE(SidecarFieldCollidesWithServePath(f.name)) << f.name;
  }
}

TEST(HeadersSidecarReplayTest, ForeignBlockWithServePathNameIsAMiss) {
  const std::string foreign = EncodeUnchecked(
      {{"Referrer-Policy", "no-referrer"}, {"Cache-Control", "max-age=1"}});
  auto plan = PlanSidecarServe(kBoth, false, foreign);
  EXPECT_EQ(plan.outcome, SidecarServeOutcome::kMissing);
  EXPECT_TRUE(plan.foreign_field);
  EXPECT_TRUE(plan.fields.empty());
  EXPECT_FALSE(plan.carried_vary);
  // An ordinary missing block is not flagged foreign.
  EXPECT_FALSE(PlanSidecarServe(kBoth, false, std::nullopt).foreign_field);
}

// --- Vary precedence -----------------------------------------------------
// The rule for naming Accept after a replayed origin Vary line is
// pagespeed::EmittedVaryNamesAccept (test/lib/cache/vary_emission_test.cc).

// --- Legacy-entry migration --------------------------------------------------

TEST(HeadersSidecarReplayTest, LegacyEntryFullFetchesOnceAtExpiry) {
  // Unclassified + genuinely expired: no conditional request, full fetch.
  EXPECT_FALSE(ConditionalRevalidationAllowed(0, true));
  EXPECT_FALSE(ConditionalRevalidationAllowed(kOther, true));
  // Not an expiry (client force-refresh, origin no-cache): unchanged.
  EXPECT_TRUE(ConditionalRevalidationAllowed(0, false));
  // Classified entries revalidate conditionally as before.
  EXPECT_TRUE(ConditionalRevalidationAllowed(kClassified, true));
  EXPECT_TRUE(ConditionalRevalidationAllowed(kBoth, true));
  EXPECT_TRUE(ConditionalRevalidationAllowed(kBoth | kOther, false));
}

// --- Stale-if-error ------------------------------------------------------

TEST(HeadersSidecarReplayTest, StaleErrorServeNeedsTheBlockWhenRequired) {
  EXPECT_TRUE(StaleErrorServeHasHeaders(kBoth, true));
  // Fail closed: the upstream error passes through.
  EXPECT_FALSE(StaleErrorServeHasHeaders(kBoth, false));
  // Entries that carry no block serve stale as before.
  EXPECT_TRUE(StaleErrorServeHasHeaders(0, false));
  EXPECT_TRUE(StaleErrorServeHasHeaders(kClassified, false));
}

TEST(HeadersSidecarReplayTest, StaleErrorDropsErrorPageLinesForClassified) {
  // Classified with or without a block: the error page's CSP/ACAO/Vary never
  // reach the client with the cached body.
  EXPECT_TRUE(StaleErrorDropsCarriedNames(kClassified));
  EXPECT_TRUE(StaleErrorDropsCarriedNames(kBoth | kOther));
  // Recorded before the gate: unchanged.
  EXPECT_FALSE(StaleErrorDropsCarriedNames(0));
  EXPECT_FALSE(StaleErrorDropsCarriedNames(kOther));
}

TEST(HeadersSidecarReplayTest, CarriedNamesAreTheAllowlist) {
  for (std::string_view name :
       {"Content-Security-Policy", "access-control-allow-origin",
        "X-Content-Type-Options", "Referrer-Policy", "Vary",
        "Cross-Origin-Resource-Policy", "Permissions-Policy", "X-Powered-By",
        "Link", "Strict-Transport-Security"}) {
    EXPECT_TRUE(IsSidecarCarriedName(name)) << name;
  }
  for (std::string_view name :
       {"Cache-Control", "Server", "X-Request-Id", "Set-Cookie",
        "Content-Security-Policy-Report-Only"}) {
    EXPECT_FALSE(IsSidecarCarriedName(name)) << name;
  }
}

TEST(HeadersSidecarReplayTest, LinkHintCarriedByTheOriginsLink) {
  const std::vector<HeaderField> fields = {
      {"Vary", "Accept"}, {"Link", "</style.css>; rel=preload; as=style"}};
  EXPECT_TRUE(SidecarLinkCarriesHint(fields, "/style.css", "preload", false));
  EXPECT_FALSE(SidecarLinkCarriesHint(fields, "/other.css", "preload", false));
  EXPECT_FALSE(SidecarLinkCarriesHint(fields, "/style.cs", "preload", false));
}

TEST(HeadersSidecarReplayTest, LinkHintIgnoresOtherFieldsAndMatchesAnyValue) {
  const std::vector<HeaderField> other = {{"X-Pingback", "</style.css>"},
                                          {"Vary", "</style.css>"}};
  EXPECT_FALSE(SidecarLinkCarriesHint(other, "/style.css", "preload", false));
  const std::vector<HeaderField> many = {
      {"link",
       "</a.css>; rel=preload; as=style, </b.js>; rel=preload, "
       "<https://x.example/c?a=1,2>; rel=\"preconnect\""},
      {"LINK", "</d.css>; REL=Preload"}};
  EXPECT_TRUE(SidecarLinkCarriesHint(many, "/a.css", "preload", false));
  EXPECT_TRUE(SidecarLinkCarriesHint(many, "/b.js", "preload", false));
  EXPECT_TRUE(SidecarLinkCarriesHint(many, "https://x.example/c?a=1,2",
                                     "preconnect", false));
  EXPECT_TRUE(SidecarLinkCarriesHint(many, "/d.css", "preload", false));
  EXPECT_FALSE(SidecarLinkCarriesHint(many, "/e.css", "preload", false));
  EXPECT_FALSE(SidecarLinkCarriesHint({}, "/a.css", "preload", false));
}

TEST(HeadersSidecarReplayTest, LinkHintNeedsTheSameRelAndCrossorigin) {
  const std::vector<HeaderField> fields = {
      {"Link", "<https://fonts.gstatic.com>; rel=preconnect"},
      {"Link", "</f.woff2>; rel=\"preload prefetch\"; as=font; crossorigin"}};
  EXPECT_TRUE(SidecarLinkCarriesHint(fields, "https://fonts.gstatic.com",
                                     "preconnect", false));
  // Same target, different rel.
  EXPECT_FALSE(SidecarLinkCarriesHint(fields, "https://fonts.gstatic.com",
                                      "preload", false));
  // Same target and rel, crossorigin mismatch (both directions).
  EXPECT_FALSE(SidecarLinkCarriesHint(fields, "https://fonts.gstatic.com",
                                      "preconnect", true));
  EXPECT_TRUE(SidecarLinkCarriesHint(fields, "/f.woff2", "preload", true));
  EXPECT_TRUE(SidecarLinkCarriesHint(fields, "/f.woff2", "prefetch", true));
  EXPECT_FALSE(SidecarLinkCarriesHint(fields, "/f.woff2", "preload", false));
}

// --- Record → serve against a real cache -----------------------------------

constexpr std::string_view kUrl = "/sidecar/asset.css";
constexpr std::string_view kHost = "example.com";
constexpr std::string_view kScheme = "https";
constexpr std::string_view kBody = "body{color:green}";
constexpr uint8_t kIdentityMask = 0x08;

class HeadersSidecarRoundTripTest : public ::testing::Test {
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

  // Record the identity the way the record arm does.
  void Record(const std::vector<HeaderField>& origin) {
    AlternateMetadata meta;
    meta.full_mask = kIdentityMask;
    meta.content_type = ContentType::kCss;
    meta.cache_inserted_at = 1000;
    auto result = RecordEntryWithHeadersSidecar(
        *cache_, kUrl, kHost, kScheme, MaskToAlternateId(kIdentityMask), meta,
        std::as_bytes(std::span(kBody)), origin, StoredIdentityState{},
        /*force_rewrite=*/false, [] { return true; });
    ASSERT_EQ(result.outcome, SidecarRecordOutcome::kWritten);
  }

  // What the handler does on a hit: read the entry, then the block by key
  // (when the entry requires it), and plan the serve.  `block_storage` keeps
  // the payload bytes alive for the plan's views.
  SidecarServePlan Serve(std::string& block_storage) {
    const auto key =
        PageSpeedCache::ComposeKeyPreNormalized(kUrl, kHost, kScheme);
    auto entry =
        cache_->ReadAlternateByKey(key, MaskToAlternateId(kIdentityMask));
    EXPECT_TRUE(entry.has_value() && entry->is_valid());
    const uint8_t flags = entry.has_value() ? entry->metadata.flags : 0;
    std::optional<std::string_view> payload;
    if (SidecarReadRequired(flags, false)) {
      auto block = cache_->ReadHeadersSidecarByKey(key);
      if (block.has_value() && block->is_valid()) {
        block_storage =
            std::string(PageSpeedCache::HeadersSidecarPayload(*block));
        payload = block_storage;
      }
    }
    return PlanSidecarServe(flags, false, payload);
  }

  std::string cache_dir_;
  std::unique_ptr<PageSpeedCache> cache_;
};

TEST_F(HeadersSidecarRoundTripTest, HitReplaysWhatTheMissRelayed) {
  const auto origin = OriginHeaders();
  Record(origin);
  std::string storage;
  auto plan = Serve(storage);
  ASSERT_EQ(plan.outcome, SidecarServeOutcome::kReplay);
  ExpectSameFields(plan.fields, CarriedOnMiss(origin));
}

TEST_F(HeadersSidecarRoundTripTest, BlockDeletedBehindTheEntryIsAMiss) {
  Record(OriginHeaders());
  ASSERT_TRUE(
      cache_->RemoveAlternate(kUrl, kHost, kScheme, kHeadersSidecarAlternateId)
          .has_value());
  std::string storage;
  EXPECT_EQ(Serve(storage).outcome, SidecarServeOutcome::kMissing);
}

TEST_F(HeadersSidecarRoundTripTest, NothingToCarryReadsNothing) {
  Record({{"Cache-Control", "public, max-age=600"}, {"ETag", "\"v1\""}});
  std::string storage;
  auto plan = Serve(storage);
  EXPECT_EQ(plan.outcome, SidecarServeOutcome::kNotRequired);
  EXPECT_TRUE(storage.empty());
}

}  // namespace
}  // namespace pagespeed
