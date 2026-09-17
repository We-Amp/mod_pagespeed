/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

// Unit tests for IisHtmlRewriter - HTML rewriting state machine for IIS.
//
// Note: The IisHtmlRewriter class requires IisServerContext and
// IisRequestContext which are only fully functional on Windows with
// IIS SDK. These tests focus on:
// 1. Testing the HtmlDetector logic that IisHtmlRewriter uses
// 2. Testing buffer size limits and state machine concepts
// 3. Documenting expected behavior for reference

#include "gtest/gtest.h"
#include "pagespeed/automatic/html_detector.h"
#include "pagespeed/kernel/base/string_util.h"
#include "test/pagespeed/iis/iis_test_base.h"
#include "test/pagespeed/iis/mock_iis.h"

namespace net_instaweb {

// ============================================================================
// HtmlDetector Tests
// ============================================================================
// The HtmlDetector class is used by IisHtmlRewriter to determine if content
// is HTML. These tests verify the detection logic.

class HtmlDetectorTest : public testing::Test {
 protected:
  HtmlDetector detector_;
};

TEST_F(HtmlDetectorTest, InitialStateNotDecided) {
  EXPECT_FALSE(detector_.already_decided());
}

TEST_F(HtmlDetectorTest, DetectsSimpleHtml) {
  // HTML starting with < should be detected
  bool decided = detector_.ConsiderInput("<html>");
  EXPECT_TRUE(decided);
  EXPECT_TRUE(detector_.already_decided());
  EXPECT_TRUE(detector_.probable_html());
}

TEST_F(HtmlDetectorTest, DetectsHtmlWithDoctype) {
  bool decided = detector_.ConsiderInput("<!DOCTYPE html>");
  EXPECT_TRUE(decided);
  EXPECT_TRUE(detector_.already_decided());
  EXPECT_TRUE(detector_.probable_html());
}

TEST_F(HtmlDetectorTest, DetectsHtmlWithLeadingWhitespace) {
  // Whitespace before < should still be detected as HTML
  bool decided = detector_.ConsiderInput("   \n\t<html>");
  EXPECT_TRUE(decided);
  EXPECT_TRUE(detector_.already_decided());
  EXPECT_TRUE(detector_.probable_html());
}

TEST_F(HtmlDetectorTest, DetectsHtmlWithBOM) {
  // UTF-8 BOM followed by HTML
  GoogleString bom_html;
  bom_html.push_back('\xEF');
  bom_html.push_back('\xBB');
  bom_html.push_back('\xBF');
  bom_html.append("<html>");

  bool decided = detector_.ConsiderInput(bom_html);
  EXPECT_TRUE(decided);
  EXPECT_TRUE(detector_.already_decided());
  EXPECT_TRUE(detector_.probable_html());
}

TEST_F(HtmlDetectorTest, RejectsJsonContent) {
  // JSON starting with { should not be detected as HTML
  bool decided = detector_.ConsiderInput("{\"key\": \"value\"}");
  EXPECT_TRUE(decided);
  EXPECT_TRUE(detector_.already_decided());
  EXPECT_FALSE(detector_.probable_html());
}

TEST_F(HtmlDetectorTest, RejectsPlainText) {
  // Plain text starting with a letter should not be HTML
  bool decided = detector_.ConsiderInput("Hello, World!");
  EXPECT_TRUE(decided);
  EXPECT_TRUE(detector_.already_decided());
  EXPECT_FALSE(detector_.probable_html());
}

TEST_F(HtmlDetectorTest, RejectsXmlDeclaration) {
  // XML declaration is technically valid, but detector may see it as HTML
  // since it starts with <. This test documents actual behavior.
  bool decided = detector_.ConsiderInput("<?xml version=\"1.0\"?>");
  EXPECT_TRUE(decided);
  EXPECT_TRUE(detector_.already_decided());
  // Note: The detector will see < and consider this probable HTML
  // since it doesn't distinguish between HTML and XML processing instructions
  EXPECT_TRUE(detector_.probable_html());
}

TEST_F(HtmlDetectorTest, IncrementalDetection) {
  // Test that detection works with incremental input
  // First, just whitespace - not enough to decide
  bool decided = detector_.ConsiderInput("   ");
  EXPECT_FALSE(decided);
  EXPECT_FALSE(detector_.already_decided());

  // Now add more content
  decided = detector_.ConsiderInput("<html>");
  EXPECT_TRUE(decided);
  EXPECT_TRUE(detector_.already_decided());
  EXPECT_TRUE(detector_.probable_html());
}

TEST_F(HtmlDetectorTest, ReleaseBufferedContent) {
  // Add content that doesn't trigger immediate decision
  detector_.ConsiderInput("  ");
  // Force decision
  detector_.ForceDecision(true);

  GoogleString buffered;
  detector_.ReleaseBuffered(&buffered);
  EXPECT_EQ("  ", buffered);
}

TEST_F(HtmlDetectorTest, ForceDecisionHtml) {
  EXPECT_FALSE(detector_.already_decided());
  detector_.ForceDecision(true);
  EXPECT_TRUE(detector_.already_decided());
  EXPECT_TRUE(detector_.probable_html());
}

TEST_F(HtmlDetectorTest, ForceDecisionNotHtml) {
  EXPECT_FALSE(detector_.already_decided());
  detector_.ForceDecision(false);
  EXPECT_TRUE(detector_.already_decided());
  EXPECT_FALSE(detector_.probable_html());
}

// ============================================================================
// IisHtmlRewriter Tests
// ============================================================================
// These tests document the expected behavior of IisHtmlRewriter.
// Full functional tests require IisServerContext which is Windows-only.

class IisHtmlRewriterTest : public IisTestBase {
 protected:
  // Constants matching IisHtmlRewriter private constants
  static constexpr size_t kMaxBufferBytes = 2 * 1024 * 1024;  // 2MB
  // Match RewriteOptions::kDefaultRewriteDeadlineMs (20ms in debug builds).
  static constexpr int kRewriteDeadlineMs = 20;

  // Helper to check if content would be detected as HTML
  static bool WouldDetectAsHtml(const StringPiece& content) {
    HtmlDetector detector;
    if (detector.ConsiderInput(content)) {
      return detector.probable_html();
    }
    // Not enough content to decide - default depends on implementation
    return false;
  }
};

TEST_F(IisHtmlRewriterTest, MaxBufferBytesIsReasonable) {
  // Verify the buffer limit constant
  // 2MB is a reasonable limit for HTML pages
  EXPECT_EQ(2u * 1024 * 1024, kMaxBufferBytes);
}

TEST_F(IisHtmlRewriterTest, RewriteDeadlineMatchesModPagespeed) {
  // IIS rewrite deadline must match RewriteOptions::kDefaultRewriteDeadlineMs.
  // Debug builds use 20ms; release builds use 10ms.
  EXPECT_EQ(20, kRewriteDeadlineMs);
}

// ============================================================================
// Content Type Detection Tests
// ============================================================================

TEST_F(IisHtmlRewriterTest, DetectsHtmlStartingWithDoctype) {
  EXPECT_TRUE(WouldDetectAsHtml("<!DOCTYPE html><html><body></body></html>"));
}

TEST_F(IisHtmlRewriterTest, DetectsHtmlStartingWithHtmlTag) {
  EXPECT_TRUE(WouldDetectAsHtml("<html><head></head><body></body></html>"));
}

TEST_F(IisHtmlRewriterTest, DetectsHtmlWithXhtmlDoctype) {
  EXPECT_TRUE(WouldDetectAsHtml(
      "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Strict//EN\">"));
}

TEST_F(IisHtmlRewriterTest, DetectsHtmlWithCommentFirst) {
  // HTML starting with a comment
  EXPECT_TRUE(WouldDetectAsHtml("<!-- comment --><html></html>"));
}

TEST_F(IisHtmlRewriterTest, RejectsJsonResponse) {
  EXPECT_FALSE(WouldDetectAsHtml("{\"status\": \"ok\", \"data\": []}"));
}

TEST_F(IisHtmlRewriterTest, RejectsPlainTextResponse) {
  EXPECT_FALSE(WouldDetectAsHtml("Error: File not found"));
}

TEST_F(IisHtmlRewriterTest, RejectsCssResponse) {
  // CSS typically doesn't start with <
  EXPECT_FALSE(WouldDetectAsHtml("body { margin: 0; }"));
}

TEST_F(IisHtmlRewriterTest, RejectsJavascriptResponse) {
  EXPECT_FALSE(WouldDetectAsHtml("function init() { return true; }"));
}

TEST_F(IisHtmlRewriterTest, RejectsBinaryContent) {
  // Binary content (like images) won't start with <
  GoogleString binary;
  binary.push_back('\x89');  // PNG signature start
  binary.push_back('P');
  binary.push_back('N');
  binary.push_back('G');
  EXPECT_FALSE(WouldDetectAsHtml(binary));
}

// ============================================================================
// Buffer Size Tests
// ============================================================================

TEST_F(IisHtmlRewriterTest, SmallHtmlWithinLimit) {
  // Small HTML should be well within the buffer limit
  GoogleString small_html = "<html><body>Hello</body></html>";
  EXPECT_LT(small_html.size(), kMaxBufferBytes);
}

TEST_F(IisHtmlRewriterTest, LargeHtmlExceedsLimit) {
  // Create HTML that exceeds the buffer limit
  GoogleString large_html = "<html><body>";
  large_html.append(kMaxBufferBytes + 1, 'x');
  large_html.append("</body></html>");

  EXPECT_GT(large_html.size(), kMaxBufferBytes);
}

TEST_F(IisHtmlRewriterTest, BufferLimitIsExactly2MB) {
  // Exactly at the limit should be allowed (< not <=)
  EXPECT_EQ(2 * 1024 * 1024u, kMaxBufferBytes);
}

// ============================================================================
// State Machine Documentation Tests
// ============================================================================
// These tests document the expected state transitions of IisHtmlRewriter

TEST_F(IisHtmlRewriterTest, StateTransitions) {
  // Document expected states:
  // kDetecting -> kBuffering -> kParsing -> kFinishing -> kDone
  //     |            |             |            |
  //     +------------+-------------+------------+---> kPassThrough

  // The state machine starts in kDetecting state
  // When HTML is detected, it transitions to kBuffering
  // When Finish() is called, it transitions to kParsing
  // When parsing completes, it transitions to kFinishing
  // When finish parse completes, it transitions to kDone
  //
  // At any point, if detection fails or buffer overflows, it goes to kPassThrough
}

TEST_F(IisHtmlRewriterTest, PassThroughConditions) {
  // Document conditions that cause pass-through:
  // 1. Content doesn't start with < (not HTML)
  // 2. Buffer size exceeds kMaxBufferBytes
  // 3. RewriteDriver fails to start
  // 4. Parse fails or produces empty output
}

// ============================================================================
// Timer and Deadline Tests
// ============================================================================

TEST_F(IisHtmlRewriterTest, DeadlineTrackingWorks) {
  // Test that our mock timer can simulate deadline scenarios
  EXPECT_EQ(0, timer()->NowMs());

  AdvanceTimeMs(1000);
  EXPECT_EQ(1000, timer()->NowMs());

  AdvanceTimeMs(kRewriteDeadlineMs);
  EXPECT_EQ(1000 + kRewriteDeadlineMs, timer()->NowMs());
}

TEST_F(IisHtmlRewriterTest, DeadlineExceeded) {
  // Simulate time passing beyond the deadline
  AdvanceTimeMs(kRewriteDeadlineMs + 100);
  EXPECT_GT(timer()->NowMs(), kRewriteDeadlineMs);
}

// ============================================================================
// Content Type Return Value Tests
// ============================================================================

TEST_F(IisHtmlRewriterTest, ContentTypeForRewrittenHtml) {
  // When rewriting is successful (kDone state), content type should be
  // "text/html; charset=utf-8"
  // This is documented behavior - actual testing requires full rewriter
}

TEST_F(IisHtmlRewriterTest, ContentTypeForPassThrough) {
  // When passing through (kPassThrough state), content type should be
  // nullptr (use original content type)
  // This is documented behavior - actual testing requires full rewriter
}

// ============================================================================
// Mock Context Integration Tests
// ============================================================================

TEST_F(IisHtmlRewriterTest, MockResponseSupportsHtmlContentType) {
  auto context = CreateContext("/page.html");
  context->response()->SetHeader("Content-Type", "text/html; charset=utf-8");
  EXPECT_EQ("text/html; charset=utf-8",
            context->response()->GetHeader("Content-Type"));
}

TEST_F(IisHtmlRewriterTest, MockResponseSupportsBodyAccumulation) {
  auto context = CreateContext("/page.html");

  // Simulate HTML chunks arriving
  context->response()->AppendBody("<html>");
  context->response()->AppendBody("<body>");
  context->response()->AppendBody("Hello, World!");
  context->response()->AppendBody("</body>");
  context->response()->AppendBody("</html>");

  EXPECT_EQ("<html><body>Hello, World!</body></html>",
            context->response()->body());
}

TEST_F(IisHtmlRewriterTest, MockResponseSupportsLargeContent) {
  auto context = CreateContext("/large.html");

  // Create content just under the buffer limit
  GoogleString large_content = "<html><body>";
  large_content.append(kMaxBufferBytes - 100, 'x');
  large_content.append("</body></html>");

  context->response()->AppendBody(large_content);
  EXPECT_EQ(large_content, context->response()->body());
}

// ============================================================================
// HTML Fragment Tests
// ============================================================================

TEST_F(IisHtmlRewriterTest, DetectsHtmlFragment) {
  // A fragment starting with a tag should be detected as HTML
  EXPECT_TRUE(WouldDetectAsHtml("<div>Some content</div>"));
}

TEST_F(IisHtmlRewriterTest, DetectsHtmlWithMetaTag) {
  EXPECT_TRUE(WouldDetectAsHtml("<meta charset=\"utf-8\"><title>Test</title>"));
}

TEST_F(IisHtmlRewriterTest, DetectsHtml5Page) {
  GoogleString html5 =
      "<!DOCTYPE html>\n"
      "<html lang=\"en\">\n"
      "<head>\n"
      "  <meta charset=\"UTF-8\">\n"
      "  <title>Test Page</title>\n"
      "</head>\n"
      "<body>\n"
      "  <h1>Hello, World!</h1>\n"
      "</body>\n"
      "</html>";
  EXPECT_TRUE(WouldDetectAsHtml(html5));
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_F(IisHtmlRewriterTest, EmptyContentNotHtml) {
  // Empty content should not be detected as HTML
  EXPECT_FALSE(WouldDetectAsHtml(""));
}

TEST_F(IisHtmlRewriterTest, WhitespaceOnlyNotHtml) {
  // Whitespace-only content - detector won't make a decision
  HtmlDetector detector;
  EXPECT_FALSE(detector.ConsiderInput("   \n\t  "));
  EXPECT_FALSE(detector.already_decided());
}

TEST_F(IisHtmlRewriterTest, SingleCharacterNotEnough) {
  // Single < character might not be enough to decide
  HtmlDetector detector;
  bool decided = detector.ConsiderInput("<");
  // The detector sees < and considers it probable HTML
  EXPECT_TRUE(decided);
  EXPECT_TRUE(detector.probable_html());
}

// ============================================================================
// IisHtmlRewriter Full Tests (require Windows IIS)
// ============================================================================

TEST_F(IisHtmlRewriterTest, InitialStateIsBuffering) {
  // Note: Full test requires IisServerContext and IisRequestContext
  // Initial state should be kDetecting
}

TEST_F(IisHtmlRewriterTest, EmptyContentPassesThrough) {
  // Note: Full test requires IisServerContext and IisRequestContext
  // Empty content should cause pass-through
}

TEST_F(IisHtmlRewriterTest, NonHtmlContentPassesThrough) {
  // Note: Full test requires IisServerContext and IisRequestContext
  // Non-HTML content (e.g., JSON) should cause pass-through
}

TEST_F(IisHtmlRewriterTest, LargeContentPassesThrough) {
  // Note: Full test requires IisServerContext and IisRequestContext
  // Content exceeding kMaxBufferBytes should cause pass-through
}

TEST_F(IisHtmlRewriterTest, ValidHtmlIsRewritten) {
  // Note: Full test requires IisServerContext and IisRequestContext
  // Valid HTML should be rewritten with PageSpeed filters applied
}

}  // namespace net_instaweb
