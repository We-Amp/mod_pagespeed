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

#include "pagespeed/apache/apache_config.h"

#include <cstddef>

#include "net/instaweb/rewriter/public/option_context.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/stdio_file_system.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "test/net/instaweb/rewriter/option_context_worst_case.h"
#include "test/net/instaweb/rewriter/rewrite_options_test_base.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/base/null_thread_system.h"

namespace net_instaweb {

class ApacheConfigTest : public RewriteOptionsTestBase<ApacheConfig> {
 protected:
  ApacheConfigTest() : config_("test", &thread_system_) {}

  NullThreadSystem thread_system_;
  ApacheConfig config_;
};

// ---------------------------------------------------------------------------
// The option-context size bound, derived against the LARGEST options class
// that ships.
//
// This is here rather than beside the other option-context tests for one
// reason: ApacheConfig is the biggest options class in the tree, and it is not
// linked where those tests live. It carries everything base RewriteOptions has,
// plus SystemRewriteOptions' ~51 registrations and its own ~3 -- and those
// additions are precisely the long-named infrastructure options (cache paths,
// cache backends, log directories) whose serialized records are the largest in
// the table. Several of them are safe_to_print=false, so their value is
// replaced by a 64-character hash, which makes their records LONGER than the
// values they stand in for rather than shorter.
//
// Deriving the bound from base RewriteOptions alone therefore measures roughly
// three quarters of the real worst case, and reports a comfortable margin that
// does not exist. That is the specific mistake this test exists to prevent.
// ---------------------------------------------------------------------------

class ApacheConfigOptionContextTest
    : public RewriteOptionsTestBase<ApacheConfig> {
 protected:
  ApacheConfigOptionContextTest() : config_("test", &thread_system_) {}

  NullThreadSystem thread_system_;
  ApacheConfig config_;
};

TEST_F(ApacheConfigOptionContextTest,
       WorstCasePayloadFitsTheBoundWithTheReserveUnspent) {
  size_t payload_size = 0;
  AssertWorstCaseOptionContextFitsTheBound(&config_, "ApacheConfig",
                                           &payload_size);
  // Reported unconditionally so the real number is visible in the log even on
  // a green run -- the next person to consider the bound should not have to
  // rediscover it by breaking something.
  GTEST_LOG_(INFO) << "ApacheConfig worst-case option context: " << payload_size
                   << " bytes; bound " << kMaxOptionContextBytes << ", reserve "
                   << kOptionContextSizeReserve;
}

TEST_F(ApacheConfigOptionContextTest, ThisBinarySeesTheFullPropertyTable) {
  // The premise of the test above, and it needs stating carefully because the
  // obvious way to write it does not work.
  //
  // Property registration is PROCESS-STATIC. ApacheConfig::Initialize() (run by
  // the fixture) populates the shared table with the base, system AND Apache
  // registrations, so once it has run, even a plain RewriteOptions instance in
  // this binary reports all of them -- comparing the classes against each other
  // here measures nothing, they are all the same number.
  //
  // What actually matters is therefore not "which class is biggest" but "does
  // THIS binary see the enlarged table", because that is what makes the
  // measurement above the real worst case rather than the base-only one. The
  // companion assertion lives in the rewriter tests
  // (OptionContextTest.TheBaseClassIsNotTheLargestOptionsClass), where no
  // server flavour is linked and the count is materially smaller. The two
  // together are the proof: same helper, two binaries, two different table
  // sizes, and the bound derived from the larger.
  EXPECT_GE(config_.all_options().size(), 200u)
      << "this binary is not seeing the system/Apache property registrations, "
         "so the worst case measured here is not the worst case that ships";
}

// ---------------------------------------------------------------------------
// The two daemon directives, and the one that must NOT exist.
// ---------------------------------------------------------------------------

TEST_F(ApacheConfigTest, DaemonPathsDefaultToUnconfigured) {
  EXPECT_TRUE(config_.daemon_socket_path().empty());
  EXPECT_TRUE(config_.daemon_volume_path().empty());
}

TEST_F(ApacheConfigTest, DaemonPathsParseFromTheirDirectives) {
  GoogleString msg;
  NullMessageHandler handler;
  EXPECT_EQ(RewriteOptions::kOptionOk,
            config_.ParseAndSetOptionFromName1(
                "DaemonSocketPath", "/var/run/pagespeed.sock", &msg, &handler));
  EXPECT_EQ(
      RewriteOptions::kOptionOk,
      config_.ParseAndSetOptionFromName1(
          "DaemonVolumePath", "/var/cache/pagespeed-daemon", &msg, &handler));
  EXPECT_STREQ("/var/run/pagespeed.sock", config_.daemon_socket_path());
  EXPECT_STREQ("/var/cache/pagespeed-daemon", config_.daemon_volume_path());
}

// There is no runtime substrate toggle, and there must not be one.  The
// mitigation for a defect on the daemon substrate is to reinstall the
// previous module and daemon package pair, not to flip a directive: a
// directive that switches substrates at run time would leave two half-warm
// caches and a serving path whose behaviour depends on which one an operator
// last edited.
//
// Two halves, because the Apache directive table has two sources.  Options
// registered in the property table become directives automatically, so the
// first half asks the table.  The static command table in mod_instaweb.cc is
// hand-written and invisible to the property table, so the second half reads
// the source, the same way the post-config call-site test does.
TEST_F(ApacheConfigTest, NoSubstrateDirectiveIsRegistered) {
  const RewriteOptions::OptionBaseVector& options = config_.all_options();
  for (RewriteOptions::OptionBase* option : options) {
    EXPECT_FALSE(StringCaseEqual(option->option_name(), "IproSubstrate"))
        << "a runtime in-place substrate toggle has been registered";
  }

  const RewriteOptions::Properties* deprecated =
      RewriteOptions::deprecated_properties();
  for (int i = 0, n = deprecated->size(); i < n; ++i) {
    EXPECT_FALSE(StringCaseEqual(deprecated->property(i)->option_name(),
                                 "IproSubstrate"))
        << "a runtime in-place substrate toggle has been registered as "
           "deprecated";
  }

  GoogleString msg;
  NullMessageHandler handler;
  EXPECT_EQ(RewriteOptions::kOptionNameUnknown,
            config_.ParseAndSetOptionFromName1("IproSubstrate", "daemon", &msg,
                                               &handler));
}

TEST_F(ApacheConfigTest, NoSubstrateDirectiveInTheStaticCommandTable) {
  StdioFileSystem file_system;
  NullMessageHandler handler;
  GoogleString source;
  const GoogleString path =
      StrCat(GTestSrcDir(), "/pagespeed/apache/mod_instaweb.cc");
  ASSERT_TRUE(file_system.ReadFile(path.c_str(), &source, &handler)) << path;
  EXPECT_EQ(GoogleString::npos, source.find("IproSubstrate"))
      << "mod_instaweb.cc mentions a substrate directive";
}

TEST_F(ApacheConfigTest, Auth) {
  StringPiece name, value, redirect;
  EXPECT_FALSE(config_.GetProxyAuth(&name, &value, &redirect));
  config_.set_proxy_auth("cookie=value:http://example.com/url");
  ASSERT_TRUE(config_.GetProxyAuth(&name, &value, &redirect));
  EXPECT_STREQ("cookie", name);
  EXPECT_STREQ("value", value);
  EXPECT_STREQ("http://example.com/url", redirect);
  config_.set_proxy_auth("cookie2=value2");
  ASSERT_TRUE(config_.GetProxyAuth(&name, &value, &redirect));
  EXPECT_STREQ("cookie2", name);
  EXPECT_STREQ("value2", value);
  EXPECT_STREQ("", redirect);
  config_.set_proxy_auth("cookie3");
  ASSERT_TRUE(config_.GetProxyAuth(&name, &value, &redirect));
  EXPECT_STREQ("cookie3", name);
  EXPECT_STREQ("", value);
  EXPECT_STREQ("", redirect);
  config_.set_proxy_auth("cookie4:http://example.com/url2");
  ASSERT_TRUE(config_.GetProxyAuth(&name, &value, &redirect));
  EXPECT_STREQ("cookie4", name);
  EXPECT_STREQ("", value);
  EXPECT_STREQ("http://example.com/url2", redirect);
}

}  // namespace net_instaweb
