// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// The nginx port consults the shared in-place URL agreement
// (pagespeed/system/inplace_url_agreement.h, unit tested in
// test/pagespeed/system/inplace_url_agreement_test.cc) with the server's own
// reading of the request.

#include <cctype>

#include "base/logging.h"
#include "pagespeed/kernel/base/file_system.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/stdio_file_system.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

// --- Wiring pin -----------------------------------------------------------
//
// The port cannot be linked into a unit test (see test/pagespeed/nginx/BUILD),
// so the one place that consults the decision is pinned by reading the
// source. StripWhitespace strips whitespace, not comments, so a pinned token
// may not appear in a comment either.

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

TEST(NgxInPlaceUrlAgreementWiringTest, TheInPlaceBranchConsultsTheDecision) {
  const GoogleString cc =
      ReadStrippedSource("pagespeed/nginx/ngx_pagespeed.cc");
  // The decision is fed the server's own normalized URI and the host the
  // module built its URL from, against the module's path and host, with the
  // server block's slash-merging setting.
  EXPECT_NE(GoogleString::npos,
            cc.find("InPlaceUrlAgrees(str_to_string_piece(r->uri),"
                    "ps_determine_host(r),url.PathSansQuery(),url.Host(),"
                    "cscf->merge_slashes!=0)"))
      << "the in-place agreement is no longer decided from the server's URI "
         "and the module's path";
  // The in-place branch -- lookup, daemon serve and recorder mark alike --
  // is entered only when the two readings agree.
  EXPECT_NE(GoogleString::npos,
            cc.find("(r->method&(NGX_HTTP_GET|NGX_HTTP_HEAD))&&!r->internal&&"
                    "ps_in_place_url_agrees(r,url)){"))
      << "the in-place branch of ps_resource_handler no longer requires the "
         "server and the module to read the request alike";
}

}  // namespace
}  // namespace net_instaweb
