// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.
//
// Keeps CSS written into the page on the checked path.
//
// CSS a filter writes into a <style> element goes through
// InlineStyleText::Prepare() and AppendInlineStyleText() (see
// pagespeed/kernel/html/inline_style_text.h and
// net/instaweb/rewriter/public/inline_style_util.h). CSS summaries that are
// written into a script go through CriticalCssBeaconFilter's
// SelectorFitsScript(). This test reads the rewriter sources and fails when
// that could have changed without review:
//   * any new place that adds a text node to the page, or rewrites one, in
//     the rewriter (the lists below are exact counts per file);
//   * any new reader of CSS summaries, or new filter built on
//     CssSummarizerBase;
//   * a summary reader that writes script without SelectorFitsScript(), or
//     style text other than through AppendInlineStyleText();
//   * a change to where style text is prepared or appended.
// When it fails for a new writer that does not write CSS-derived text, add
// the file to the list with its count. When the writer does write CSS, route
// it through the functions above first.

#include <cstdlib>
#include <map>
#include <vector>

#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/stdio_file_system.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {

namespace {

using FileCounts = std::map<GoogleString, int>;

class InlineStyleWritersTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const char* paths = std::getenv("INLINE_STYLE_LINT_SOURCES");
    ASSERT_TRUE(paths != nullptr);
    StringPieceVector list;
    SplitStringPieceToVector(paths, " ", &list, true);
    StdioFileSystem file_system;
    NullMessageHandler handler;
    for (StringPiece path : list) {
      GoogleString contents;
      ASSERT_TRUE(file_system.ReadFile(RunfilePath(path.as_string()).c_str(),
                                       &contents, &handler))
          << path;
      // Keyed by the name below net/instaweb/rewriter/.
      static const char kPrefix[] = "net/instaweb/rewriter/";
      const size_t at = path.find(kPrefix);
      ASSERT_NE(StringPiece::npos, at) << path;
      sources_[path.substr(at + STATIC_STRLEN(kPrefix)).as_string()] = contents;
    }
    ASSERT_LT(100u, sources_.size());
  }

  static int Count(StringPiece text, StringPiece pattern) {
    int n = 0;
    for (size_t pos = text.find(pattern); pos != StringPiece::npos;
         pos = text.find(pattern, pos + pattern.size())) {
      ++n;
    }
    return n;
  }

  // The files that contain `pattern`, with how often, among those whose name
  // ends in `suffix`.
  FileCounts CountsOf(StringPiece pattern, StringPiece suffix = ".cc") {
    FileCounts counts;
    for (const auto& file : sources_) {
      if (!StringPiece(file.first).ends_with(suffix)) {
        continue;
      }
      const int n = Count(file.second, pattern);
      if (n > 0) {
        counts[file.first] = n;
      }
    }
    return counts;
  }

  const GoogleString& Source(const GoogleString& name) {
    return sources_[name];
  }

  std::map<GoogleString, GoogleString> sources_;
};

TEST_F(InlineStyleWritersTest, TextNodeWritersAreKnown) {
  // Every place in the rewriter that adds a text node to the page. Only
  // inline_style_util.cc may add one under a <style> element made from CSS.
  const FileCounts kNewCharactersNode = {
      {"add_instrumentation_filter.cc", 2},
      {"common_filter.cc", 1},
      {"defer_iframe_filter.cc", 1},
      {"image_rewrite_filter.cc", 1},
      {"inline_style_util.cc", 1},
      {"insert_speculation_rules_filter.cc", 1},
      {"js_combine_filter.cc", 1},
      {"js_disable_filter.cc", 1},
      {"js_inline_filter.cc", 2},
      {"lazyload_images_filter.cc", 2},
      {"make_show_ads_async_filter.cc", 1},
      {"redirect_on_size_limit_filter.cc", 1},
      {"support_noscript_filter.cc", 1},
  };
  EXPECT_EQ(kNewCharactersNode, CountsOf("NewCharactersNode("));

  // Every place that appends to the text of a node.
  const FileCounts kAppend = {
      {"js_inline_filter.cc", 2},
  };
  EXPECT_EQ(kAppend, CountsOf("->Append("));

  // Every place that rewrites the text of an existing node.
  const FileCounts kMutableContents = {
      {"css_inline_import_to_link_filter.cc", 1},
      {"inline_resource_slot.cc", 1},
      {"javascript_filter.cc", 1},
  };
  EXPECT_EQ(kMutableContents, CountsOf("mutable_contents()"));
}

TEST_F(InlineStyleWritersTest, StyleTextIsPrepared) {
  const FileCounts kPrepare = {
      {"critical_selector_filter.cc", 1},
      {"css_filter.cc", 1},
      {"css_inline_filter.cc", 2},
      {"css_summarizer_base.cc", 1},
  };
  EXPECT_EQ(kPrepare, CountsOf("InlineStyleText::Prepare("));

  const FileCounts kAppend = {
      {"critical_selector_filter.cc", 1},
      {"css_inline_filter.cc", 1},
      {"inline_style_util.cc", 1},
  };
  EXPECT_EQ(kAppend, CountsOf("AppendInlineStyleText("));

  // The slot that writes cached inline style text checks it.
  const FileCounts kSlots = {{"rewrite_driver.cc", 1}};
  EXPECT_EQ(kSlots, CountsOf("new InlineResourceSlot("));
  EXPECT_EQ(1,
            Count(Source("inline_resource_slot.cc"), "IsSafeInlineStyleText("));

  // The old check that wrote what it had not checked is gone.
  EXPECT_TRUE(CountsOf("MayContainMarkup").empty());
  EXPECT_TRUE(CountsOf("MayContainMarkup", ".h").empty());
}

TEST_F(InlineStyleWritersTest, SummaryReadersAreKnown) {
  const FileCounts kSubclasses = {
      {"public/critical_css_beacon_filter.h", 1},
      {"public/critical_selector_filter.h", 1},
  };
  EXPECT_EQ(kSubclasses, CountsOf("public CssSummarizerBase", ".h"));

  const FileCounts kReaders = {
      {"critical_css_beacon_filter.cc", 1},
      {"critical_selector_filter.cc", 1},
  };
  EXPECT_EQ(kReaders, CountsOf("GetSummaryForStyle("));

  // Only the critical CSS filter's summaries are written as <style> text.
  const FileCounts kStyleText = {
      {"public/critical_selector_filter.h", 1},
      {"public/css_summarizer_base.h", 1},
  };
  EXPECT_EQ(kStyleText, CountsOf("bool SummariesAreStyleText()", ".h"));
}

TEST_F(InlineStyleWritersTest, SummariesReachScriptsOnlyChecked) {
  // The beacon writes summaries into a script: each selector is checked as
  // it is written.
  const GoogleString& beacon = Source("critical_css_beacon_filter.cc");
  EXPECT_EQ(2, Count(beacon, "SelectorFitsScript("));
  EXPECT_EQ(3, Count(beacon, "EscapeToJsStringLiteral("));
  EXPECT_EQ(1, Count(beacon, "AddJsToElement("));
  EXPECT_EQ(0, Count(beacon, "NewCharactersNode("));
  EXPECT_EQ(0, Count(beacon, "mutable_contents()"));

  // The critical CSS filter writes one script, the loader from the static
  // assets, and no summary text into it.
  const GoogleString& critical = Source("critical_selector_filter.cc");
  EXPECT_EQ(1, Count(critical, "AddJsToElement("));
  EXPECT_EQ(0, Count(critical, "EscapeToJsStringLiteral("));
  EXPECT_EQ(0, Count(critical, "NewCharactersNode("));
  EXPECT_EQ(0, Count(critical, "mutable_contents()"));
}

}  // namespace

}  // namespace net_instaweb
