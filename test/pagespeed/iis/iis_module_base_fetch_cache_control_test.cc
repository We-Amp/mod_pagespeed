// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Caching headers on rewritten HTML (CollectHeaders): the disabled-caching
// Cache-Control keeps directives such as no-store, like the Apache and nginx
// ports, and ModifyCachingHeaders off leaves the origin's caching headers
// alone.

#include <cstring>
#include <map>
#include <memory>
#include <vector>

#include "pagespeed/iis/iis_module_base_fetch.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "pagespeed/kernel/thread/win_thread_system.h"
#include "pagespeed/opt/http/request_context.h"
#include "test/pagespeed/iis/stub_iis_http_context.h"
#include "test/pagespeed/kernel/base/gtest.h"

class ConfigFactory;

namespace net_instaweb {

// The module's DLL entry point defines this; a test binary has none.
ConfigFactory* cf = nullptr;

namespace {

class FakeRequest : public StubHttpRequest {
 public:
  FakeRequest() { memset(&raw_, 0, sizeof(raw_)); }
  HTTP_REQUEST* GetRawHttpRequest() override { return &raw_; }
  const HTTP_REQUEST* GetRawHttpRequest() const override { return &raw_; }
  VOID GetHttpVersion(USHORT* major, USHORT* minor) const override {
    *major = 1;
    *minor = 1;
  }

 private:
  HTTP_REQUEST raw_;
};

// The IHttpCachePolicy surface CollectHeaders drives: two HTTP_CACHE_POLICY
// structs it may rewrite, and the flag guarding the user one.
class FakeCachePolicy : public IHttpCachePolicy {
 public:
  FakeCachePolicy() {
    kernel_.Policy = HttpCachePolicyTimeToLive;
    kernel_.SecondsToLive = 60;
    user_.Policy = HttpCachePolicyTimeToLive;
    user_.SecondsToLive = 60;
  }

  HTTP_CACHE_POLICY* GetKernelCachePolicy(VOID) override { return &kernel_; }
  VOID SetKernelCacheInvalidatorSet(VOID) override {}
  HTTP_CACHE_POLICY* GetUserCachePolicy(VOID) override { return &user_; }
  HRESULT AppendVaryByHeader(PCSTR) override { return S_OK; }
  PCSTR GetVaryByHeaders(VOID) const override { return nullptr; }
  HRESULT AppendVaryByQueryString(PCSTR) override { return S_OK; }
  PCSTR GetVaryByQueryStrings(VOID) const override { return nullptr; }
  HRESULT SetVaryByValue(PCSTR) override { return S_OK; }
  PCSTR GetVaryByValue(VOID) const override { return nullptr; }
  BOOL IsUserCacheEnabled(VOID) const override { return TRUE; }
  VOID DisableUserCache(VOID) override {}
  BOOL IsCached(VOID) const override { return FALSE; }
  VOID SetIsCached(VOID) override {}
  BOOL GetKernelCacheInvalidatorSet(VOID) const override { return FALSE; }

  const HTTP_CACHE_POLICY& kernel() const { return kernel_; }

 private:
  HTTP_CACHE_POLICY kernel_;
  HTTP_CACHE_POLICY user_;
};

// Records what CollectHeaders sends down the IIS response.  Both SetHeader
// overloads address the same slot, like IIS, where a known header id and its
// name are the same header; an empty header() result means "not set".
class FakeResponse : public StubHttpResponse {
 public:
  IHttpCachePolicy* GetCachePolicy() override { return &cache_policy_; }

  HRESULT SetHeader(PCSTR pszHeaderName, PCSTR pszHeaderValue, USHORT,
                    BOOL) override {
    headers_[pszHeaderName] = pszHeaderValue;
    return S_OK;
  }
  HRESULT SetHeader(HTTP_HEADER_ID ulHeaderIndex, PCSTR pszHeaderValue, USHORT,
                    BOOL) override {
    headers_[KnownHeaderName(ulHeaderIndex)] = pszHeaderValue;
    return S_OK;
  }
  HRESULT DeleteHeader(PCSTR pszHeaderName) override {
    headers_.erase(pszHeaderName);
    deleted_.push_back(pszHeaderName);
    return S_OK;
  }

  GoogleString header(const char* name) const {
    auto it = headers_.find(name);
    return it == headers_.end() ? GoogleString() : it->second;
  }
  const std::vector<GoogleString>& deleted() const { return deleted_; }
  const FakeCachePolicy& cache_policy() const { return cache_policy_; }

 private:
  // CollectHeaders sets Cache-Control by header id and nothing else that way.
  static const char* KnownHeaderName(HTTP_HEADER_ID ulHeaderIndex) {
    EXPECT_EQ(HttpHeaderCacheControl, ulHeaderIndex);
    return "Cache-Control";
  }

  FakeCachePolicy cache_policy_;
  std::map<GoogleString, GoogleString> headers_;
  std::vector<GoogleString> deleted_;
};

class FakeContext : public StubHttpContext {
 public:
  IHttpRequest* GetRequest() override { return &request_; }
  IHttpResponse* GetResponse() override { return &response_; }
  IHttpContext* GetRootContext() const override {
    return const_cast<FakeContext*>(this);
  }
  const FakeResponse& response() const { return response_; }

 private:
  FakeRequest request_;
  FakeResponse response_;
};

class IisModuleBaseFetchCacheControlTest : public testing::Test {
 protected:
  // The fetch is reference counted; release both references when done.
  struct Releaser {
    void operator()(IisModuleBaseFetch* fetch) const {
      fetch->ReleaseRef();
      fetch->ReleaseRef();
    }
  };
  using FetchPtr = std::unique_ptr<IisModuleBaseFetch, Releaser>;

  FetchPtr NewHtmlFetch(FakeContext* context) {
    FetchPtr fetch(new IisModuleBaseFetch(
        context, kHtml, &handler_,
        RequestContext::NewTestRequestContext(&thread_system_), nullptr));
    fetch->response_headers()->set_status_code(200);
    return fetch;
  }

  WinThreadSystem thread_system_;
  NullMessageHandler handler_;
};

TEST_F(IisModuleBaseFetchCacheControlTest,
       HtmlDisableCachingKeepsNoStoreAndDropsPrivate) {
  FakeContext context;
  FetchPtr fetch = NewHtmlFetch(&context);
  // What ProxyFetch hands the base fetch for an origin that answered
  // "Cache-Control: no-store, private".
  fetch->response_headers()->Add(HttpAttributes::kCacheControl,
                                 "no-store, private");
  ASSERT_EQ(0, fetch->CollectHeaders());

  // The value the other ports send (Apache header_util.cc, nginx
  // ngx_pagespeed.cc): caching disabled with no-store kept and private
  // dropped, not a bare no-cache that loses the directive.
  EXPECT_EQ(
      StrCat(HttpAttributes::kNoCacheMaxAge0, ", ", HttpAttributes::kNoStore),
      context.response().header("Cache-Control"));
  EXPECT_EQ(HttpCachePolicyNocache,
            context.response().cache_policy().kernel().Policy);
  EXPECT_EQ(0u, context.response().cache_policy().kernel().SecondsToLive);
  // E-Tag is the misspelling CollectHeaders deletes today: IIS puts ETag on
  // the response, so that call never matches it.  The expectation pins the
  // current spelling, not desired behaviour; renaming it is a follow-up.
  EXPECT_EQ(std::vector<GoogleString>({"E-Tag", "Expires", "Content-MD5",
                                       "Last-Modified", "Accept-Ranges"}),
            context.response().deleted());
}

TEST_F(IisModuleBaseFetchCacheControlTest,
       ModifyCachingHeadersOffLeavesOriginCachingHeaders) {
  FakeContext context;
  const char kOriginCacheControl[] = "no-store, private";
  // Seed the origin header through the response pointer the module code
  // uses; response() is a const view for the assertions below.
  context.GetResponse()->SetHeader(
      "Cache-Control", kOriginCacheControl,
      static_cast<USHORT>(strlen(kOriginCacheControl)), TRUE);
  FetchPtr fetch = NewHtmlFetch(&context);
  fetch->set_modify_cache_headers(false);
  fetch->response_headers()->Add(HttpAttributes::kCacheControl,
                                 kOriginCacheControl);
  fetch->response_headers()->Add(HttpAttributes::kExpires,
                                 "Fri, 01 Jan 2027 00:00:00 GMT");
  ASSERT_EQ(0, fetch->CollectHeaders());

  // The origin's directive is neither replaced nor deleted, the headers
  // ProxyFetch computed are not copied over it, and the kernel policy is
  // left as the origin set it.
  EXPECT_EQ(kOriginCacheControl, context.response().header("Cache-Control"));
  EXPECT_EQ("", context.response().header("Expires"));
  EXPECT_TRUE(context.response().deleted().empty());
  EXPECT_EQ(HttpCachePolicyTimeToLive,
            context.response().cache_policy().kernel().Policy);
  EXPECT_EQ(60u, context.response().cache_policy().kernel().SecondsToLive);
}

}  // namespace
}  // namespace net_instaweb
