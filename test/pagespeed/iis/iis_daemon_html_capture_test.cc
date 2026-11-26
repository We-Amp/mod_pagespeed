// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

// DaemonHtmlCache on IIS: the views and the body the port's capture hands to
// the HTML cache engine, read where the engine hands them to the library's
// store policy.  Against the stub library that exports the policy; the
// policy's own rules are tested in the optimizer.

#include "pagespeed/iis/iis_daemon_html_capture.h"

#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "net/instaweb/rewriter/public/rewrite_stats.h"
#include "pagespeed/kernel/base/google_message_handler.h"
#include "pagespeed/kernel/base/null_mutex.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/util/platform.h"
#include "pagespeed/kernel/util/simple_stats.h"
#include "pagespeed/system/daemon_abi.h"
#include "pagespeed/system/daemon_html_engine.h"
#include "test/pagespeed/kernel/base/mock_timer.h"
#include "test/pagespeed/system/daemon_stub_test_util.h"
#include "test/pagespeed/system/recording_daemon_abi.h"

namespace net_instaweb {
namespace {

// Every call of the store classifier, with the view and the body facts.
class ClassifyRecordingAbi : public RecordingDaemonAbi {
 public:
  struct Call {
    int mode = 0;
    DaemonHtmlHeaderLines headers;
    uint64_t body_bytes = 0;
    int body_complete = -1;
    std::string prefix;
    uint64_t max_body_bytes = 0;
  };

  explicit ClassifyRecordingAbi(const DaemonAbi* stub)
      : RecordingDaemonAbi(stub), stub_(stub) {}

  bool PublishesHtmlStorePolicy() const override {
    return stub_->PublishesHtmlStorePolicy();
  }
  int HtmlStoreClassify(int mode, const char* method, const char* url_path,
                        uint32_t request_flags, int status,
                        const char* const* names, const char* const* values,
                        size_t count, uint64_t body_bytes, int body_complete,
                        const char* body_prefix, size_t body_prefix_len,
                        uint64_t max_body_bytes, uint32_t now_seconds,
                        int* out_reason, uint32_t* out_lifetime_s,
                        uint32_t* out_flags, uint16_t* out_cc_flags,
                        int* out_sidecar, char* detail_buf,
                        size_t detail_capacity,
                        size_t* out_detail_len) const override {
    Call call;
    call.mode = mode;
    for (size_t i = 0; i < count; ++i) {
      call.headers.emplace_back(names[i], values[i]);
    }
    call.body_bytes = body_bytes;
    call.body_complete = body_complete;
    call.prefix.assign(body_prefix, body_prefix_len);
    call.max_body_bytes = max_body_bytes;
    calls.push_back(call);
    return stub_->HtmlStoreClassify(
        mode, method, url_path, request_flags, status, names, values, count,
        body_bytes, body_complete, body_prefix, body_prefix_len, max_body_bytes,
        now_seconds, out_reason, out_lifetime_s, out_flags, out_cc_flags,
        out_sidecar, detail_buf, detail_capacity, out_detail_len);
  }
  const char* HtmlStoreReasonName(int reason) const override {
    return stub_->HtmlStoreReasonName(reason);
  }
  int HtmlDecisionFormat(const char* const* keys, const char* const* values,
                         size_t count, char* buf, size_t capacity,
                         size_t* out_len) const override {
    return stub_->HtmlDecisionFormat(keys, values, count, buf, capacity,
                                     out_len);
  }

  // Two per recording: the store view, then the final view.
  mutable std::vector<Call> calls;

 private:
  const DaemonAbi* stub_;
};

// A static file as StaticFileModule hands it to the module: the origin.
const DaemonHtmlHeaderLines kOrigin = {
    {"Content-Type", "text/html"},
    {"Cache-Control", "max-age=3600"},
    {"Last-Modified", "Wed, 25 Nov 2026 10:00:00 GMT"},
    {"ETag", "\"1d9a\""},
    {"Accept-Ranges", "bytes"},
};

// The same response once the module set its own headers: its caching
// headers, its signature, its Vary token.
const DaemonHtmlHeaderLines kModuleView = {
    {"Content-Type", "text/html"},
    {"Cache-Control", "max-age=0, no-cache"},
    {"Vary", "Accept-Encoding"},
    {"X-Page-Speed", "2.11.0"},
};

class IisDaemonHtmlCaptureTest : public ::testing::Test {
 protected:
  IisDaemonHtmlCaptureTest()
      : thread_system_(Platform::CreateThreadSystem()),
        stats_(thread_system_.get()),
        timer_(new NullMutex, 1777777777000LL) {
    RewriteStats::InitStats(&stats_);
    unsetenv("PS_STUB_HTML_ACTION");
    unsetenv("PS_STUB_HTML_REASON");
    unsetenv("PS_STUB_MINOR");
    GoogleString error;
    stub_.reset(LoadDaemonAbi(StubLibraryPath("libdaemon_stub_html_policy.so"),
                              &error));
    EXPECT_TRUE(stub_ != nullptr) << error;
    abi_.reset(new ClassifyRecordingAbi(stub_.get()));
  }

  std::unique_ptr<IisDaemonHtmlCapture> NewCapture(int64 max_bytes = 5 << 20) {
    DaemonHtmlEngine::Config config;
    config.mode = DaemonHtmlMode::kReportOnly;
    config.max_bytes = max_bytes;
    DaemonHtmlEngine::Deps deps;
    deps.substrate.abi = abi_.get();
    deps.statistics = &stats_;
    deps.timer = &timer_;
    deps.handler = &handler_;
    engine_.reset(new DaemonHtmlEngine(std::move(config), std::move(deps)));
    DaemonHtmlRequestFacts facts;
    facts.method = "GET";
    facts.path = "/page.html";
    facts.host = "www.example.com";
    std::unique_ptr<DaemonHtmlRecording> recording =
        engine_->StartRecording(facts, /*port=*/nullptr);
    EXPECT_NE(nullptr, recording);
    return std::unique_ptr<IisDaemonHtmlCapture>(
        new IisDaemonHtmlCapture(std::move(recording)));
  }

  const ClassifyRecordingAbi::Call& StoreView() const {
    return abi_->calls.at(0);
  }
  const ClassifyRecordingAbi::Call& FinalView() const {
    return abi_->calls.at(1);
  }

  std::unique_ptr<ThreadSystem> thread_system_;
  SimpleStats stats_;
  MockTimer timer_;
  GoogleMessageHandler handler_;
  std::unique_ptr<DaemonAbi> stub_;
  std::unique_ptr<ClassifyRecordingAbi> abi_;
  std::unique_ptr<DaemonHtmlEngine> engine_;
};

TEST_F(IisDaemonHtmlCaptureTest, ChunksAreRecordedInOrderAsOneBody) {
  std::unique_ptr<IisDaemonHtmlCapture> capture = NewCapture();
  capture->SetOrigin(200, kOrigin, /*body_inflated=*/false);
  capture->AddBody("<html><head>", 12);
  capture->AddBody("", 0);
  capture->AddBody("</head><body>a", 14);
  capture->AddBody("</body></html>", 14);
  capture->NoteBodyEnd();
  capture->NoteModuleView(kModuleView);
  capture->NoteFinalView(200, kModuleView);
  EXPECT_FALSE(capture->Finish());
  ASSERT_EQ(2u, abi_->calls.size());
  for (const auto& call : abi_->calls) {
    EXPECT_EQ(40u, call.body_bytes);
    EXPECT_EQ("<html><head></head><body>a</body></html>", call.prefix);
    EXPECT_EQ(1, call.body_complete);
  }
}

TEST_F(IisDaemonHtmlCaptureTest, TheCapIsTheEnginesAndTheWholeSizeIsCounted) {
  std::unique_ptr<IisDaemonHtmlCapture> capture = NewCapture(/*max_bytes=*/64);
  capture->SetOrigin(200, kOrigin, false);
  const std::string chunk(100, 'x');
  for (int i = 0; i < 3; ++i) capture->AddBody(chunk.data(), chunk.size());
  capture->NoteBodyEnd();
  capture->Finish();
  ASSERT_EQ(2u, abi_->calls.size());
  // The library refuses by the byte count against the cap it is given.
  EXPECT_EQ(300u, StoreView().body_bytes);
  EXPECT_EQ(64u, StoreView().max_body_bytes);
  EXPECT_LE(StoreView().prefix.size(), 300u);
}

TEST_F(IisDaemonHtmlCaptureTest, NothingIsRecordedAfterTheBodyEnded) {
  // The module's rewritten output never passes through the send that
  // records; should anything reach the capture after the handler's last
  // send, it is not part of the body.
  std::unique_ptr<IisDaemonHtmlCapture> capture = NewCapture();
  capture->SetOrigin(200, kOrigin, false);
  capture->AddBody("<p>origin</p>", 13);
  capture->NoteBodyEnd();
  capture->AddBody("<p>rewritten</p>", 16);
  capture->Finish();
  ASSERT_EQ(2u, abi_->calls.size());
  EXPECT_EQ(13u, StoreView().body_bytes);
  EXPECT_EQ("<p>origin</p>", StoreView().prefix);
}

TEST_F(IisDaemonHtmlCaptureTest, TheStoreViewIsTheOriginWithoutModuleLines) {
  std::unique_ptr<IisDaemonHtmlCapture> capture = NewCapture();
  DaemonHtmlHeaderLines origin = kOrigin;
  origin.emplace_back("Transfer-Encoding", "chunked");
  origin.emplace_back("X-Pagespeed-Init-Status", "service-unreachable");
  origin.emplace_back("PageSpeedFilters", "+collapse_whitespace");
  capture->SetOrigin(200, origin, false);
  capture->AddBody("<html></html>", 13);
  capture->NoteBodyEnd();
  capture->NoteModuleView(kModuleView);
  capture->NoteFinalView(200, kModuleView);
  capture->Finish();
  ASSERT_EQ(2u, abi_->calls.size());
  EXPECT_EQ(kOrigin, StoreView().headers);
  // Nothing changed after the module's own edits: the final view is the
  // origin too.
  EXPECT_EQ(kOrigin, FinalView().headers);
}

TEST_F(IisDaemonHtmlCaptureTest, AnInflatedBodyLeavesTheEncodingOut) {
  // Static compression ran before the module: the body is recorded after
  // the HTML path's inflater, so the origin's coding and length are not
  // the recorded body's.
  std::unique_ptr<IisDaemonHtmlCapture> capture = NewCapture();
  DaemonHtmlHeaderLines origin = kOrigin;
  origin.emplace_back("Content-Encoding", "gzip");
  origin.emplace_back("Content-Length", "175");
  origin.emplace_back("Vary", "Accept-Encoding");
  capture->SetOrigin(200, origin, /*body_inflated=*/true);
  capture->AddBody("<html></html>", 13);
  capture->NoteBodyEnd();
  capture->Finish();
  ASSERT_EQ(2u, abi_->calls.size());
  DaemonHtmlHeaderLines expected = kOrigin;
  expected.emplace_back("Vary", "Accept-Encoding");
  EXPECT_EQ(expected, StoreView().headers);
  EXPECT_EQ(13u, StoreView().body_bytes);
}

TEST_F(IisDaemonHtmlCaptureTest, CustomHeadersReachTheFinalViewOnly) {
  // What ProtocolSupportModule wrote after the module's send: an operator
  // no-store merged into the module's Cache-Control, and two customHeaders
  // lines.  Dynamic compression's coding and Vary token do not count.
  std::unique_ptr<IisDaemonHtmlCapture> capture = NewCapture();
  capture->SetOrigin(200, kOrigin, false);
  capture->AddBody("<html></html>", 13);
  capture->NoteBodyEnd();
  capture->NoteModuleView(kModuleView);
  EXPECT_TRUE(capture->needs_final_view());
  capture->NoteFinalView(200,
                         {{"Content-Type", "text/html"},
                          {"Cache-Control", "max-age=0, no-cache,no-store"},
                          {"Content-Encoding", "gzip"},
                          {"Vary", "Accept-Encoding,Accept-Encoding"},
                          {"X-Page-Speed", "2.11.0"},
                          {"X-Powered-By", "ASP.NET"},
                          {"X-Custom", "v"}});
  EXPECT_FALSE(capture->needs_final_view());
  // A second view is ignored.
  capture->NoteFinalView(200, {{"X-Late", "1"}});
  EXPECT_FALSE(capture->Finish());
  ASSERT_EQ(2u, abi_->calls.size());
  EXPECT_EQ(kOrigin, StoreView().headers);
  DaemonHtmlHeaderLines expected = kOrigin;
  expected.emplace_back("Cache-Control", "max-age=0, no-cache,no-store");
  expected.emplace_back("X-Powered-By", "ASP.NET");
  expected.emplace_back("X-Custom", "v");
  EXPECT_EQ(expected, FinalView().headers);
}

TEST_F(IisDaemonHtmlCaptureTest, WithoutTheFinalViewTheStoreViewIsRead) {
  // The final-view entry did not run for the site: Finish says so, and
  // the response is classified on the store view, whole.
  std::unique_ptr<IisDaemonHtmlCapture> capture = NewCapture();
  capture->SetOrigin(200, kOrigin, false);
  capture->AddBody("<html></html>", 13);
  capture->NoteBodyEnd();
  capture->NoteModuleView(kModuleView);
  EXPECT_TRUE(capture->Finish());
  EXPECT_FALSE(capture->Finish());
  ASSERT_EQ(2u, abi_->calls.size());
  EXPECT_EQ(kOrigin, StoreView().headers);
  EXPECT_EQ(kOrigin, FinalView().headers);
  EXPECT_EQ(1, FinalView().body_complete);
}

TEST_F(IisDaemonHtmlCaptureTest, AFinalViewBeforeTheModuleViewIsIgnored) {
  std::unique_ptr<IisDaemonHtmlCapture> capture = NewCapture();
  capture->SetOrigin(200, kOrigin, false);
  EXPECT_FALSE(capture->needs_final_view());
  capture->NoteFinalView(200, {{"Cache-Control", "no-store"}});
  capture->AddBody("<html></html>", 13);
  capture->NoteBodyEnd();
  // No module view (the headers were suppressed): nothing is missing.
  EXPECT_FALSE(capture->Finish());
  ASSERT_EQ(2u, abi_->calls.size());
  EXPECT_EQ(kOrigin, FinalView().headers);
}

TEST_F(IisDaemonHtmlCaptureTest, OnlyTheFirstModuleViewCounts) {
  std::unique_ptr<IisDaemonHtmlCapture> capture = NewCapture();
  capture->SetOrigin(200, kOrigin, false);
  capture->AddBody("<html></html>", 13);
  capture->NoteBodyEnd();
  capture->NoteModuleView(kModuleView);
  capture->NoteFinalView(200, kModuleView);
  // A later module view would make the final view a diff against it.
  capture->NoteModuleView({{"Content-Type", "text/html"}});
  capture->Finish();
  ASSERT_EQ(2u, abi_->calls.size());
  EXPECT_EQ(kOrigin, FinalView().headers);
}

TEST_F(IisDaemonHtmlCaptureTest, AReplacedResponseIsNotWhole) {
  // CustomErrorModule replaced the response between the two
  // registrations: what goes out is not what was recorded.
  std::unique_ptr<IisDaemonHtmlCapture> capture = NewCapture();
  capture->SetOrigin(200, kOrigin, false);
  capture->AddBody("<html></html>", 13);
  capture->NoteBodyEnd();
  capture->NoteModuleView(kModuleView);
  capture->NoteFinalView(500, {{"Content-Type", "text/html"}});
  EXPECT_FALSE(capture->Finish());
  ASSERT_EQ(2u, abi_->calls.size());
  EXPECT_EQ(0, StoreView().body_complete);
  EXPECT_EQ(0, FinalView().body_complete);
}

TEST_F(IisDaemonHtmlCaptureTest, ABrokenOrAbortedBodyIsNotWhole) {
  for (int broken = 0; broken < 2; ++broken) {
    abi_->calls.clear();
    std::unique_ptr<IisDaemonHtmlCapture> capture = NewCapture();
    capture->SetOrigin(200, kOrigin, false);
    capture->AddBody("<html>", 6);
    if (broken) {
      capture->NoteBodyBroken();
      capture->AddBody("</html>", 7);
    } else {
      capture->NoteAborted();
    }
    capture->NoteBodyEnd();
    capture->Finish();
    ASSERT_EQ(2u, abi_->calls.size());
    EXPECT_EQ(0, StoreView().body_complete) << broken;
    EXPECT_EQ(6u, StoreView().body_bytes) << broken;
  }
}

TEST(IisDaemonHtmlViewLinesTest, DropsOnlyTheModulesOwnLines) {
  const DaemonHtmlHeaderLines lines =
      IisDaemonHtmlViewLines({{"x-page-speed", "1"},
                              {"X-Mod-Pagespeed", "1"},
                              {"Content-MD5", "abc"},
                              {"PageSpeed", "off"},
                              {"PageSpeedJsInlineMaxBytes", "10"},
                              {"X-PageSpeed-Other", "kept"},
                              {"Cache-Control", "private"}});
  EXPECT_EQ(DaemonHtmlHeaderLines(
                {{"X-PageSpeed-Other", "kept"}, {"Cache-Control", "private"}}),
            lines);
}

TEST(IisParseRequestStartMsTest, ReadsOnlyAPlainPositiveDecimal) {
  int64 ms = 7;
  EXPECT_TRUE(IisParseRequestStartMs(L"1777777777000", 13, &ms));
  EXPECT_EQ(1777777777000LL, ms);
  ms = 7;
  EXPECT_FALSE(IisParseRequestStartMs(nullptr, 0, &ms));
  EXPECT_FALSE(IisParseRequestStartMs(L"", 0, &ms));
  EXPECT_FALSE(IisParseRequestStartMs(L"0", 1, &ms));
  EXPECT_FALSE(IisParseRequestStartMs(L"-5", 2, &ms));
  EXPECT_FALSE(IisParseRequestStartMs(L"12a4", 4, &ms));
  EXPECT_FALSE(IisParseRequestStartMs(L" 1234", 5, &ms));
  EXPECT_FALSE(IisParseRequestStartMs(L"9999999999999999999", 19, &ms));
  EXPECT_EQ(7, ms);
  EXPECT_TRUE(IisParseRequestStartMs(L"999999999999999999", 18, &ms));
}

}  // namespace
}  // namespace net_instaweb
