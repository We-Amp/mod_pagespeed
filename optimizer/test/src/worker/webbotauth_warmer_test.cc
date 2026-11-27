// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Hermetic unit tests for the Web Bot Auth key-directory warmer wiring.
// The fetch effect is injected, so full refresh cycles run with no
// network, no threads, and explicit clocks; the published keys file is
// written to a per-test temp dir and read back through the same
// WarmedKeyStore codec nginx uses.

#include "src/worker/webbotauth_warmer.h"

#include <cstdint>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "absl/strings/str_cat.h"
#include "gtest/gtest.h"
#include "src/crypto/webbotauth/key_directory.h"
#include "test/test_util/temp_dir.h"

namespace pagespeed {
namespace {

using webbotauth::KeyLookupResult;
using webbotauth::WarmedKeyStore;

// base64url (no pad) of 32 bytes (see key_directory_test.cc).
const char kX[] = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
const char kX2[] = "BAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

std::string OneKeyDoc(const char* kid, const char* x) {
  return absl::StrCat(
      "{\"keys\":[{\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"kid\":\"", kid,
      "\",\"x\":\"", x, "\"}]}");
}

std::string ReadFileOrEmpty(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f.is_open()) return {};
  return std::string(std::istreambuf_iterator<char>(f),
                     std::istreambuf_iterator<char>());
}

// ---- WebBotAuthDirectoryHosts ----------------------------------------------

TEST(WebBotAuthDirectoryHostsTest, ExtractsDedupedLowercasedHosts) {
  std::vector<std::string> hosts = WebBotAuthDirectoryHosts(
      {"https://Keys.Example.com/.well-known/http-message-signatures-"
       "directory",
       "https://keys.example.com/other-path",
       "https://second.example.org/.well-known/http-message-signatures-"
       "directory",
       "not a url"});
  ASSERT_EQ(2u, hosts.size());
  EXPECT_EQ("keys.example.com", hosts[0]);
  EXPECT_EQ("second.example.org", hosts[1]);
}

// ---- ParseMaxAgeSeconds ------------------------------------------------------

TEST(ParseMaxAgeSecondsTest, ParsesWellFormedDirective) {
  EXPECT_EQ(3600, ParseMaxAgeSeconds("max-age=3600"));
  EXPECT_EQ(600, ParseMaxAgeSeconds("public, max-age=600, immutable"));
  EXPECT_EQ(60, ParseMaxAgeSeconds("MAX-AGE=60"));
}

TEST(ParseMaxAgeSecondsTest, RejectsMalformedOrAbsent) {
  EXPECT_EQ(0, ParseMaxAgeSeconds(""));
  EXPECT_EQ(0, ParseMaxAgeSeconds("no-store"));
  EXPECT_EQ(0, ParseMaxAgeSeconds("max-age=abc"));
  EXPECT_EQ(0, ParseMaxAgeSeconds("max-age=-5"));
  EXPECT_EQ(0, ParseMaxAgeSeconds("max-age=12garbage"));
  // s-maxage is not max-age; the token-boundary check must not prefix-match.
  EXPECT_EQ(0, ParseMaxAgeSeconds("s-maxage=99"));
}

// ---- RefreshWebBotAuthKeys ---------------------------------------------------

class WebBotAuthWarmerTest : public ::testing::Test {
 protected:
  WebBotAuthWarmerConfig MakeConfig() {
    WebBotAuthWarmerConfig config;
    config.directory_urls = {
        "https://keys.example.com/.well-known/"
        "http-message-signatures-directory"};
    config.keys_file_path = temp_dir_.path() + "/webbotauth-keys.conf";
    return config;
  }

  pagespeed::test::TempDir temp_dir_;
};

TEST_F(WebBotAuthWarmerTest, SuccessfulRefreshWarmsStoreAndPublishesFile) {
  WebBotAuthWarmerConfig config = MakeConfig();
  WebBotAuthWarmerState state;
  const int64_t now = 10000;

  JwksFetchFn fetch = [](const std::string&) {
    JwksFetchResult r;
    r.ok = true;
    r.body = OneKeyDoc("k1", kX);
    return r;
  };

  EXPECT_EQ(1, RefreshWebBotAuthKeys(config, fetch, now, &state));
  EXPECT_EQ(
      KeyLookupResult::kFound,
      state.store
          .Get(WarmedKeyStore::StoreKey("wba", "keys.example.com", "k1"), now)
          .status);

  // The published file round-trips through the same codec nginx uses.
  WarmedKeyStore nginx_view;
  ASSERT_TRUE(WarmedKeyStore::Deserialize(
      ReadFileOrEmpty(config.keys_file_path), &nginx_view));
  EXPECT_EQ(
      KeyLookupResult::kFound,
      nginx_view
          .Get(WarmedKeyStore::StoreKey("wba", "keys.example.com", "k1"), now)
          .status);
}

TEST_F(WebBotAuthWarmerTest, FailedFetchWritesNothingStrictExpiry) {
  WebBotAuthWarmerConfig config = MakeConfig();
  WebBotAuthWarmerState state;
  const int64_t now = 10000;

  // Cycle 1: success.
  JwksFetchFn ok_fetch = [](const std::string&) {
    JwksFetchResult r;
    r.ok = true;
    r.body = OneKeyDoc("k1", kX);
    return r;
  };
  ASSERT_EQ(1, RefreshWebBotAuthKeys(config, ok_fetch, now, &state));

  // Cycle 2: fetch failure.  Nothing is written for the host
  // — the previously warmed key stays until its OWN TTL lapses (no negative
  // entry, no eviction).
  JwksFetchFn fail_fetch = [](const std::string&) { return JwksFetchResult{}; };
  EXPECT_EQ(0, RefreshWebBotAuthKeys(config, fail_fetch, now + 60, &state));
  EXPECT_EQ(KeyLookupResult::kFound,
            state.store
                .Get(WarmedKeyStore::StoreKey("wba", "keys.example.com", "k1"),
                     now + 60)
                .status);
  // ... and lapses strictly at the positive TTL (default 3600).
  EXPECT_EQ(KeyLookupResult::kNotFound,
            state.store
                .Get(WarmedKeyStore::StoreKey("wba", "keys.example.com", "k1"),
                     now + 3600)
                .status);
}

TEST_F(WebBotAuthWarmerTest, VanishedKidGetsNegativeEntry) {
  WebBotAuthWarmerConfig config = MakeConfig();
  WebBotAuthWarmerState state;
  const int64_t now = 10000;

  JwksFetchFn two_kids = [](const std::string&) {
    JwksFetchResult r;
    r.ok = true;
    r.body = absl::StrCat(
        "{\"keys\":[{\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"kid\":\"k1\","
        "\"x\":\"",
        kX, "\"},{\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"kid\":\"k2\",\"x\":\"",
        kX2, "\"}]}");
    return r;
  };
  ASSERT_EQ(2, RefreshWebBotAuthKeys(config, two_kids, now, &state));

  // Next refresh: k2 vanished from the directory.  Its still-live positive
  // entry is replaced by a kNegativeTtlSec tombstone (prompt propagation of
  // the removal), while k1 is re-warmed.
  JwksFetchFn one_kid = [](const std::string&) {
    JwksFetchResult r;
    r.ok = true;
    r.body = OneKeyDoc("k1", kX);
    return r;
  };
  EXPECT_EQ(1, RefreshWebBotAuthKeys(config, one_kid, now + 60, &state));
  EXPECT_EQ(KeyLookupResult::kFound,
            state.store
                .Get(WarmedKeyStore::StoreKey("wba", "keys.example.com", "k1"),
                     now + 60)
                .status);
  EXPECT_EQ(KeyLookupResult::kNotFound,
            state.store
                .Get(WarmedKeyStore::StoreKey("wba", "keys.example.com", "k2"),
                     now + 60)
                .status);

  // The tombstone crossed into the published file too.
  WarmedKeyStore nginx_view;
  ASSERT_TRUE(WarmedKeyStore::Deserialize(
      ReadFileOrEmpty(config.keys_file_path), &nginx_view));
  EXPECT_EQ(KeyLookupResult::kNotFound,
            nginx_view
                .Get(WarmedKeyStore::StoreKey("wba", "keys.example.com", "k2"),
                     now + 60)
                .status);
}

TEST_F(WebBotAuthWarmerTest, CacheControlHintOverridesDefaultTtl) {
  WebBotAuthWarmerConfig config = MakeConfig();
  WebBotAuthWarmerState state;
  const int64_t now = 10000;

  JwksFetchFn fetch = [](const std::string&) {
    JwksFetchResult r;
    r.ok = true;
    r.body = OneKeyDoc("k1", kX);
    r.ttl_hint_sec = 600;  // directory said max-age=600
    return r;
  };
  ASSERT_EQ(1, RefreshWebBotAuthKeys(config, fetch, now, &state));

  const std::string key =
      WarmedKeyStore::StoreKey("wba", "keys.example.com", "k1");
  EXPECT_EQ(KeyLookupResult::kFound, state.store.Get(key, now + 599).status);
  EXPECT_EQ(KeyLookupResult::kNotFound, state.store.Get(key, now + 600).status);
}

TEST_F(WebBotAuthWarmerTest, SameHostTwoUrlsDoNotEvictEachOther) {
  // Regression: two directory URLs on ONE host share the store's host
  // namespace.  Tombstoning is keyed per URL and consults the merged
  // per-host kid set, so URL B's refresh must never write a tombstone over
  // a key URL A just re-warmed (previously this evicted it every cycle).
  WebBotAuthWarmerConfig config = MakeConfig();
  config.directory_urls = {"https://keys.example.com/dir-a",
                           "https://keys.example.com/dir-b"};
  config.keys_file_path = temp_dir_.path() + "/webbotauth-keys.conf";
  WebBotAuthWarmerState state;

  JwksFetchFn fetch = [](const std::string& url) {
    JwksFetchResult r;
    r.ok = true;
    r.body = url.find("dir-b") != std::string::npos ? OneKeyDoc("kb", kX2)
                                                    : OneKeyDoc("ka", kX);
    return r;
  };

  const std::string key_a =
      WarmedKeyStore::StoreKey("wba", "keys.example.com", "ka");
  const std::string key_b =
      WarmedKeyStore::StoreKey("wba", "keys.example.com", "kb");

  // Two full cycles: cycle 2 is where the per-host bug used to bite (each
  // URL's pass saw the other URL's kids as "vanished" from the host).
  ASSERT_EQ(2, RefreshWebBotAuthKeys(config, fetch, 10000, &state));
  ASSERT_EQ(2, RefreshWebBotAuthKeys(config, fetch, 10060, &state));
  EXPECT_EQ(KeyLookupResult::kFound, state.store.Get(key_a, 10060).status);
  EXPECT_EQ(KeyLookupResult::kFound, state.store.Get(key_b, 10060).status);

  // But a kid that REALLY vanishes from its own URL (and is not published
  // by the other same-host URL) still gets its tombstone.
  JwksFetchFn b_dropped = [](const std::string& url) {
    JwksFetchResult r;
    r.ok = true;
    r.body = url.find("dir-b") != std::string::npos ? "{\"keys\":[]}"
                                                    : OneKeyDoc("ka", kX);
    return r;
  };
  EXPECT_EQ(1, RefreshWebBotAuthKeys(config, b_dropped, 10120, &state));
  EXPECT_EQ(KeyLookupResult::kFound, state.store.Get(key_a, 10120).status);
  EXPECT_EQ(KeyLookupResult::kNotFound, state.store.Get(key_b, 10120).status);
}

TEST_F(WebBotAuthWarmerTest, MultipleDirectoriesWarmUnderTheirOwnHosts) {
  WebBotAuthWarmerConfig config = MakeConfig();
  config.directory_urls.push_back(
      "https://second.example.org/.well-known/"
      "http-message-signatures-directory");
  WebBotAuthWarmerState state;
  const int64_t now = 10000;

  JwksFetchFn fetch = [](const std::string& url) {
    JwksFetchResult r;
    r.ok = true;
    r.body = url.find("second.example.org") != std::string::npos
                 ? OneKeyDoc("k2", kX2)
                 : OneKeyDoc("k1", kX);
    return r;
  };
  EXPECT_EQ(2, RefreshWebBotAuthKeys(config, fetch, now, &state));
  EXPECT_EQ(
      KeyLookupResult::kFound,
      state.store
          .Get(WarmedKeyStore::StoreKey("wba", "keys.example.com", "k1"), now)
          .status);
  EXPECT_EQ(
      KeyLookupResult::kFound,
      state.store
          .Get(WarmedKeyStore::StoreKey("wba", "second.example.org", "k2"), now)
          .status);
  // No cross-host leakage.
  EXPECT_EQ(
      KeyLookupResult::kNotFound,
      state.store
          .Get(WarmedKeyStore::StoreKey("wba", "keys.example.com", "k2"), now)
          .status);
}

// ---------------------------------------------------------------------------
// Fetch-outcome stats (#876): the caller's total-failure retry policy needs
// "every directory unreachable" (boot ordering) to be distinguishable from
// "reachable but legitimately empty" — keys_written alone conflates them.
// ---------------------------------------------------------------------------

TEST_F(WebBotAuthWarmerTest, FetchStatsTotalFailure) {
  WebBotAuthWarmerConfig config = MakeConfig();
  WebBotAuthWarmerState state;
  WebBotAuthRefreshFetchStats fs;

  JwksFetchFn fail_fetch = [](const std::string&) { return JwksFetchResult{}; };
  EXPECT_EQ(0, RefreshWebBotAuthKeys(config, fail_fetch, 10000, &state, &fs));
  EXPECT_EQ(1, fs.attempted);
  EXPECT_EQ(0, fs.fetched_ok);
}

TEST_F(WebBotAuthWarmerTest, FetchStatsEmptyDirectoryIsNotTotalFailure) {
  WebBotAuthWarmerConfig config = MakeConfig();
  WebBotAuthWarmerState state;
  WebBotAuthRefreshFetchStats fs;

  // Reachable directory legitimately serving zero keys: fetched_ok ticks even
  // though nothing is warmed, so the caller does NOT enter failure backoff.
  JwksFetchFn empty_fetch = [](const std::string&) {
    JwksFetchResult r;
    r.ok = true;
    r.body = R"({"keys":[]})";
    return r;
  };
  EXPECT_EQ(0, RefreshWebBotAuthKeys(config, empty_fetch, 10000, &state, &fs));
  EXPECT_EQ(1, fs.attempted);
  EXPECT_EQ(1, fs.fetched_ok);
}

TEST_F(WebBotAuthWarmerTest, FetchStatsMixedAndReset) {
  WebBotAuthWarmerConfig config = MakeConfig();
  config.directory_urls.push_back(
      "https://second.example.org/.well-known/"
      "http-message-signatures-directory");
  // An unparseable URL is skipped before fetching: it must not count as an
  // attempt (a config typo alone must never trigger failure backoff).
  config.directory_urls.push_back("not a url");
  WebBotAuthWarmerState state;
  WebBotAuthRefreshFetchStats fs;
  // Pre-populate to verify the stats reset at the start of each cycle.
  fs.attempted = 99;
  fs.fetched_ok = 99;

  JwksFetchFn fetch = [](const std::string& url) {
    JwksFetchResult r;
    if (url.find("second.example.org") != std::string::npos) {
      return r;  // one directory down
    }
    r.ok = true;
    r.body = OneKeyDoc("k1", kX);
    return r;
  };
  EXPECT_EQ(1, RefreshWebBotAuthKeys(config, fetch, 10000, &state, &fs));
  EXPECT_EQ(2, fs.attempted);
  EXPECT_EQ(1, fs.fetched_ok);
}

}  // namespace
}  // namespace pagespeed
