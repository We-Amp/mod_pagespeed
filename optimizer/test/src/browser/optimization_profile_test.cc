// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Optimization Profile Tests

#include "src/browser/optimization_profile.h"

#include <cstdint>
#include <string>

#include "gtest/gtest.h"
#include "lib/classify/capability_mask.h"

namespace pagespeed {
namespace {

OptimizationProfile MakeTestProfile() {
  OptimizationProfile profile;
  profile.template_hash_hex = "0123456789abcdef";
  profile.analyzed_url = "https://example.com/product/1";
  profile.created_at = 1700000000;
  profile.expires_at = 1700086400;

  // Mobile viewport.
  profile.mobile.critical_css = "body{margin:0}";
  profile.mobile.lcp_selector = "main > img.hero";
  profile.mobile.lcp_url = "https://cdn.example.com/hero.webp";
  profile.mobile.above_fold_selectors = {"header",
                                         "main > section:first-child"};
  profile.mobile.below_fold_selectors = {"footer", "aside"};
  profile.mobile.image_dimensions = {
      {"img.hero", 375, 200, 1500, 800},
      {"img.thumb", 100, 100, 400, 400},
  };

  // Desktop viewport.
  profile.desktop.critical_css = "body{margin:0} .sidebar{display:block}";
  profile.desktop.lcp_selector = "main > img.hero";
  profile.desktop.lcp_url = "https://cdn.example.com/hero-large.webp";
  profile.desktop.above_fold_selectors = {"header", "main", ".sidebar"};

  // Preload hints.
  profile.preload_hints = {
      {"https://cdn.example.com/hero.webp", "image", "image/webp"},
      {"https://cdn.example.com/style.css", "style", ""},
  };

  // Defer-safe scripts.
  profile.defer_safe_scripts = {"analytics.js", "tracking.js"};

  return profile;
}

TEST(OptimizationProfileTest, RoundTripJson) {
  auto original = MakeTestProfile();
  std::string json = original.ToJson();
  auto result = OptimizationProfile::FromJson(json);
  ASSERT_TRUE(result.ok()) << result.status().message();

  const auto& restored = result.value();
  EXPECT_EQ(restored.template_hash_hex, original.template_hash_hex);
  EXPECT_EQ(restored.analyzed_url, original.analyzed_url);
  EXPECT_EQ(restored.created_at, original.created_at);
  EXPECT_EQ(restored.expires_at, original.expires_at);
}

TEST(OptimizationProfileTest, MobileViewportRoundTrip) {
  auto original = MakeTestProfile();
  std::string json = original.ToJson();
  auto result = OptimizationProfile::FromJson(json);
  ASSERT_TRUE(result.ok());

  const auto& mobile = result->mobile;
  EXPECT_EQ(mobile.critical_css, "body{margin:0}");
  EXPECT_EQ(mobile.lcp_selector, "main > img.hero");
  EXPECT_EQ(mobile.lcp_url, "https://cdn.example.com/hero.webp");
  EXPECT_EQ(mobile.above_fold_selectors.size(), 2u);
  EXPECT_EQ(mobile.below_fold_selectors.size(), 2u);
  EXPECT_EQ(mobile.image_dimensions.size(), 2u);

  EXPECT_EQ(mobile.image_dimensions[0].selector, "img.hero");
  EXPECT_EQ(mobile.image_dimensions[0].rendered_width, 375u);
  EXPECT_EQ(mobile.image_dimensions[0].rendered_height, 200u);
  EXPECT_EQ(mobile.image_dimensions[0].natural_width, 1500u);
  EXPECT_EQ(mobile.image_dimensions[0].natural_height, 800u);
}

TEST(OptimizationProfileTest, DesktopViewportRoundTrip) {
  auto original = MakeTestProfile();
  std::string json = original.ToJson();
  auto result = OptimizationProfile::FromJson(json);
  ASSERT_TRUE(result.ok());

  const auto& desktop = result->desktop;
  EXPECT_EQ(desktop.critical_css, "body{margin:0} .sidebar{display:block}");
  EXPECT_EQ(desktop.above_fold_selectors.size(), 3u);
}

TEST(OptimizationProfileTest, EmptyTabletViewport) {
  auto original = MakeTestProfile();
  std::string json = original.ToJson();
  auto result = OptimizationProfile::FromJson(json);
  ASSERT_TRUE(result.ok());

  // Tablet was not populated.
  EXPECT_TRUE(result->tablet.critical_css.empty());
  EXPECT_TRUE(result->tablet.lcp_selector.empty());
  EXPECT_TRUE(result->tablet.image_dimensions.empty());
}

TEST(OptimizationProfileTest, PreloadHintsRoundTrip) {
  auto original = MakeTestProfile();
  std::string json = original.ToJson();
  auto result = OptimizationProfile::FromJson(json);
  ASSERT_TRUE(result.ok());

  ASSERT_EQ(result->preload_hints.size(), 2u);
  EXPECT_EQ(result->preload_hints[0].url, "https://cdn.example.com/hero.webp");
  EXPECT_EQ(result->preload_hints[0].as, "image");
  EXPECT_EQ(result->preload_hints[0].type, "image/webp");
  EXPECT_EQ(result->preload_hints[1].url, "https://cdn.example.com/style.css");
  EXPECT_EQ(result->preload_hints[1].as, "style");
  EXPECT_TRUE(result->preload_hints[1].type.empty());
}

TEST(OptimizationProfileTest, DeferSafeScriptsRoundTrip) {
  auto original = MakeTestProfile();
  std::string json = original.ToJson();
  auto result = OptimizationProfile::FromJson(json);
  ASSERT_TRUE(result.ok());

  ASSERT_EQ(result->defer_safe_scripts.size(), 2u);
  EXPECT_EQ(result->defer_safe_scripts[0], "analytics.js");
  EXPECT_EQ(result->defer_safe_scripts[1], "tracking.js");
}

TEST(OptimizationProfileTest, InvalidJsonReturnsError) {
  auto result = OptimizationProfile::FromJson("not json");
  EXPECT_FALSE(result.ok());
  EXPECT_TRUE(absl::IsInvalidArgument(result.status()));
}

TEST(OptimizationProfileTest, NonObjectJsonReturnsError) {
  auto result = OptimizationProfile::FromJson("[1, 2, 3]");
  EXPECT_FALSE(result.ok());
}

TEST(OptimizationProfileTest, WrongVersionReturnsError) {
  auto result = OptimizationProfile::FromJson(
      R"({"version": 99, "template_hash": "abc"})");
  EXPECT_FALSE(result.ok());
  EXPECT_TRUE(absl::IsInvalidArgument(result.status()));
}

TEST(OptimizationProfileTest, EmptyProfileRoundTrip) {
  OptimizationProfile empty;
  std::string json = empty.ToJson();
  auto result = OptimizationProfile::FromJson(json);
  ASSERT_TRUE(result.ok());
  EXPECT_TRUE(result->template_hash_hex.empty());
  EXPECT_TRUE(result->analyzed_url.empty());
  EXPECT_TRUE(result->mobile.critical_css.empty());
  EXPECT_EQ(result->created_at, 0);
}

TEST(OptimizationProfileTest, CacheUrlFormat) {
  std::string url = OptimizationProfile::CacheUrl(0x0123456789ABCDEF);
  EXPECT_EQ(url, "__pagespeed_profile__/0123456789abcdef");
}

TEST(OptimizationProfileTest, CacheUrlZero) {
  std::string url = OptimizationProfile::CacheUrl(0);
  EXPECT_EQ(url, "__pagespeed_profile__/0000000000000000");
}

TEST(OptimizationProfileTest, ProfileHostname) {
  EXPECT_EQ(OptimizationProfile::kProfileHostname, "__internal__");
}

TEST(OptimizationProfileTest, LargeJsonRoundTrip) {
  OptimizationProfile profile;
  profile.template_hash_hex = "deadbeef";
  profile.analyzed_url = "https://example.com/";
  profile.created_at = 1700000000;
  profile.expires_at = 1700086400;

  // Large critical CSS (typical real-world size).
  std::string large_css(50000, 'x');
  profile.mobile.critical_css = large_css;
  profile.desktop.critical_css = large_css;

  // Many image dimensions.
  for (int i = 0; i < 100; ++i) {
    ImageDimension dim;
    dim.selector = "img:nth-child(" + std::to_string(i) + ")";
    dim.rendered_width = 100 + i;
    dim.rendered_height = 100 + i;
    dim.natural_width = 1000;
    dim.natural_height = 1000;
    profile.mobile.image_dimensions.push_back(dim);
  }

  std::string json = profile.ToJson();
  auto result = OptimizationProfile::FromJson(json);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->mobile.critical_css.size(), 50000u);
  EXPECT_EQ(result->mobile.image_dimensions.size(), 100u);
}

// --- Additional edge case tests ---

TEST(OptimizationProfileTest, Utf8InSelectors) {
  OptimizationProfile profile;
  profile.template_hash_hex = "utf8test";
  profile.analyzed_url = "https://example.com/\xc3\xa9\xc3\xa0\xc3\xbc";
  profile.mobile.lcp_selector = "main > img.\xc3\xa9l\xc3\xa9ment";
  profile.mobile.above_fold_selectors = {".caf\xc3\xa9",
                                         ".r\xc3\xa9sum\xc3\xa9"};
  profile.preload_hints = {
      {"https://cdn.example.com/\xc3\xb6sterreich.png", "image", ""},
  };
  profile.created_at = 1700000000;
  profile.expires_at = 1700086400;

  std::string json = profile.ToJson();
  auto result = OptimizationProfile::FromJson(json);
  ASSERT_TRUE(result.ok()) << result.status().message();

  EXPECT_EQ(result->analyzed_url,
            "https://example.com/\xc3\xa9\xc3\xa0\xc3\xbc");
  EXPECT_EQ(result->mobile.lcp_selector, "main > img.\xc3\xa9l\xc3\xa9ment");
  EXPECT_EQ(result->mobile.above_fold_selectors.size(), 2u);
  EXPECT_EQ(result->preload_hints[0].url,
            "https://cdn.example.com/\xc3\xb6sterreich.png");
}

TEST(OptimizationProfileTest, MissingVersionDefaultsToZero) {
  // JSON without a version field should default to 0 and fail.
  auto result = OptimizationProfile::FromJson(R"({"template_hash": "abc"})");
  EXPECT_FALSE(result.ok());
  EXPECT_TRUE(absl::IsInvalidArgument(result.status()));
}

TEST(OptimizationProfileTest, VersionZeroReturnsError) {
  auto result = OptimizationProfile::FromJson(
      R"({"version": 0, "template_hash": "abc"})");
  EXPECT_FALSE(result.ok());
}

TEST(OptimizationProfileTest, NegativeVersionReturnsError) {
  auto result = OptimizationProfile::FromJson(
      R"({"version": -1, "template_hash": "abc"})");
  EXPECT_FALSE(result.ok());
}

TEST(OptimizationProfileTest, EmptyJsonStringReturnsError) {
  auto result = OptimizationProfile::FromJson("");
  EXPECT_FALSE(result.ok());
}

TEST(OptimizationProfileTest, MissingOptionalFieldsRoundTrip) {
  // A profile with only version set, all other fields missing from JSON.
  auto result = OptimizationProfile::FromJson(R"({"version": 1})");
  ASSERT_TRUE(result.ok());
  EXPECT_TRUE(result->template_hash_hex.empty());
  EXPECT_TRUE(result->analyzed_url.empty());
  EXPECT_TRUE(result->mobile.critical_css.empty());
  EXPECT_TRUE(result->preload_hints.empty());
  EXPECT_TRUE(result->defer_safe_scripts.empty());
  EXPECT_EQ(result->created_at, 0);
  EXPECT_EQ(result->expires_at, 0);
}

TEST(OptimizationProfileTest, AllViewportsFilled) {
  OptimizationProfile profile;
  profile.template_hash_hex = "full";
  profile.analyzed_url = "https://example.com/";
  profile.created_at = 1700000000;
  profile.expires_at = 1700086400;

  profile.mobile.critical_css = "body{margin:0}";
  profile.mobile.lcp_selector = "img.hero-mobile";
  profile.tablet.critical_css = "body{margin:0;padding:10px}";
  profile.tablet.lcp_selector = "img.hero-tablet";
  profile.desktop.critical_css = "body{margin:0}.sidebar{display:block}";
  profile.desktop.lcp_selector = "img.hero-desktop";
  profile.desktop.lcp_url = "https://cdn.example.com/hero.jpg";

  // Add image dimensions to each viewport.
  profile.mobile.image_dimensions = {{"img.hero-mobile", 375, 200, 750, 400}};
  profile.tablet.image_dimensions = {{"img.hero-tablet", 768, 400, 1536, 800}};
  profile.desktop.image_dimensions = {
      {"img.hero-desktop", 1440, 600, 2880, 1200}};

  std::string json = profile.ToJson();
  auto result = OptimizationProfile::FromJson(json);
  ASSERT_TRUE(result.ok());

  EXPECT_EQ(result->mobile.lcp_selector, "img.hero-mobile");
  EXPECT_EQ(result->tablet.lcp_selector, "img.hero-tablet");
  EXPECT_EQ(result->desktop.lcp_selector, "img.hero-desktop");
  EXPECT_EQ(result->mobile.image_dimensions.size(), 1u);
  EXPECT_EQ(result->tablet.image_dimensions.size(), 1u);
  EXPECT_EQ(result->desktop.image_dimensions.size(), 1u);
}

TEST(OptimizationProfileTest, CacheUrlMaxValue) {
  std::string url = OptimizationProfile::CacheUrl(UINT64_MAX);
  EXPECT_EQ(url, "__pagespeed_profile__/ffffffffffffffff");
}

TEST(OptimizationProfileTest, PreloadHintWithTypeOmitted) {
  // PreloadHint with empty type should round-trip correctly.
  OptimizationProfile profile;
  profile.template_hash_hex = "hint";
  profile.analyzed_url = "https://example.com/";
  profile.created_at = 1700000000;
  profile.expires_at = 1700086400;
  profile.preload_hints = {
      {"https://cdn.example.com/style.css", "style", ""},
  };

  std::string json = profile.ToJson();
  auto result = OptimizationProfile::FromJson(json);
  ASSERT_TRUE(result.ok());
  ASSERT_EQ(result->preload_hints.size(), 1u);
  EXPECT_TRUE(result->preload_hints[0].type.empty());
}

TEST(OptimizationProfileTest, PreloadHintWithType) {
  OptimizationProfile profile;
  profile.template_hash_hex = "hint2";
  profile.analyzed_url = "https://example.com/";
  profile.created_at = 1700000000;
  profile.expires_at = 1700086400;
  profile.preload_hints = {
      {"https://cdn.example.com/font.woff2", "font", "font/woff2"},
  };

  std::string json = profile.ToJson();
  auto result = OptimizationProfile::FromJson(json);
  ASSERT_TRUE(result.ok());
  ASSERT_EQ(result->preload_hints.size(), 1u);
  EXPECT_EQ(result->preload_hints[0].type, "font/woff2");
}

TEST(OptimizationProfileTest, BelowFoldSelectorsRoundTrip) {
  OptimizationProfile profile;
  profile.template_hash_hex = "belowfold";
  profile.analyzed_url = "https://example.com/";
  profile.created_at = 1700000000;
  profile.expires_at = 1700086400;
  profile.mobile.below_fold_selectors = {"footer", "aside", ".comments"};

  std::string json = profile.ToJson();
  auto result = OptimizationProfile::FromJson(json);
  ASSERT_TRUE(result.ok());
  ASSERT_EQ(result->mobile.below_fold_selectors.size(), 3u);
  EXPECT_EQ(result->mobile.below_fold_selectors[2], ".comments");
}

TEST(OptimizationProfileTest, LargeTimestampValues) {
  OptimizationProfile profile;
  profile.template_hash_hex = "ts";
  profile.analyzed_url = "https://example.com/";
  profile.created_at = INT64_MAX;
  profile.expires_at = INT64_MAX;

  std::string json = profile.ToJson();
  auto result = OptimizationProfile::FromJson(json);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->created_at, INT64_MAX);
  EXPECT_EQ(result->expires_at, INT64_MAX);
}

TEST(OptimizationProfileTest, CssCoverageStatsRoundTrip) {
  OptimizationProfile profile;
  profile.template_hash_hex = "csstest";
  profile.analyzed_url = "https://example.com/";
  profile.created_at = 1700000000;
  profile.expires_at = 1700086400;
  profile.mobile.css_coverage_ratio = 0.42f;
  profile.mobile.total_css_bytes = 10000;
  profile.mobile.unused_css_bytes = 5800;
  profile.desktop.css_coverage_ratio = 0.65f;
  profile.desktop.total_css_bytes = 12000;
  profile.desktop.unused_css_bytes = 4200;

  std::string json = profile.ToJson();
  auto result = OptimizationProfile::FromJson(json);
  ASSERT_TRUE(result.ok());

  EXPECT_FLOAT_EQ(result->mobile.css_coverage_ratio, 0.42f);
  EXPECT_EQ(result->mobile.total_css_bytes, 10000u);
  EXPECT_EQ(result->mobile.unused_css_bytes, 5800u);
  EXPECT_FLOAT_EQ(result->desktop.css_coverage_ratio, 0.65f);
  EXPECT_EQ(result->desktop.total_css_bytes, 12000u);
  EXPECT_EQ(result->desktop.unused_css_bytes, 4200u);
  // Tablet defaults to zero.
  EXPECT_FLOAT_EQ(result->tablet.css_coverage_ratio, 0.0f);
  EXPECT_EQ(result->tablet.total_css_bytes, 0u);
  EXPECT_EQ(result->tablet.unused_css_bytes, 0u);
}

// --- Empirical-validation fields (the async-CSS deferral gate) ---

TEST(OptimizationProfileTest, RoundTripsValidationFields) {
  OptimizationProfile profile;
  profile.template_hash_hex = "validated";
  profile.analyzed_url = "https://example.com/";
  profile.created_at = 1700000000;
  profile.expires_at = 1700086400;
  profile.mobile.critical_css = "body{margin:0}";
  profile.mobile.critical_css_validated = true;
  profile.mobile.validation_diff_ratio = 0.0021f;
  profile.mobile.validated_critical_css_hash = std::string(64, 'a');
  profile.mobile.validated_combined_css_hash = std::string(64, 'b');

  std::string json = profile.ToJson();
  auto result = OptimizationProfile::FromJson(json);
  ASSERT_TRUE(result.ok()) << result.status().message();

  EXPECT_TRUE(result->mobile.critical_css_validated);
  EXPECT_FLOAT_EQ(result->mobile.validation_diff_ratio, 0.0021f);
  EXPECT_EQ(result->mobile.validated_critical_css_hash, std::string(64, 'a'));
  EXPECT_EQ(result->mobile.validated_combined_css_hash, std::string(64, 'b'));

  // An unpopulated viewport carries the safe default, not the sibling's bit.
  EXPECT_FALSE(result->desktop.critical_css_validated);
  EXPECT_FLOAT_EQ(result->desktop.validation_diff_ratio, -1.0f);
  EXPECT_TRUE(result->desktop.validated_combined_css_hash.empty());
}

// M8: a worker restart makes on-disk profiles unreachable through the
// in-memory template detector, so the "legacy profile after an upgrade"
// scenario is exercised at the JSON layer — where it is actually reachable —
// rather than as a cross-restart worker test.
TEST(OptimizationProfileTest, MissingValidationFieldsDefaultToUnvalidated) {
  // A version-1 document written before the validation fields existed.
  auto result = OptimizationProfile::FromJson(
      R"({"version": 1, "template_hash": "legacy",
          "mobile": {"critical_css": "body{margin:0}",
                     "css_coverage_ratio": 0.2}})");
  ASSERT_TRUE(result.ok()) << result.status().message();

  EXPECT_EQ(result->mobile.critical_css, "body{margin:0}");
  EXPECT_FALSE(result->mobile.critical_css_validated)
      << "a profile with no validation record must never read back as "
         "validated";
  EXPECT_FLOAT_EQ(result->mobile.validation_diff_ratio, -1.0f);
  EXPECT_TRUE(result->mobile.validated_critical_css_hash.empty());
  EXPECT_TRUE(result->mobile.validated_combined_css_hash.empty());
}

// L7: the version stays 1. FromJson hard-rejects anything else, so bumping it
// would make every profile written by a newer worker unreadable by an older
// one AND unreadable by this one.
TEST(OptimizationProfileTest, ValidationFieldsDoNotBumpTheVersion) {
  OptimizationProfile profile;
  profile.mobile.critical_css_validated = true;
  profile.mobile.validated_combined_css_hash = "abc";
  std::string json = profile.ToJson();
  EXPECT_NE(json.find("\"version\":1"), std::string::npos) << json;
}

TEST(OptimizationProfileTest, CombinedCssHashIsStableAndContentSensitive) {
  const std::string css_a = ".hero{color:red}";
  const std::string css_b = ".hero{color:blue}";

  EXPECT_EQ(CombinedCssValidationHash(css_a), CombinedCssValidationHash(css_a));
  EXPECT_NE(CombinedCssValidationHash(css_a), CombinedCssValidationHash(css_b));
  // Hex-encoded SHA-256 — the primitive the cache already content-addresses
  // with (SentinelId::kContentHash).
  EXPECT_EQ(CombinedCssValidationHash(css_a).size(), 64u);
  EXPECT_EQ(
      CombinedCssValidationHash(css_a).find_first_not_of("0123456789abcdef"),
      std::string::npos);
}

// An empty sheet must never produce a hash that a stored value can match, or
// the M6(a) path (profile present, combined_css empty) would sail through the
// gate on a profile validated against an equally empty sheet.
TEST(OptimizationProfileTest, EmptyCombinedCssHashesToNothingMatchable) {
  EXPECT_TRUE(CombinedCssValidationHash("").empty());
  EXPECT_NE(CombinedCssValidationHash(""), CombinedCssValidationHash("x"));
}

// The binding is salted only for pages whose <noscript> content
// renders with scripts off, and the salted binding hashes differently.
TEST(ValidationBindingTest, SaltsOnlyPagesWhoseNoscriptRenders) {
  EXPECT_EQ(ValidationBindingFor("", false, "",
                                 CapabilityMask::Viewport::kDesktop, false),
            "");
  EXPECT_EQ(ValidationBindingFor("unproven", false, "",
                                 CapabilityMask::Viewport::kDesktop, false),
            "unproven");
  EXPECT_EQ(ValidationBindingFor("", true, "",
                                 CapabilityMask::Viewport::kDesktop, false),
            "noscript-render");
  EXPECT_EQ(ValidationBindingFor("proven @layer a;", true, "",
                                 CapabilityMask::Viewport::kDesktop, false),
            "proven @layer a; noscript-render");
  const std::string css = ".hero{color:red}";
  EXPECT_EQ(
      CombinedCssValidationHash(
          css, ValidationBindingFor("", false, "",
                                    CapabilityMask::Viewport::kDesktop, false)),
      CombinedCssValidationHash(css));
  EXPECT_NE(
      CombinedCssValidationHash(
          css, ValidationBindingFor("", true, "",
                                    CapabilityMask::Viewport::kDesktop, false)),
      CombinedCssValidationHash(css));
}

// A sheet that escapes a quote, a brace or a comma outside a
// string was misread before the fix, so its records are salted once; every
// other sheet, including one that escapes those characters only inside a
// string or a comment, keeps its binding.
TEST(ValidationBindingTest, SaltsOnlySheetsWithStructuralEscapes) {
  EXPECT_TRUE(CombinedCssHasStructuralEscape(".a\\'b{color:red}"));
  EXPECT_TRUE(CombinedCssHasStructuralEscape(".a\\\"b{}"));
  EXPECT_TRUE(CombinedCssHasStructuralEscape(".a\\{b{}"));
  EXPECT_TRUE(CombinedCssHasStructuralEscape(".a\\}b{}"));
  EXPECT_TRUE(CombinedCssHasStructuralEscape(".a\\,b{}"));
  EXPECT_TRUE(CombinedCssHasStructuralEscape(
      ".\\[\\&_svg\\:not\\(\\[class\\*\\=\\'size-\\'\\]\\)\\]\\:size-4 svg{}"));

  EXPECT_FALSE(CombinedCssHasStructuralEscape(""));
  EXPECT_FALSE(CombinedCssHasStructuralEscape(
      ".md\\:flex{display:flex}.w-\\[10px\\]{width:10px}.\\31 0{}"))
      << "Tailwind's ordinary escapes are not structural";
  EXPECT_FALSE(CombinedCssHasStructuralEscape(".a{content:\"it\\'s {,}\"}"))
      << "escapes inside a string were always read right";
  EXPECT_FALSE(CombinedCssHasStructuralEscape("/* .a\\'b */.c{}"));
  EXPECT_FALSE(CombinedCssHasStructuralEscape(".a\\\\{color:red}"))
      << "an escaped backslash, then a real brace";

  const std::string escaped = ".a\\'b{color:red}.c{color:blue}";
  EXPECT_EQ(ValidationBindingFor("", false, escaped,
                                 CapabilityMask::Viewport::kDesktop, false),
            "css-escape");
  EXPECT_EQ(ValidationBindingFor("proven @layer a;", true, escaped,
                                 CapabilityMask::Viewport::kDesktop, false),
            "proven @layer a; noscript-render css-escape");
  EXPECT_NE(CombinedCssValidationHash(
                escaped, ValidationBindingFor(
                             "", false, escaped,
                             CapabilityMask::Viewport::kDesktop, false)),
            CombinedCssValidationHash(escaped));
  const std::string plain = ".md\\:flex{display:flex}";
  EXPECT_EQ(ValidationBindingFor("unproven", false, plain,
                                 CapabilityMask::Viewport::kDesktop, false),
            "unproven");
  EXPECT_EQ(CombinedCssValidationHash(
                plain, ValidationBindingFor("", false, plain,
                                            CapabilityMask::Viewport::kDesktop,
                                            false)),
            CombinedCssValidationHash(plain));
}

// The tablet viewport's records are salted, so every one made
// while the tablet rendered as a desktop window mismatches once; desktop
// records are not. Touch emulation adds the touch salt to the tablet's as well.
TEST(ValidationBindingTest, SaltsTabletRecords) {
  EXPECT_EQ(ValidationBindingFor("", false, "",
                                 CapabilityMask::Viewport::kTablet, false),
            "tablet-mobile touch ua");
  EXPECT_EQ(ValidationBindingFor("proven @layer a;", true, "",
                                 CapabilityMask::Viewport::kTablet, false),
            "proven @layer a; noscript-render tablet-mobile touch ua");
  const std::string css = ".hero{color:red}";
  EXPECT_NE(
      CombinedCssValidationHash(
          css, ValidationBindingFor("", false, css,
                                    CapabilityMask::Viewport::kTablet, false)),
      CombinedCssValidationHash(css));
  EXPECT_EQ(
      CombinedCssValidationHash(
          css, ValidationBindingFor("", false, css,
                                    CapabilityMask::Viewport::kDesktop, false)),
      CombinedCssValidationHash(css));
}

// The mobile and tablet viewports' records are salted ("touch"),
// so every one made while their renders emulated no touch screen mismatches
// once and the page is validated again; desktop records keep their hash.
// User-Agent emulation adds "ua" after it (the phone's and tablet's user agent
// and the
// fresh window), for the same two classes only.
TEST(ValidationBindingTest, SaltsMobileAndTabletRecordsForTouch) {
  EXPECT_EQ(ValidationBindingFor("", false, "",
                                 CapabilityMask::Viewport::kMobile, false),
            "touch ua");
  EXPECT_EQ(ValidationBindingFor("unproven", false, "",
                                 CapabilityMask::Viewport::kMobile, false),
            "unproven touch ua");
  EXPECT_EQ(ValidationBindingFor("proven @layer a;", true, ".a\\'b{color:red}",
                                 CapabilityMask::Viewport::kMobile, true),
            "proven @layer a; noscript-render css-escape touch ua "
            "anon-layers-dropped")
      << "the salts have a fixed order";
  EXPECT_EQ(ValidationBindingFor("", false, "",
                                 CapabilityMask::Viewport::kDesktop, false),
            "");
  const std::string css = ".hero{color:red}";
  const std::string mobile = CombinedCssValidationHash(
      css, ValidationBindingFor("", false, css,
                                CapabilityMask::Viewport::kMobile, false));
  const std::string tablet = CombinedCssValidationHash(
      css, ValidationBindingFor("", false, css,
                                CapabilityMask::Viewport::kTablet, false));
  const std::string desktop = CombinedCssValidationHash(
      css, ValidationBindingFor("", false, css,
                                CapabilityMask::Viewport::kDesktop, false));
  EXPECT_NE(mobile, CombinedCssValidationHash(css));
  EXPECT_NE(tablet, CombinedCssValidationHash(css));
  EXPECT_NE(mobile, tablet) << "the tablet keeps its own salt as well";
  EXPECT_EQ(desktop, CombinedCssValidationHash(css))
      << "desktop records do not revalidate";
}

// A mobile or tablet record made since touch emulation but before the
// user agent override ("touch" without "ua") mismatches once; the desktop's
// binding bytes are those it had before either change.
TEST(ValidationBindingTest, SaltsMobileAndTabletRecordsForUserAgent) {
  const std::string css = ".hero{color:red}";
  EXPECT_NE(
      CombinedCssValidationHash(
          css, ValidationBindingFor("", false, css,
                                    CapabilityMask::Viewport::kMobile, false)),
      CombinedCssValidationHash(css, "touch"))
      << "an earlier mobile record is made again";
  EXPECT_NE(
      CombinedCssValidationHash(
          css, ValidationBindingFor("", false, css,
                                    CapabilityMask::Viewport::kTablet, false)),
      CombinedCssValidationHash(css, "tablet-mobile touch"))
      << "an earlier tablet record is made again";
  EXPECT_EQ(ValidationBindingFor("proven @layer a;", true, ".a\\'b{c:d}",
                                 CapabilityMask::Viewport::kDesktop, true),
            "proven @layer a; noscript-render css-escape anon-layers-dropped")
      << "no device salt on the desktop";
  EXPECT_EQ(ValidationBindingFor("unproven", false, "",
                                 CapabilityMask::Viewport::kDesktop, false),
            "unproven");
}

// A record about a block whose derivation left every
// anonymous-layer rule out (CriticalCssResult::anonymous_layers_dropped) is
// salted, on both sides from the same derivation. The layer binding's `u2`
// marker is keyed on the combined sheet; the salt comes from the derivation
// itself, so a block that only the check on the finished block moved after
// the sheets is covered too. A derivation that dropped nothing adds nothing.
TEST(ValidationBindingTest, SaltsWhenTheDerivationDroppedAnonymousLayers) {
  const std::string css = "@layer a{} @layer{.x{color:red}}";
  EXPECT_EQ(ValidationBindingFor("unproven u2", false, css,
                                 CapabilityMask::Viewport::kDesktop, true),
            "unproven u2 anon-layers-dropped");
  EXPECT_EQ(ValidationBindingFor("proven @layer a;", false, css,
                                 CapabilityMask::Viewport::kDesktop, true),
            "proven @layer a; anon-layers-dropped")
      << "the finished-block check moved this one; the sheet's marker is "
         "silent and the salt speaks";
  EXPECT_EQ(ValidationBindingFor("proven @layer a;", true, ".a\\'b{color:red}",
                                 CapabilityMask::Viewport::kDesktop, true),
            "proven @layer a; noscript-render css-escape anon-layers-dropped");
  EXPECT_EQ(ValidationBindingFor("unproven u2", false, css,
                                 CapabilityMask::Viewport::kDesktop, false),
            "unproven u2");
  EXPECT_EQ(
      ValidationBindingFor("unproven u2", true, css,
                           CapabilityMask::Viewport::kTablet, true),
      "unproven u2 noscript-render tablet-mobile touch ua anon-layers-dropped")
      << "the salts have a fixed order";
  EXPECT_EQ(ValidationBindingFor("", false, css,
                                 CapabilityMask::Viewport::kDesktop, false),
            "");
  EXPECT_NE(
      CombinedCssValidationHash(
          css, ValidationBindingFor("unproven u2", false, css,
                                    CapabilityMask::Viewport::kDesktop, true)),
      CombinedCssValidationHash(
          css,
          ValidationBindingFor("unproven u2", false, css,
                               CapabilityMask::Viewport::kDesktop, false)));
}

}  // namespace
}  // namespace pagespeed
