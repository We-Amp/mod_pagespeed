// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Config File Persistence Tests

#include "src/worker/config_file.h"

#ifdef _WIN32
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#include <sys/types.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>

#include "gtest/gtest.h"
#include "nlohmann/json.hpp"
#include "src/worker/posix_compat.h"

namespace pagespeed {
namespace {

using json = nlohmann::json;

class ConfigFileTest : public ::testing::Test {
 protected:
  void SetUp() override {
    tmp_dir_ = std::filesystem::temp_directory_path() /
               ("config_file_test_" + std::to_string(
#ifdef _WIN32
                                          _getpid()
#else
                                          getpid()
#endif
                                              ));
    std::filesystem::create_directories(tmp_dir_);
  }

  void TearDown() override { std::filesystem::remove_all(tmp_dir_); }

  std::filesystem::path tmp_dir_;
};

// --- ConfigFilePath tests ---

TEST_F(ConfigFileTest, PathDerivation) {
  std::string cache_path = (tmp_dir_ / "cache.vol").string();
  std::string expected = (tmp_dir_ / "pagespeed.json").string();
  EXPECT_EQ(ConfigFilePath(cache_path), expected);
}

TEST_F(ConfigFileTest, PathDerivationNestedDir) {
  std::string cache_path = "/var/cache/pagespeed/cache.vol";
  EXPECT_EQ(std::filesystem::path(ConfigFilePath(cache_path)),
            std::filesystem::path("/var/cache/pagespeed/pagespeed.json"));
}

TEST_F(ConfigFileTest, PathEmptyCachePath) {
  EXPECT_EQ(ConfigFilePath(""), "");
}

// --- ReadConfigFile tests ---

TEST_F(ConfigFileTest, ReadMissingFile) {
  std::string error;
  auto j = ReadConfigFile((tmp_dir_ / "nonexistent.json").string(), &error);
  EXPECT_TRUE(j.is_object());
  EXPECT_TRUE(j.empty());
  EXPECT_TRUE(error.empty());
}

TEST_F(ConfigFileTest, ReadEmptyPath) {
  std::string error;
  auto j = ReadConfigFile("", &error);
  EXPECT_TRUE(j.is_object());
  EXPECT_TRUE(j.empty());
}

TEST_F(ConfigFileTest, ReadCorruptJson) {
  std::string path = (tmp_dir_ / "bad.json").string();
  {
    std::ofstream f(path);
    f << "not json {{{";
  }
  std::string error;
  auto j = ReadConfigFile(path, &error);
  EXPECT_TRUE(j.is_object());
  EXPECT_TRUE(j.empty());
  EXPECT_FALSE(error.empty());
}

TEST_F(ConfigFileTest, ReadNonObjectJson) {
  std::string path = (tmp_dir_ / "array.json").string();
  {
    std::ofstream f(path);
    f << "[1, 2, 3]";
  }
  std::string error;
  auto j = ReadConfigFile(path, &error);
  EXPECT_TRUE(j.is_object());
  EXPECT_TRUE(j.empty());
  EXPECT_EQ(error, "Config file must contain a JSON object");
}

TEST_F(ConfigFileTest, ReadValidJson) {
  std::string path = (tmp_dir_ / "good.json").string();
  {
    std::ofstream f(path);
    f << R"({"jpeg_quality": 90})";
  }
  std::string error;
  auto j = ReadConfigFile(path, &error);
  EXPECT_TRUE(error.empty());
  EXPECT_EQ(j["jpeg_quality"], 90);
}

TEST_F(ConfigFileTest, ReadNullError) {
  std::string path = (tmp_dir_ / "bad2.json").string();
  {
    std::ofstream f(path);
    f << "!!!";
  }
  // Should not crash with nullptr error.
  auto j = ReadConfigFile(path, nullptr);
  EXPECT_TRUE(j.empty());
}

TEST_F(ConfigFileTest, ReadRejectsOversizedFile) {
  std::string path = (tmp_dir_ / "huge.json").string();
  // Create a file larger than 1 MB.  Content doesn't need to be valid JSON
  // because the size check runs before parsing.
  {
    std::ofstream f(path, std::ios::binary);
    std::string filler(1024 * 1024 + 1, ' ');
    f.write(filler.data(), filler.size());
  }
  std::string error;
  auto j = ReadConfigFile(path, &error);
  EXPECT_TRUE(j.is_object());
  EXPECT_TRUE(j.empty());
  EXPECT_FALSE(error.empty());
  EXPECT_NE(error.find("size limit"), std::string::npos);
}

TEST_F(ConfigFileTest, ReadRejectsDeeplyNestedJson) {
  std::string path = (tmp_dir_ / "deep.json").string();
  // Build a JSON string with 50 levels of nesting (exceeds limit of 32).
  {
    std::ofstream f(path);
    for (int i = 0; i < 50; ++i) f << "{\"a\":";
    f << "1";
    for (int i = 0; i < 50; ++i) f << "}";
  }
  std::string error;
  auto j = ReadConfigFile(path, &error);
  EXPECT_TRUE(j.is_object());
  EXPECT_TRUE(j.empty());
  EXPECT_FALSE(error.empty());
  EXPECT_NE(error.find("nesting"), std::string::npos);
}

// --- WriteConfigFile tests ---

TEST_F(ConfigFileTest, WriteAndReadRoundTrip) {
  std::string path = (tmp_dir_ / "pagespeed.json").string();
  json data = {{"jpeg_quality", 90}, {"disable_html", true}};
  ASSERT_TRUE(WriteConfigFile(path, data));

  std::string error;
  auto loaded = ReadConfigFile(path, &error);
  EXPECT_TRUE(error.empty());
  EXPECT_EQ(loaded["jpeg_quality"], 90);
  EXPECT_EQ(loaded["disable_html"], true);
}

TEST_F(ConfigFileTest, WriteOverwritesExisting) {
  std::string path = (tmp_dir_ / "pagespeed.json").string();
  ASSERT_TRUE(WriteConfigFile(path, {{"jpeg_quality", 80}}));
  ASSERT_TRUE(WriteConfigFile(path, {{"jpeg_quality", 95}}));

  std::string error;
  auto loaded = ReadConfigFile(path, &error);
  EXPECT_EQ(loaded["jpeg_quality"], 95);
}

TEST_F(ConfigFileTest, WritePermissions) {
  std::string path = (tmp_dir_ / "pagespeed.json").string();
  ASSERT_TRUE(WriteConfigFile(path, {{"jpeg_quality", 80}}));

#ifndef _WIN32
  // Unix file permission checks — not applicable on Windows (no mode_t 0644).
  struct stat st;
  ASSERT_EQ(::stat(path.c_str(), &st), 0);
  // 0644: world-readable (not secret like license).
  EXPECT_EQ(st.st_mode & static_cast<mode_t>(0777), static_cast<mode_t>(0644));
#endif
}

TEST_F(ConfigFileTest, WriteEmptyPath) {
  EXPECT_FALSE(WriteConfigFile("", {{"a", 1}}));
}

TEST_F(ConfigFileTest, WriteNoTmpLeftover) {
  std::string path = (tmp_dir_ / "pagespeed.json").string();
  std::string tmp_path = path + ".tmp";
  ASSERT_TRUE(WriteConfigFile(path, {{"a", 1}}));
  EXPECT_FALSE(std::filesystem::exists(tmp_path));
}

// --- IsNonReloadable tests ---

TEST_F(ConfigFileTest, NonReloadableFields) {
  EXPECT_TRUE(IsNonReloadable("socket_path"));
  EXPECT_TRUE(IsNonReloadable("cache_path"));
  EXPECT_TRUE(IsNonReloadable("cache_size_bytes"));
  EXPECT_TRUE(IsNonReloadable("num_threads"));
  EXPECT_TRUE(IsNonReloadable("max_connections"));
  EXPECT_TRUE(IsNonReloadable("max_request_size"));
  // Issue #934: lease knobs are consumed once at cache open.
  EXPECT_TRUE(IsNonReloadable("read_lease_duration_ms"));
  EXPECT_TRUE(IsNonReloadable("lease_wrap_ceiling_ms"));
}

TEST_F(ConfigFileTest, ReloadableFields) {
  EXPECT_FALSE(IsNonReloadable("jpeg_quality"));
  EXPECT_FALSE(IsNonReloadable("disable_html"));
  EXPECT_FALSE(IsNonReloadable("svg_mode"));
  EXPECT_FALSE(IsNonReloadable("unknown_field"));
}

// --- ConfigToJsonPersistable tests ---

TEST_F(ConfigFileTest, PersistableCarriesNoLicensingKeys) {
  // The 2.0 licensing settings are gone from the config surface.
  WorkerConfig config;
  auto j = ConfigToJsonPersistable(config);
  EXPECT_FALSE(j.contains("license_key"));
  EXPECT_FALSE(j.contains("license_renewal_url"));
  EXPECT_FALSE(j.contains("fastspring_storefront"));
  EXPECT_FALSE(j.contains("fastspring_product"));
  // (rsl_cap_requested_license — the unrelated RSL-CAP setting — may appear.)
  EXPECT_EQ(j.dump().find("license_key"), std::string::npos);
  EXPECT_EQ(j.dump().find("license_renewal"), std::string::npos);
}

TEST_F(ConfigFileTest, PersistableIncludesHotReloadable) {
  WorkerConfig config;
  config.jpeg_quality = 92;
  config.svg_mode = SvgMode::kAuto;
  config.html_mode = HtmlMode::kOff;
  auto j = ConfigToJsonPersistable(config);
  EXPECT_EQ(j["jpeg_quality"], 92);
  EXPECT_EQ(j["svg_mode"], "auto");
  EXPECT_EQ(j["disable_html"], true);
}

TEST_F(ConfigFileTest, PersistableSvgModes) {
  WorkerConfig config;

  config.svg_mode = SvgMode::kDetect;
  EXPECT_EQ(ConfigToJsonPersistable(config)["svg_mode"], "detect");

  config.svg_mode = SvgMode::kPreview;
  EXPECT_EQ(ConfigToJsonPersistable(config)["svg_mode"], "preview");

  config.svg_mode = SvgMode::kAuto;
  EXPECT_EQ(ConfigToJsonPersistable(config)["svg_mode"], "auto");
}

// --- ApplyConfigJson tests ---

TEST_F(ConfigFileTest, ApplyIntegerField) {
  WorkerConfig config;
  json body = {{"jpeg_quality", 92}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_EQ(config.jpeg_quality, 92);
  EXPECT_EQ(result.applied["jpeg_quality"], 92);
  EXPECT_TRUE(result.rejected.empty());
}

TEST_F(ConfigFileTest, ApplyClampsToBounds) {
  WorkerConfig config;
  json body = {{"jpeg_quality", 200}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_EQ(config.jpeg_quality, 100);
  EXPECT_EQ(result.applied["jpeg_quality"], 100);
}

TEST_F(ConfigFileTest, ApplyBooleanField) {
  WorkerConfig config;
  EXPECT_EQ(config.html_mode, HtmlMode::kFull);
  json body = {{"disable_html", true}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_EQ(config.html_mode, HtmlMode::kOff);
  EXPECT_EQ(result.applied["disable_html"], true);
}

// --- html_mode and the legacy disable_html key ---

TEST_F(ConfigFileTest, DefaultHtmlModeIsFull) {
  WorkerConfig config;
  EXPECT_EQ(config.html_mode, HtmlMode::kFull);
  auto j = ConfigToJsonPersistable(config);
  EXPECT_EQ(j["html_mode"], "full");
  EXPECT_EQ(j["disable_html"], false);
}

TEST_F(ConfigFileTest, ApplyHtmlModeValues) {
  for (const char* name :
       {"off", "markdown-static", "markdown-rendered", "full"}) {
    WorkerConfig config;
    auto result = ApplyConfigJson(json{{"html_mode", name}}, &config);
    EXPECT_EQ(HtmlModeName(config.html_mode), name);
    EXPECT_EQ(result.applied["html_mode"], name);
    EXPECT_TRUE(result.rejected.empty()) << result.rejected.dump();
    EXPECT_TRUE(result.warnings.empty()) << result.warnings.dump();
  }
}

TEST_F(ConfigFileTest, ApplyHtmlModeRejectsUnknownValueAndType) {
  WorkerConfig config;
  config.html_mode = HtmlMode::kMarkdownStatic;
  for (const json& bad : {json("Full"), json("markdown"), json(""), json(3),
                          json(true), json(nullptr)}) {
    auto result = ApplyConfigJson(json{{"html_mode", bad}}, &config);
    EXPECT_TRUE(result.rejected.contains("html_mode")) << bad.dump();
    EXPECT_FALSE(result.applied.contains("html_mode")) << bad.dump();
    EXPECT_EQ(config.html_mode, HtmlMode::kMarkdownStatic) << bad.dump();
  }
}

TEST_F(ConfigFileTest, ApplyLegacyDisableHtmlMapsToOffAndFull) {
  WorkerConfig config;
  config.html_mode = HtmlMode::kMarkdownRendered;
  auto r1 = ApplyConfigJson(json{{"disable_html", true}}, &config);
  EXPECT_EQ(config.html_mode, HtmlMode::kOff);
  EXPECT_EQ(r1.applied["disable_html"], true);
  // false is full, also from a markdown-* mode (the legacy key cannot say
  // anything else).
  config.html_mode = HtmlMode::kMarkdownStatic;
  auto r2 = ApplyConfigJson(json{{"disable_html", false}}, &config);
  EXPECT_EQ(config.html_mode, HtmlMode::kFull);
  EXPECT_EQ(r2.applied["disable_html"], false);
}

TEST_F(ConfigFileTest, ApplyHtmlModeWinsOverDisableHtml) {
  // Conflicting: html_mode wins, disable_html is ignored with a warning.
  WorkerConfig config;
  auto r = ApplyConfigJson(
      json{{"disable_html", true}, {"html_mode", "markdown-static"}}, &config);
  EXPECT_EQ(config.html_mode, HtmlMode::kMarkdownStatic);
  EXPECT_EQ(r.applied["html_mode"], "markdown-static");
  EXPECT_FALSE(r.applied.contains("disable_html"));
  ASSERT_EQ(r.warnings.size(), 1u) << r.warnings.dump();
  EXPECT_NE(r.warnings[0].get<std::string>().find("disable_html"),
            std::string::npos);

  WorkerConfig c2;
  c2.html_mode = HtmlMode::kOff;
  auto r2 =
      ApplyConfigJson(json{{"html_mode", "full"}, {"disable_html", true}}, &c2);
  EXPECT_EQ(c2.html_mode, HtmlMode::kFull);
  EXPECT_EQ(r2.warnings.size(), 1u) << r2.warnings.dump();
}

TEST_F(ConfigFileTest, ApplyHtmlModeWithAgreeingDisableHtmlIsQuiet) {
  // Every writer emits both keys; agreeing values apply without a warning.
  for (HtmlMode mode : {HtmlMode::kOff, HtmlMode::kMarkdownStatic,
                        HtmlMode::kMarkdownRendered, HtmlMode::kFull}) {
    WorkerConfig config;
    auto r =
        ApplyConfigJson(json{{"html_mode", std::string(HtmlModeName(mode))},
                             {"disable_html", HtmlModeDisablesHtml(mode)}},
                        &config);
    EXPECT_EQ(config.html_mode, mode);
    EXPECT_TRUE(r.applied.contains("html_mode"));
    EXPECT_TRUE(r.applied.contains("disable_html"));
    EXPECT_TRUE(r.warnings.empty()) << r.warnings.dump();
    EXPECT_TRUE(r.rejected.empty()) << r.rejected.dump();
  }
}

TEST_F(ConfigFileTest, ApplyRejectedHtmlModeLeavesDisableHtmlInCharge) {
  WorkerConfig config;
  auto r = ApplyConfigJson(
      json{{"html_mode", "markdown-future"}, {"disable_html", true}}, &config);
  EXPECT_EQ(config.html_mode, HtmlMode::kOff);
  EXPECT_TRUE(r.rejected.contains("html_mode"));
  EXPECT_EQ(r.applied["disable_html"], true);
}

TEST_F(ConfigFileTest, PersistableHtmlModeRoundTrip) {
  for (HtmlMode mode : {HtmlMode::kOff, HtmlMode::kMarkdownStatic,
                        HtmlMode::kMarkdownRendered, HtmlMode::kFull}) {
    WorkerConfig original;
    original.html_mode = mode;
    auto j = ConfigToJsonPersistable(original);
    EXPECT_EQ(j["html_mode"], std::string(HtmlModeName(mode)));
    EXPECT_EQ(j["disable_html"], mode == HtmlMode::kOff);
    WorkerConfig restored;
    auto r = ApplyConfigJson(j, &restored);
    EXPECT_EQ(restored.html_mode, mode);
    EXPECT_TRUE(r.warnings.empty()) << r.warnings.dump();
  }
}

TEST_F(ConfigFileTest, OlderReaderOfNewFileKeepsWorking) {
  // A worker that predates html_mode rejects the key as unknown (not fatal)
  // and applies disable_html.  Model it by dropping the key: off stays off
  // and every other mode reads as full, as it did before the mode existed.
  for (HtmlMode mode : {HtmlMode::kOff, HtmlMode::kMarkdownStatic,
                        HtmlMode::kMarkdownRendered, HtmlMode::kFull}) {
    WorkerConfig original;
    original.html_mode = mode;
    original.jpeg_quality = 77;
    auto j = ConfigToJsonPersistable(original);
    j.erase("html_mode");
    WorkerConfig older;
    auto r = ApplyConfigJson(j, &older);
    EXPECT_EQ(older.html_mode,
              mode == HtmlMode::kOff ? HtmlMode::kOff : HtmlMode::kFull);
    EXPECT_EQ(older.jpeg_quality, 77);
    EXPECT_TRUE(r.applied.contains("disable_html"));
  }
}

TEST_F(ConfigFileTest, ApplyFloatField) {
  WorkerConfig config;
  json body = {{"target_ssimulacra2", 80.5}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_FLOAT_EQ(config.target_ssimulacra2, 80.5f);
  EXPECT_TRUE(result.rejected.empty());
}

TEST_F(ConfigFileTest, ApplyRejectsNaN) {
  WorkerConfig config;
  float original = config.target_ssimulacra2;
  json body = {
      {"target_ssimulacra2", std::numeric_limits<double>::quiet_NaN()}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_FLOAT_EQ(config.target_ssimulacra2, original);
  EXPECT_TRUE(result.rejected.contains("target_ssimulacra2"));
}

TEST_F(ConfigFileTest, ApplyRejectsOutOfRange) {
  WorkerConfig config;
  json body = {{"target_ssimulacra2", 150.0}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.rejected.contains("target_ssimulacra2"));
}

TEST_F(ConfigFileTest, ApplyAsyncCssMinCoverage) {
  WorkerConfig config;
  json body = {{"async_css_min_coverage", 0.25}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_FLOAT_EQ(config.async_css_min_coverage, 0.25f);
  EXPECT_TRUE(result.rejected.empty());
}

TEST_F(ConfigFileTest, ApplyAsyncCssMinCoverageRejectsNaNAndOutOfRange) {
  WorkerConfig config;
  float original = config.async_css_min_coverage;
  auto nan = ApplyConfigJson(json{{"async_css_min_coverage",
                                   std::numeric_limits<double>::quiet_NaN()}},
                             &config);
  EXPECT_FLOAT_EQ(config.async_css_min_coverage, original);
  EXPECT_TRUE(nan.rejected.contains("async_css_min_coverage"));

  auto high = ApplyConfigJson(json{{"async_css_min_coverage", 1.5}}, &config);
  EXPECT_FLOAT_EQ(config.async_css_min_coverage, original);
  EXPECT_TRUE(high.rejected.contains("async_css_min_coverage"));
}

TEST_F(ConfigFileTest, ApplyAsyncCssMinDeferredBytes) {
  WorkerConfig config;
  json body = {{"async_css_min_deferred_bytes", 20000}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_EQ(config.async_css_min_deferred_bytes, 20000u);
  EXPECT_TRUE(result.rejected.empty());
}

TEST_F(ConfigFileTest, ApplyRejectsWrongType) {
  WorkerConfig config;
  json body = {{"jpeg_quality", "not_a_number"}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.rejected.contains("jpeg_quality"));
}

TEST_F(ConfigFileTest, ApplyRejectsNonReloadable) {
  WorkerConfig config;
  json body = {{"cache_path", "/new/path"}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.rejected.contains("cache_path"));
  EXPECT_EQ(result.rejected["cache_path"], "Cannot change at runtime");
}

TEST_F(ConfigFileTest, ApplyRejectsUnknownField) {
  WorkerConfig config;
  json body = {{"totally_unknown_field", 42}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.rejected.contains("totally_unknown_field"));
}

TEST_F(ConfigFileTest, ApplyRejectsRetiredLicensingFieldsNonFatally) {
  // A stale 2.0 pagespeed.json (or an old console) may still carry
  // the licensing settings.  They must never be fatal — they come back as
  // rejected with a reason that names the 2.1 change, other fields in the
  // same body still apply, and nothing about them is stored.
  WorkerConfig config;
  json body = {{"license_key", "some-old-token"},
               {"license_renewal_url", "https://renew.example"},
               {"fastspring_storefront", "shop.onfastspring.com/x"},
               {"fastspring_product", "old-plan"},
               {"jpeg_quality", 77}};
  auto result = ApplyConfigJson(body, &config);
  for (const char* key : {"license_key", "license_renewal_url",
                          "fastspring_storefront", "fastspring_product"}) {
    EXPECT_TRUE(IsRetiredLicensingKey(key)) << key;
    ASSERT_TRUE(result.rejected.contains(key)) << key;
    EXPECT_FALSE(result.applied.contains(key)) << key;
    const std::string reason = result.rejected[key].get<std::string>();
    EXPECT_NE(reason.find("2.1"), std::string::npos) << reason;
    EXPECT_NE(reason.find("can be deleted"), std::string::npos) << reason;
  }
  EXPECT_FALSE(IsRetiredLicensingKey("jpeg_quality"));
  EXPECT_EQ(config.jpeg_quality, 77);
  EXPECT_TRUE(result.applied.contains("jpeg_quality"));
  // The persisted form never round-trips a retired key.
  const std::string persisted = ConfigToJsonPersistable(config).dump();
  EXPECT_EQ(persisted.find("license_key"), std::string::npos);
  EXPECT_EQ(persisted.find("license_renewal"), std::string::npos);
  EXPECT_EQ(persisted.find("fastspring"), std::string::npos);
}

TEST_F(ConfigFileTest, ApplySvgModeEnum) {
  WorkerConfig config;
  EXPECT_EQ(config.svg_mode, SvgMode::kDetect);

  json body = {{"svg_mode", "auto"}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_EQ(config.svg_mode, SvgMode::kAuto);
  EXPECT_EQ(result.applied["svg_mode"], "auto");
}

TEST_F(ConfigFileTest, ApplySvgModeInvalid) {
  WorkerConfig config;
  json body = {{"svg_mode", "turbo"}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.rejected.contains("svg_mode"));
  EXPECT_EQ(config.svg_mode, SvgMode::kDetect);  // unchanged
}

TEST_F(ConfigFileTest, ApplyCacheModeSafe) {
  WorkerConfig config;
  json body = {{"cache_mode", "safe"}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_EQ(config.cache_mode, "safe");
  EXPECT_EQ(result.applied["cache_mode"], "safe");
}

TEST_F(ConfigFileTest, ApplyCacheModeAggressive) {
  WorkerConfig config;
  json body = {{"cache_mode", "aggressive"}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_EQ(config.cache_mode, "aggressive");
  EXPECT_EQ(result.applied["cache_mode"], "aggressive");
}

TEST_F(ConfigFileTest, ApplyCacheModeInvalid) {
  WorkerConfig config;
  json body = {{"cache_mode", "turbo"}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.rejected.contains("cache_mode"));
  EXPECT_EQ(config.cache_mode, "safe");  // unchanged
}

TEST_F(ConfigFileTest, PersistableCacheMode) {
  WorkerConfig config;
  config.cache_mode = "aggressive";
  auto j = ConfigToJsonPersistable(config);
  EXPECT_EQ(j["cache_mode"], "aggressive");

  config.cache_mode = "safe";
  j = ConfigToJsonPersistable(config);
  EXPECT_EQ(j["cache_mode"], "safe");
}

TEST_F(ConfigFileTest, ApplyWarningsOnLowQuality) {
  WorkerConfig config;
  json body = {{"jpeg_quality", 10}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_EQ(config.jpeg_quality, 10);
  EXPECT_FALSE(result.warnings.empty());
}

TEST_F(ConfigFileTest, ApplyWarningsOnHighSsimulacra2) {
  WorkerConfig config;
  json body = {{"target_ssimulacra2", 95.0}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_FALSE(result.warnings.empty());
}

// Clamping warnings (issue #166) — out-of-range integer values are silently
// bounded by std::clamp, but the client should see a warning so it can
// distinguish "accepted verbatim" from "bounded to range". One spot check per
// clamp site, plus a separate test verifying in-range values stay quiet.

TEST_F(ConfigFileTest, ClampWarningOnHighJpegQuality) {
  WorkerConfig config;
  json body = {{"jpeg_quality", 500}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_EQ(config.jpeg_quality, 100);
  EXPECT_TRUE(result.applied.contains("jpeg_quality"));
  ASSERT_FALSE(result.warnings.empty());
  // The warning names the field, raw value, clamped value, and range.
  bool found = false;
  for (const auto& w : result.warnings) {
    std::string s = w.get<std::string>();
    if (s.find("jpeg_quality") != std::string::npos &&
        s.find("500") != std::string::npos &&
        s.find("100") != std::string::npos) {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found) << "Expected clamp warning referencing raw value 500";
}

TEST_F(ConfigFileTest, ClampWarningOnNegativeWebpQuality) {
  WorkerConfig config;
  json body = {{"webp_quality", -10}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_EQ(config.webp_quality, 0);
  bool found = false;
  for (const auto& w : result.warnings) {
    std::string s = w.get<std::string>();
    if (s.find("webp_quality") != std::string::npos &&
        s.find("-10") != std::string::npos) {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found);
}

TEST_F(ConfigFileTest, ClampWarningOnHighAvifSpeed) {
  WorkerConfig config;
  json body = {{"avif_speed", 99}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_EQ(config.avif_speed, 10);
  bool found = false;
  for (const auto& w : result.warnings) {
    if (w.get<std::string>().find("avif_speed") != std::string::npos) {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found);
}

TEST_F(ConfigFileTest, ClampWarningOnHighGzipLevel) {
  WorkerConfig config;
  json body = {{"gzip_level", 99}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_EQ(config.gzip_level, 9);
  bool found = false;
  for (const auto& w : result.warnings) {
    if (w.get<std::string>().find("gzip_level") != std::string::npos) {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found);
}

TEST_F(ConfigFileTest, ClampWarningOnHighBrotliLevel) {
  WorkerConfig config;
  json body = {{"brotli_level", 42}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_EQ(config.brotli_level, 11);
  bool found = false;
  for (const auto& w : result.warnings) {
    if (w.get<std::string>().find("brotli_level") != std::string::npos) {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found);
}

TEST_F(ConfigFileTest, ClampWarningOnHighSavedataJpegQuality) {
  WorkerConfig config;
  json body = {{"savedata_jpeg_quality", 200}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_EQ(config.savedata_jpeg_quality, 100);
  bool found = false;
  for (const auto& w : result.warnings) {
    if (w.get<std::string>().find("savedata_jpeg_quality") !=
        std::string::npos) {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found);
}

TEST_F(ConfigFileTest, ClampWarningOnHighQualityCapMargin) {
  WorkerConfig config;
  json body = {{"quality_cap_margin", 200}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_EQ(config.quality_cap_margin, 50);
  bool found = false;
  for (const auto& w : result.warnings) {
    if (w.get<std::string>().find("quality_cap_margin") != std::string::npos) {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found);
}

static bool HasDeprecationWarning(const ConfigApplyResult& r) {
  for (const auto& w : r.warnings) {
    if (w.get<std::string>() == kQualityCapMarginDeprecatedWarning) return true;
  }
  return false;
}

TEST_F(ConfigFileTest, QualityCapMarginWarnsWheneverSetToNonDefault) {
  WorkerConfig config;
  // Reported on every request that carries it (config file, then each PATCH).
  EXPECT_TRUE(HasDeprecationWarning(
      ApplyConfigJson(json{{"quality_cap_margin", 20}}, &config)));
  EXPECT_TRUE(HasDeprecationWarning(
      ApplyConfigJson(json{{"quality_cap_margin", 5}}, &config)));
}

TEST_F(ConfigFileTest, NoQualityCapMarginNoDeprecationWarning) {
  WorkerConfig config;
  EXPECT_FALSE(HasDeprecationWarning(ApplyConfigJson(
      json{{"jpeg_quality", 80}, {"no_quality_cap", true}}, &config)));
}

TEST_F(ConfigFileTest, PersistedDefaultConfigDoesNotWarnQualityCapMargin) {
  WorkerConfig config;
  EXPECT_FALSE(HasDeprecationWarning(
      ApplyConfigJson(ConfigToJsonPersistable(WorkerConfig{}), &config)));
}

TEST_F(ConfigFileTest, NoClampWarningOnInRangeValues) {
  // In-range values for every clamp site should not emit a clamp warning
  // (low-quality warnings for jpeg/webp/avif_quality <30 are orthogonal and
  // deliberately excluded here — we use values >=30).
  WorkerConfig config;
  json body = {{"jpeg_quality", 80},          {"webp_quality", 75},
               {"avif_quality", 60},          {"avif_speed", 6},
               {"savedata_jpeg_quality", 50}, {"savedata_webp_quality", 40},
               {"savedata_avif_quality", 35}, {"gzip_level", 5},
               {"brotli_level", 6},           {"svg_candidacy_threshold", 50},
               {"quality_cap_margin", 10}};
  auto result = ApplyConfigJson(body, &config);
  // No warning should mention the word "clamped".
  for (const auto& w : result.warnings) {
    std::string s = w.get<std::string>();
    EXPECT_EQ(s.find("clamped"), std::string::npos)
        << "Unexpected clamp warning: " << s;
  }
}

TEST_F(ConfigFileTest, ClampWarningOnJpegQualityMinBoundary) {
  // jpeg_quality min is 1 (not 0). 0 should clamp to 1.
  WorkerConfig config;
  json body = {{"jpeg_quality", 0}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_EQ(config.jpeg_quality, 1);
  bool found = false;
  for (const auto& w : result.warnings) {
    std::string s = w.get<std::string>();
    if (s.find("jpeg_quality") != std::string::npos &&
        s.find("clamped") != std::string::npos) {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found);
}

TEST_F(ConfigFileTest, ApplyViewportWidthValidation) {
  WorkerConfig config;
  // Valid: 0 (disable) is allowed. Use unsigned literal for is_number_unsigned.
  json body = {{"mobile_width", 0u}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_EQ(config.mobile_width, 0u);
  EXPECT_TRUE(result.applied.contains("mobile_width"));

  // Invalid: 50 is below minimum 160.
  body = {{"mobile_width", 50u}};
  result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.rejected.contains("mobile_width"));
}

// --- Full round-trip test covering ALL persistable fields ---

TEST_F(ConfigFileTest, FullRoundTrip) {
  // Set EVERY persistable field to a non-default value, persist via
  // ConfigToJsonPersistable -> WriteConfigFile -> ReadConfigFile ->
  // ApplyConfigJson, then verify every field survives the round-trip.
  WorkerConfig original;

  // Quality settings (defaults: 85, 75, 60, 60, 50, 45)
  original.jpeg_quality = 92;
  original.webp_quality = 80;
  original.avif_quality = 55;
  original.avif_speed = 8;
  original.savedata_jpeg_quality = 50;
  original.savedata_webp_quality = 40;
  original.savedata_avif_quality = 35;

  // Viewport widths (defaults: 480, 768, 0)
  original.mobile_width = 320;
  original.tablet_width = 1024;
  original.desktop_width = 1920;

  // Proactive variant toggles (defaults: all true)
  original.proactive_image_variants = false;
  original.proactive_viewport_variants = false;
  original.proactive_savedata_variants = false;
  original.proactive_density_variants = false;

  // Content analysis (default: true)
  original.content_analysis = false;

  // Quality verification (default: true)
  original.quality_verify = false;

  // SSIMULACRA2 targets (defaults: 70.0, 5.0)
  original.target_ssimulacra2 = 80.5f;
  original.ssimulacra2_tolerance = 8.0f;

  // Learned quality (defaults: all true, reduction 15.0)
  original.learned_quality = false;
  original.learned_quality_jpeg = false;
  original.learned_quality_webp = false;
  original.learned_quality_avif = false;
  original.savedata_score_reduction = 20.0f;

  // Warmup (default: true)
  original.enable_warmup = false;

  // Cache mode (default: "safe")
  original.cache_mode = "aggressive";

  // Content-type toggles (defaults: all false)
  original.html_mode = HtmlMode::kOff;
  original.disable_css = true;
  original.disable_js = true;
  original.disable_image = true;

  // HTML transformation toggles (defaults: all false except
  // enable_speculation_rules which defaults to false)
  original.disable_lazy_load = true;
  original.disable_image_dimensions = true;
  original.disable_lcp_preload = true;
  original.disable_preconnect_injection = true;
  original.enable_speculation_rules = true;
  original.disable_css_import_flattening = true;

  // SVG settings (defaults: kDetect, 50, 65536, 1, 0, 4, 500, 262144,
  // 55.0, true, 500)
  original.svg_mode = SvgMode::kPreview;
  original.svg_candidacy_threshold = 75;
  original.svg_max_pixels = 131072;
  original.svg_preset = 2;
  original.svg_color_precision = 6;
  original.svg_filter_speckle = 8;
  original.svg_max_paths = 1000;
  original.svg_max_svg_bytes = 524288;
  original.svg_fidelity_threshold = 65.0f;
  original.svg_exclude_lcp = false;
  original.svg_timeout_ms = 1000;

  // Compression levels (defaults: 6, 6)
  original.gzip_level = 9;
  original.brotli_level = 11;

  // Security limits (defaults: 8192, 5MB, 2MB, 2MB, 10MB)
  original.max_url_length = 4096;
  original.max_html_size = static_cast<size_t>(10 * 1024 * 1024);
  original.max_css_size = static_cast<size_t>(4 * 1024 * 1024);
  original.max_js_size = static_cast<size_t>(4 * 1024 * 1024);
  original.max_image_size = static_cast<size_t>(20 * 1024 * 1024);

  // Async-CSS FOUC sufficiency gate (defaults: 0.10f, 15000)
  original.async_css_min_coverage = 0.30f;
  original.async_css_min_deferred_bytes = 25000;

  // Serialize -> write -> read -> apply
  auto persistable = ConfigToJsonPersistable(original);

  // Guard: if a new field is added to ConfigToJsonPersistable without updating
  // this test, this assertion will fail.
  EXPECT_EQ(persistable.size(), 60u);

  std::string path = (tmp_dir_ / "pagespeed.json").string();
  ASSERT_TRUE(WriteConfigFile(path, persistable));

  std::string error;
  auto loaded = ReadConfigFile(path, &error);
  EXPECT_TRUE(error.empty());

  WorkerConfig restored;
  auto result = ApplyConfigJson(loaded, &restored);
  EXPECT_TRUE(result.rejected.empty())
      << "Rejected fields: " << result.rejected.dump();

  // --- Verify every field ---

  // Quality settings
  EXPECT_EQ(restored.jpeg_quality, 92);
  EXPECT_EQ(restored.webp_quality, 80);
  EXPECT_EQ(restored.avif_quality, 55);
  EXPECT_EQ(restored.avif_speed, 8);
  EXPECT_EQ(restored.savedata_jpeg_quality, 50);
  EXPECT_EQ(restored.savedata_webp_quality, 40);
  EXPECT_EQ(restored.savedata_avif_quality, 35);

  // Viewport widths
  EXPECT_EQ(restored.mobile_width, 320u);
  EXPECT_EQ(restored.tablet_width, 1024u);
  EXPECT_EQ(restored.desktop_width, 1920u);

  // Proactive variant toggles
  EXPECT_EQ(restored.proactive_image_variants, false);
  EXPECT_EQ(restored.proactive_viewport_variants, false);
  EXPECT_EQ(restored.proactive_savedata_variants, false);
  EXPECT_EQ(restored.proactive_density_variants, false);

  // Content analysis
  EXPECT_EQ(restored.content_analysis, false);

  // Quality verification
  EXPECT_EQ(restored.quality_verify, false);

  // SSIMULACRA2 targets
  EXPECT_FLOAT_EQ(restored.target_ssimulacra2, 80.5f);
  EXPECT_FLOAT_EQ(restored.ssimulacra2_tolerance, 8.0f);

  // Learned quality
  EXPECT_EQ(restored.learned_quality, false);
  EXPECT_EQ(restored.learned_quality_jpeg, false);
  EXPECT_EQ(restored.learned_quality_webp, false);
  EXPECT_EQ(restored.learned_quality_avif, false);
  EXPECT_FLOAT_EQ(restored.savedata_score_reduction, 20.0f);

  // Warmup
  EXPECT_EQ(restored.enable_warmup, false);

  // Cache mode
  EXPECT_EQ(restored.cache_mode, "aggressive");

  // Content-type toggles
  EXPECT_EQ(restored.html_mode, HtmlMode::kOff);
  EXPECT_EQ(restored.disable_css, true);
  EXPECT_EQ(restored.disable_js, true);
  EXPECT_EQ(restored.disable_image, true);

  // HTML transformation toggles
  EXPECT_EQ(restored.disable_lazy_load, true);
  EXPECT_EQ(restored.disable_image_dimensions, true);
  EXPECT_EQ(restored.disable_lcp_preload, true);
  EXPECT_EQ(restored.disable_preconnect_injection, true);
  EXPECT_EQ(restored.enable_speculation_rules, true);
  EXPECT_EQ(restored.disable_css_import_flattening, true);

  // SVG settings
  EXPECT_EQ(restored.svg_mode, SvgMode::kPreview);
  EXPECT_EQ(restored.svg_candidacy_threshold, 75);
  EXPECT_EQ(restored.svg_max_pixels, 131072);
  EXPECT_EQ(restored.svg_preset, 2);
  EXPECT_EQ(restored.svg_color_precision, 6);
  EXPECT_EQ(restored.svg_filter_speckle, 8);
  EXPECT_EQ(restored.svg_max_paths, 1000);
  EXPECT_EQ(restored.svg_max_svg_bytes, 524288);
  EXPECT_FLOAT_EQ(restored.svg_fidelity_threshold, 65.0f);
  EXPECT_EQ(restored.svg_exclude_lcp, false);
  EXPECT_EQ(restored.svg_timeout_ms, 1000);

  // Compression levels
  EXPECT_EQ(restored.gzip_level, 9);
  EXPECT_EQ(restored.brotli_level, 11);

  // Security limits
  EXPECT_EQ(restored.max_url_length, 4096u);
  EXPECT_EQ(restored.max_html_size, 10u * 1024 * 1024);
  EXPECT_EQ(restored.max_css_size, 4u * 1024 * 1024);
  EXPECT_EQ(restored.max_js_size, 4u * 1024 * 1024);
  EXPECT_EQ(restored.max_image_size, 20u * 1024 * 1024);

  EXPECT_FLOAT_EQ(restored.async_css_min_coverage, 0.30f);
  EXPECT_EQ(restored.async_css_min_deferred_bytes, 25000u);
}

// --- Viewport width validation: reject out-of-range values ---

TEST_F(ConfigFileTest, ApplyViewportWidthRejectsTooSmall) {
  WorkerConfig config;
  // 100 is below the minimum of 160.
  json body = {{"mobile_width", 100u}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.rejected.contains("mobile_width"));
  EXPECT_NE(config.mobile_width, 100u);
}

TEST_F(ConfigFileTest, ApplyViewportWidthRejectsTooLarge) {
  WorkerConfig config;
  // 20000 exceeds the maximum of 10000.
  json body = {{"tablet_width", 20000u}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.rejected.contains("tablet_width"));
}

TEST_F(ConfigFileTest, ApplyDesktopWidthRejectsOutOfRange) {
  WorkerConfig config;
  // 100 is below 160.
  json body = {{"desktop_width", 100u}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.rejected.contains("desktop_width"));

  // 0 is allowed (disable).
  body = {{"desktop_width", 0u}};
  result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.applied.contains("desktop_width"));
  EXPECT_EQ(config.desktop_width, 0u);
}

TEST_F(ConfigFileTest, ApplyViewportWidthAcceptsBoundaries) {
  WorkerConfig config;
  // Minimum boundary: 160.
  json body = {{"mobile_width", 160u}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.applied.contains("mobile_width"));
  EXPECT_EQ(config.mobile_width, 160u);

  // Maximum boundary: 10000.
  body = {{"tablet_width", 10000u}};
  result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.applied.contains("tablet_width"));
  EXPECT_EQ(config.tablet_width, 10000u);
}

// --- target_ssimulacra2 boundary tests ---

TEST_F(ConfigFileTest, ApplyTargetSsimulacra2RejectsNegative) {
  WorkerConfig config;
  float original = config.target_ssimulacra2;
  json body = {{"target_ssimulacra2", -1.0}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.rejected.contains("target_ssimulacra2"));
  EXPECT_FLOAT_EQ(config.target_ssimulacra2, original);
}

TEST_F(ConfigFileTest, ApplyTargetSsimulacra2RejectsInfinity) {
  WorkerConfig config;
  float original = config.target_ssimulacra2;
  json body = {{"target_ssimulacra2", std::numeric_limits<double>::infinity()}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.rejected.contains("target_ssimulacra2"));
  EXPECT_FLOAT_EQ(config.target_ssimulacra2, original);
}

TEST_F(ConfigFileTest, ApplyTargetSsimulacra2AcceptsZero) {
  WorkerConfig config;
  json body = {{"target_ssimulacra2", 0.0}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.applied.contains("target_ssimulacra2"));
  EXPECT_FLOAT_EQ(config.target_ssimulacra2, 0.0f);
}

TEST_F(ConfigFileTest, ApplyTargetSsimulacra2AcceptsUpperBound) {
  WorkerConfig config;
  json body = {{"target_ssimulacra2", 100.0}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.applied.contains("target_ssimulacra2"));
  EXPECT_FLOAT_EQ(config.target_ssimulacra2, 100.0f);
}

// --- Security limits: max_url_length ---

TEST_F(ConfigFileTest, ApplyMaxUrlLengthRejectsTooSmall) {
  WorkerConfig config;
  json body = {{"max_url_length", 32u}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.rejected.contains("max_url_length"));
}

TEST_F(ConfigFileTest, ApplyMaxUrlLengthRejectsTooLarge) {
  WorkerConfig config;
  json body = {{"max_url_length", 100000u}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.rejected.contains("max_url_length"));
}

TEST_F(ConfigFileTest, ApplyMaxUrlLengthAcceptsBoundaries) {
  WorkerConfig config;
  json body = {{"max_url_length", 64u}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.applied.contains("max_url_length"));
  EXPECT_EQ(config.max_url_length, 64u);

  body = {{"max_url_length", 65536u}};
  result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.applied.contains("max_url_length"));
  EXPECT_EQ(config.max_url_length, 65536u);
}

// --- Security limits: max_html_size ---

TEST_F(ConfigFileTest, ApplyMaxHtmlSizeRejectsTooSmall) {
  WorkerConfig config;
  json body = {{"max_html_size", 512u}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.rejected.contains("max_html_size"));
}

TEST_F(ConfigFileTest, ApplyMaxHtmlSizeRejectsTooLarge) {
  WorkerConfig config;
  json body = {{"max_html_size", 128u * 1024u * 1024u}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.rejected.contains("max_html_size"));
}

TEST_F(ConfigFileTest, ApplyMaxHtmlSizeAcceptsBoundaries) {
  WorkerConfig config;
  json body = {{"max_html_size", 1024u}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.applied.contains("max_html_size"));
  EXPECT_EQ(config.max_html_size, 1024u);

  body = {{"max_html_size", 64u * 1024u * 1024u}};
  result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.applied.contains("max_html_size"));
  EXPECT_EQ(config.max_html_size, 64u * 1024u * 1024u);
}

// --- Security limits: max_css_size ---

TEST_F(ConfigFileTest, ApplyMaxCssSizeRejectsTooSmall) {
  WorkerConfig config;
  json body = {{"max_css_size", 100u}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.rejected.contains("max_css_size"));
}

TEST_F(ConfigFileTest, ApplyMaxCssSizeRejectsTooLarge) {
  WorkerConfig config;
  json body = {{"max_css_size", 128u * 1024u * 1024u}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.rejected.contains("max_css_size"));
}

// --- Security limits: max_js_size ---

TEST_F(ConfigFileTest, ApplyMaxJsSizeRejectsTooSmall) {
  WorkerConfig config;
  json body = {{"max_js_size", 500u}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.rejected.contains("max_js_size"));
}

TEST_F(ConfigFileTest, ApplyMaxJsSizeRejectsTooLarge) {
  WorkerConfig config;
  json body = {{"max_js_size", 128u * 1024u * 1024u}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.rejected.contains("max_js_size"));
}

// --- Security limits: max_image_size ---

TEST_F(ConfigFileTest, ApplyMaxImageSizeRejectsTooSmall) {
  WorkerConfig config;
  json body = {{"max_image_size", 256u}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.rejected.contains("max_image_size"));
}

TEST_F(ConfigFileTest, ApplyMaxImageSizeRejectsTooLarge) {
  WorkerConfig config;
  json body = {{"max_image_size", 512u * 1024u * 1024u}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.rejected.contains("max_image_size"));
}

TEST_F(ConfigFileTest, ApplyMaxImageSizeAcceptsBoundaries) {
  WorkerConfig config;
  json body = {{"max_image_size", 1024u}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.applied.contains("max_image_size"));
  EXPECT_EQ(config.max_image_size, 1024u);

  body = {{"max_image_size", 256u * 1024u * 1024u}};
  result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.applied.contains("max_image_size"));
  EXPECT_EQ(config.max_image_size, 256u * 1024u * 1024u);
}

// --- SVG config validation rejections ---

TEST_F(ConfigFileTest, ApplySvgMaxPixelsRejectsOutOfRange) {
  WorkerConfig config;
  json body = {{"svg_max_pixels", 0}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.rejected.contains("svg_max_pixels"));

  body = {{"svg_max_pixels", 20000000}};
  result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.rejected.contains("svg_max_pixels"));
}

TEST_F(ConfigFileTest, ApplySvgPresetRejectsOutOfRange) {
  WorkerConfig config;
  json body = {{"svg_preset", -1}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.rejected.contains("svg_preset"));

  body = {{"svg_preset", 5}};
  result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.rejected.contains("svg_preset"));
}

TEST_F(ConfigFileTest, ApplySvgColorPrecisionRejectsOutOfRange) {
  WorkerConfig config;
  json body = {{"svg_color_precision", -1}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.rejected.contains("svg_color_precision"));

  body = {{"svg_color_precision", 10}};
  result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.rejected.contains("svg_color_precision"));
}

TEST_F(ConfigFileTest, ApplySvgFilterSpeckleRejectsOutOfRange) {
  WorkerConfig config;
  json body = {{"svg_filter_speckle", -1}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.rejected.contains("svg_filter_speckle"));

  body = {{"svg_filter_speckle", 2000}};
  result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.rejected.contains("svg_filter_speckle"));
}

TEST_F(ConfigFileTest, ApplySvgMaxPathsRejectsOutOfRange) {
  WorkerConfig config;
  json body = {{"svg_max_paths", 0}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.rejected.contains("svg_max_paths"));

  body = {{"svg_max_paths", 200000}};
  result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.rejected.contains("svg_max_paths"));
}

TEST_F(ConfigFileTest, ApplySvgMaxSvgBytesRejectsOutOfRange) {
  WorkerConfig config;
  json body = {{"svg_max_svg_bytes", 100}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.rejected.contains("svg_max_svg_bytes"));

  body = {{"svg_max_svg_bytes", 32 * 1024 * 1024}};
  result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.rejected.contains("svg_max_svg_bytes"));
}

TEST_F(ConfigFileTest, ApplySvgFidelityThresholdRejectsOutOfRange) {
  WorkerConfig config;
  json body = {{"svg_fidelity_threshold", -1.0}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.rejected.contains("svg_fidelity_threshold"));

  body = {{"svg_fidelity_threshold", 101.0}};
  result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.rejected.contains("svg_fidelity_threshold"));

  body = {{"svg_fidelity_threshold", std::numeric_limits<double>::quiet_NaN()}};
  result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.rejected.contains("svg_fidelity_threshold"));
}

TEST_F(ConfigFileTest, ApplySvgTimeoutMsRejectsOutOfRange) {
  WorkerConfig config;
  json body = {{"svg_timeout_ms", 5}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.rejected.contains("svg_timeout_ms"));

  body = {{"svg_timeout_ms", 70000}};
  result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.rejected.contains("svg_timeout_ms"));
}

// --- Float field rejections ---

TEST_F(ConfigFileTest, ApplySavedataScoreReductionRejectsOutOfRange) {
  WorkerConfig config;
  json body = {{"savedata_score_reduction", -1.0}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.rejected.contains("savedata_score_reduction"));

  body = {{"savedata_score_reduction", 60.0}};
  result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.rejected.contains("savedata_score_reduction"));
}

TEST_F(ConfigFileTest, ApplySsimulacra2ToleranceRejectsOutOfRange) {
  WorkerConfig config;
  json body = {{"ssimulacra2_tolerance", -1.0}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.rejected.contains("ssimulacra2_tolerance"));

  body = {{"ssimulacra2_tolerance", 55.0}};
  result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.rejected.contains("ssimulacra2_tolerance"));

  body = {{"ssimulacra2_tolerance", std::numeric_limits<double>::infinity()}};
  result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.rejected.contains("ssimulacra2_tolerance"));
}

// --- Warnings for low quality on WebP/AVIF ---

TEST_F(ConfigFileTest, ApplyWarningsOnLowWebpQuality) {
  WorkerConfig config;
  json body = {{"webp_quality", 10}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_EQ(config.webp_quality, 10);
  EXPECT_FALSE(result.warnings.empty());
}

TEST_F(ConfigFileTest, ApplyWarningsOnLowAvifQuality) {
  WorkerConfig config;
  json body = {{"avif_quality", 5}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_EQ(config.avif_quality, 5);
  EXPECT_FALSE(result.warnings.empty());
}

// --- Compression levels ---

TEST_F(ConfigFileTest, ApplyCompressionLevelsClamped) {
  WorkerConfig config;
  json body = {{"gzip_level", -5}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_EQ(config.gzip_level, 0);  // clamped to min
  EXPECT_TRUE(result.applied.contains("gzip_level"));

  body = {{"gzip_level", 15}};
  result = ApplyConfigJson(body, &config);
  EXPECT_EQ(config.gzip_level, 9);  // clamped to max

  body = {{"brotli_level", -1}};
  result = ApplyConfigJson(body, &config);
  EXPECT_EQ(config.brotli_level, 0);

  body = {{"brotli_level", 20}};
  result = ApplyConfigJson(body, &config);
  EXPECT_EQ(config.brotli_level, 11);
}

// --- Multiple fields in a single request ---

TEST_F(ConfigFileTest, ApplyMultipleFieldsMixedResults) {
  WorkerConfig config;
  json body = {
      {"jpeg_quality", 90},         // Applied
      {"cache_path", "/new/path"},  // Rejected (non-reloadable)
      {"svg_max_pixels", 0},        // Rejected (out of range)
      {"disable_html", true},       // Applied
  };
  auto result = ApplyConfigJson(body, &config);
  EXPECT_EQ(config.jpeg_quality, 90);
  EXPECT_EQ(config.html_mode, HtmlMode::kOff);
  EXPECT_TRUE(result.applied.contains("jpeg_quality"));
  EXPECT_TRUE(result.applied.contains("disable_html"));
  EXPECT_TRUE(result.rejected.contains("cache_path"));
  EXPECT_TRUE(result.rejected.contains("svg_max_pixels"));
}

// --- Missing IsNonReloadable assertions ---

TEST_F(ConfigFileTest, NonReloadableFieldsComplete) {
  // Verify ALL non-reloadable fields are checked (some were missing).
  EXPECT_TRUE(IsNonReloadable("max_buffer_size"));
  EXPECT_TRUE(IsNonReloadable("connection_timeout_ms"));
  EXPECT_TRUE(IsNonReloadable("shutdown_timeout_ms"));
}

// --- Individual savedata quality apply tests ---

TEST_F(ConfigFileTest, ApplySavedataJpegQuality) {
  WorkerConfig config;
  json body = {{"savedata_jpeg_quality", 50}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_EQ(config.savedata_jpeg_quality, 50);
  EXPECT_TRUE(result.applied.contains("savedata_jpeg_quality"));
  EXPECT_TRUE(result.rejected.empty());
}

TEST_F(ConfigFileTest, ApplySavedataJpegQualityClamped) {
  WorkerConfig config;
  json body = {{"savedata_jpeg_quality", 0}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_EQ(config.savedata_jpeg_quality, 1);  // clamped to min 1
  EXPECT_TRUE(result.applied.contains("savedata_jpeg_quality"));
}

TEST_F(ConfigFileTest, ApplySavedataWebpQuality) {
  WorkerConfig config;
  json body = {{"savedata_webp_quality", 40}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_EQ(config.savedata_webp_quality, 40);
  EXPECT_TRUE(result.applied.contains("savedata_webp_quality"));
}

TEST_F(ConfigFileTest, ApplySavedataWebpQualityClamped) {
  WorkerConfig config;
  json body = {{"savedata_webp_quality", -5}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_EQ(config.savedata_webp_quality, 0);  // clamped to min 0
}

TEST_F(ConfigFileTest, ApplySavedataAvifQuality) {
  WorkerConfig config;
  json body = {{"savedata_avif_quality", 35}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_EQ(config.savedata_avif_quality, 35);
  EXPECT_TRUE(result.applied.contains("savedata_avif_quality"));
}

TEST_F(ConfigFileTest, ApplySavedataAvifQualityClamped) {
  WorkerConfig config;
  json body = {{"savedata_avif_quality", 200}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_EQ(config.savedata_avif_quality, 100);  // clamped to max 100
}

// --- WebP/AVIF quality boundary clamping ---

TEST_F(ConfigFileTest, ApplyWebpQualityClamped) {
  WorkerConfig config;
  // Lower bound: webp_quality allows 0 (lossless flag).
  json body = {{"webp_quality", -5}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_EQ(config.webp_quality, 0);  // clamped to min 0
  EXPECT_TRUE(result.applied.contains("webp_quality"));

  body = {{"webp_quality", 150}};
  result = ApplyConfigJson(body, &config);
  EXPECT_EQ(config.webp_quality, 100);  // clamped to max 100
}

TEST_F(ConfigFileTest, ApplyAvifQualityClamped) {
  WorkerConfig config;
  json body = {{"avif_quality", -5}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_EQ(config.avif_quality, 0);  // clamped to min 0
  EXPECT_TRUE(result.applied.contains("avif_quality"));

  body = {{"avif_quality", 150}};
  result = ApplyConfigJson(body, &config);
  EXPECT_EQ(config.avif_quality, 100);  // clamped to max 100
}

// --- JPEG quality clamp lower bound ---

TEST_F(ConfigFileTest, ApplyJpegQualityClampedLowerBound) {
  WorkerConfig config;
  json body = {{"jpeg_quality", -5}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_EQ(config.jpeg_quality, 1);  // clamped to min 1
  EXPECT_TRUE(result.applied.contains("jpeg_quality"));
}

// --- Direct svg_mode apply for detect and preview ---

TEST_F(ConfigFileTest, ApplySvgModeDetect) {
  WorkerConfig config;
  config.svg_mode = SvgMode::kAuto;  // start at non-default
  json body = {{"svg_mode", "detect"}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_EQ(config.svg_mode, SvgMode::kDetect);
  EXPECT_EQ(result.applied["svg_mode"], "detect");
}

TEST_F(ConfigFileTest, ApplySvgModePreview) {
  WorkerConfig config;
  json body = {{"svg_mode", "preview"}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_EQ(config.svg_mode, SvgMode::kPreview);
  EXPECT_EQ(result.applied["svg_mode"], "preview");
}

// --- Individual boolean toggles (ensure direct coverage) ---

TEST_F(ConfigFileTest, ApplyProactiveImageVariants) {
  WorkerConfig config;
  EXPECT_FALSE(config.proactive_image_variants);
  json body = {{"proactive_image_variants", true}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(config.proactive_image_variants);
  EXPECT_EQ(result.applied["proactive_image_variants"], true);
}

TEST_F(ConfigFileTest, ApplyProactiveViewportVariants) {
  WorkerConfig config;
  EXPECT_TRUE(config.proactive_viewport_variants);
  json body = {{"proactive_viewport_variants", false}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_FALSE(config.proactive_viewport_variants);
  EXPECT_EQ(result.applied["proactive_viewport_variants"], false);
}

TEST_F(ConfigFileTest, ApplyProactiveSavedataVariants) {
  WorkerConfig config;
  EXPECT_TRUE(config.proactive_savedata_variants);
  json body = {{"proactive_savedata_variants", false}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_FALSE(config.proactive_savedata_variants);
}

TEST_F(ConfigFileTest, ApplyProactiveDensityVariants) {
  WorkerConfig config;
  EXPECT_TRUE(config.proactive_density_variants);
  json body = {{"proactive_density_variants", false}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_FALSE(config.proactive_density_variants);
}

TEST_F(ConfigFileTest, ApplyContentAnalysis) {
  WorkerConfig config;
  EXPECT_TRUE(config.content_analysis);
  json body = {{"content_analysis", false}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_FALSE(config.content_analysis);
  EXPECT_EQ(result.applied["content_analysis"], false);
}

TEST_F(ConfigFileTest, ApplyQualityVerify) {
  WorkerConfig config;
  EXPECT_TRUE(config.quality_verify);
  json body = {{"quality_verify", false}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_FALSE(config.quality_verify);
}

TEST_F(ConfigFileTest, ApplyLearnedQualityToggles) {
  WorkerConfig config;
  EXPECT_TRUE(config.learned_quality);
  EXPECT_TRUE(config.learned_quality_jpeg);
  EXPECT_TRUE(config.learned_quality_webp);
  EXPECT_TRUE(config.learned_quality_avif);

  json body = {{"learned_quality", false},
               {"learned_quality_jpeg", false},
               {"learned_quality_webp", false},
               {"learned_quality_avif", false}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_FALSE(config.learned_quality);
  EXPECT_FALSE(config.learned_quality_jpeg);
  EXPECT_FALSE(config.learned_quality_webp);
  EXPECT_FALSE(config.learned_quality_avif);
}

TEST_F(ConfigFileTest, ApplyDisableCss) {
  WorkerConfig config;
  json body = {{"disable_css", true}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(config.disable_css);
  EXPECT_EQ(result.applied["disable_css"], true);
}

TEST_F(ConfigFileTest, ApplyDisableJs) {
  WorkerConfig config;
  json body = {{"disable_js", true}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(config.disable_js);
}

TEST_F(ConfigFileTest, ApplyDisableImage) {
  WorkerConfig config;
  json body = {{"disable_image", true}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(config.disable_image);
}

TEST_F(ConfigFileTest, ApplyDisableLazyLoad) {
  WorkerConfig config;
  json body = {{"disable_lazy_load", true}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(config.disable_lazy_load);
}

TEST_F(ConfigFileTest, ApplyDisableImageDimensions) {
  WorkerConfig config;
  json body = {{"disable_image_dimensions", true}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(config.disable_image_dimensions);
}

TEST_F(ConfigFileTest, ApplyDisableLcpPreload) {
  WorkerConfig config;
  json body = {{"disable_lcp_preload", true}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(config.disable_lcp_preload);
}

TEST_F(ConfigFileTest, ApplyDisablePreconnectInjection) {
  WorkerConfig config;
  json body = {{"disable_preconnect_injection", true}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(config.disable_preconnect_injection);
}

TEST_F(ConfigFileTest, ApplyEnableSpeculationRules) {
  WorkerConfig config;
  json body = {{"enable_speculation_rules", true}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(config.enable_speculation_rules);
}

TEST_F(ConfigFileTest, ApplyDisableCssImportFlattening) {
  WorkerConfig config;
  json body = {{"disable_css_import_flattening", true}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(config.disable_css_import_flattening);
}

TEST_F(ConfigFileTest, ApplyEnableWarmup) {
  WorkerConfig config;
  // Default is true, toggle it off.
  json body = {{"enable_warmup", false}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_FALSE(config.enable_warmup);
  EXPECT_EQ(result.applied["enable_warmup"], false);
}

// --- SVG integer config fields: direct apply ---

TEST_F(ConfigFileTest, ApplySvgCandidacyThreshold) {
  WorkerConfig config;
  json body = {{"svg_candidacy_threshold", 75}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_EQ(config.svg_candidacy_threshold, 75);
  EXPECT_TRUE(result.applied.contains("svg_candidacy_threshold"));
}

TEST_F(ConfigFileTest, ApplySvgCandidacyThresholdClamped) {
  WorkerConfig config;
  json body = {{"svg_candidacy_threshold", 200}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_EQ(config.svg_candidacy_threshold, 100);  // clamped
}

TEST_F(ConfigFileTest, ApplySvgMaxPixelsValid) {
  WorkerConfig config;
  json body = {{"svg_max_pixels", 131072}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_EQ(config.svg_max_pixels, 131072);
  EXPECT_TRUE(result.applied.contains("svg_max_pixels"));
}

TEST_F(ConfigFileTest, ApplySvgPresetValid) {
  WorkerConfig config;
  json body = {{"svg_preset", 2}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_EQ(config.svg_preset, 2);
  EXPECT_TRUE(result.applied.contains("svg_preset"));
}

TEST_F(ConfigFileTest, ApplySvgColorPrecisionValid) {
  WorkerConfig config;
  json body = {{"svg_color_precision", 6}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_EQ(config.svg_color_precision, 6);
}

TEST_F(ConfigFileTest, ApplySvgFilterSpeckleValid) {
  WorkerConfig config;
  json body = {{"svg_filter_speckle", 8}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_EQ(config.svg_filter_speckle, 8);
}

TEST_F(ConfigFileTest, ApplySvgMaxPathsValid) {
  WorkerConfig config;
  json body = {{"svg_max_paths", 1000}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_EQ(config.svg_max_paths, 1000);
}

TEST_F(ConfigFileTest, ApplySvgMaxSvgBytesValid) {
  WorkerConfig config;
  json body = {{"svg_max_svg_bytes", 524288}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_EQ(config.svg_max_svg_bytes, 524288);
}

TEST_F(ConfigFileTest, ApplySvgFidelityThresholdValid) {
  WorkerConfig config;
  json body = {{"svg_fidelity_threshold", 65.0}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_FLOAT_EQ(config.svg_fidelity_threshold, 65.0f);
  EXPECT_TRUE(result.applied.contains("svg_fidelity_threshold"));
}

TEST_F(ConfigFileTest, ApplySvgExcludeLcp) {
  WorkerConfig config;
  EXPECT_TRUE(config.svg_exclude_lcp);  // default true
  json body = {{"svg_exclude_lcp", false}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_FALSE(config.svg_exclude_lcp);
  EXPECT_EQ(result.applied["svg_exclude_lcp"], false);
}

TEST_F(ConfigFileTest, ApplySvgTimeoutMsValid) {
  WorkerConfig config;
  json body = {{"svg_timeout_ms", 1000}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_EQ(config.svg_timeout_ms, 1000);
  EXPECT_TRUE(result.applied.contains("svg_timeout_ms"));
}

// --- Security limits: direct valid apply tests ---

TEST_F(ConfigFileTest, ApplyMaxUrlLengthValid) {
  WorkerConfig config;
  json body = {{"max_url_length", 4096u}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_EQ(config.max_url_length, 4096u);
  EXPECT_TRUE(result.applied.contains("max_url_length"));
}

TEST_F(ConfigFileTest, ApplyMaxCssSizeValid) {
  WorkerConfig config;
  json body = {{"max_css_size", 4u * 1024u * 1024u}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_EQ(config.max_css_size, 4u * 1024u * 1024u);
  EXPECT_TRUE(result.applied.contains("max_css_size"));
}

TEST_F(ConfigFileTest, ApplyMaxJsSizeValid) {
  WorkerConfig config;
  json body = {{"max_js_size", 4u * 1024u * 1024u}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_EQ(config.max_js_size, 4u * 1024u * 1024u);
  EXPECT_TRUE(result.applied.contains("max_js_size"));
}

// --- Float fields: valid apply paths ---

// The removed denoise options still load: they are ignored with a warning,
// so a config file or API call that sets them keeps working.
TEST_F(ConfigFileTest, RemovedDenoiseOptionsLoadAsDeprecated) {
  WorkerConfig config;
  json body = {{"denoise_threshold", 0.5},
               {"denoise_sigma_spatial", 5.0},
               {"denoise_sigma_range", 30.0},
               {"jpeg_quality", 77}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_FALSE(result.rejected.contains("denoise_threshold"));
  EXPECT_FALSE(result.rejected.contains("denoise_sigma_spatial"));
  EXPECT_FALSE(result.rejected.contains("denoise_sigma_range"));
  EXPECT_EQ(3u, result.warnings.size());
  EXPECT_TRUE(result.applied.contains("jpeg_quality"));
  EXPECT_EQ(77, config.jpeg_quality);
}

TEST_F(ConfigFileTest, ApplySavedataScoreReductionValid) {
  WorkerConfig config;
  json body = {{"savedata_score_reduction", 20.0}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_FLOAT_EQ(config.savedata_score_reduction, 20.0f);
  EXPECT_TRUE(result.applied.contains("savedata_score_reduction"));
}

TEST_F(ConfigFileTest, ApplySsimulacra2ToleranceValid) {
  WorkerConfig config;
  json body = {{"ssimulacra2_tolerance", 8.0}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_FLOAT_EQ(config.ssimulacra2_tolerance, 8.0f);
  EXPECT_TRUE(result.applied.contains("ssimulacra2_tolerance"));
}

// --- Write failure: non-existent directory ---

TEST_F(ConfigFileTest, WriteFailsForNonexistentDirectory) {
  EXPECT_FALSE(
      WriteConfigFile("/nonexistent/directory/pagespeed.json", {{"a", 1}}));
}

// --- ReadConfigFile with arrays in nesting ---

TEST_F(ConfigFileTest, ReadRejectsArrayNesting) {
  std::string path = (tmp_dir_ / "deep_array.json").string();
  {
    std::ofstream f(path);
    for (int i = 0; i < 50; ++i) f << "[";
    f << "1";
    for (int i = 0; i < 50; ++i) f << "]";
  }
  std::string error;
  auto j = ReadConfigFile(path, &error);
  EXPECT_TRUE(j.is_object());
  EXPECT_TRUE(j.empty());
  EXPECT_FALSE(error.empty());
  EXPECT_NE(error.find("nesting"), std::string::npos);
}

// --- Apply rejects wrong types for specific fields ---

TEST_F(ConfigFileTest, ApplyRejectsBooleanAsInteger) {
  WorkerConfig config;
  json body = {{"disable_html", 1}};  // integer instead of boolean
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.rejected.contains("disable_html"));
}

TEST_F(ConfigFileTest, ApplyRejectsStringAsInteger) {
  WorkerConfig config;
  json body = {{"webp_quality", "high"}};
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.rejected.contains("webp_quality"));
}

TEST_F(ConfigFileTest, ApplyRejectsIntegerAsBoolean) {
  WorkerConfig config;
  json body = {{"svg_mode", 42}};  // integer instead of string
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.rejected.contains("svg_mode"));
}

TEST_F(ConfigFileTest, ApplyRejectsFloatForIntegerField) {
  WorkerConfig config;
  json body = {{"gzip_level", 5.5}};  // float instead of integer
  auto result = ApplyConfigJson(body, &config);
  EXPECT_TRUE(result.rejected.contains("gzip_level"));
}

#ifndef _WIN32
TEST_F(ConfigFileTest, OpenRejectsSymlink) {
  // ps_open_exclusive_nofollow must refuse to follow symlinks even when the
  // target does not exist — preventing attacker-controlled file creation.
  std::string target = (tmp_dir_ / "target").string();
  std::string link = (tmp_dir_ / "link").string();
  ASSERT_EQ(::symlink(target.c_str(), link.c_str()), 0);
  int fd = ps_open_exclusive_nofollow(link.c_str(), 0644);
  EXPECT_EQ(fd, -1);
  // Target must not have been created (symlink was not followed).
  EXPECT_FALSE(std::filesystem::exists(target));
}
#endif

}  // namespace
}  // namespace pagespeed
