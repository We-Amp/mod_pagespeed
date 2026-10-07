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

// Which device class a page view's property-cache lookup is keyed by.
//
// The nonce handed out with an instrumented page is stored in the entry the
// page view looked up, and a browser's report is accepted only when its nonce
// is found in the entry for the REPORTING browser's device class.  So the
// page view and the report have to classify one browser the same way, or the
// report is dropped and that device class is never optimized from its own
// reports.
//
// The page view's context needs a live httpd request to construct, so the
// decision is tested through the helper that makes it, and the call site is
// pinned by reading the source (the idiom daemon_seam_test.cc uses).

#include "pagespeed/apache/instaweb_context.h"

#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "pagespeed/kernel/base/file_system.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/stdio_file_system.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/request_headers.h"
#include "pagespeed/kernel/http/user_agent_matcher.h"
#include "test/net/instaweb/rewriter/rewrite_test_base.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {

namespace {

// User agents the matcher's own tests classify as phone, tablet and desktop.
const char kPhoneUserAgent[] =
    "Mozilla/5.0 (iPhone; CPU iPhone OS 5_0_1 like Mac OS X) AppleWebKit/534.46"
    " (KHTML, like Gecko) Version/5.1 Mobile/9A405 Safari/7534.48.3";
const char kTabletUserAgent[] =
    "Mozilla/5.0 (iPad; U; CPU OS 3_2 like Mac OS X; en-us) "
    "AppleWebKit/531.21.10 (KHTML, like Gecko) Version/4.0.4 "
    "Mobile/7B334b Safari/531.21.10";
const char kDesktopUserAgent[] =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/137.0.0.0 Safari/537.36";

class InstawebContextDeviceTypeTest : public RewriteTestBase {
 protected:
  // Classifies a request the way the page view's lookup does: with a driver
  // that has not been given the request headers yet, which is the state the
  // driver is in when the lookup runs.
  UserAgentMatcher::DeviceType LookupDeviceType(const char* user_agent) {
    EXPECT_TRUE(rewrite_driver()->user_agent().empty())
        << "the lookup runs before the driver has the request headers";
    RequestHeaders request_headers;
    if (user_agent != nullptr) {
      request_headers.Add(HttpAttributes::kUserAgent, user_agent);
    }
    return InstawebContext::DeviceTypeForPropertyCacheLookup(request_headers,
                                                             rewrite_driver());
  }

  // The class the shared beacon handler files a report under: it classifies
  // the posting browser's User-Agent header with the server's matcher.
  UserAgentMatcher::DeviceType ReportDeviceType(const char* user_agent) {
    return server_context()->user_agent_matcher()->GetDeviceTypeForUA(
        user_agent);
  }
};

TEST_F(InstawebContextDeviceTypeTest, APhoneIsLookedUpAsAPhone) {
  EXPECT_EQ(UserAgentMatcher::kMobile, LookupDeviceType(kPhoneUserAgent));
  EXPECT_EQ(UserAgentMatcher::kMobile, rewrite_driver()->device_type());
}

TEST_F(InstawebContextDeviceTypeTest, ATabletIsLookedUpAsATablet) {
  EXPECT_EQ(UserAgentMatcher::kTablet, LookupDeviceType(kTabletUserAgent));
  EXPECT_EQ(UserAgentMatcher::kTablet, rewrite_driver()->device_type());
}

TEST_F(InstawebContextDeviceTypeTest, ADesktopBrowserIsLookedUpAsDesktop) {
  EXPECT_EQ(UserAgentMatcher::kDesktop, LookupDeviceType(kDesktopUserAgent));
  EXPECT_EQ(UserAgentMatcher::kDesktop, rewrite_driver()->device_type());
}

TEST_F(InstawebContextDeviceTypeTest, ARequestWithoutAUserAgentIsDesktop) {
  EXPECT_EQ(UserAgentMatcher::kDesktop, LookupDeviceType(nullptr));
  EXPECT_EQ(UserAgentMatcher::kDesktop, rewrite_driver()->device_type());
}

TEST_F(InstawebContextDeviceTypeTest, TheLookupAndTheReportUseTheSameEntry) {
  // The cache key suffix is what has to match between the page view that
  // stores a nonce and the report that redeems it.
  for (const char* user_agent :
       {kPhoneUserAgent, kTabletUserAgent, kDesktopUserAgent}) {
    EXPECT_EQ(UserAgentMatcher::DeviceTypeSuffix(ReportDeviceType(user_agent)),
              UserAgentMatcher::DeviceTypeSuffix(LookupDeviceType(user_agent)))
        << user_agent;
  }
}

TEST(InstawebContextSourcePinTest, TheLookupClassifiesFromTheRequestHeaders) {
  // The helper above is only worth something if the lookup calls it.  That
  // call site needs a live httpd request to execute, so it is pinned by
  // reading the source.
  StdioFileSystem file_system;
  NullMessageHandler handler;
  GoogleString source;
  const GoogleString path =
      StrCat(GTestSrcDir(), "/pagespeed/apache/instaweb_context.cc");
  ASSERT_TRUE(file_system.ReadFile(path.c_str(), &source, &handler)) << path;

  const size_t lookup =
      source.find("void InstawebContext::BlockingPropertyCacheLookup()");
  ASSERT_NE(GoogleString::npos, lookup)
      << "the lookup has been renamed; this pin needs updating";
  const size_t lookup_end = source.find("\n}\n", lookup);
  ASSERT_NE(GoogleString::npos, lookup_end);
  const GoogleString body = source.substr(lookup, lookup_end - lookup);

  EXPECT_NE(GoogleString::npos,
            body.find("DeviceTypeForPropertyCacheLookup(*request_headers_"))
      << "the lookup no longer takes the device class from the request's own "
         "headers";
  EXPECT_EQ(GoogleString::npos, body.find("user_agent()"))
      << "the lookup reads the driver's user agent, which is empty until the "
         "driver is given the request headers: every request would be looked "
         "up as desktop";
  EXPECT_NE(GoogleString::npos, body.find("device_type,"))
      << "the device class is no longer passed to the property page";

  // And nothing in the file reads the driver's user agent to classify.
  EXPECT_EQ(0, CountSubstring(source, "GetDeviceTypeForUA(driver->user_agent"))
      << "the device class is derived from the driver's user agent again";
  EXPECT_EQ(0, CountSubstring(
                   source, "GetDeviceTypeForUA(rewrite_driver_->user_agent"));
}

}  // namespace

}  // namespace net_instaweb
