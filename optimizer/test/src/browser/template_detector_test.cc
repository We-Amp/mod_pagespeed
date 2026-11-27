// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Template Detector Tests

#include "src/browser/template_detector.h"

#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "src/worker/html_scanner.h"

namespace pagespeed {
namespace {

// Helper to build a minimal HtmlScanResult with given elements.
HtmlScanResult MakeResult(std::vector<std::pair<std::string, int>> tag_depths,
                          int num_stylesheets = 0,
                          bool has_inline_css = false) {
  HtmlScanResult result;
  result.success = true;
  int index = 0;
  for (auto& [tag, depth] : tag_depths) {
    CollectedElement elem;
    elem.tag_name = tag;
    elem.depth = depth;
    elem.element_index = index++;
    result.elements.push_back(std::move(elem));
  }
  for (int i = 0; i < num_stylesheets; ++i) {
    StylesheetLink link;
    link.href = "style" + std::to_string(i) + ".css";
    result.stylesheets.push_back(std::move(link));
  }
  if (has_inline_css) {
    result.inline_css = "body { color: red; }";
  }
  return result;
}

// --- HashStructure tests ---

TEST(TemplateDetectorTest, EmptyResultHashesConsistently) {
  HtmlScanResult empty;
  empty.success = true;
  uint64_t h1 = TemplateDetector::HashStructure(empty);
  uint64_t h2 = TemplateDetector::HashStructure(empty);
  EXPECT_EQ(h1, h2);
}

TEST(TemplateDetectorTest, SameStructureProducesSameHash) {
  // Two "product pages" with same tag structure but different content.
  auto result1 = MakeResult({{"html", 0},
                             {"head", 1},
                             {"title", 2},
                             {"body", 1},
                             {"header", 2},
                             {"nav", 3},
                             {"main", 2},
                             {"section", 3},
                             {"h1", 4},
                             {"p", 4},
                             {"img", 4},
                             {"footer", 2}},
                            2, true);

  auto result2 = MakeResult({{"html", 0},
                             {"head", 1},
                             {"title", 2},
                             {"body", 1},
                             {"header", 2},
                             {"nav", 3},
                             {"main", 2},
                             {"section", 3},
                             {"h1", 4},
                             {"p", 4},
                             {"img", 4},
                             {"footer", 2}},
                            2, true);

  // Add different ids/classes (should not affect hash).
  result1.elements[4].id = "site-header";
  result1.elements[4].classes = {"header-primary"};
  result2.elements[4].id = "main-header";
  result2.elements[4].classes = {"header-alt"};

  EXPECT_EQ(TemplateDetector::HashStructure(result1),
            TemplateDetector::HashStructure(result2));
}

TEST(TemplateDetectorTest, DifferentTagsDifferentHash) {
  auto result1 = MakeResult({{"html", 0}, {"body", 1}, {"div", 2}, {"p", 3}});
  auto result2 =
      MakeResult({{"html", 0}, {"body", 1}, {"section", 2}, {"p", 3}});

  EXPECT_NE(TemplateDetector::HashStructure(result1),
            TemplateDetector::HashStructure(result2));
}

TEST(TemplateDetectorTest, DifferentDepthsDifferentHash) {
  auto result1 = MakeResult({{"div", 2}, {"p", 3}});
  auto result2 = MakeResult({{"div", 3}, {"p", 4}});

  EXPECT_NE(TemplateDetector::HashStructure(result1),
            TemplateDetector::HashStructure(result2));
}

TEST(TemplateDetectorTest, DifferentElementCountDifferentHash) {
  auto result1 = MakeResult({{"div", 0}, {"p", 1}});
  auto result2 = MakeResult({{"div", 0}, {"p", 1}, {"p", 1}});

  EXPECT_NE(TemplateDetector::HashStructure(result1),
            TemplateDetector::HashStructure(result2));
}

TEST(TemplateDetectorTest, StylesheetCountAffectsHash) {
  auto result1 = MakeResult({{"html", 0}, {"body", 1}}, 1);
  auto result2 = MakeResult({{"html", 0}, {"body", 1}}, 2);

  EXPECT_NE(TemplateDetector::HashStructure(result1),
            TemplateDetector::HashStructure(result2));
}

TEST(TemplateDetectorTest, InlineCssPresenceAffectsHash) {
  auto result1 = MakeResult({{"html", 0}, {"body", 1}}, 0, false);
  auto result2 = MakeResult({{"html", 0}, {"body", 1}}, 0, true);

  EXPECT_NE(TemplateDetector::HashStructure(result1),
            TemplateDetector::HashStructure(result2));
}

TEST(TemplateDetectorTest, TagNameConcatenationNoCollision) {
  // "ab" at depth 1 + "c" at depth 2 should differ from
  // "a" at depth 1 + "bc" at depth 2.
  auto result1 = MakeResult({{"ab", 1}, {"c", 2}});
  auto result2 = MakeResult({{"a", 1}, {"bc", 2}});

  EXPECT_NE(TemplateDetector::HashStructure(result1),
            TemplateDetector::HashStructure(result2));
}

TEST(TemplateDetectorTest, LargeDepthClamped) {
  // Depth > 255 is clamped to 255.
  auto result1 = MakeResult({{"div", 300}});
  auto result2 = MakeResult({{"div", 500}});

  // Both should hash the same (both clamped to 255).
  EXPECT_EQ(TemplateDetector::HashStructure(result1),
            TemplateDetector::HashStructure(result2));
}

// --- Reprocessed-output fixed-point ---

// The worker caches its OWN optimized output (cache slot 0x08) and, on a
// revalidation pass, scans THAT instead of the raw origin HTML. The optimized
// variant carries injected nodes the raw HTML never had: the critical
// <style data-pagespeed-critical>, the async <noscript data-pagespeed-async-
// fallback> + child <link>, the async-loader <script>, and preconnect/preload
// <link data-pagespeed-hint>. If the scanner collects those into
// result.elements, the element-tree component of HashStructure differs between
// the raw and reprocessed passes, the template hash flaps, and the worker
// burns a redundant browser analysis plus a duplicate profile slot. The
// structural fingerprint must be identical across both passes.
TEST(TemplateDetectorTest, ReprocessedOutputHashesSameAsRawOrigin) {
  HtmlScanner scanner;

  // Raw origin HTML as the customer authored it.
  std::string raw =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/a.css\">"
      "</head><body>"
      "<h1>Hi</h1>"
      "<p>x</p>"
      "<script src=\"/app.js\"></script>"
      "</body></html>";

  // The same page after the worker's transforms, as re-scanned on revalidation:
  // the original <link> is async-swapped in place; critical CSS, the async
  // <noscript> fallback, the loader <script>, and a preconnect hint are
  // injected.
  std::string optimized =
      "<html><head>"
      "<link rel=\"preload\" as=\"style\" href=\"/a.css\" "
      "data-pagespeed-media=\"all\" data-pagespeed-async=\"\">"
      "<style data-pagespeed-critical=\"\">h1{color:red}</style>"
      "<noscript data-pagespeed-async-fallback=\"\">"
      "<link rel=\"stylesheet\" href=\"/a.css\" "
      "data-pagespeed-async-fallback=\"\"></noscript>"
      "<script src=\"/pagespeed_static/async_css.js\" defer "
      "data-pagespeed-async-loader=\"\"></script>"
      "<link rel=\"preconnect\" href=\"https://cdn.example.com\" "
      "data-pagespeed-hint=\"\">"
      "</head><body>"
      "<h1>Hi</h1>"
      "<p>x</p>"
      "<script type=\"speculationrules\" data-pagespeed-hint=\"\">"
      "{\"prefetch\":[{\"source\":\"list\",\"urls\":[\"/next\"]}]}</script>"
      "<script src=\"/app.js\" data-pagespeed-defer=\"\"></script>"
      "</body></html>";

  HtmlScanResult raw_result = scanner.Scan("http://example.com/", raw);
  HtmlScanResult optimized_result =
      scanner.Scan("http://example.com/", optimized);
  ASSERT_TRUE(raw_result.success);
  ASSERT_TRUE(optimized_result.success);

  // Pin each of the three HashStructure inputs independently, so the combined
  // hash equality below cannot pass for an offsetting reason: the element tree
  // (count + order), the stylesheet count, and inline-CSS presence must each
  // match across the raw and reprocessed passes.
  EXPECT_EQ(raw_result.elements.size(), optimized_result.elements.size());
  EXPECT_EQ(raw_result.stylesheets.size(), optimized_result.stylesheets.size());
  EXPECT_EQ(raw_result.inline_css.empty(), optimized_result.inline_css.empty());

  EXPECT_EQ(TemplateDetector::HashStructure(raw_result),
            TemplateDetector::HashStructure(optimized_result));
}

// The scanner leaves the elements inside <noscript> (and
// <template>, <noembed>, <noframes>) out of `elements`, and HashStructure
// merges them back in, so a page with them keeps the template hash it had
// when they were collected. A changed hash would send every such template
// back to analysis at once.
TEST(TemplateDetectorTest, LeftOutNoscriptElementsKeepTheHash) {
  HtmlScanner scanner;
  HtmlScanResult scan = scanner.Scan(
      "http://example.com/",
      "<html><head><noscript><link rel=\"stylesheet\" href=\"/n.css\">"
      "</noscript></head><body><noscript><div><p>JS</p></div></noscript>"
      "<div>Hero</div><template><span></span></template>"
      "<p>x</p><noscript><img src=\"/p.gif\"></noscript></body></html>");
  ASSERT_TRUE(scan.success);
  ASSERT_FALSE(scan.inert_elements.empty());

  // Every element, in document order, as the scanner collected them before.
  HtmlScanResult all = MakeResult({{"html", 0},
                                   {"head", 1},
                                   {"noscript", 2},
                                   {"link", 3},
                                   {"body", 1},
                                   {"noscript", 2},
                                   {"div", 3},
                                   {"p", 4},
                                   {"div", 2},
                                   {"template", 2},
                                   {"span", 3},
                                   {"p", 2},
                                   {"noscript", 2},
                                   {"img", 3}});
  EXPECT_EQ(TemplateDetector::HashStructure(scan),
            TemplateDetector::HashStructure(all));

  // And the left-out elements still count: without them the hash differs.
  HtmlScanResult rendered_only = scan;
  rendered_only.inert_elements.clear();
  EXPECT_NE(TemplateDetector::HashStructure(rendered_only),
            TemplateDetector::HashStructure(all));
}

// --- Profile tracking tests ---

TEST(TemplateDetectorTest, HasProfileReturnsFalseInitially) {
  TemplateDetector detector;
  EXPECT_FALSE(detector.HasProfile(12345));
  EXPECT_EQ(detector.size(), 0u);
}

TEST(TemplateDetectorTest, RecordAndLookup) {
  TemplateDetector detector;
  detector.RecordProfile(42, "https://example.com/product/1");
  EXPECT_TRUE(detector.HasProfile(42));
  EXPECT_EQ(detector.GetAnalyzedUrl(42), "https://example.com/product/1");
  EXPECT_EQ(detector.size(), 1u);
}

TEST(TemplateDetectorTest, UpdateExistingProfile) {
  TemplateDetector detector;
  detector.RecordProfile(42, "https://example.com/old");
  detector.RecordProfile(42, "https://example.com/new");
  EXPECT_EQ(detector.GetAnalyzedUrl(42), "https://example.com/new");
  EXPECT_EQ(detector.size(), 1u);
}

TEST(TemplateDetectorTest, RemoveProfile) {
  TemplateDetector detector;
  detector.RecordProfile(42, "https://example.com/");
  EXPECT_TRUE(detector.RemoveProfile(42));
  EXPECT_FALSE(detector.HasProfile(42));
  EXPECT_EQ(detector.size(), 0u);
}

TEST(TemplateDetectorTest, RemoveNonexistentReturnsFalse) {
  TemplateDetector detector;
  EXPECT_FALSE(detector.RemoveProfile(999));
}

TEST(TemplateDetectorTest, GetAnalyzedUrlReturnsEmptyForMissing) {
  TemplateDetector detector;
  EXPECT_EQ(detector.GetAnalyzedUrl(999), "");
}

TEST(TemplateDetectorTest, FIFOEvictionAtCapacity) {
  TemplateDetector detector;

  // Fill to capacity.
  for (size_t i = 0; i < TemplateDetector::kMaxTemplates; ++i) {
    detector.RecordProfile(i, "url" + std::to_string(i));
  }
  EXPECT_EQ(detector.size(), TemplateDetector::kMaxTemplates);

  // Adding one more evicts the oldest (hash=0).
  detector.RecordProfile(TemplateDetector::kMaxTemplates, "new_url");
  EXPECT_EQ(detector.size(), TemplateDetector::kMaxTemplates);
  EXPECT_FALSE(detector.HasProfile(0));
  EXPECT_TRUE(detector.HasProfile(1));
  EXPECT_TRUE(detector.HasProfile(TemplateDetector::kMaxTemplates));
}

TEST(TemplateDetectorTest, ConcurrentAccess) {
  TemplateDetector detector;
  constexpr int kThreads = 8;
  constexpr int kOpsPerThread = 200;

  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&detector, t]() {
      for (int i = 0; i < kOpsPerThread; ++i) {
        auto hash = static_cast<uint64_t>(t) * kOpsPerThread + i;
        detector.RecordProfile(hash, "url");
        detector.HasProfile(hash);
        detector.GetAnalyzedUrl(hash);
      }
    });
  }
  for (auto& t : threads) {
    t.join();
  }
  // All operations should complete without data races.
  EXPECT_LE(detector.size(), TemplateDetector::kMaxTemplates);
}

// --- Additional edge case tests ---

TEST(TemplateDetectorTest, RemoveFromMiddleOfDeque) {
  // Insert multiple profiles and remove one from the middle.
  // This exercises the for loop in RemoveProfile that iterates
  // past non-matching entries (covers line 104).
  TemplateDetector detector;
  detector.RecordProfile(10, "url10");
  detector.RecordProfile(20, "url20");
  detector.RecordProfile(30, "url30");
  detector.RecordProfile(40, "url40");
  detector.RecordProfile(50, "url50");

  EXPECT_EQ(detector.size(), 5u);

  // Remove from the middle (not front, not back).
  EXPECT_TRUE(detector.RemoveProfile(30));
  EXPECT_EQ(detector.size(), 4u);
  EXPECT_FALSE(detector.HasProfile(30));

  // Other entries remain.
  EXPECT_TRUE(detector.HasProfile(10));
  EXPECT_TRUE(detector.HasProfile(20));
  EXPECT_TRUE(detector.HasProfile(40));
  EXPECT_TRUE(detector.HasProfile(50));
}

TEST(TemplateDetectorTest, RemoveLastElement) {
  // Remove the last inserted element (tail of deque).
  TemplateDetector detector;
  detector.RecordProfile(10, "url10");
  detector.RecordProfile(20, "url20");
  detector.RecordProfile(30, "url30");

  EXPECT_TRUE(detector.RemoveProfile(30));
  EXPECT_EQ(detector.size(), 2u);
  EXPECT_TRUE(detector.HasProfile(10));
  EXPECT_TRUE(detector.HasProfile(20));
}

TEST(TemplateDetectorTest, RemoveFirstElement) {
  // Remove the first inserted element (front of deque).
  TemplateDetector detector;
  detector.RecordProfile(10, "url10");
  detector.RecordProfile(20, "url20");
  detector.RecordProfile(30, "url30");

  EXPECT_TRUE(detector.RemoveProfile(10));
  EXPECT_EQ(detector.size(), 2u);
  EXPECT_TRUE(detector.HasProfile(20));
  EXPECT_TRUE(detector.HasProfile(30));
}

TEST(TemplateDetectorTest, EvictionOrderAfterRemoveAndReinsert) {
  // Verify FIFO eviction order is correct after a middle removal
  // and reinsertion.
  TemplateDetector detector;

  // Fill to near capacity.
  for (size_t i = 0; i < TemplateDetector::kMaxTemplates - 1; ++i) {
    detector.RecordProfile(i, "url");
  }

  // Remove from the middle.
  detector.RemoveProfile(500);
  EXPECT_EQ(detector.size(), TemplateDetector::kMaxTemplates - 2);

  // Add two more to fill up.
  detector.RecordProfile(TemplateDetector::kMaxTemplates + 1, "new1");
  detector.RecordProfile(TemplateDetector::kMaxTemplates + 2, "new2");
  EXPECT_EQ(detector.size(), TemplateDetector::kMaxTemplates);

  // Adding one more should evict hash=0 (oldest remaining).
  detector.RecordProfile(TemplateDetector::kMaxTemplates + 3, "new3");
  EXPECT_FALSE(detector.HasProfile(0));
  EXPECT_TRUE(detector.HasProfile(1));
}

TEST(TemplateDetectorTest, HashWithManyElements) {
  // Test hashing with a large number of elements.
  auto result = MakeResult({}, 0, false);
  for (int i = 0; i < 500; ++i) {
    CollectedElement elem;
    elem.tag_name = "div";
    elem.depth = i % 20;
    elem.element_index = i;
    result.elements.push_back(std::move(elem));
  }

  uint64_t h1 = TemplateDetector::HashStructure(result);
  uint64_t h2 = TemplateDetector::HashStructure(result);
  EXPECT_EQ(h1, h2);
  // Hash should be non-trivial.
  EXPECT_NE(h1, 0u);
}

TEST(TemplateDetectorTest, HashEmptyTagName) {
  // Empty tag name should still produce a valid hash.
  auto result = MakeResult({{"", 0}});
  uint64_t h = TemplateDetector::HashStructure(result);
  EXPECT_NE(h, 0u);
}

TEST(TemplateDetectorTest, ConcurrentRemoveAndRecord) {
  TemplateDetector detector;
  constexpr int kThreads = 4;
  constexpr int kOps = 100;

  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&detector, t]() {
      for (int i = 0; i < kOps; ++i) {
        auto hash = static_cast<uint64_t>(t) * kOps + i;
        detector.RecordProfile(hash, "url");
        // Remove some entries while adding new ones.
        if (i > 0) {
          detector.RemoveProfile(hash - 1);
        }
      }
    });
  }
  for (auto& t : threads) {
    t.join();
  }
  // Should complete without data races.
  EXPECT_LE(detector.size(), TemplateDetector::kMaxTemplates);
}

}  // namespace
}  // namespace pagespeed
