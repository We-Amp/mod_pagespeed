// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

// Source pin: a request served under a location with pagespeed settings of
// its own takes the server's current cache invalidation state.
//
// Location options are a copy of the server's, made when the configuration is
// read, and each request served under the location gets a copy of them. Cache
// purges and flushes are recorded on the server's options only, so the request
// copy has to take the server's current PurgeSet (the mechanism is tested in
// rewrite_options_test, CopyTakesLaterPurgesFromPurgeSet). nginx cannot be
// linked into a unit test (see the comment at the top of BUILD), so this reads
// the source. Whitespace is stripped, not comments, so the pinned tokens may
// not appear in a comment either.

#include <cctype>

#include "base/logging.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/stdio_file_system.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

GoogleString ReadStrippedSource(const GoogleString& relative_path) {
  StdioFileSystem file_system;
  NullMessageHandler handler;
  GoogleString source;
  const GoogleString path = StrCat(GTestSrcDir(), "/", relative_path);
  CHECK(file_system.ReadFile(path.c_str(), &source, &handler)) << path;
  CHECK(!source.empty()) << path;
  GoogleString out;
  for (const char c : source) {
    if (!isspace(static_cast<unsigned char>(c))) {
      out.push_back(c);
    }
  }
  return out;
}

// Returns the body of the function whose definition starts with `def`, or ""
// if it was renamed. Counts braces only.
GoogleString FunctionBody(const GoogleString& cc, const char* def) {
  const size_t fn = cc.find(def);
  if (fn == GoogleString::npos) {
    return "";
  }
  const size_t open = cc.find('{', fn);
  int depth = 0;
  for (size_t i = open; i < cc.size(); ++i) {
    if (cc[i] == '{') {
      ++depth;
    } else if (cc[i] == '}' && --depth == 0) {
      return cc.substr(open, i - open);
    }
  }
  return "";
}

TEST(NgxLocationPurgePinTest, LocationOptionsTakeTheServersPurgeSet) {
  const GoogleString cc =
      ReadStrippedSource("pagespeed/nginx/ngx_pagespeed.cc");
  const GoogleString body = FunctionBody(cc, "boolps_determine_options(");
  ASSERT_FALSE(body.empty()) << "ps_determine_options was renamed";

  // The location branch: both ways the request copy is made from the
  // location's options, then the server's current PurgeSet.
  const char kBranch[] =
      "if(directory_options!=nullptr){"
      "if(*options!=nullptr){(*options)->Merge(*directory_options);}"
      "else{*options=directory_options->Clone();}";
  const size_t branch = body.find(kBranch);
  ASSERT_NE(GoogleString::npos, branch) << "the location branch changed";
  const size_t refresh = body.find(
      "(*options)->UpdateCachePurgeSet(global_options->purge_set());", branch);
  ASSERT_NE(GoogleString::npos, refresh)
      << "a request under a location no longer takes the server's purges";
  // Inside the location branch: the other branch needs no refresh, since its
  // copy is made from the server's options on each request.
  const size_t other_branch = body.find(
      "}else{if(*options==nullptr){*options=global_options->Clone();}", branch);
  ASSERT_NE(GoogleString::npos, other_branch) << "the server branch changed";
  EXPECT_LT(refresh, other_branch)
      << "the refresh moved out of the location branch";
}

}  // namespace
}  // namespace net_instaweb
