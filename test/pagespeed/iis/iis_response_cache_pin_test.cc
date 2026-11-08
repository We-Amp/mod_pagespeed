// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Which responses the IIS port keeps out of IIS's own response caches.
//
// HTTP.sys keeps a kernel copy of a response it sees requested often, and IIS
// can keep a user-mode output-cache copy; either answers later requests for
// the URL without the module, to every client alike.  So every response of a
// request the module takes up for in-place optimization, and every answer to
// the module's own input fetches, is kept out of both caches; plain static
// files the module never takes up keep IIS's caching.  An HTML page the
// module leaves plain for one request (an AJAX request) is kept out of both
// caches as well.  The calls sit behind the IIS runtime headers, so they are
// pinned by reading the port source as data (the idiom of
// iis_request_stage_pin_test.cc); the effect is checked on a real IIS by
// test/system/iis/test_iis_inplace_cache_policy.ps1.

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

GoogleString ReadStrippedSource(const GoogleString& file_name) {
  StdioFileSystem file_system;
  NullMessageHandler handler;
  GoogleString source;
  const GoogleString path = RunfilePath(StrCat("pagespeed/iis/", file_name));
  CHECK(file_system.ReadFile(path.c_str(), &source, &handler)) << path;
  GoogleString out;
  for (const char c : source) {
    if (!isspace(static_cast<unsigned char>(c))) {
      out.push_back(c);
    }
  }
  return out;
}

TEST(IisResponseCachePinTest, TheHelperTurnsBothCachesOff) {
  const GoogleString cc = ReadStrippedSource("iis_misc.cpp");
  const size_t fn =
      cc.find("voidDisarmResponseCaches(IHttpResponse*response){");
  ASSERT_NE(GoogleString::npos, fn);
  const size_t end = cc.find("std::stringws2s(", fn);
  ASSERT_NE(GoogleString::npos, end);
  const GoogleString body = cc.substr(fn, end - fn);
  for (const char* step :
       {"response->DisableKernelCache();", "GetKernelCachePolicy()",
        "kernel->Policy=HttpCachePolicyNocache;", "GetUserCachePolicy()",
        "user->Policy=HttpCachePolicyNocache;",
        "policy->DisableUserCache();"}) {
    EXPECT_NE(GoogleString::npos, body.find(step)) << step;
  }
}

TEST(IisResponseCachePinTest, TheModulesOwnFetchIsNotCached) {
  const GoogleString cc = ReadStrippedSource("iis_http_module.cpp");
  const size_t bail = cc.find("if(ua.find(\"mod_pagespeed\")!=ua.npos){");
  ASSERT_NE(GoogleString::npos, bail)
      << "the module's own fetch is no longer recognised here; this pin "
         "needs updating";
  const size_t disarm =
      cc.find("DisarmResponseCaches(pHttpContext->GetResponse());", bail);
  const size_t ret = cc.find("returnRQ_NOTIFICATION_CONTINUE;", bail);
  ASSERT_NE(GoogleString::npos, disarm);
  EXPECT_LT(disarm, ret) << "the module's own fetch returns before its "
                            "answer is kept out of IIS's caches";
}

TEST(IisResponseCachePinTest, APageLeftPlainForAnAjaxRequestIsNotCached) {
  // An HTML page the module leaves plain for one request (an AJAX request)
  // is the same URL every visitor asks for: kept by HTTP.sys or the output
  // cache, it would reach all of them without the module.
  const GoogleString cc = ReadStrippedSource("iis_http_module.cpp");
  const size_t ajax =
      cc.find("log(pHttpContext,\"OnSendResponse---AJAXrequest->nohtmlrewriting.\");");
  ASSERT_NE(GoogleString::npos, ajax)
      << "the AJAX decline is no longer here; this pin needs updating";
  const size_t close = cc.find("}else{", ajax);
  ASSERT_NE(GoogleString::npos, close);
  EXPECT_NE(GoogleString::npos,
            cc.substr(ajax, close - ajax).find("DisarmResponseCaches(pHttpResponse);"))
      << "the plain page an AJAX request gets can be kept by IIS's caches";
}

TEST(IisResponseCachePinTest, OnlyOptimizableTypesLoseIisCaching) {
  const GoogleString cc = ReadStrippedSource("iis_misc.cpp");
  const size_t fn = cc.find("boolIsInPlaceOptimizableType(");
  ASSERT_NE(GoogleString::npos, fn);
  const GoogleString body = cc.substr(fn, 400);
  EXPECT_NE(GoogleString::npos,
            body.find("(type->IsCss()||type->IsJsLike()||type->IsImage())"));
}

TEST(IisResponseCachePinTest, InPlaceResponsesOfOptimizableTypesAreNotCached) {
  const GoogleString cc = ReadStrippedSource("iis_http_module.cpp");
  const size_t send = cc.find("IisHttpModule::OnSendResponse(");
  ASSERT_NE(GoogleString::npos, send);
  const size_t check = cc.find(
      "if(innerContext->in_place_candidate()){USHORTct_len=0;PCSTRct="
      "pHttpContext->GetResponse()->GetHeader(HttpHeaderContentType,&ct_len);"
      "if(IsInPlaceOptimizableType(ct,ct_len)){DisarmResponseCaches("
      "pHttpContext->GetResponse());}}",
      send);
  ASSERT_NE(GoogleString::npos, check)
      << "the send notification no longer keeps in-place responses of "
         "optimizable types out of IIS's caches";
  // Before any of the send notification's early returns for this request.
  for (const char* bail :
       {"if(innerContext->is_resource()||innerContext->leave())",
        "OnSendResponse-notGETorPOST,bail"}) {
    const size_t at = cc.find(bail, send);
    ASSERT_NE(GoogleString::npos, at) << bail;
    EXPECT_LT(check, at) << bail;
  }
}

TEST(IisResponseCachePinTest, EveryInPlaceRequestIsMarked) {
  const GoogleString cc = ReadStrippedSource("iis_http_module.cpp");
  const size_t branch = cc.find(
      "elseif(options->in_place_rewriting_enabled()&&options->enabled()");
  ASSERT_NE(GoogleString::npos, branch);
  const size_t open = cc.find("){", branch);
  const size_t disarm =
      cc.find("innerContext->set_in_place_candidate(true);", branch);
  ASSERT_NE(GoogleString::npos, disarm);
  // First in the branch: before the agreement check leaves a request to IIS,
  // and before every answer the module gives or records.
  EXPECT_EQ(GoogleString::npos, cc.substr(open, disarm - open).find(';'))
      << "the in-place branch does something before keeping its responses "
         "out of IIS's caches";
  for (const char* step :
       {"IisInPlaceUrlAgreesForRequest(", "ServeFromIisDaemonSubstrate(",
        "BuildDaemonRecordRequest(", "FetchInPlaceResource("}) {
    const size_t at = cc.find(step, branch);
    ASSERT_NE(GoogleString::npos, at) << step;
    EXPECT_LT(disarm, at) << step;
  }
}

TEST(IisResponseCachePinTest, TheModulesInPlaceAnswersAreNotCached) {
  const GoogleString fetch = ReadStrippedSource("iis_module_base_fetch.cpp");
  const size_t call = fetch.find("DisarmResponseCaches(r);");
  ASSERT_NE(GoogleString::npos, call);
  const size_t in_place =
      fetch.rfind("if(fetch_type_==FetchType::kInPlace){", call);
  ASSERT_NE(GoogleString::npos, in_place);
  EXPECT_EQ(GoogleString::npos,
            fetch.substr(in_place, call - in_place).find(';'))
      << "the module's in-place answers can be kept by IIS's caches";
  const GoogleString cc = ReadStrippedSource("iis_http_module.cpp");
  EXPECT_NE(GoogleString::npos, cc.find("DisarmResponseCaches(iis_response);"))
      << "the daemon substrate's answers can be kept by IIS's caches";
}

}  // namespace
}  // namespace net_instaweb
