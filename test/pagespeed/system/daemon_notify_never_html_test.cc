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

// Every notification the in-place arms can send goes through one builder
// (BuildModuleNotifyParams), and this drives all four senders -- the record
// arm's, and the serve arm's fallback re-notify, origin-refreshed sentinel
// and lost-copy ask -- with HTML responses and stored HTML entries.  What the
// worker is sent is read off the binding, not off the code under test:
//
//   * no notification carries HTML;
//   * every one carries exactly the from-module flag, no agent request, the
//     request's site, and never the worker's llms.txt mask;
//   * the binding the arms run against does not export the three unsited
//     cache calls at all, so none of them can have been resolved.

#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

#include "net/instaweb/rewriter/public/option_context.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "pagespeed/kernel/base/null_mutex.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/http_options.h"
#include "pagespeed/kernel/http/request_headers.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "pagespeed/kernel/util/platform.h"
#include "pagespeed/system/daemon_abi.h"
#include "pagespeed/system/daemon_ipro_recorder.h"
#include "pagespeed/system/daemon_notify_params.h"
#include "pagespeed/system/daemon_record_arm.h"
#include "pagespeed/system/daemon_serve_arm.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/base/mock_timer.h"
#include "test/pagespeed/system/recording_daemon_abi.h"

namespace net_instaweb {

namespace {

// The site of the block that admitted the requests, sha256("site-test").
const char kSite[] =
    "2007f4b98af3e43f6c81272c8606ea77a35978dd7a5c0ee623f15036edac5831";
const char kSocket[] = "/tmp/ps-worker.sock";
// Mirrors the optimizer's llms.txt notification mask (its llms sentinel); a
// local literal on purpose, so this check does not depend on the module
// having a name for a value it must never send.
const uint32_t kOptimizerLlmsSentinelMask = 0xFFFFFFFDu;

class DaemonNotifyNeverHtmlTest : public testing::Test {
 public:
  DaemonNotifyNeverHtmlTest()
      : thread_system_(Platform::CreateThreadSystem()),
        timer_(new NullMutex, 1000 * Timer::kSecondMs) {
    RewriteOptions::Initialize();
  }
  ~DaemonNotifyNeverHtmlTest() override { RewriteOptions::Terminate(); }

 protected:
  void SetUp() override {
    unsetenv("PS_STUB_LOG");
    unsetenv("PS_STUB_SITE");
    unsetenv("PS_STUB_NOTIFY_FAIL");
    library_path_ =
        StrCat(GTestSrcDir(), "/test/pagespeed/system/libdaemon_stub.so");
    GoogleString error;
    abi_.reset(LoadDaemonAbi(library_path_, &error));
    ASSERT_TRUE(abi_ != nullptr) << error;
    ASSERT_EQ(kPsOk, abi_->CacheOpen(nullptr, &cache_));
    recording_.reset(new RecordingDaemonAbi(abi_.get()));
    RewriteOptions options(thread_system_.get());
    ASSERT_EQ(OptionContextStatus::kOk,
              OptionContext::Compute(options, &context_, &signature_));
  }

  void TearDown() override {
    unsetenv("PS_STUB_ALTERNATES");
    unsetenv("PS_STUB_READ_CONTENT_TYPE");
    unsetenv("PS_STUB_ORIGIN_CT");
    // Whatever each case did, nothing the worker was sent breaks a rule.
    for (const RecordedNotify& n : recording_->notifies()) {
      EXPECT_NE(kPsContentHtml, n.content_type) << n.url;
      EXPECT_EQ(kPsNotifyFlagFromModule, n.notify_flags) << n.url;
      EXPECT_EQ(0, n.agent_request) << n.url;
      EXPECT_NE(kOptimizerLlmsSentinelMask, n.mask) << n.url;
      EXPECT_FALSE(n.site_null) << n.url;
      EXPECT_EQ(kSite, n.site) << n.url;
    }
  }

  DaemonServeRequest Request(StringPiece url) const {
    DaemonServeRequest request;
    request.url = url;
    request.hostname = "example.com";
    request.scheme = "http";
    request.site = kSite;
    request.accept = "text/html,*/*";
    request.user_agent = "Mozilla/5.0";
    request.accept_encoding = "identity";
    return request;
  }

  // The three serve-side senders with the one flag each acts on forced on,
  // over a decision whose content class is `content_type`.  Returns how many
  // were sent.
  int SendAllThreeServeNotifications(const DaemonServeRequest& request,
                                     int content_type) {
    int sent = 0;
    DaemonServeDecision decision;
    decision.ps_content_type = content_type;
    decision.client_mask = 0x09;
    decision.verdict = DaemonServeVerdict::kServeOptimized;
    decision.fallback_hit = true;
    sent += DaemonServeFallbackRenotify(*recording_, kSocket, request, decision,
                                        context_, signature_, nullptr, nullptr);
    decision = DaemonServeDecision();
    decision.ps_content_type = content_type;
    decision.verdict = DaemonServeVerdict::kFallThrough;
    decision.stale_variant_expired_by_age = true;
    sent += DaemonServeOriginRefreshedNotify(*recording_, kSocket, request,
                                             decision, context_, signature_,
                                             nullptr, nullptr);
    decision = DaemonServeDecision();
    decision.ps_content_type = content_type;
    decision.client_mask = 0x09;
    decision.verdict = DaemonServeVerdict::kServeOriginal;
    decision.lost_optimized_copy = true;
    sent += DaemonServeLostCopyNotify(*recording_, kSocket, request, decision,
                                      context_, signature_, nullptr, nullptr);
    return sent;
  }

  DaemonRecordOutcome Record(StringPiece content_type) {
    DaemonRecordRequest request;
    request.url = "/a/page";
    request.hostname = "example.com";
    request.scheme = "http";
    request.site = kSite;
    request.accept = "text/html,*/*";
    request.user_agent = "Mozilla/5.0";
    request.option_context = context_;
    request.option_signature = signature_;
    request.request_properties =
        RequestHeaders::Properties(false, false, false);
    ResponseHeaders headers;
    headers.set_major_version(1);
    headers.set_minor_version(1);
    headers.SetStatusAndReason(HttpStatus::kOK);
    headers.Add(HttpAttributes::kContentType, content_type);
    headers.Add(HttpAttributes::kCacheControl, "max-age=600, public");
    headers.SetDate(timer_.NowMs());
    headers.ComputeCaching();
    DaemonRecordOutcome outcome;
    DaemonIproRecorder* recorder = new DaemonIproRecorder(
        recording_.get(), &cache_token_, kSocket, request,
        kDefaultHttpOptionsForTests, &timer_, nullptr, nullptr);
    recorder->set_outcome_sink_for_testing(&outcome);
    recorder->Write("<html><body>x</body></html>", nullptr);
    recorder->DoneAndSetHeaders(&headers, true);
    return outcome;
  }

  std::unique_ptr<ThreadSystem> thread_system_;
  MockTimer timer_;
  GoogleString library_path_;
  std::unique_ptr<DaemonAbi> abi_;
  std::unique_ptr<RecordingDaemonAbi> recording_;
  void* cache_ = nullptr;
  int cache_token_ = 0;
  GoogleString context_;
  GoogleString signature_;
};

TEST_F(DaemonNotifyNeverHtmlTest, TheRecordArmNeverAsksForHtml) {
  const DaemonRecordOutcome html = Record("text/html");
  EXPECT_TRUE(html.done);
  EXPECT_FALSE(html.notified);
  EXPECT_TRUE(recording_->notifies().empty());

  // Handed an HTML decision directly, the record arm's sender builds
  // nothing and sends nothing.
  DaemonRecordInput input;
  input.url = "/a/page";
  input.hostname = "example.com";
  input.scheme = "http";
  input.site = kSite;
  input.option_context = context_;
  input.option_signature = signature_;
  DaemonRecordDecision decision;
  decision.verdict = DaemonRecordVerdict::kStoreAndNotify;
  decision.content_type = kPsContentHtml;
  decision.notify_mask = 0x09;
  const int64 refused_before = DaemonNotifyHtmlRefused();
  EXPECT_NE(kPsOk, NotifyDaemon(*recording_, kSocket, input, decision));
  EXPECT_EQ(refused_before + 1, DaemonNotifyHtmlRefused());
  EXPECT_TRUE(recording_->notifies().empty());

  // The control: a stylesheet is asked for, by the same sender.
  const DaemonRecordOutcome css = Record("text/css");
  EXPECT_TRUE(css.notified);
  EXPECT_EQ(1u, recording_->notifies().size());
}

TEST_F(DaemonNotifyNeverHtmlTest, TheServeArmNeverAsksForAStoredHtmlEntry) {
  // A stored HTML entry: the arm declines it, and none of its three senders
  // asks for it, whatever flag the decision carries.
  setenv("PS_STUB_READ_CONTENT_TYPE", "0", 1);
  setenv("PS_STUB_ORIGIN_CT", "text/html", 1);
  setenv("PS_STUB_ALTERNATES", "08:00:1,0c:00:0", 1);
  const DaemonServeRequest request = Request("/a/page.html");
  DaemonServeReader reader(recording_.get(), cache_);
  const DaemonServeDecision decision = reader.Serve(request, 1100);
  EXPECT_EQ(DaemonServeVerdict::kFallThrough, decision.verdict);
  EXPECT_STREQ(daemon_serve_reason::kHtmlClassicChain, decision.reason);
  EXPECT_FALSE(DaemonServeFallbackRenotify(*recording_, kSocket, request,
                                           decision, context_, signature_,
                                           nullptr, nullptr));
  EXPECT_FALSE(DaemonServeOriginRefreshedNotify(*recording_, kSocket, request,
                                                decision, context_, signature_,
                                                nullptr, nullptr));
  EXPECT_FALSE(DaemonServeLostCopyNotify(*recording_, kSocket, request,
                                         decision, context_, signature_,
                                         nullptr, nullptr));

  const int64 refused_before = DaemonNotifyHtmlRefused();
  EXPECT_EQ(0, SendAllThreeServeNotifications(request, kPsContentHtml));
  EXPECT_EQ(refused_before + 3, DaemonNotifyHtmlRefused());
  EXPECT_TRUE(recording_->notifies().empty());

  // The control: the same three for a stylesheet are all sent.
  EXPECT_EQ(3, SendAllThreeServeNotifications(Request("/a/site.css"),
                                              kPsContentCss));
  EXPECT_EQ(3u, recording_->notifies().size());
}

TEST_F(DaemonNotifyNeverHtmlTest, TheBuilderBuildsNothingForHtml) {
  ModuleNotifyInput input;
  input.url = "/a/page";
  input.hostname = "example.com";
  input.scheme = "http";
  input.site = kSite;
  input.content_type = kPsContentHtml;
  input.mask = 0x09;
  ModuleNotifyParams built;
  EXPECT_FALSE(BuildModuleNotifyParams(*abi_, input, &built));
  EXPECT_EQ(0u, built.params().struct_size);

  input.content_type = kPsContentImage;
  ASSERT_TRUE(BuildModuleNotifyParams(*abi_, input, &built));
  EXPECT_EQ(sizeof(PsNotifyParams), built.params().struct_size);
  EXPECT_EQ(kPsNotifyFlagFromModule, built.params().notify_flags);
  EXPECT_EQ(0, built.params().agent_request);
  EXPECT_STREQ(kSite, built.params().site);
  // No context given: both halves absent, not empty strings.
  EXPECT_EQ(nullptr, built.params().option_context);
  EXPECT_EQ(nullptr, built.params().option_signature);

  input.site = "";
  EXPECT_FALSE(BuildModuleNotifyParams(*abi_, input, &built));
  EXPECT_EQ(0u, built.params().struct_size);
}

TEST_F(DaemonNotifyNeverHtmlTest, TheBindingHasNoUnsitedCacheCalls) {
  // The arms ran against this library in every case above.  It does not
  // export the unsited cache calls, so the module cannot have resolved them.
  void* handle = dlopen(library_path_.c_str(), RTLD_NOW | RTLD_LOCAL);
  ASSERT_TRUE(handle != nullptr) << dlerror();
  EXPECT_EQ(nullptr, dlsym(handle, "ps_cache_write_original"));
  EXPECT_EQ(nullptr, dlsym(handle, "ps_cache_read_best"));
  EXPECT_EQ(nullptr, dlsym(handle, "ps_cache_read_alternate"));
  EXPECT_NE(nullptr, dlsym(handle, "ps_cache_write_original_site"));
  EXPECT_NE(nullptr, dlsym(handle, "ps_cache_read_best_site"));
  EXPECT_NE(nullptr, dlsym(handle, "ps_cache_read_alternate_site"));
  dlclose(handle);
}

}  // namespace

}  // namespace net_instaweb
