// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.
//
// The nginx half of the module entity tag rule.  The handler sits behind
// the nginx runtime headers (nginx cannot be linked into a test), so its
// registrations and the steps a removal must take are pinned by reading
// ngx_pagespeed.cc as data.  The list removal itself is unit tested in
// ngx_list_filter_test.cc; the behaviour on a live nginx in
// test/system/system/test_module_entity_tags.py.

#include <cctype>

#include "base/logging.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/stdio_file_system.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

GoogleString ReadStrippedModuleSource() {
  StdioFileSystem file_system;
  NullMessageHandler handler;
  GoogleString source;
  const GoogleString path =
      StrCat(GTestSrcDir(), "/pagespeed/nginx/ngx_pagespeed.cc");
  CHECK(file_system.ReadFile(path.c_str(), &source, &handler)) << path;
  GoogleString out;
  for (const char c : source) {
    if (!isspace(static_cast<unsigned char>(c))) {
      out.push_back(c);
    }
  }
  return out;
}

GoogleString HandlerBody(const GoogleString& src) {
  const size_t at =
      src.find("ngx_int_tps_strip_module_entity_tags_post_read_handler(");
  if (at == GoogleString::npos) {
    return "";
  }
  const size_t open = src.find('{', at);
  int depth = 0;
  for (size_t i = open; i < src.size(); ++i) {
    if (src[i] == '{') {
      ++depth;
    } else if (src[i] == '}' && --depth == 0) {
      return src.substr(open, i + 1 - open);
    }
  }
  return "";
}

// POST_READ for the ordinary request, REWRITE for a request nginx failed
// before the phase engine and sent through error_page (it starts after
// POST_READ, at a location or a named location).
TEST(NgxModuleEntityTagsPinTest, HandlerRunsInPostReadAndRewrite) {
  const GoogleString src = ReadStrippedModuleSource();
  ASSERT_FALSE(src.empty());
  EXPECT_NE(GoogleString::npos,
            src.find("ngx_array_push(&cmcf->phases[NGX_HTTP_POST_READ_PHASE]"
                     ".handlers));if(strip_h==nullptr){returnNGX_ERROR;}"
                     "*strip_h=ps_strip_module_entity_tags_post_read_handler;"));
  EXPECT_NE(GoogleString::npos,
            src.find("ngx_array_push(&cmcf->phases[NGX_HTTP_REWRITE_PHASE]"
                     ".handlers));if(strip_rewrite_h==nullptr){returnNGX_ERROR;"
                     "}*strip_rewrite_h=ps_strip_module_entity_tags_post_read_"
                     "handler;"));
}

// The fields leave the header list, and the cached pointers nginx's own
// handlers read (the not-modified filter among them) are cleared with them.
TEST(NgxModuleEntityTagsPinTest, RemovalClearsTheCachedPointers) {
  const GoogleString body = HandlerBody(ReadStrippedModuleSource());
  ASSERT_FALSE(body.empty()) << "the handler was renamed; update this pin";
  const size_t removal = body.find("NgxListRemoveIf<ngx_table_elt_t,");
  ASSERT_NE(GoogleString::npos, removal);
  const size_t inm = body.find("r->headers_in.if_none_match=nullptr;", removal);
  const size_t ims =
      body.find("r->headers_in.if_modified_since=nullptr;", removal);
  EXPECT_NE(GoogleString::npos, inm);
  EXPECT_NE(GoogleString::npos, ims);
  // A failed removal never forwards.
  EXPECT_NE(GoogleString::npos,
            body.find("returnNGX_HTTP_INTERNAL_SERVER_ERROR;", removal));
  // Idempotent: a stash already kept ends a later pass.
  EXPECT_NE(GoogleString::npos,
            body.find("ps_client_conditional_stash(r)!=nullptr"));
}

}  // namespace
}  // namespace net_instaweb
