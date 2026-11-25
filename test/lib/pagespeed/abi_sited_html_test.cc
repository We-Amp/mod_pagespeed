// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The PS_API 1.13 site-scoped HTML store surface: the sited, paired headers
// sidecar and its block accessors, whole-key removal in one site, the
// per-site purge epoch, the origin_html_hash write field, the withheld
// marker id and the published headers-sidecar flag bits.

#include <stddef.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "lib/cache/cache.h"
#include "lib/classify/alternate_id.h"
#include "lib/classify/alternate_metadata.h"
#include "lib/pagespeed/pagespeed.h"
#include "test/test_util/temp_dir.h"

namespace {

const std::string kSiteA(PS_SITE_ID_CHARS, 'a');
const std::string kSiteB = std::string(PS_SITE_ID_CHARS - 1, 'a') + "b";
constexpr char kUrl[] = "/page.html";
constexpr char kHost[] = "example.com";
constexpr char kScheme[] = "https";

std::array<uint8_t, 32> Hash(uint8_t seed) {
  std::array<uint8_t, 32> h{};
  for (size_t i = 0; i < h.size(); ++i) {
    h[i] = static_cast<uint8_t>(seed + i + 1);
  }
  return h;
}

uint64_t Now() { return static_cast<uint64_t>(std::time(nullptr)); }

class AbiSitedHtmlTest : public ::testing::Test {
 protected:
  void SetUp() override {
    temp_dir_ = pagespeed::test::MakeTempDir();
    path_ = temp_dir_ + "/cache.vol";
    cache_ = Open();
    ASSERT_NE(cache_, nullptr);
  }

  void TearDown() override {
    for (ps_cache_t* c : extra_) ps_cache_close(c);
    if (cache_ != nullptr) ps_cache_close(cache_);
    std::filesystem::remove_all(temp_dir_);
  }

  // A handle on the same volume, as a second process opens it.
  ps_cache_t* Open(size_t ram_cache_size = 0) {
    ps_cache_config_t config;
    ps_cache_config_init(&config);
    config.volume_path = path_.c_str();
    config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
    config.ram_cache_size = ram_cache_size;
    ps_cache_t* c = nullptr;
    if (ps_cache_open(&config, &c) != PS_OK) return nullptr;
    return c;
  }
  ps_cache_t* OpenExtra(size_t ram_cache_size = 0) {
    ps_cache_t* c = Open(ram_cache_size);
    if (c != nullptr) extra_.push_back(c);
    return c;
  }

  ps_sidecar_write_params_t SidecarParams(const char* site,
                                          const uint8_t* hash) {
    ps_sidecar_write_params_t p;
    std::memset(&p, 0, sizeof(p));
    p.struct_size = sizeof(p);
    p.url = kUrl;
    p.hostname = kHost;
    p.scheme = kScheme;
    p.site = site;
    p.origin_html_hash = hash;
    return p;
  }

  // Writes a durable original with a params struct of `struct_size` bytes.
  ps_error_t WriteOriginal(ps_cache_t* c, const char* site,
                           const std::string& body, const uint8_t* hash,
                           size_t struct_size = sizeof(ps_write_params_t)) {
    ps_write_params_t params;
    ps_write_params_init_sized(&params, struct_size);
    params.content_length = body.size();
    params.content_type = PS_CONTENT_HTML;
    params.origin_ct = "text/html; charset=utf-8";
    params.flags = PS_FLAG_HEADERS_CLASSIFIED;
    if (struct_size >= sizeof(ps_write_params_t)) {
      params.origin_html_hash = hash;
    }
    ps_write_handle_t* wh = nullptr;
    ps_error_t err = ps_cache_write_original_site(c, kUrl, kHost, kScheme, site,
                                                  &params, &wh);
    if (err != PS_OK) return err;
    err = ps_write_data(wh, body.data(), body.size());
    if (err != PS_OK) {
      ps_write_abort(wh);
      return err;
    }
    return ps_write_close(wh);
  }

  // The stamp on the stored original, or "" when unbound / missing.
  std::string ReadStamp(ps_cache_t* c, const char* site) {
    ps_read_result_t* r = nullptr;
    if (ps_cache_read_alternate_site(c, kUrl, kHost, kScheme, site,
                                     PS_SENTINEL_ORIGINAL, &r) != PS_OK) {
      return "<miss>";
    }
    const uint8_t* h = ps_read_origin_html_hash(r);
    std::string out =
        h == nullptr ? "" : std::string(reinterpret_cast<const char*>(h), 32);
    ps_read_free(r);
    return out;
  }

  bool Has(ps_cache_t* c, const char* site, uint8_t id) {
    ps_read_result_t* r = nullptr;
    const ps_error_t err =
        ps_cache_read_alternate_site(c, kUrl, kHost, kScheme, site, id, &r);
    if (err == PS_OK) ps_read_free(r);
    return err == PS_OK;
  }

  std::string temp_dir_;
  std::string path_;
  ps_cache_t* cache_ = nullptr;
  std::vector<ps_cache_t*> extra_;
};

TEST(AbiSitedHtmlVersion, TheSurfaceIsPsApi113) {
  EXPECT_EQ(ps_version_major(), 1);
  EXPECT_GE(ps_version_minor(), 13);
  EXPECT_EQ(PS_FLAG_HEADERS_CLASSIFIED, 0x10);
  EXPECT_EQ(PS_FLAG_HEADERS_SIDECAR_REQUIRED, 0x20);
  EXPECT_EQ(PS_SENTINEL_MARKDOWN_WITHHELD, 0xCC);
  EXPECT_EQ(PS_SENTINEL_MARKDOWN_WITHHELD,
            static_cast<int>(pagespeed::SentinelId::kAgentMarkdownWithheld));
}

// ------------------------------------------------------------ sited sidecar

TEST_F(AbiSitedHtmlTest, ASitedSidecarRoundTripsByteExact) {
  const char* names[] = {"Content-Type", "Referrer-Policy",
                         "X-Content-Type-Options", "Link"};
  const char* values[] = {"text/html", "  strict-origin \t", "nosniff",
                          "</a.css>; rel=preload"};
  const auto h = Hash(1);
  const auto params = SidecarParams(kSiteA.c_str(), h.data());
  int verdict = -1;
  int stored = -1;
  ASSERT_EQ(PS_OK, ps_cache_write_headers_sidecar_site(
                       cache_, &params, names, values, 4, &verdict, &stored));
  EXPECT_EQ(verdict, PS_SIDECAR_SWAP_ELIGIBLE);
  EXPECT_EQ(stored, 1);

  ps_header_block_t* block = nullptr;
  ASSERT_EQ(PS_OK, ps_headers_sidecar_read(cache_, kUrl, kHost, kScheme,
                                           kSiteA.c_str(), &block));
  ASSERT_NE(block, nullptr);
  // Content-Type is reproduced by the entry metadata and is not carried.
  ASSERT_EQ(ps_header_block_count(block), 3u);
  const std::vector<std::pair<std::string, std::string>> want = {
      {"Referrer-Policy", "  strict-origin \t"},
      {"X-Content-Type-Options", "nosniff"},
      {"Link", "</a.css>; rel=preload"}};
  for (size_t i = 0; i < want.size(); ++i) {
    const char* n = nullptr;
    const char* v = nullptr;
    size_t nl = 0;
    size_t vl = 0;
    ASSERT_EQ(PS_OK, ps_header_block_get(block, i, &n, &nl, &v, &vl));
    EXPECT_EQ(std::string(n, nl), want[i].first);
    EXPECT_EQ(std::string(v, vl), want[i].second);
    EXPECT_EQ(std::strlen(n), nl);
    EXPECT_EQ(std::strlen(v), vl);
  }
  EXPECT_EQ(PS_ERR_INVALID_ARG,
            ps_header_block_get(block, 3, nullptr, nullptr, nullptr, nullptr));
  const uint8_t* ph = ps_header_block_pairing_hash(block);
  ASSERT_NE(ph, nullptr);
  EXPECT_EQ(0, std::memcmp(ph, h.data(), 32));
  ps_header_block_free(block);

  // The raw blob read by id is the paired format, version byte first.
  ps_read_result_t* r = nullptr;
  ASSERT_EQ(PS_OK, ps_cache_read_alternate_site(
                       cache_, kUrl, kHost, kScheme, kSiteA.c_str(),
                       PS_SENTINEL_HEADERS_SIDECAR, &r));
  const uint8_t* data = nullptr;
  size_t len = 0;
  ASSERT_EQ(PS_OK, ps_read_content(r, &data, &len));
  ASSERT_GT(len, 33u);
  EXPECT_EQ(data[0], 2);
  EXPECT_EQ(0, std::memcmp(data + 1, h.data(), 32));
  ps_read_free(r);
}

TEST_F(AbiSitedHtmlTest, AnUnsitedOrOtherSiteReadMissesASitedSidecar) {
  const char* names[] = {"Referrer-Policy"};
  const char* values[] = {"no-referrer"};
  const auto params = SidecarParams(kSiteA.c_str(), nullptr);
  ASSERT_EQ(PS_OK, ps_cache_write_headers_sidecar_site(
                       cache_, &params, names, values, 1, nullptr, nullptr));

  ps_header_block_t* block = reinterpret_cast<ps_header_block_t*>(0x1);
  EXPECT_EQ(
      PS_ERR_NOT_FOUND,
      ps_headers_sidecar_read(cache_, kUrl, kHost, kScheme, nullptr, &block));
  EXPECT_EQ(block, nullptr);
  EXPECT_EQ(PS_ERR_NOT_FOUND,
            ps_headers_sidecar_read(cache_, kUrl, kHost, kScheme,
                                    kSiteB.c_str(), &block));
  ps_read_result_t* r = nullptr;
  EXPECT_EQ(PS_ERR_NOT_FOUND,
            ps_cache_read_alternate(cache_, kUrl, kHost, kScheme,
                                    PS_SENTINEL_HEADERS_SIDECAR, &r));
  ASSERT_EQ(PS_OK, ps_headers_sidecar_read(cache_, kUrl, kHost, kScheme,
                                           kSiteA.c_str(), &block));
  EXPECT_EQ(ps_header_block_pairing_hash(block), nullptr);
  ps_header_block_free(block);
}

TEST_F(AbiSitedHtmlTest, PairingTellsTheStoredOriginalFromAnother) {
  const auto h1 = Hash(10);
  const auto h2 = Hash(20);
  const char* names[] = {"Referrer-Policy"};
  const char* values[] = {"no-referrer"};

  // Stored pair: block and original carry the same stamp.
  auto params = SidecarParams(kSiteA.c_str(), h1.data());
  ASSERT_EQ(PS_OK, ps_cache_write_headers_sidecar_site(
                       cache_, &params, names, values, 1, nullptr, nullptr));
  ASSERT_EQ(PS_OK,
            WriteOriginal(cache_, kSiteA.c_str(), "<html>1</html>", h1.data()));

  auto pairing = [&]() {
    ps_read_result_t* r = nullptr;
    EXPECT_EQ(PS_OK, ps_cache_read_alternate_site(cache_, kUrl, kHost, kScheme,
                                                  kSiteA.c_str(),
                                                  PS_SENTINEL_ORIGINAL, &r));
    ps_header_block_t* block = nullptr;
    EXPECT_EQ(PS_OK, ps_headers_sidecar_read(cache_, kUrl, kHost, kScheme,
                                             kSiteA.c_str(), &block));
    const int result =
        ps_header_block_pairing(block, ps_read_origin_html_hash(r));
    ps_header_block_free(block);
    ps_read_free(r);
    return result;
  };
  EXPECT_EQ(pairing(), PS_SIDECAR_PAIRED);

  // A re-store writes the new block first: until the original's write lands,
  // the block belongs to another original.
  params = SidecarParams(kSiteA.c_str(), h2.data());
  ASSERT_EQ(PS_OK, ps_cache_write_headers_sidecar_site(
                       cache_, &params, names, values, 1, nullptr, nullptr));
  EXPECT_EQ(pairing(), PS_SIDECAR_UNPAIRED);
  ASSERT_EQ(PS_OK,
            WriteOriginal(cache_, kSiteA.c_str(), "<html>2</html>", h2.data()));
  EXPECT_EQ(pairing(), PS_SIDECAR_PAIRED);

  // An unpaired block never pairs, and nothing pairs with an unbound original.
  params = SidecarParams(kSiteA.c_str(), nullptr);
  ASSERT_EQ(PS_OK, ps_cache_write_headers_sidecar_site(
                       cache_, &params, names, values, 1, nullptr, nullptr));
  EXPECT_EQ(pairing(), PS_SIDECAR_UNPAIRED);
  ps_header_block_t* block = nullptr;
  params = SidecarParams(kSiteA.c_str(), h2.data());
  ASSERT_EQ(PS_OK, ps_cache_write_headers_sidecar_site(
                       cache_, &params, names, values, 1, nullptr, nullptr));
  ASSERT_EQ(PS_OK, ps_headers_sidecar_read(cache_, kUrl, kHost, kScheme,
                                           kSiteA.c_str(), &block));
  EXPECT_EQ(ps_header_block_pairing(block, nullptr), PS_SIDECAR_UNPAIRED);
  EXPECT_EQ(ps_header_block_pairing(nullptr, h2.data()), -1);
  ps_header_block_free(block);
}

TEST_F(AbiSitedHtmlTest, APairedWriteWithNothingToCarryIsStored) {
  const char* names[] = {"Content-Type", "Cache-Control"};
  const char* values[] = {"text/html", "max-age=60"};
  const auto h = Hash(30);
  const auto params = SidecarParams(kSiteA.c_str(), h.data());
  int stored = 0;
  ASSERT_EQ(PS_OK, ps_cache_write_headers_sidecar_site(
                       cache_, &params, names, values, 2, nullptr, &stored));
  EXPECT_EQ(stored, 1);
  ps_header_block_t* block = nullptr;
  ASSERT_EQ(PS_OK, ps_headers_sidecar_read(cache_, kUrl, kHost, kScheme,
                                           kSiteA.c_str(), &block));
  EXPECT_EQ(ps_header_block_count(block), 0u);
  EXPECT_EQ(ps_header_block_pairing(block, h.data()), PS_SIDECAR_PAIRED);
  ps_header_block_free(block);
}

TEST_F(AbiSitedHtmlTest, MalformedSidecarCallsAreRefused) {
  const char* names[] = {"Referrer-Policy"};
  const char* values[] = {"no-referrer"};
  const std::array<uint8_t, 32> zero{};
  int verdict = 77;
  int stored = 77;

  auto params = SidecarParams(kSiteA.c_str(), zero.data());
  EXPECT_EQ(PS_ERR_INVALID_ARG,
            ps_cache_write_headers_sidecar_site(cache_, &params, names, values,
                                                1, &verdict, &stored));
  params = SidecarParams("not-a-site", nullptr);
  EXPECT_EQ(PS_ERR_INVALID_ARG,
            ps_cache_write_headers_sidecar_site(cache_, &params, names, values,
                                                1, &verdict, &stored));
  params = SidecarParams(kSiteA.c_str(), nullptr);
  params.struct_size = sizeof(params) - 1;
  EXPECT_EQ(PS_ERR_INVALID_ARG,
            ps_cache_write_headers_sidecar_site(cache_, &params, names, values,
                                                1, &verdict, &stored));
  params = SidecarParams(kSiteA.c_str(), nullptr);
  params.url = nullptr;
  EXPECT_EQ(PS_ERR_INVALID_ARG,
            ps_cache_write_headers_sidecar_site(cache_, &params, names, values,
                                                1, &verdict, &stored));
  EXPECT_EQ(PS_ERR_INVALID_ARG,
            ps_cache_write_headers_sidecar_site(cache_, nullptr, names, values,
                                                1, &verdict, &stored));
  EXPECT_EQ(verdict, 77);
  EXPECT_EQ(stored, 77);
  EXPECT_EQ(ps_header_block_count(nullptr), 0u);
  EXPECT_EQ(ps_header_block_pairing_hash(nullptr), nullptr);
  ps_header_block_free(nullptr);
}

// ------------------------------------------------------------ origin hash

TEST_F(AbiSitedHtmlTest, An80BytePre113WriteParamsStillWorks) {
  // A caller built against the 1.2..1.12 header passes 80 bytes and never
  // set origin_html_hash: the write succeeds and the original is unbound.
  ASSERT_EQ(PS_OK, WriteOriginal(cache_, kSiteA.c_str(), "<html>old</html>",
                                 nullptr, 80));
  EXPECT_EQ(ReadStamp(cache_, kSiteA.c_str()), "");
}

TEST_F(AbiSitedHtmlTest, TheStampReadsBackOnEveryHandle) {
  const auto h = Hash(40);
  const std::string want(reinterpret_cast<const char*>(h.data()), 32);
  ASSERT_EQ(PS_OK,
            WriteOriginal(cache_, kSiteA.c_str(), "<html>s</html>", h.data()));
  EXPECT_EQ(ReadStamp(cache_, kSiteA.c_str()), want);
  EXPECT_EQ(ReadStamp(cache_, kSiteA.c_str()), want);  // RAM tier
  ps_cache_t* other = OpenExtra();
  ASSERT_NE(other, nullptr);
  EXPECT_EQ(ReadStamp(other, kSiteA.c_str()), want);  // volume only

  // A re-store without a hash replaces the binding with none.
  ASSERT_EQ(PS_OK,
            WriteOriginal(cache_, kSiteA.c_str(), "<html>s</html>", nullptr));
  EXPECT_EQ(ReadStamp(cache_, kSiteA.c_str()), "");
}

// ------------------------------------------------------------ withheld marker

TEST_F(AbiSitedHtmlTest, TheMarkerIsNeitherWritableNorSelectable) {
  ps_write_handle_t* wh = nullptr;
  EXPECT_EQ(PS_ERR_INVALID_ARG,
            ps_cache_write_sentinel(cache_, kUrl, kHost, kScheme,
                                    PS_SENTINEL_MARKDOWN_WITHHELD, 4, &wh));

  // Written the way the optimizer will write it, then read through the
  // C surface: selection never returns it, its own id does.
  pagespeed::PageSpeedCacheConfig config;
  config.volume_path = path_;
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  auto writer = pagespeed::PageSpeedCache::Create(config);
  ASSERT_TRUE(writer.has_value());
  const std::string payload = "\x01m";
  auto mw = (*writer)->WriteSentinel(
      kUrl, kHost, kScheme, pagespeed::SentinelId::kAgentMarkdownWithheld,
      payload.size(), kSiteA);
  ASSERT_TRUE(mw.has_value());
  ASSERT_TRUE(mw->write_sync(std::as_bytes(std::span(payload))).has_value());
  ASSERT_TRUE(mw->close_sync().has_value());

  ps_read_result_t* r = nullptr;
  EXPECT_EQ(PS_ERR_NOT_FOUND,
            ps_cache_read_best_site(cache_, kUrl, kHost, kScheme,
                                    kSiteA.c_str(), 0x08, &r));
  EXPECT_TRUE(Has(cache_, kSiteA.c_str(), PS_SENTINEL_MARKDOWN_WITHHELD));

  // A durable original written for the key clears it.
  ASSERT_EQ(PS_OK,
            WriteOriginal(cache_, kSiteA.c_str(), "<html>r</html>", nullptr));
  EXPECT_FALSE(Has(cache_, kSiteA.c_str(), PS_SENTINEL_MARKDOWN_WITHHELD));
}

// ------------------------------------------------------------ remove_site

TEST_F(AbiSitedHtmlTest, RemoveSiteDropsTheWholeKeyInThatSiteOnly) {
  pagespeed::PageSpeedCacheConfig config;
  config.volume_path = path_;
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  auto cpp = pagespeed::PageSpeedCache::Create(config);
  ASSERT_TRUE(cpp.has_value());
  pagespeed::PageSpeedCache& c = **cpp;

  const uint8_t kVariant = 0x08;
  const std::vector<uint8_t> ids = {PS_SENTINEL_ORIGINAL,
                                    PS_SENTINEL_HEADERS_SIDECAR,
                                    PS_SENTINEL_CONTENT_HASH,
                                    0x7C,
                                    kVariant,
                                    PS_SENTINEL_MARKDOWN_WITHHELD};
  auto seed = [&](const std::string& site) {
    const auto h = Hash(50);
    const char* names[] = {"Referrer-Policy"};
    const char* values[] = {"no-referrer"};
    const auto params = SidecarParams(site.c_str(), h.data());
    ASSERT_EQ(PS_OK, ps_cache_write_headers_sidecar_site(
                         cache_, &params, names, values, 1, nullptr, nullptr));
    ASSERT_EQ(PS_OK,
              WriteOriginal(cache_, site.c_str(), "<html>o</html>", h.data()));
    auto write_raw = [&](pagespeed::SentinelId id, std::string_view bytes) {
      auto wh = c.WriteSentinel(kUrl, kHost, kScheme, id, bytes.size(), site);
      ASSERT_TRUE(wh.has_value());
      ASSERT_TRUE(wh->write_sync(std::as_bytes(std::span(bytes))).has_value());
      ASSERT_TRUE(wh->close_sync().has_value());
    };
    write_raw(pagespeed::SentinelId::kContentHash,
              std::string_view(reinterpret_cast<const char*>(h.data()), 32));
    write_raw(pagespeed::SentinelId::kAgentMarkdownWithheld, "\x01m");
    pagespeed::AlternateMetadata md;
    md.full_mask = 0x7C;
    md.origin_content_type = "text/markdown";
    const std::string body = "# md";
    auto aw =
        c.WriteAgentAlternate(kUrl, kHost, kScheme, body.size(), md, site);
    ASSERT_TRUE(aw.has_value());
    ASSERT_TRUE(aw->write_sync(std::as_bytes(std::span(body))).has_value());
    ASSERT_TRUE(aw->close_sync().has_value());
    pagespeed::AlternateMetadata vm;
    vm.full_mask = kVariant;
    vm.content_type = pagespeed::ContentType::kHtml;
    const std::string vbody = "<html>v</html>";
    auto vw = c.WriteAlternate(kUrl, kHost, kScheme, kVariant, vbody.size(), vm,
                               site);
    ASSERT_TRUE(vw.has_value());
    ASSERT_TRUE(vw->write_sync(std::as_bytes(std::span(vbody))).has_value());
    ASSERT_TRUE(vw->close_sync().has_value());
  };
  seed(kSiteA);
  seed(kSiteB);
  seed("");  // the unsited keyspace
  for (uint8_t id : ids) {
    SCOPED_TRACE(static_cast<int>(id));
    ASSERT_TRUE(c.AlternateExists(kUrl, kHost, kScheme, id, kSiteA));
  }

  ASSERT_EQ(PS_OK,
            ps_cache_remove_site(cache_, kUrl, kHost, kScheme, kSiteA.c_str()));
  for (uint8_t id : ids) {
    SCOPED_TRACE(static_cast<int>(id));
    EXPECT_FALSE(c.AlternateExists(kUrl, kHost, kScheme, id, kSiteA));
    EXPECT_FALSE(Has(cache_, kSiteA.c_str(), id));
    EXPECT_TRUE(c.AlternateExists(kUrl, kHost, kScheme, id, kSiteB));
    EXPECT_TRUE(c.AlternateExists(kUrl, kHost, kScheme, id, ""));
  }
  EXPECT_EQ(PS_ERR_NOT_FOUND,
            ps_cache_remove_site(cache_, kUrl, kHost, kScheme, kSiteA.c_str()));
  EXPECT_EQ(PS_ERR_INVALID_ARG,
            ps_cache_remove_site(cache_, kUrl, kHost, kScheme, "bad"));
  EXPECT_EQ(PS_ERR_INVALID_ARG,
            ps_cache_remove_site(nullptr, kUrl, kHost, kScheme, nullptr));
}

// ------------------------------------------------------------ purge epoch

TEST_F(AbiSitedHtmlTest, TheEpochRoundTripsPerSiteAndMaxMerges) {
  uint64_t e = 99;
  ASSERT_EQ(PS_OK, ps_cache_read_site_purge_epoch(cache_, kSiteA.c_str(), &e));
  EXPECT_EQ(e, 0u);

  const uint64_t now = Now();
  ASSERT_EQ(PS_OK,
            ps_cache_write_site_purge_epoch(cache_, kSiteA.c_str(), now - 60));
  ASSERT_EQ(PS_OK,
            ps_cache_write_site_purge_epoch(cache_, kSiteA.c_str(), now - 600));
  ASSERT_EQ(PS_OK, ps_cache_read_site_purge_epoch(cache_, kSiteA.c_str(), &e));
  EXPECT_EQ(e, now - 60) << "a lower value must be a no-op";
  ASSERT_EQ(PS_OK, ps_cache_read_site_purge_epoch(cache_, kSiteB.c_str(), &e));
  EXPECT_EQ(e, 0u) << "another site's write moved this site's epoch";

  // A whole-key removal in the site does not touch it.
  EXPECT_EQ(PS_ERR_NOT_FOUND,
            ps_cache_remove_site(cache_, kUrl, kHost, kScheme, kSiteA.c_str()));
  ps_cache_t* other = OpenExtra();
  ASSERT_NE(other, nullptr);
  ASSERT_EQ(PS_OK, ps_cache_read_site_purge_epoch(other, kSiteA.c_str(), &e));
  EXPECT_EQ(e, now - 60);
}

TEST_F(AbiSitedHtmlTest, AFutureEpochAndABadSiteAreRefused) {
  const uint64_t now = Now();
  EXPECT_EQ(PS_ERR_INVALID_ARG, ps_cache_write_site_purge_epoch(
                                    cache_, kSiteA.c_str(), now + 3600));
  uint64_t e = 7;
  ASSERT_EQ(PS_OK, ps_cache_read_site_purge_epoch(cache_, kSiteA.c_str(), &e));
  EXPECT_EQ(e, 0u);
  EXPECT_EQ(PS_ERR_INVALID_ARG,
            ps_cache_write_site_purge_epoch(cache_, nullptr, now));
  EXPECT_EQ(PS_ERR_INVALID_ARG,
            ps_cache_write_site_purge_epoch(cache_, "", now));
  EXPECT_EQ(PS_ERR_INVALID_ARG,
            ps_cache_write_site_purge_epoch(cache_, "xyz", now));
  EXPECT_EQ(PS_ERR_INVALID_ARG,
            ps_cache_read_site_purge_epoch(cache_, kSiteA.c_str(), nullptr));
  EXPECT_EQ(PS_ERR_INVALID_ARG,
            ps_cache_read_site_purge_epoch(cache_, nullptr, &e));
}

TEST_F(AbiSitedHtmlTest, ConcurrentWritersOfDifferentValuesLeaveTheLarger) {
  ps_cache_t* other = OpenExtra();
  ASSERT_NE(other, nullptr);
  const uint64_t now = Now();
  for (int round = 0; round < 100; ++round) {
    const uint64_t base = now - 50'000 + static_cast<uint64_t>(round) * 100;
    ps_cache_t* low = (round % 2 == 0) ? cache_ : other;
    ps_cache_t* high = (round % 2 == 0) ? other : cache_;
    std::atomic<int> ready{0};
    auto run = [&ready](ps_cache_t* c, uint64_t value) {
      ready.fetch_add(1);
      while (ready.load() < 2) {
      }
      EXPECT_EQ(PS_OK,
                ps_cache_write_site_purge_epoch(c, kSiteA.c_str(), value));
    };
    std::thread t1(run, low, base);
    std::thread t2(run, high, base + 32);
    t1.join();
    t2.join();
    uint64_t e = 0;
    ASSERT_EQ(PS_OK,
              ps_cache_read_site_purge_epoch(cache_, kSiteA.c_str(), &e));
    ASSERT_EQ(e, base + 32) << "round " << round;
  }
}

TEST_F(AbiSitedHtmlTest, TheEpochFileIsBesideTheVolumeAndFailsClosed) {
  const uint64_t now = Now();
  ASSERT_EQ(PS_OK,
            ps_cache_write_site_purge_epoch(cache_, kSiteA.c_str(), now - 9));
  const std::string file = path_ + ".purge-epoch." + kSiteA;
  ASSERT_TRUE(std::filesystem::exists(file));
  {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << "pspe1 1";  // truncated: no newline
  }
  uint64_t e = 42;
  EXPECT_EQ(PS_ERR_CORRUPTED,
            ps_cache_read_site_purge_epoch(cache_, kSiteA.c_str(), &e));
  EXPECT_EQ(e, 42u) << "the out-param was written on a failed read";
  EXPECT_NE(ps_last_error_message(), nullptr);
}

TEST_F(AbiSitedHtmlTest, AnyFailedEpochReadLeavesTheOutParamAlone) {
  // An I/O failure (a directory where the file should be), not only a
  // corrupted record: non-OK, and the out-parameter untouched, so a caller
  // that fences on any non-OK cannot be handed a 0.
  std::filesystem::create_directories(path_ + ".purge-epoch." + kSiteA);
  uint64_t e = 4242;
  const ps_error_t err =
      ps_cache_read_site_purge_epoch(cache_, kSiteA.c_str(), &e);
  EXPECT_NE(err, PS_OK);
  EXPECT_EQ(e, 4242u);
  EXPECT_NE(PS_OK,
            ps_cache_write_site_purge_epoch(cache_, kSiteA.c_str(), Now()));
}

// ------------------------------------------------------------ RAM tier

TEST_F(AbiSitedHtmlTest, WithTheRamTierOnRemovalAndReStoreAreVisible) {
  ps_cache_t* ram = OpenExtra(/*ram_cache_size=*/4 * 1024 * 1024);
  ASSERT_NE(ram, nullptr);
  const auto h1 = Hash(60);
  const auto h2 = Hash(61);
  const char* names[] = {"Referrer-Policy"};
  const char* values[] = {"no-referrer"};
  auto params = SidecarParams(kSiteA.c_str(), h1.data());
  ASSERT_EQ(PS_OK, ps_cache_write_headers_sidecar_site(
                       ram, &params, names, values, 1, nullptr, nullptr));
  ASSERT_EQ(PS_OK,
            WriteOriginal(ram, kSiteA.c_str(), "<html>1</html>", h1.data()));
  const std::string want1(reinterpret_cast<const char*>(h1.data()), 32);
  const std::string want2(reinterpret_cast<const char*>(h2.data()), 32);
  for (int i = 0; i < 3; ++i) {  // later reads are RAM hits
    EXPECT_EQ(ReadStamp(ram, kSiteA.c_str()), want1);
    ps_header_block_t* block = nullptr;
    ASSERT_EQ(PS_OK, ps_headers_sidecar_read(ram, kUrl, kHost, kScheme,
                                             kSiteA.c_str(), &block));
    EXPECT_EQ(ps_header_block_pairing(block, h1.data()), PS_SIDECAR_PAIRED);
    ps_header_block_free(block);
  }
  params = SidecarParams(kSiteA.c_str(), h2.data());
  ASSERT_EQ(PS_OK, ps_cache_write_headers_sidecar_site(
                       ram, &params, names, values, 1, nullptr, nullptr));
  ASSERT_EQ(PS_OK,
            WriteOriginal(ram, kSiteA.c_str(), "<html>2</html>", h2.data()));
  EXPECT_EQ(ReadStamp(ram, kSiteA.c_str()), want2);
  ps_header_block_t* block = nullptr;
  ASSERT_EQ(PS_OK, ps_headers_sidecar_read(ram, kUrl, kHost, kScheme,
                                           kSiteA.c_str(), &block));
  EXPECT_EQ(ps_header_block_pairing(block, h2.data()), PS_SIDECAR_PAIRED);
  ps_header_block_free(block);

  // Removal evicts this handle's RAM copies.
  ASSERT_EQ(PS_OK,
            ps_cache_remove_site(ram, kUrl, kHost, kScheme, kSiteA.c_str()));
  EXPECT_EQ(ReadStamp(ram, kSiteA.c_str()), "<miss>");
  EXPECT_FALSE(Has(ram, kSiteA.c_str(), PS_SENTINEL_HEADERS_SIDECAR));
}

}  // namespace
