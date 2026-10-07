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

// The IIS port's in-place recorder seam.
//
// The port builds its classic in-place recorder through the shared record
// gate and holds it by the InPlaceResourceRecorder interface, the same shape
// the Apache and nginx ports already have. There is no behaviour to execute
// here -- the gate's answer follows the site's daemon health, and with no
// daemon option configured that is always the classic disposition, so the
// recorder is the same object it always was -- and there can be no linkable
// unit test for the wiring either (the module's construction site sits
// behind the IIS runtime headers).
// What can and must be pinned from here is that the gate is the ONE
// construction site: no source under the port constructs the recorder
// directly.

#include <cctype>

#include "pagespeed/kernel/base/file_system.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/stdio_file_system.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {

class IisRecorderGateSeamTest : public testing::Test {
 protected:
  StdioFileSystem file_system_;
  NullMessageHandler handler_;
};

TEST_F(IisRecorderGateSeamTest, TheIisPortConstructsNoRecorderOutsideTheGate) {
  // The gate is only the single owner of recorder construction if nothing in
  // this port builds one directly, so this reads EVERY source file in the
  // port rather than the one file the seam happens to live in today -- a
  // construction site added next door is exactly the regression worth
  // catching, and a one-file check would have called that green.
  const GoogleString dir = StrCat(GTestSrcDir(), "/pagespeed/iis");

  StringVector files;
  ASSERT_TRUE(file_system_.ListContents(dir, &files, &handler_)) << dir;

  int scanned = 0;
  for (const GoogleString& path : files) {
    if (!StringCaseEndsWith(path, ".cpp") && !StringCaseEndsWith(path, ".cc") &&
        !StringCaseEndsWith(path, ".h")) {
      continue;
    }
    GoogleString source;
    ASSERT_TRUE(file_system_.ReadFile(path.c_str(), &source, &handler_))
        << path;
    ++scanned;
    EXPECT_EQ(GoogleString::npos, source.find("new InPlaceResourceRecorder"))
        << path
        << " constructs an in-place recorder directly, bypassing the gate "
           "that is supposed to be the only construction site";
  }
  // Fail closed: a listing that silently returned nothing would make every
  // assertion above vacuous.
  EXPECT_GT(scanned, 30) << "only " << scanned
                         << " iis source files were scanned; the listing "
                            "looks wrong, so this pin proved nothing";
}

TEST_F(IisRecorderGateSeamTest, TheDispositionComesFromTheSitesDaemonHealth) {
  // The gate's answer must be a function of the SITE's daemon health, not a
  // literal: the startup check's verdict (not configured, unavailable, ready)
  // is what decides classic versus daemon substrate.  Read as text, like the
  // pin above -- the construction site sits behind the IIS runtime headers.
  const GoogleString dir = StrCat(GTestSrcDir(), "/pagespeed/iis");
  const GoogleString source_path = dir + "/iis_module_base_fetch.cpp";
  GoogleString source;
  ASSERT_TRUE(file_system_.ReadFile(source_path.c_str(), &source, &handler_))
      << source_path;
  EXPECT_NE(GoogleString::npos,
            source.find("IproDispositionFor(server_context->daemon_health())"))
      << source_path << " does not feed the gate from the site's daemon health";
  EXPECT_EQ(GoogleString::npos,
            source.find("IproDispositionFor(DaemonHealth::kNotConfigured)"))
      << source_path
      << " hard-codes the disposition the startup check should decide";
}

TEST_F(IisRecorderGateSeamTest, TheDaemonRecorderHasOneConstructionSite) {
  // The daemon recorder is built through the one factory a serving seam may
  // construct it from, in ONE place in this port -- the counterpart of the
  // classic gate's single-construction-site rule.  A second site would be a
  // second disposition decision, and the two could disagree.
  const GoogleString dir = StrCat(GTestSrcDir(), "/pagespeed/iis");

  StringVector files;
  ASSERT_TRUE(file_system_.ListContents(dir, &files, &handler_)) << dir;

  int sites = 0;
  for (const GoogleString& path : files) {
    if (!StringCaseEndsWith(path, ".cpp") && !StringCaseEndsWith(path, ".cc") &&
        !StringCaseEndsWith(path, ".h")) {
      continue;
    }
    GoogleString source;
    ASSERT_TRUE(file_system_.ReadFile(path.c_str(), &source, &handler_))
        << path;
    size_t at = source.find("MakeDaemonIproRecorderIfReady");
    while (at != GoogleString::npos) {
      ++sites;
      at = source.find("MakeDaemonIproRecorderIfReady", at + 1);
    }
  }
  // One construction site in the port's request path (iis_http_module.cpp);
  // the factory's own header in the shared tree is not under this dir.
  EXPECT_EQ(1, sites) << "the port must construct its daemon recorder in "
                         "exactly one place";
}

TEST_F(IisRecorderGateSeamTest, TheAuthorizationFactIsRetakenAfterAuth) {
  // Connection-based Windows authentication sends no Authorization header on
  // follow-up requests on one keep-alive connection, so the record decision
  // must re-take the authorization fact at the response site, where IIS
  // authentication has run.  Read as text, like the pins above.
  const GoogleString dir = StrCat(GTestSrcDir(), "/pagespeed/iis");
  const GoogleString source_path = dir + "/iis_http_module.cpp";
  GoogleString source;
  ASSERT_TRUE(file_system_.ReadFile(source_path.c_str(), &source, &handler_))
      << source_path;
  EXPECT_NE(GoogleString::npos,
            source.find("IisRequestIsAuthorized(ReadIisAuthFacts("))
      << source_path
      << " does not re-take the authorization fact at the "
         "response site";
  // And it must come before the recorder is first offered headers, inside
  // the send-response handler itself (where authentication has run), not
  // merely somewhere earlier in the file.
  const size_t on_send = source.find("IisHttpModule::OnSendResponse");
  const size_t auth_at =
      source.find("IisRequestIsAuthorized(ReadIisAuthFacts(");
  const size_t headers_at = source.find("ConsiderResponseHeaders");
  ASSERT_NE(GoogleString::npos, on_send);
  ASSERT_NE(GoogleString::npos, auth_at);
  ASSERT_NE(GoogleString::npos, headers_at);
  EXPECT_LT(on_send, auth_at) << source_path
                              << " takes the authorization fact outside the "
                                 "send-response handler";
  EXPECT_LT(auth_at, headers_at) << source_path
                                 << " offers the recorder headers before the "
                                    "authorization decision";
}

TEST_F(IisRecorderGateSeamTest, TheServeHitIsRecordedWithoutAHost) {
  // The optimizer attributes a serve to a host only when the module names
  // one its configuration vouches for (VouchedServeHost).  The IIS port does
  // not read its sites' bound host names yet, so it supplies no configured
  // names and every serve is recorded without a host -- the request's Host
  // value is never passed on its own.  The shared recorder then makes the
  // host-less call (pinned behaviourally in daemon_serve_arm_test.cc, which
  // runs on every platform).  Read as text, like the pins above: the call
  // site sits behind the IIS runtime headers.
  const GoogleString path =
      StrCat(GTestSrcDir(), "/pagespeed/iis/iis_http_module.cpp");
  GoogleString source;
  ASSERT_TRUE(file_system_.ReadFile(path.c_str(), &source, &handler_))
      << path;
  const size_t fn = source.find("static bool ServeFromIisDaemonSubstrate(");
  ASSERT_NE(GoogleString::npos, fn)
      << "ServeFromIisDaemonSubstrate was renamed; this pin needs updating";
  // The function's closing brace sits at column 0; "\n}" also matches a
  // CRLF checkout on Windows, where this target runs too.
  const size_t fn_end = source.find("\n}", fn);
  ASSERT_NE(GoogleString::npos, fn_end);
  GoogleString body;
  for (const char c : source.substr(fn, fn_end - fn)) {
    if (!isspace(static_cast<unsigned char>(c))) body.push_back(c);
  }
  EXPECT_NE(GoogleString::npos,
            body.find("decision.stored_mask,VouchedServeHost(serve_request."
                      "hostname,ConfiguredHostNames()));"))
      << path << ": the recorded serve hit does not go through the shared "
                 "rule with no configured names";
  EXPECT_EQ(GoogleString::npos,
            body.find("decision.stored_mask,serve_request.hostname);"))
      << path << " passes the request's own host to the recorder";
  EXPECT_EQ(GoogleString::npos, body.find("decision.stored_mask);"))
      << path << " records a serve hit outside the shared rule";
  EXPECT_NE(GoogleString::npos,
            source.find("#include \"pagespeed/system/serve_host_names.h\""))
      << path << " does not include the shared rule";
}

TEST_F(IisRecorderGateSeamTest, TheIisPortDoesNotAskForLostCopies) {
  // Recognising a lost optimized copy is opt-in per port: a reader that is
  // not given the limiter makes no extra read and reports nothing.  The IIS
  // port has not opted in -- its serve path is unchanged -- and this pins
  // that it has neither half: an ask without the opt-in could never fire,
  // and an opt-in without the ask would pay for the look and drop the
  // answer.  On IIS a lost copy is healed when the stored original expires.
  // Both files of the port that touch the serve path.
  for (const char* file : {"/pagespeed/iis/iis_http_module.cpp",
                           "/pagespeed/iis/iis_daemon_serve.cc"}) {
    const GoogleString path = StrCat(GTestSrcDir(), file);
    GoogleString source;
    ASSERT_TRUE(file_system_.ReadFile(path.c_str(), &source, &handler_))
        << path;
    EXPECT_EQ(GoogleString::npos, source.find("set_heal_notify_limiter"))
        << path << " opts in to the lost-copy look without the ask";
    EXPECT_EQ(GoogleString::npos, source.find("DaemonServeLostCopyNotify"))
        << path << " asks for lost copies without the opt-in";
    EXPECT_EQ(GoogleString::npos, source.find("lost_optimized_copy")) << path;
  }
}

}  // namespace net_instaweb
