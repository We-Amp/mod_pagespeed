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

#include <memory>

#include "net/instaweb/rewriter/public/js_replacer.h"

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "test/pagespeed/kernel/base/gtest.h"

using pagespeed::js::JsTokenizerPatterns;

namespace net_instaweb {

namespace {

class JsReplacerTest : public testing::Test {
 public:
  JsReplacerTest() : replacer_(&patterns_) {}

  void AppendTail(GoogleString* str) { StrAppend(str, " with tail"); }

  void AppendHead(GoogleString* str) { *str = "head with " + *str; }

  // Replaces the value with one that contains a single quote, a double quote, a
  // backslash, and a newline -- all characters that must be escaped before the
  // value is spliced back into a JS string literal.
  void InjectSpecials(GoogleString* str) { *str = "a'b\"c\\d\ne"; }

  // Leaves the value exactly as-is.
  void Passthrough(GoogleString* str) {}

 protected:
  JsTokenizerPatterns patterns_;
  JsReplacer replacer_;
};

TEST_F(JsReplacerTest, EmptyNoOp) {
  const char kIn[] = "function foo() {\n  return 42;\n}";
  GoogleString out;
  replacer_.Transform(kIn, &out);
  EXPECT_EQ(kIn, out);
}

TEST_F(JsReplacerTest, BasicMatch) {
  const char kIn[] = "a.b.c = \"42\"; document.domain = 'whatever.com';";
  const char kOut[] =
      "a.b.c = \"42\"; document.domain = 'whatever.com with tail';";
  std::unique_ptr<JsReplacer::StringRewriter> rewriter(
      NewPermanentCallback(this, &JsReplacerTest::AppendTail));
  replacer_.AddPattern("document", "domain", rewriter.get());

  GoogleString out;
  replacer_.Transform(kIn, &out);
  EXPECT_EQ(kOut, out);
}

TEST_F(JsReplacerTest, RedundantPattern) {
  // Make sure the documented behavior of redundant patterns actually happens.
  const char kIn[] = "a.b.c = \"42\"; document.domain = 'whatever.com';";
  const char kOut[] =
      "a.b.c = \"42\"; document.domain = 'whatever.com with tail';";
  std::unique_ptr<JsReplacer::StringRewriter> rewriter(
      NewPermanentCallback(this, &JsReplacerTest::AppendTail));
  std::unique_ptr<JsReplacer::StringRewriter> rewriter2(
      NewPermanentCallback(this, &JsReplacerTest::AppendHead));
  replacer_.AddPattern("document", "domain", rewriter.get());
  replacer_.AddPattern("document", "domain", rewriter2.get());

  GoogleString out;
  replacer_.Transform(kIn, &out);
  EXPECT_EQ(kOut, out);
}

TEST_F(JsReplacerTest, TwoPatterns) {
  // Test two different patterns.
  const char kIn[] = "a.b.c = \"42\"; document.domain = 'whatever.com';";
  const char kOut[] =
      "a.b.c = \"head with 42\"; document.domain = 'whatever.com with tail';";
  std::unique_ptr<JsReplacer::StringRewriter> rewriter(
      NewPermanentCallback(this, &JsReplacerTest::AppendTail));
  std::unique_ptr<JsReplacer::StringRewriter> rewriter2(
      NewPermanentCallback(this, &JsReplacerTest::AppendHead));
  replacer_.AddPattern("document", "domain", rewriter.get());
  replacer_.AddPattern("b", "c", rewriter2.get());

  GoogleString out;
  replacer_.Transform(kIn, &out);
  EXPECT_EQ(kOut, out);
}

TEST_F(JsReplacerTest, CommentsOk) {
  const char kIn[] =
      "a.b.c = \"42\"; document.domain = /*relax*/ 'whatever.com';";
  const char kOut[] =
      "a.b.c = \"42\"; document.domain = /*relax*/ 'whatever.com with tail';";
  std::unique_ptr<JsReplacer::StringRewriter> rewriter(
      NewPermanentCallback(this, &JsReplacerTest::AppendTail));
  replacer_.AddPattern("document", "domain", rewriter.get());

  GoogleString out;
  replacer_.Transform(kIn, &out);
  EXPECT_EQ(kOut, out);
}

TEST_F(JsReplacerTest, EscapesRewrittenValue) {
  // A rewritten value containing characters that are significant inside a JS
  // string literal must be escaped so we don't emit broken or injectable JS.
  const char kIn[] = "document.domain = 'x';";
  const char kOut[] = "document.domain = 'a\\'b\\\"c\\\\d\\ne';";
  std::unique_ptr<JsReplacer::StringRewriter> rewriter(
      NewPermanentCallback(this, &JsReplacerTest::InjectSpecials));
  replacer_.AddPattern("document", "domain", rewriter.get());

  GoogleString out;
  replacer_.Transform(kIn, &out);
  EXPECT_EQ(kOut, out);
}

TEST_F(JsReplacerTest, PreservesExistingEscapesRoundTrip) {
  // A rewriter that leaves the value untouched must reproduce the original
  // source byte-for-byte, even when the literal already contains escapes: the
  // value is decoded before the rewriter runs and re-encoded afterwards, so
  // pre-existing escapes must not get double-escaped.
  const char kIn[] =
      "document.domain = 'it\\'s'; a.b.c = \"x\\\\y\"; d.e.f = 'plain';";
  std::unique_ptr<JsReplacer::StringRewriter> rewriter(
      NewPermanentCallback(this, &JsReplacerTest::Passthrough));
  std::unique_ptr<JsReplacer::StringRewriter> rewriter2(
      NewPermanentCallback(this, &JsReplacerTest::Passthrough));
  std::unique_ptr<JsReplacer::StringRewriter> rewriter3(
      NewPermanentCallback(this, &JsReplacerTest::Passthrough));
  replacer_.AddPattern("document", "domain", rewriter.get());
  replacer_.AddPattern("b", "c", rewriter2.get());
  replacer_.AddPattern("e", "f", rewriter3.get());

  GoogleString out;
  replacer_.Transform(kIn, &out);
  EXPECT_EQ(kIn, out);
}

TEST_F(JsReplacerTest, SolidusEscapeDecodes) {
  // "\/" is a legal escape the encoder itself emits (to break "</script"), so
  // the decoder must accept it. Re-encoding emits a bare "/" when no breakout
  // sequence is present: semantically identical, not byte-identical.
  const char kIn[] = "document.domain = 'https:\\/\\/example.com';";
  std::unique_ptr<JsReplacer::StringRewriter> rewriter(
      NewPermanentCallback(this, &JsReplacerTest::Passthrough));
  replacer_.AddPattern("document", "domain", rewriter.get());

  GoogleString out;
  replacer_.Transform(kIn, &out);
  EXPECT_EQ("document.domain = 'https://example.com';", out);
}

TEST_F(JsReplacerTest, MalformedEscapeLeftUntouched) {
  // An unrecognized escape sequence must not be decoded; the candidate passes
  // through verbatim and the rewriter is not invoked.
  const char kIn[] = "document.domain = 'a\\q';";
  std::unique_ptr<JsReplacer::StringRewriter> rewriter(
      NewPermanentCallback(this, &JsReplacerTest::AppendTail));
  replacer_.AddPattern("document", "domain", rewriter.get());

  GoogleString out;
  replacer_.Transform(kIn, &out);
  EXPECT_EQ(kIn, out);
}

}  // namespace

}  // namespace net_instaweb
