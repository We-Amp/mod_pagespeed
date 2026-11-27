// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Direct lint + compile coverage for the 2.0-owned lib/js/compat/ shim
// headers (#1104 item 3). Before this TU, compat/logging.h was included
// ONLY by the vendored kernel .cc files — all lint-excluded TUs — so it
// was never diagnosed at all; that is the genuinely new direct surface.
// The other four already reached linted TUs transitively (e.g. compat/re2.h
// via js_tokenizer.h: -exclude-header-filter suppresses diagnostics located
// IN the vendored headers, not in headers they include), but only through
// vendored include chains that a canonical-side refactor could silently
// drop. This TU includes every compat header DIRECTLY, so ci.yml's
// -header-filter picks all five up as first-party on every full-mode lint
// run (a compat-header edit forces full mode) independent of what the
// vendored files happen to include. Keep the include list in sync with the
// hdrs of //lib/js:compat.
//
// The tests are deliberately trivial smoke assertions over the shims'
// contracts; the real behavioral coverage of the kernel lives in the
// sibling js_tokenizer/js_minify suites and the differential job.

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

#include "gtest/gtest.h"
#include "lib/js/compat/basictypes.h"
#include "lib/js/compat/logging.h"
#include "lib/js/compat/re2.h"
#include "lib/js/compat/string.h"
#include "lib/js/compat/string_util.h"

namespace {

TEST(JsCompatHeadersTest, AliasesAreTrueStdAliases) {
  static_assert(std::is_same_v<GoogleString, std::string>);
  static_assert(std::is_same_v<StringPiece, std::string_view>);
  static_assert(std::is_same_v<Re2StringPiece, std::string_view>);
  static_assert(std::is_same_v<stringpiece_ssize_type, size_t>);
  static_assert(std::is_same_v<int32, int32_t>);
  static_assert(std::is_same_v<uint32, uint32_t>);
  static_assert(std::is_same_v<int64, int64_t>);
  static_assert(std::is_same_v<uint64, uint64_t>);
}

TEST(JsCompatHeadersTest, StringUtilShims) {
  EXPECT_TRUE(strings::StartsWith("function f()", "function"));
  EXPECT_FALSE(strings::StartsWith("f", "function"));
  EXPECT_TRUE(strings::EndsWith("a.min.js", ".js"));
  EXPECT_FALSE(strings::EndsWith("a.css", ".js"));
}

TEST(JsCompatHeadersTest, LoggingMacrosCompileAndPassingChecksAreQuiet) {
  DCHECK(true) << "streamed message is discarded";
  DCHECK_EQ(1, 1) << 1;
  DCHECK_NE(1, 2);
  DCHECK_GE(2, 1);
  DCHECK_GT(2, 1);
  DCHECK_LE(1, 2);
  DCHECK_LT(1, 2);
  // LOG(DFATAL) asserts when executed in !NDEBUG builds; instantiate the
  // expansion without running it.
  auto unreachable_by_contract = [] { LOG(DFATAL) << "never executed"; };
  (void)unreachable_by_contract;
}

TEST(JsCompatHeadersTest, Re2GlueRoundTripsAndMatches) {
  const StringPiece text = "var x = 42;";
  EXPECT_EQ(Re2ToStringPiece(StringPieceToRe2(text)), text);
  const RE2 posix_re("[a-z]+", re2::posix_syntax);
  EXPECT_TRUE(RE2::PartialMatch(StringPieceToRe2(text), posix_re));
  EXPECT_TRUE(RE2::FullMatch("42", "[0-9]+"));
}

}  // namespace
