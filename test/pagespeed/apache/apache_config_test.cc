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
#include <memory>

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

// The daemon's management API socket (the /v1/daemon/* console proxy, issue
// #886) defaults to the daemon's packaged path and is disableable with an
// empty value.
TEST_F(ApacheConfigTest, DaemonApiSocketPathDefaultAndParse) {
  EXPECT_STREQ("/run/pagespeed-optimizer/api.sock",
               config_.daemon_api_socket_path());
  GoogleString msg;
  NullMessageHandler handler;
  EXPECT_EQ(RewriteOptions::kOptionOk,
            config_.ParseAndSetOptionFromName1(
                "DaemonApiSocketPath", "/run/custom/api.sock", &msg, &handler));
  EXPECT_STREQ("/run/custom/api.sock", config_.daemon_api_socket_path());
  EXPECT_EQ(RewriteOptions::kOptionOk,
            config_.ParseAndSetOptionFromName1("DaemonApiSocketPath", "",
                                               &msg, &handler));
  EXPECT_TRUE(config_.daemon_api_socket_path().empty());
}

// The switch for the optimizer's stored compressed copies.  Off unless an
// operator turns it on, per server or virtual host, and it changes which
// stored bytes go out -- never the configuration the optimizer files its
// work under.
TEST_F(ApacheConfigTest, ServingStoredEncodingsIsOffByDefault) {
  EXPECT_FALSE(config_.daemon_serve_stored_encodings());
}

TEST_F(ApacheConfigTest, ServingStoredEncodingsParsesOnAndOff) {
  GoogleString msg;
  NullMessageHandler handler;
  EXPECT_EQ(RewriteOptions::kOptionOk,
            config_.ParseAndSetOptionFromName1("DaemonServeStoredEncodings",
                                               "on", &msg, &handler));
  EXPECT_TRUE(config_.daemon_serve_stored_encodings());
  EXPECT_EQ(RewriteOptions::kOptionOk,
            config_.ParseAndSetOptionFromName1("DaemonServeStoredEncodings",
                                               "off", &msg, &handler));
  EXPECT_FALSE(config_.daemon_serve_stored_encodings());
  EXPECT_EQ(RewriteOptions::kOptionValueInvalid,
            config_.ParseAndSetOptionFromName1("DaemonServeStoredEncodings",
                                               "sometimes", &msg, &handler));
  EXPECT_FALSE(config_.daemon_serve_stored_encodings());
}

TEST_F(ApacheConfigTest, ServingStoredEncodingsIsPerVirtualHost) {
  ApacheConfig vhost("vhost", &thread_system_);
  GoogleString msg;
  NullMessageHandler handler;
  ASSERT_EQ(RewriteOptions::kOptionOk,
            vhost.ParseAndSetOptionFromName1("DaemonServeStoredEncodings",
                                             "on", &msg, &handler));
  std::unique_ptr<ApacheConfig> merged(config_.Clone());
  merged->Merge(vhost);
  EXPECT_TRUE(merged->daemon_serve_stored_encodings());
  EXPECT_FALSE(config_.daemon_serve_stored_encodings());
}

// The switch for letting a client's reload headers bypass the optimizer's
// copy.  Off unless an operator turns it on; per server or virtual host; and,
// like the switch above, it never renames the configuration.
TEST_F(ApacheConfigTest, HonoringRequestFreshnessIsOffByDefault) {
  EXPECT_FALSE(config_.daemon_serve_honor_request_freshness());
}

TEST_F(ApacheConfigTest, HonoringRequestFreshnessParsesOnAndOff) {
  GoogleString msg;
  NullMessageHandler handler;
  EXPECT_EQ(RewriteOptions::kOptionOk,
            config_.ParseAndSetOptionFromName1(
                "DaemonServeHonorRequestFreshness", "on", &msg, &handler));
  EXPECT_TRUE(config_.daemon_serve_honor_request_freshness());
  EXPECT_EQ(RewriteOptions::kOptionOk,
            config_.ParseAndSetOptionFromName1(
                "DaemonServeHonorRequestFreshness", "off", &msg, &handler));
  EXPECT_FALSE(config_.daemon_serve_honor_request_freshness());
  EXPECT_EQ(RewriteOptions::kOptionValueInvalid,
            config_.ParseAndSetOptionFromName1(
                "DaemonServeHonorRequestFreshness", "sometimes", &msg,
                &handler));
}

TEST_F(ApacheConfigTest, HonoringRequestFreshnessIsPerVirtualHost) {
  ApacheConfig vhost("vhost", &thread_system_);
  GoogleString msg;
  NullMessageHandler handler;
  ASSERT_EQ(RewriteOptions::kOptionOk,
            vhost.ParseAndSetOptionFromName1("DaemonServeHonorRequestFreshness",
                                             "on", &msg, &handler));
  std::unique_ptr<ApacheConfig> merged(config_.Clone());
  merged->Merge(vhost);
  EXPECT_TRUE(merged->daemon_serve_honor_request_freshness());
  EXPECT_FALSE(config_.daemon_serve_honor_request_freshness());
}

TEST_F(ApacheConfigTest, HonoringRequestFreshnessDoesNotRenameTheConfiguration) {
  GoogleString context_off, signature_off, context_on, signature_on;
  ASSERT_EQ(OptionContextStatus::kOk,
            OptionContext::Compute(config_, &context_off, &signature_off));
  GoogleString msg;
  NullMessageHandler handler;
  ASSERT_EQ(RewriteOptions::kOptionOk,
            config_.ParseAndSetOptionFromName1(
                "DaemonServeHonorRequestFreshness", "on", &msg, &handler));
  ASSERT_EQ(OptionContextStatus::kOk,
            OptionContext::Compute(config_, &context_on, &signature_on));
  EXPECT_EQ(context_off, context_on);
  EXPECT_EQ(signature_off, signature_on);
}

TEST_F(ApacheConfigTest, ServingStoredEncodingsDoesNotRenameTheConfiguration) {
  // Every notification to the optimizer carries the configuration's
  // context; turning the switch on must not file new work under a second
  // configuration for the same host.
  GoogleString context_off, signature_off, context_on, signature_on;
  ASSERT_EQ(OptionContextStatus::kOk,
            OptionContext::Compute(config_, &context_off, &signature_off));
  GoogleString msg;
  NullMessageHandler handler;
  ASSERT_EQ(RewriteOptions::kOptionOk,
            config_.ParseAndSetOptionFromName1("DaemonServeStoredEncodings",
                                               "on", &msg, &handler));
  ASSERT_EQ(OptionContextStatus::kOk,
            OptionContext::Compute(config_, &context_on, &signature_on));
  EXPECT_EQ(context_off, context_on);
  EXPECT_EQ(signature_off, signature_on);
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

// The rule_packs options: per virtual host, never per directory, and out of
// the configuration the optimizer files its work under.
TEST_F(ApacheConfigTest, RulePackOptionsParseAndRoundTrip) {
  EXPECT_TRUE(config_.rule_pack_files().empty());
  EXPECT_EQ(RewriteOptions::kRulePackModeReport, config_.rule_pack_mode());

  ApacheConfig vhost("vhost", &thread_system_);
  GoogleString msg;
  NullMessageHandler handler;
  ASSERT_EQ(
      RewriteOptions::kOptionOk,
      vhost.ParseAndSetOptionFromName1(
          "RulePackFile", "/etc/pagespeed/packs/site.json", &msg, &handler));
  ASSERT_EQ(RewriteOptions::kOptionOk,
            vhost.ParseAndSetOptionFromName1("RulePackMode", "enforce", &msg,
                                             &handler));
  EXPECT_EQ(RewriteOptions::kOptionValueInvalid,
            vhost.ParseAndSetOptionFromName1("RulePackMode", "enforc", &msg,
                                             &handler));

  std::unique_ptr<ApacheConfig> merged(config_.Clone());
  merged->Merge(vhost);
  EXPECT_EQ(std::vector<GoogleString>{"/etc/pagespeed/packs/site.json"},
            merged->rule_pack_files());
  EXPECT_EQ(RewriteOptions::kRulePackModeEnforce, merged->rule_pack_mode());
  std::unique_ptr<ApacheConfig> clone(merged->Clone());
  EXPECT_EQ(std::vector<GoogleString>{"/etc/pagespeed/packs/site.json"},
            clone->rule_pack_files());
  EXPECT_EQ(RewriteOptions::kRulePackModeEnforce, clone->rule_pack_mode());

  // Server scope is what keeps the directives out of .htaccess and
  // <Directory>; a help string is what makes them directives at all.
  for (RewriteOptions::OptionBase* option : config_.all_options()) {
    if (StringCaseEqual(option->option_name(), "RulePackFile") ||
        StringCaseEqual(option->option_name(), "RulePackMode")) {
      EXPECT_EQ(RewriteOptions::kServerScope, option->scope())
          << option->option_name();
      EXPECT_TRUE(option->help_text() != nullptr) << option->option_name();
    }
  }

  GoogleString context_off, signature_off, context_on, signature_on;
  ASSERT_EQ(OptionContextStatus::kOk,
            OptionContext::Compute(config_, &context_off, &signature_off));
  ASSERT_EQ(OptionContextStatus::kOk,
            OptionContext::Compute(*clone, &context_on, &signature_on));
  EXPECT_EQ(context_off, context_on);
}

TEST_F(ApacheConfigTest, RulePackFileListPerScope) {
  GoogleString msg;
  NullMessageHandler handler;
  ApacheConfig global("global", &thread_system_);
  ASSERT_EQ(RewriteOptions::kOptionOk,
            global.ParseAndSetOptionFromName1("RulePackFile", "/p/g.json",
                                              &msg, &handler));
  ASSERT_EQ(1u, global.rule_pack_files().size());

  ApacheConfig vhost("vhost", &thread_system_);
  ASSERT_EQ(RewriteOptions::kOptionOk,
            vhost.ParseAndSetOptionFromName1("RulePackFile", "/p/a.json", &msg,
                                             &handler));
  ASSERT_EQ(RewriteOptions::kOptionOk,
            vhost.ParseAndSetOptionFromName1("RulePackFile", "/p/b.json", &msg,
                                             &handler));
  std::unique_ptr<ApacheConfig> merged(config_.Clone());
  merged->Merge(global);
  merged->Merge(vhost);
  ASSERT_EQ(2u, merged->rule_pack_files().size());
  EXPECT_EQ("/p/a.json", merged->rule_pack_files()[0]);
  EXPECT_EQ("/p/b.json", merged->rule_pack_files()[1]);

  // The ninth line is a configuration error.
  for (int i = 3; i <= 8; ++i) {
    ASSERT_EQ(RewriteOptions::kOptionOk,
              vhost.ParseAndSetOptionFromName1(
                  "RulePackFile", StrCat("/p/", IntegerToString(i), ".json"),
                  &msg, &handler));
  }
  EXPECT_EQ(RewriteOptions::kOptionValueInvalid,
            vhost.ParseAndSetOptionFromName1("RulePackFile", "/p/9.json", &msg,
                                             &handler));
  EXPECT_NE(GoogleString::npos, msg.find("at most 8"));

  // Not allowed in .htaccess / <Directory>: a directory-scope parse refuses.
  ApacheConfig dir("dir", &thread_system_);
  EXPECT_NE(RewriteOptions::kOptionOk,
            dir.ParseAndSetOptionFromNameWithScope(
                "RulePackFile", "/p/x.json", RewriteOptions::kDirectoryScope,
                &msg, &handler));
  EXPECT_TRUE(dir.rule_pack_files().empty());

  // Not part of the options signature.
  GoogleString c1, s1, c2, s2;
  ASSERT_EQ(OptionContextStatus::kOk, OptionContext::Compute(config_, &c1, &s1));
  ASSERT_EQ(OptionContextStatus::kOk, OptionContext::Compute(*merged, &c2, &s2));
  EXPECT_EQ(c1, c2);
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
