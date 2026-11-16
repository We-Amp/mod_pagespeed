// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

// The IIS port removes the RequestOptionOverride token from every response:
// the helper against a fake response, its header names against the engine's,
// and its call at the top of OnSendResponse (pinned by reading the port
// source as data, the idiom of iis_request_stage_pin_test.cc).  The effect on
// a real IIS was checked by hand against an origin that sends the token.

#include "pagespeed/iis/iis_option_token.h"

#include <cctype>
#include <iterator>
#include <utility>
#include <vector>

#include "base/logging.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/rewrite_query.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/stdio_file_system.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

// Response headers with IHttpResponse's lookup rules: names compare without
// regard to case, and DeleteHeader removes the first match.
class FakeResponse {
 public:
  void Add(const GoogleString& name, const GoogleString& value) {
    headers_.emplace_back(name, value);
  }
  const char* GetHeader(const char* name) const {
    for (const auto& h : headers_) {
      if (StringCaseEqual(h.first, name)) {
        return h.second.c_str();
      }
    }
    return nullptr;
  }
  long DeleteHeader(const char* name) {  // NOLINT(runtime/int): HRESULT
    ++deletes_;
    if (fail_deletes_) {
      return -1;
    }
    for (auto it = headers_.begin(); it != headers_.end(); ++it) {
      if (StringCaseEqual(it->first, name)) {
        headers_.erase(it);
        return 0;
      }
    }
    return 0;
  }
  int Count(const char* name) const {
    int n = 0;
    for (const auto& h : headers_) {
      n += StringCaseEqual(h.first, name) ? 1 : 0;
    }
    return n;
  }
  size_t size() const { return headers_.size(); }
  int deletes() const { return deletes_; }
  void set_fail_deletes(bool x) { fail_deletes_ = x; }

 private:
  std::vector<std::pair<GoogleString, GoogleString>> headers_;
  int deletes_ = 0;
  bool fail_deletes_ = false;
};

TEST(IisOptionTokenTest, NamesMatchTheEngine) {
  ASSERT_EQ(2u, std::size(kIisOptionOverrideTokenHeaders));
  EXPECT_STREQ(StrCat(RewriteQuery::kModPagespeed,
                      RewriteOptions::kRequestOptionOverride)
                   .c_str(),
               kIisOptionOverrideTokenHeaders[0]);
  EXPECT_STREQ(
      StrCat(RewriteQuery::kPageSpeed, RewriteOptions::kRequestOptionOverride)
          .c_str(),
      kIisOptionOverrideTokenHeaders[1]);
}

TEST(IisOptionTokenTest, BothNamesAreRemovedAndOthersKept) {
  FakeResponse response;
  response.Add("Content-Type", "text/html");
  response.Add("PageSpeedRequestOptionOverride", "secret-1");
  response.Add("PageSpeedFilters", "+remove_comments");
  response.Add("ModPagespeedRequestOptionOverride", "secret-2");
  response.Add("X-Other", "kept");
  RemoveOptionOverrideToken(&response);
  EXPECT_EQ(0, response.Count("PageSpeedRequestOptionOverride"));
  EXPECT_EQ(0, response.Count("ModPagespeedRequestOptionOverride"));
  EXPECT_EQ(3u, response.size());
  EXPECT_STREQ("text/html", response.GetHeader("Content-Type"));
  EXPECT_STREQ("+remove_comments", response.GetHeader("PageSpeedFilters"));
  EXPECT_STREQ("kept", response.GetHeader("X-Other"));
}

TEST(IisOptionTokenTest, EveryCopyAndSpellingIsRemoved) {
  FakeResponse response;
  response.Add("PageSpeedRequestOptionOverride", "a");
  response.Add("pagespeedrequestoptionoverride", "b");
  response.Add("PAGESPEEDREQUESTOPTIONOVERRIDE", "c");
  response.Add("modpagespeedrequestoptionoverride", "d");
  response.Add("ModPagespeedRequestOptionOverride", "e");
  RemoveOptionOverrideToken(&response);
  EXPECT_EQ(0u, response.size());
}

TEST(IisOptionTokenTest, NoTokenNoDelete) {
  FakeResponse response;
  response.Add("Content-Type", "text/css");
  RemoveOptionOverrideToken(&response);
  EXPECT_EQ(1u, response.size());
  EXPECT_EQ(0, response.deletes());
  RemoveOptionOverrideToken(static_cast<FakeResponse*>(nullptr));
}

TEST(IisOptionTokenTest, AFailingDeleteDoesNotLoop) {
  FakeResponse response;
  response.Add("PageSpeedRequestOptionOverride", "a");
  response.set_fail_deletes(true);
  RemoveOptionOverrideToken(&response);
  EXPECT_EQ(1, response.deletes());
}

TEST(IisOptionTokenTest, ManyCopiesStopAtTheBound) {
  FakeResponse response;
  for (int i = 0; i < kIisOptionOverrideTokenMaxCopies + 3; ++i) {
    response.Add("PageSpeedRequestOptionOverride", "a");
  }
  RemoveOptionOverrideToken(&response);
  EXPECT_EQ(kIisOptionOverrideTokenMaxCopies, response.deletes());
  EXPECT_EQ(3, response.Count("PageSpeedRequestOptionOverride"));
}

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

// OnSendResponse runs for every response IIS sends on a site with the module,
// including the module's own answers and responses the module steps aside
// from.  The removal comes before its first early return, so no bail path
// sends the token on.
TEST(IisOptionTokenTest, OnSendResponseRemovesTheTokenFirst) {
  const GoogleString cc = ReadStrippedSource("iis_http_module.cpp");
  const size_t fn = cc.find(
      "IisHttpModule::OnSendResponse(INIHttpContext*pHttpContext,"
      "INISendResponseProvider*pProvider){");
  ASSERT_NE(GoogleString::npos, fn)
      << "OnSendResponse is no longer found; this pin needs updating";
  const size_t call =
      cc.find("RemoveOptionOverrideToken(pHttpContext->GetResponse());", fn);
  ASSERT_NE(GoogleString::npos, call);
  const size_t first_return = cc.find("return", fn);
  ASSERT_NE(GoogleString::npos, first_return);
  EXPECT_LT(call, first_return);
  const size_t context = cc.find("GetRequestContext(pHttpContext)", fn);
  ASSERT_NE(GoogleString::npos, context);
  EXPECT_LT(call, context);
}

}  // namespace
}  // namespace net_instaweb
