// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The rule_packs options on nginx.
//
// RulePackFile and RulePackMode are registered once, in RewriteOptions, with
// server scope; their parsing, merging and scope are executed by
// rewrite_options_test and by the option tests of the ports that can be
// linked. nginx cannot be linked into a unit test (nginx is a headers-only
// library in this build graph; see the comment at the top of BUILD), so what
// is pinned here, by reading the source, is that the nginx options class
// leaves both names to the base class: its name-based parse falls through to
// SystemRewriteOptions, its scope lookup reads the registered scope, and
// neither name is listed in the nginx-only scope tables (which would change
// where the directive may appear).

#include <cctype>

#include "pagespeed/kernel/base/file_system.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/stdio_file_system.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

const char kNgxOptionsCc[] = "pagespeed/nginx/ngx_rewrite_options.cc";

GoogleString ReadSource(const GoogleString& relative_path) {
  StdioFileSystem file_system;
  NullMessageHandler handler;
  GoogleString source;
  const GoogleString path = StrCat(GTestSrcDir(), "/", relative_path);
  EXPECT_TRUE(file_system.ReadFile(path.c_str(), &source, &handler)) << path;
  return source;
}

// Whitespace-free, so a pin does not depend on where clang-format breaks a
// line.
GoogleString StripWhitespace(const GoogleString& text) {
  GoogleString out;
  for (const char c : text) {
    if (!isspace(static_cast<unsigned char>(c))) {
      out.push_back(c);
    }
  }
  return out;
}

// The body of the function whose definition starts with `signature`, up to
// the next function definition at column zero.
GoogleString FunctionBody(const GoogleString& source,
                          const GoogleString& signature) {
  const size_t start = source.find(signature);
  if (start == GoogleString::npos) {
    return GoogleString();
  }
  const size_t end = source.find("\n}\n", start);
  return source.substr(
      start, end == GoogleString::npos ? GoogleString::npos : end - start);
}

TEST(NgxRulePackOptionsPinTest, TheNamesAreLeftToTheBaseClass) {
  const GoogleString source = ReadSource(kNgxOptionsCc);
  ASSERT_FALSE(source.empty());
  // Neither name appears anywhere in the nginx options class: no nginx-only
  // registration, no entry in the server-only or main-only scope tables, no
  // special case in the parser.
  EXPECT_EQ(GoogleString::npos, source.find("RulePackFile"));
  EXPECT_EQ(GoogleString::npos, source.find("RulePackMode"));
  EXPECT_EQ(GoogleString::npos, source.find("kRulePack"));
}

TEST(NgxRulePackOptionsPinTest, ParseFallsThroughToTheSystemOptions) {
  const GoogleString source = ReadSource(kNgxOptionsCc);
  const GoogleString body = StripWhitespace(
      FunctionBody(source, "NgxRewriteOptions::ParseAndSetOptionFromName1("));
  ASSERT_FALSE(body.empty());
  EXPECT_NE(GoogleString::npos,
            body.find("returnSystemRewriteOptions::ParseAndSetOptionFromName1("
                      "name,arg,msg,handler);"));
}

TEST(NgxRulePackOptionsPinTest, ScopeLookupReadsTheRegisteredScope) {
  const GoogleString source = ReadSource(kNgxOptionsCc);
  const GoogleString body =
      StripWhitespace(FunctionBody(source,
                                   "RewriteOptions::OptionScope "
                                   "NgxRewriteOptions::GetOptionScope("));
  ASSERT_FALSE(body.empty());
  // Options not in the nginx tables get the scope they were registered with
  // (server scope for both rule pack options), so a location block refuses
  // them.
  EXPECT_NE(GoogleString::npos, body.find("all_options()"));
  EXPECT_NE(GoogleString::npos, body.find(":option->scope();"));
}

}  // namespace
}  // namespace net_instaweb
