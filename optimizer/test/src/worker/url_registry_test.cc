// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Unit tests for UrlRegistry.

#include "src/worker/url_registry.h"

#include <cstdio>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "src/worker/known_sites.h"

namespace pagespeed {
namespace {

TEST(UrlRegistryTest, EmptyOnConstruction) {
  UrlRegistry reg;
  EXPECT_EQ(reg.size(), 0u);
}

TEST(UrlRegistryTest, RecordAndSize) {
  UrlRegistry reg;
  reg.Record("/a.html", "example.com", "https");
  EXPECT_EQ(reg.size(), 1u);
  reg.Record("/b.html", "example.com", "https");
  EXPECT_EQ(reg.size(), 2u);
}

TEST(UrlRegistryTest, DuplicateRecordDoesNotIncrease) {
  UrlRegistry reg;
  reg.Record("/a.html", "example.com", "https");
  reg.Record("/a.html", "example.com", "https");
  EXPECT_EQ(reg.size(), 1u);
}

TEST(UrlRegistryTest, SameUrlDifferentHostnameAreSeparate) {
  UrlRegistry reg;
  reg.Record("/a.html", "alpha.com", "https");
  reg.Record("/a.html", "beta.com", "https");
  EXPECT_EQ(reg.size(), 2u);
}

TEST(UrlRegistryTest, Remove) {
  UrlRegistry reg;
  reg.Record("/a.html", "example.com", "https");
  reg.Record("/b.html", "example.com", "https");
  reg.Remove("/a.html", "example.com", "https");
  EXPECT_EQ(reg.size(), 1u);
}

TEST(UrlRegistryTest, RemoveNonexistent) {
  UrlRegistry reg;
  reg.Record("/a.html", "example.com", "https");
  reg.Remove("/nonexistent.html", "example.com", "https");
  EXPECT_EQ(reg.size(), 1u);
}

TEST(UrlRegistryTest, ClearAll) {
  UrlRegistry reg;
  reg.Record("/a.html", "example.com", "https");
  reg.Record("/b.html", "example.com", "https");
  reg.Record("/c.html", "other.com", "https");

  auto entries = reg.ClearAll();
  EXPECT_EQ(reg.size(), 0u);
  EXPECT_EQ(entries.size(), 3u);
}

TEST(UrlRegistryTest, ClearAllReturnsCorrectEntries) {
  UrlRegistry reg;
  reg.Record("/a.html", "example.com", "https");
  auto entries = reg.ClearAll();
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].url, "/a.html");
  EXPECT_EQ(entries[0].hostname, "example.com");
  EXPECT_EQ(entries[0].scheme, "https");
}

TEST(UrlRegistryTest, ListUnfiltered) {
  UrlRegistry reg;
  reg.Record("/a.html", "example.com", "https");
  reg.Record("/b.html", "example.com", "https");
  reg.Record("/c.html", "example.com", "https");

  auto page = reg.List(0, 10);
  EXPECT_EQ(page.entries.size(), 3u);
  EXPECT_EQ(page.total, 3u);
  EXPECT_FALSE(page.has_more);
}

TEST(UrlRegistryTest, ListPagination) {
  UrlRegistry reg;
  reg.Record("/a.html", "example.com", "https");
  reg.Record("/b.html", "example.com", "https");
  reg.Record("/c.html", "example.com", "https");
  reg.Record("/d.html", "example.com", "https");
  reg.Record("/e.html", "example.com", "https");

  auto page1 = reg.List(0, 2);
  EXPECT_EQ(page1.entries.size(), 2u);
  EXPECT_TRUE(page1.has_more);
  EXPECT_EQ(page1.next_offset, 2u);
  EXPECT_EQ(page1.total, 5u);

  auto page2 = reg.List(page1.next_offset, 2);
  EXPECT_EQ(page2.entries.size(), 2u);
  EXPECT_TRUE(page2.has_more);

  auto page3 = reg.List(page2.next_offset, 2);
  EXPECT_EQ(page3.entries.size(), 1u);
  EXPECT_FALSE(page3.has_more);
}

TEST(UrlRegistryTest, ListBeyondEnd) {
  UrlRegistry reg;
  reg.Record("/a.html", "example.com", "https");

  auto page = reg.List(100, 10);
  EXPECT_EQ(page.entries.size(), 0u);
  EXPECT_FALSE(page.has_more);
  EXPECT_EQ(page.total, 1u);
}

TEST(UrlRegistryTest, ListFilteredByHostname) {
  UrlRegistry reg;
  reg.Record("/a.html", "alpha.com", "https");
  reg.Record("/b.html", "beta.com", "https");
  reg.Record("/c.html", "alpha.com", "https");
  reg.Record("/d.html", "beta.com", "https");

  auto page = reg.List(0, 10, "alpha.com");
  EXPECT_EQ(page.entries.size(), 2u);
  EXPECT_EQ(page.total, 2u);
  for (const auto& e : page.entries) {
    EXPECT_EQ(e.hostname, "alpha.com");
  }
}

TEST(UrlRegistryTest, ListFilteredPagination) {
  UrlRegistry reg;
  reg.Record("/1.html", "a.com", "https");
  reg.Record("/2.html", "b.com", "https");
  reg.Record("/3.html", "a.com", "https");
  reg.Record("/4.html", "b.com", "https");
  reg.Record("/5.html", "a.com", "https");

  auto page1 = reg.List(0, 2, "a.com");
  EXPECT_EQ(page1.entries.size(), 2u);
  EXPECT_TRUE(page1.has_more);
  EXPECT_EQ(page1.total, 3u);

  auto page2 = reg.List(page1.next_offset, 10, "a.com");
  EXPECT_EQ(page2.entries.size(), 1u);
  EXPECT_FALSE(page2.has_more);
}

TEST(UrlRegistryTest, ListFilteredNoMatch) {
  UrlRegistry reg;
  reg.Record("/a.html", "example.com", "https");

  auto page = reg.List(0, 10, "nonexistent.com");
  EXPECT_EQ(page.entries.size(), 0u);
  EXPECT_EQ(page.total, 0u);
  EXPECT_FALSE(page.has_more);
}

TEST(UrlRegistryTest, MaxEntriesEviction) {
  UrlRegistry reg(3);
  reg.Record("/a.html", "example.com", "https");
  reg.Record("/b.html", "example.com", "https");
  reg.Record("/c.html", "example.com", "https");
  EXPECT_EQ(reg.size(), 3u);

  // Adding a 4th should evict the oldest (/a.html).
  reg.Record("/d.html", "example.com", "https");
  EXPECT_EQ(reg.size(), 3u);

  // Verify /a.html was evicted.
  auto page = reg.List(0, 10);
  bool found_a = false;
  for (const auto& e : page.entries) {
    if (e.url == "/a.html") found_a = true;
  }
  EXPECT_FALSE(found_a);
}

TEST(UrlRegistryTest, DuplicateRecordMovesToFront) {
  UrlRegistry reg(3);
  reg.Record("/a.html", "example.com", "https");
  reg.Record("/b.html", "example.com", "https");
  reg.Record("/c.html", "example.com", "https");

  // Touch /a.html to move it to front (MRU).
  reg.Record("/a.html", "example.com", "https");
  EXPECT_EQ(reg.size(), 3u);

  // Now add /d.html. Should evict /b.html (now oldest), not /a.html.
  reg.Record("/d.html", "example.com", "https");
  EXPECT_EQ(reg.size(), 3u);

  auto page = reg.List(0, 10);
  bool found_a = false, found_b = false;
  for (const auto& e : page.entries) {
    if (e.url == "/a.html") found_a = true;
    if (e.url == "/b.html") found_b = true;
  }
  EXPECT_TRUE(found_a);
  EXPECT_FALSE(found_b);
}

TEST(UrlRegistryTest, MaxEntriesClampedToOne) {
  UrlRegistry reg(0);  // Should clamp to 1.
  reg.Record("/a.html", "example.com", "https");
  EXPECT_EQ(reg.size(), 1u);
  reg.Record("/b.html", "example.com", "https");
  EXPECT_EQ(reg.size(), 1u);
}

TEST(UrlRegistryTest, ListOrderMostRecentFirst) {
  UrlRegistry reg;
  reg.Record("/old.html", "example.com", "https");
  reg.Record("/new.html", "example.com", "https");

  auto page = reg.List(0, 10);
  ASSERT_EQ(page.entries.size(), 2u);
  // Most recently recorded should be first.
  EXPECT_EQ(page.entries[0].url, "/new.html");
  EXPECT_EQ(page.entries[1].url, "/old.html");
}

// HasHostname normalization: Record() normalizes the hostname before inserting
// into known_hostnames_, so HasHostname() lookups with normalized forms always
// match even when the caller passed a non-normalized hostname to Record().
TEST(UrlRegistryTest, HasHostnameNormalizesOnRecord) {
  UrlRegistry reg;

  // Record with uppercase — should be stored as lowercase.
  reg.Record("/a.html", "Example.COM", "https");
  EXPECT_TRUE(reg.HasHostname("example.com"));

  // Record with trailing dot — should be stripped.
  reg.Record("/b.html", "other.com.", "https");
  EXPECT_TRUE(reg.HasHostname("other.com"));

  // Record with default HTTPS port — should be stripped.
  reg.Record("/c.html", "secure.com:443", "https");
  EXPECT_TRUE(reg.HasHostname("secure.com"));

  // Record with default HTTP port — should be stripped.
  reg.Record("/d.html", "plain.com:80", "http");
  EXPECT_TRUE(reg.HasHostname("plain.com"));

  // Non-default port must be preserved.
  reg.Record("/e.html", "custom.com:8080", "https");
  EXPECT_TRUE(reg.HasHostname("custom.com:8080"));
  EXPECT_FALSE(reg.HasHostname("custom.com"));
}

// SetAlternateCount updates the cached count for a recorded URL, and List()
// returns it (the /v1/cache/urls listing reads this instead of doing a per-row
// cache traversal).  The count defaults to 0 before any refresh.
TEST(UrlRegistryTest, SetAlternateCountUpdatesEntry) {
  UrlRegistry reg;
  reg.Record("/a.html", "example.com", "https");

  auto page = reg.List(0, 10);
  ASSERT_EQ(page.entries.size(), 1u);
  EXPECT_EQ(page.entries[0].alternate_count, 0u);

  reg.SetAlternateCount("/a.html", "example.com", "https", 5);
  page = reg.List(0, 10);
  ASSERT_EQ(page.entries.size(), 1u);
  EXPECT_EQ(page.entries[0].alternate_count, 5u);

  // Overwrites (not max/merge): a later, smaller count replaces the old one,
  // so the listing reflects alternates shrinking (purge / origin-refresh).
  reg.SetAlternateCount("/a.html", "example.com", "https", 2);
  page = reg.List(0, 10);
  ASSERT_EQ(page.entries.size(), 1u);
  EXPECT_EQ(page.entries[0].alternate_count, 2u);
}

// SetAlternateCount on a URL that is not tracked is a no-op: it must not insert
// an entry (which would resurrect an evicted URL out of LRU order) or crash.
TEST(UrlRegistryTest, SetAlternateCountUnknownUrlIsNoop) {
  UrlRegistry reg;
  reg.SetAlternateCount("/missing.html", "example.com", "https", 7);
  EXPECT_EQ(reg.size(), 0u);

  // A triple that differs only in scheme is a different key and is ignored.
  reg.Record("/a.html", "example.com", "https");
  reg.SetAlternateCount("/a.html", "example.com", "http", 9);
  auto page = reg.List(0, 10);
  ASSERT_EQ(page.entries.size(), 1u);
  EXPECT_EQ(page.entries[0].alternate_count, 0u);
}

// A duplicate Record() (LRU move-to-front) must NOT reset a count already set
// via SetAlternateCount — the count is owned by SetAlternateCount, not Record.
TEST(UrlRegistryTest, DuplicateRecordPreservesAlternateCount) {
  UrlRegistry reg;
  reg.Record("/a.html", "example.com", "https");
  reg.SetAlternateCount("/a.html", "example.com", "https", 3);

  reg.Record("/a.html", "example.com", "https");  // duplicate
  auto page = reg.List(0, 10);
  ASSERT_EQ(page.entries.size(), 1u);
  EXPECT_EQ(page.entries[0].alternate_count, 3u);
}

// Eviction drops the entry and its count; SetAlternateCount on the evicted URL
// is then a no-op (does not resurrect it).
TEST(UrlRegistryTest, EvictionDropsAlternateCount) {
  UrlRegistry reg(1);  // capacity 1
  reg.Record("/a.html", "example.com", "https");
  reg.SetAlternateCount("/a.html", "example.com", "https", 4);

  reg.Record("/b.html", "example.com", "https");  // evicts /a.html
  reg.SetAlternateCount("/a.html", "example.com", "https", 99);  // no-op

  auto page = reg.List(0, 10);
  ASSERT_EQ(page.entries.size(), 1u);
  EXPECT_EQ(page.entries[0].url, "/b.html");
  EXPECT_EQ(page.entries[0].alternate_count, 0u);
}

// --- The site dimension -----------------------------------------------------

const std::string kSiteA(64, 'a');
const std::string kSiteB(64, 'b');

TEST(UrlRegistryTest, SameUrlOnTwoSitesAreSeparateEntries) {
  UrlRegistry reg;
  reg.Record("/a.html", "example.com", "https", kSiteA);
  reg.Record("/a.html", "example.com", "https", kSiteB);
  reg.Record("/a.html", "example.com", "https");
  EXPECT_EQ(reg.size(), 3u);

  reg.SetAlternateCount("/a.html", "example.com", "https", 7, kSiteA);
  auto a = reg.List(0, 10, {}, kSiteA);
  ASSERT_EQ(a.entries.size(), 1u);
  EXPECT_EQ(a.total, 1u);
  EXPECT_EQ(a.entries[0].site, kSiteA);
  EXPECT_EQ(a.entries[0].alternate_count, 7u);
  auto b = reg.List(0, 10, "example.com", kSiteB);
  ASSERT_EQ(b.entries.size(), 1u);
  EXPECT_EQ(b.entries[0].site, kSiteB);
  EXPECT_EQ(b.entries[0].alternate_count, 0u);
  // No site filter lists every keyspace.
  EXPECT_EQ(reg.List(0, 10).total, 3u);

  reg.Remove("/a.html", "example.com", "https", kSiteA);
  EXPECT_EQ(reg.size(), 2u);
  EXPECT_EQ(reg.List(0, 10, {}, kSiteA).total, 0u);
  EXPECT_EQ(reg.List(0, 10, {}, kSiteB).total, 1u);
}

TEST(UrlRegistryTest, RecordingASiteMakesItKnown) {
  UrlRegistry reg;
  EXPECT_FALSE(reg.HasSite(kSiteA));
  reg.Record("/a.html", "example.com", "https");  // unsited
  EXPECT_TRUE(reg.KnownSites().empty());
  reg.Record("/a.html", "example.com", "https", kSiteB);
  reg.Record("/b.html", "example.com", "https", kSiteA);
  EXPECT_TRUE(reg.HasSite(kSiteA));
  EXPECT_TRUE(reg.HasSite(kSiteB));
  EXPECT_FALSE(reg.HasSite(""));
  EXPECT_EQ(reg.KnownSites(), (std::vector<std::string>{kSiteA, kSiteB}));
}

TEST(UrlRegistryTest, KnownSitesSurviveEvictionAndClearAll) {
  UrlRegistry reg(/*max_entries=*/1);
  reg.Record("/a.html", "example.com", "https", kSiteA);
  reg.Record("/b.html", "example.com", "https", kSiteB);  // evicts /a.html
  EXPECT_EQ(reg.size(), 1u);
  EXPECT_TRUE(reg.HasSite(kSiteA));
  (void)reg.ClearAll();
  EXPECT_EQ(reg.size(), 0u);
  EXPECT_TRUE(reg.HasSite(kSiteA));
  EXPECT_TRUE(reg.HasSite(kSiteB));
}

TEST(UrlRegistryTest, ClearKnownSitesDropsThemAndMarksAChange) {
  UrlRegistry reg;
  reg.Record("/a.html", "example.com", "https", kSiteA);
  ASSERT_TRUE(reg.TakeKnownSitesIfChanged().has_value());
  EXPECT_FALSE(reg.TakeKnownSitesIfChanged().has_value());

  reg.ClearKnownSites();
  EXPECT_FALSE(reg.HasSite(kSiteA));
  EXPECT_TRUE(reg.KnownSites().empty());
  auto changed = reg.TakeKnownSitesIfChanged();
  ASSERT_TRUE(changed.has_value());
  EXPECT_TRUE(changed->empty()) << "the file is rewritten as empty";
}

TEST(UrlRegistryTest, LoadedSitesAreKnownWithoutAChange) {
  UrlRegistry reg;
  reg.LoadKnownSites({kSiteA});
  EXPECT_TRUE(reg.HasSite(kSiteA));
  EXPECT_FALSE(reg.TakeKnownSitesIfChanged().has_value());
  reg.Record("/a.html", "example.com", "https", kSiteA);  // already known
  EXPECT_FALSE(reg.TakeKnownSitesIfChanged().has_value());
}

TEST(UrlRegistryTest, SitesPastTheCapAreCountedNotRemembered) {
  UrlRegistry reg(/*max_entries=*/4);
  std::vector<std::string> full;
  full.reserve(kMaxKnownSites);
  char buf[65];
  for (size_t i = 0; i < kMaxKnownSites; ++i) {
    std::snprintf(buf, sizeof(buf), "%064zx", i);
    full.emplace_back(buf);
  }
  reg.LoadKnownSites(full);
  EXPECT_EQ(reg.KnownSites().size(), kMaxKnownSites);
  EXPECT_EQ(reg.known_sites_overflow(), 0u);

  std::snprintf(buf, sizeof(buf), "%064zx", kMaxKnownSites);
  reg.Record("/a.html", "example.com", "https", buf);
  EXPECT_EQ(reg.known_sites_overflow(), 1u);
  EXPECT_FALSE(reg.HasSite(buf));
  EXPECT_EQ(reg.size(), 1u) << "the entry itself is still tracked";
}

}  // namespace
}  // namespace pagespeed
