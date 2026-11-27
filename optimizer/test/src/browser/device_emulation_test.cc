// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Tests for the device emulation every browser-analysis render uses,
// and for its one source, device_emulation.json, which
// the async-css probe reads too.

#include "src/browser/device_emulation.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "gtest/gtest.h"
#include "lib/classify/capability_mask.h"
#include "nlohmann/json.hpp"
#include "src/browser/cdp_types.h"
#include "uv.h"

namespace pagespeed {
namespace {

nlohmann::json ReadSource() {
  const char* srcdir = std::getenv("TEST_SRCDIR");
  const char* workspace = std::getenv("TEST_WORKSPACE");
  std::string path = "src/browser/device_emulation.json";
  if (srcdir != nullptr && workspace != nullptr) {
    path = std::string(srcdir) + "/" + workspace + "/" + path;
  }
  std::ifstream file(path);
  std::ostringstream ss;
  ss << file.rdbuf();
  return nlohmann::json::parse(ss.str(), nullptr, /*allow_exceptions=*/false);
}

// The probe reads the JSON, the product the constants: they must be equal,
// or the probe measures a render the product never makes. Every field the
// probe reads is checked here, so a field added to one side without the other
// fails this test rather than drifting.
TEST(DeviceEmulationTest, TheConstantsAreTheJsonSource) {
  const nlohmann::json source = ReadSource();
  ASSERT_TRUE(source.is_object()) << "device_emulation.json did not parse";
  ASSERT_TRUE(source.contains("mobile_max_width"));
  ASSERT_TRUE(source.contains("mobile_has_touch"));
  ASSERT_TRUE(source.contains("max_touch_points"));
  EXPECT_EQ(source["mobile_max_width"].get<uint32_t>(),
            kMobileEmulationMaxWidth);
  EXPECT_EQ(source["mobile_has_touch"].get<bool>(), kMobileEmulatesTouch);
  EXPECT_EQ(source["max_touch_points"].get<uint32_t>(), kMaxTouchPoints);
  // The old per-class flag is gone: the probe reads mobile_has_touch
  // and must not find a stale key that silently means something else.
  EXPECT_FALSE(source.contains("has_touch"));
  // The user agents, verbatim as the product sends them. The
  // probe sends these exact objects, so the whole object is compared.
  ASSERT_TRUE(source.contains("phone_max_width"));
  ASSERT_TRUE(source.contains("user_agent_overrides"));
  EXPECT_EQ(source["phone_max_width"].get<uint32_t>(), kPhoneMaxWidth);
  const nlohmann::json& overrides = source["user_agent_overrides"];
  ASSERT_TRUE(overrides.is_object());
  EXPECT_EQ(overrides.size(), 2u) << "a phone and a tablet, no desktop";
  ASSERT_TRUE(UserAgentOverrideParams(kAnalysisViewportWidths[0]));
  ASSERT_TRUE(UserAgentOverrideParams(kAnalysisViewportWidths[1]));
  EXPECT_EQ(overrides.value("phone", nlohmann::json()),
            *UserAgentOverrideParams(kAnalysisViewportWidths[0]));
  EXPECT_EQ(overrides.value("tablet", nlohmann::json()),
            *UserAgentOverrideParams(kAnalysisViewportWidths[1]));
}

// The phone/tablet split of the user agent is the front end's phone class.
TEST(DeviceEmulationTest, PhoneMaxWidthIsThePhoneClass) {
  EXPECT_EQ(kPhoneMaxWidth, CapabilityMask::ViewportWidthRange(
                                CapabilityMask::Viewport::kMobile)
                                .max_px);
  EXPECT_LE(kAnalysisViewportWidths[0], kPhoneMaxWidth);
  EXPECT_GT(kAnalysisViewportWidths[1], kPhoneMaxWidth);
  EXPECT_LE(kAnalysisViewportWidths[1], kMobileEmulationMaxWidth);
}

// The class the front end serves a request with this UA (ParseViewport via
// CapabilityMask::FromHeaders).
CapabilityMask::Viewport ClassOf(std::string_view user_agent) {
  return CapabilityMask::FromHeaders(/*accept=*/"", user_agent,
                                     /*save_data=*/"", /*accept_encoding=*/"")
      .viewport();
}

// The UA an analysis render reports must classify, in the front
// end, to the class whose variant that render analyses, or a page that
// adapts to the UA is analysed as one class and served as another. Read
// from device_emulation.json, the strings the probe sends too.
TEST(DeviceEmulationTest, EmulatedUserAgentsClassifyToTheirViewport) {
  const nlohmann::json source = ReadSource();
  ASSERT_TRUE(source.is_object()) << "device_emulation.json did not parse";
  const nlohmann::json& overrides = source["user_agent_overrides"];
  const std::string phone = overrides["phone"].value("userAgent", "");
  const std::string tablet = overrides["tablet"].value("userAgent", "");
  ASSERT_FALSE(phone.empty());
  ASSERT_FALSE(tablet.empty());
  EXPECT_EQ(ClassOf(phone), CapabilityMask::Viewport::kMobile) << phone;
  EXPECT_EQ(ClassOf(tablet), CapabilityMask::Viewport::kTablet) << tablet;
  // The client hints agree with the string: Android, mobile on the phone
  // only (an Android tablet sends Sec-CH-UA-Mobile: ?0).
  EXPECT_EQ(overrides["phone"]["userAgentMetadata"].value("platform", ""),
            "Android");
  EXPECT_EQ(overrides["tablet"]["userAgentMetadata"].value("platform", ""),
            "Android");
  EXPECT_TRUE(overrides["phone"]["userAgentMetadata"].value("mobile", false));
  EXPECT_FALSE(overrides["tablet"]["userAgentMetadata"].value("mobile", true));

  // Per analysis viewport: each class's render reports a UA of that class.
  using Viewport = CapabilityMask::Viewport;
  const Viewport classes[] = {Viewport::kMobile, Viewport::kTablet};
  for (int i = 0; i < 2; ++i) {
    const std::optional<nlohmann::json> params =
        UserAgentOverrideParams(kAnalysisViewportWidths[i]);
    ASSERT_TRUE(params.has_value()) << i;
    EXPECT_EQ(ClassOf(params->value("userAgent", "")), classes[i]) << i;
  }

  // The desktop render is sent no override: it keeps the browser's own UA,
  // which is a desktop one (the headless UAs of the pinned Chromium 145, as
  // Browser.getVersion reports them for --headless=new and
  // chrome-headless-shell, on Linux and macOS).
  EXPECT_FALSE(UserAgentOverrideParams(kAnalysisViewportWidths[2]));
  for (const char* own : {
           "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like "
           "Gecko) HeadlessChrome/145.0.0.0 Safari/537.36",
           "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like "
           "Gecko) HeadlessChrome/145.0.7632.6 Safari/537.36",
           "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
           "(KHTML, like Gecko) HeadlessChrome/145.0.0.0 Safari/537.36",
       }) {
    EXPECT_EQ(ClassOf(own), Viewport::kDesktop) << own;
  }
  // And why the tablet is not an iPad: iPadOS Safari's default UA is a
  // desktop one, which the front end serves the desktop variant.
  EXPECT_EQ(ClassOf("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
                    "AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 "
                    "Safari/605.1.15"),
            Viewport::kDesktop);
}

// The analysis viewports (kAnalysisViewportWidths): the phone and the tablet
// render as mobile devices WITH touch, the desktop as a window without.
TEST(DeviceEmulationTest, PhoneAndTabletRenderAsTouchDevices) {
  EXPECT_TRUE(EmulationForViewportWidth(375).mobile);
  EXPECT_TRUE(EmulationForViewportWidth(768).mobile);  // was false before
  EXPECT_FALSE(EmulationForViewportWidth(769).mobile);
  EXPECT_FALSE(EmulationForViewportWidth(1440).mobile);
  // Touch follows `mobile`, for the phone and the tablet alike.
  EXPECT_TRUE(EmulationForViewportWidth(375).has_touch);
  EXPECT_TRUE(EmulationForViewportWidth(768).has_touch);
  EXPECT_FALSE(EmulationForViewportWidth(769).has_touch);
  EXPECT_FALSE(EmulationForViewportWidth(1440).has_touch);
}

// The per-class view the critical-CSS extractor reads (RetentionPointer) is
// the emulation of that class's analysis render, nothing else.
TEST(DeviceEmulationTest, EmulationForViewportIsTheAnalysisRenders) {
  using Viewport = CapabilityMask::Viewport;
  EXPECT_TRUE(EmulationForViewport(Viewport::kMobile).mobile);
  EXPECT_TRUE(EmulationForViewport(Viewport::kMobile).has_touch);
  EXPECT_TRUE(EmulationForViewport(Viewport::kTablet).mobile);
  EXPECT_TRUE(EmulationForViewport(Viewport::kTablet).has_touch);
  EXPECT_FALSE(EmulationForViewport(Viewport::kDesktop).mobile);
  EXPECT_FALSE(EmulationForViewport(Viewport::kDesktop).has_touch);
  const Viewport classes[] = {Viewport::kMobile, Viewport::kTablet,
                              Viewport::kDesktop};
  for (int i = 0; i < 3; ++i) {
    const DeviceEmulation by_class = EmulationForViewport(classes[i]);
    const DeviceEmulation by_width =
        EmulationForViewportWidth(kAnalysisViewportWidths[i]);
    EXPECT_EQ(by_class.mobile, by_width.mobile) << i;
    EXPECT_EQ(by_class.has_touch, by_width.has_touch) << i;
    EXPECT_EQ(by_class.user_agent, by_width.user_agent) << i;
  }
  using UserAgent = DeviceEmulation::UserAgent;
  EXPECT_EQ(EmulationForViewport(Viewport::kMobile).user_agent,
            UserAgent::kAndroidPhone);
  EXPECT_EQ(EmulationForViewport(Viewport::kTablet).user_agent,
            UserAgent::kAndroidTablet);
  EXPECT_EQ(EmulationForViewport(Viewport::kDesktop).user_agent,
            UserAgent::kBrowserDefault);
}

// The user agent follows the width: a phone up to kPhoneMaxWidth, a tablet
// above it while the viewport is a mobile device, and no override for a
// desktop window.
TEST(DeviceEmulationTest, UserAgentOverrideParams) {
  using UserAgent = DeviceEmulation::UserAgent;
  EXPECT_EQ(EmulationForViewportWidth(375).user_agent,
            UserAgent::kAndroidPhone);
  EXPECT_EQ(EmulationForViewportWidth(479).user_agent,
            UserAgent::kAndroidPhone);
  EXPECT_EQ(EmulationForViewportWidth(480).user_agent,
            UserAgent::kAndroidTablet);
  EXPECT_EQ(EmulationForViewportWidth(768).user_agent,
            UserAgent::kAndroidTablet);
  EXPECT_EQ(EmulationForViewportWidth(769).user_agent,
            UserAgent::kBrowserDefault);
  EXPECT_EQ(EmulationForViewportWidth(1440).user_agent,
            UserAgent::kBrowserDefault);

  const nlohmann::json brands =
      nlohmann::json::array({{{"brand", "Chromium"}, {"version", "145"}},
                             {{"brand", "Not:A-Brand"}, {"version", "99"}}});
  EXPECT_EQ(UserAgentOverrideParams(375),
            (nlohmann::json{
                {"userAgent",
                 "Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36 "
                 "(KHTML, like Gecko) Chrome/145.0.0.0 Mobile Safari/537.36"},
                {"platform", "Linux armv81"},
                {"userAgentMetadata",
                 {{"brands", brands},
                  {"platform", "Android"},
                  {"platformVersion", ""},
                  {"architecture", ""},
                  {"model", ""},
                  {"mobile", true}}}}));
  const std::optional<nlohmann::json> tablet = UserAgentOverrideParams(768);
  ASSERT_TRUE(tablet.has_value());
  EXPECT_EQ(tablet->value("userAgent", ""),
            "Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36 (KHTML, "
            "like Gecko) Chrome/145.0.0.0 Safari/537.36");
  EXPECT_EQ((*tablet)["userAgentMetadata"]["mobile"], false);
  EXPECT_EQ((*tablet)["userAgentMetadata"]["brands"], brands);
  EXPECT_FALSE(UserAgentOverrideParams(769).has_value());
  EXPECT_FALSE(UserAgentOverrideParams(1440).has_value());
}

TEST(DeviceEmulationTest, DeviceMetricsOverrideParams) {
  EXPECT_EQ(DeviceMetricsOverrideParams(768, 1024),
            (nlohmann::json{{"width", 768},
                            {"height", 1024},
                            {"deviceScaleFactor", 1},
                            {"mobile", true}}));
  EXPECT_EQ(DeviceMetricsOverrideParams(1440, 900)["mobile"], false);
}

// Emulation.setTouchEmulationEnabled is sent to every render: enabled with
// the phone's touch points for a mobile device, disabled for a desktop
// window, so no target is left in an inherited state.
TEST(DeviceEmulationTest, TouchEmulationParams) {
  EXPECT_EQ(TouchEmulationParams(375),
            (nlohmann::json{{"enabled", true}, {"maxTouchPoints", 5}}));
  EXPECT_EQ(TouchEmulationParams(768),
            (nlohmann::json{{"enabled", true}, {"maxTouchPoints", 5}}));
  EXPECT_EQ(TouchEmulationParams(1440),
            (nlohmann::json{{"enabled", false}, {"maxTouchPoints", 5}}));
}

// EmulateDevice drives a session through its commands (metrics, touch, and
// for a phone or a tablet the user agent), in order, on the session's own
// id, and runs the continuation only after all of them succeeded.
// LoadFreshBlankDocument navigates the session to about:blank and waits for
// that document to commit.
//
// OWNERSHIP. EmulateDevice's callbacks capture the shared_ptr to the session,
// as the real sessions' do, and this fake stores the callbacks it is handed,
// so while a reply is pending the session owns a reference to itself. A real
// CdpClient drops a callback once it has run it; Reply() does the same (moves
// it out before invoking), and the fixture clears whatever a test left
// unanswered, so no cycle survives a test (LeakSanitizer reports one as an
// indirect leak of the sent commands' JSON).
// The loop LoadFreshBlankDocument paces its frame-tree reads on.
struct FakeClient {
  uv_loop_t* event_loop = nullptr;
  uv_loop_t* loop() const { return event_loop; }
};

struct FakeSession {
  std::string session_id = "sess-7";
  FakeClient* client = nullptr;
  bool completed = false;
  std::vector<CdpCommand> sent;
  std::vector<CdpResponseCallback> pending;
  std::string error;
  bool Send(const CdpCommand& cmd, CdpResponseCallback cb) {
    sent.push_back(cmd);
    pending.push_back(std::move(cb));
    return true;
  }
  void FinishError(const std::string& message) { error = message; }
  // Answer the i-th command: the callback is released before it runs, as a
  // CdpClient releases one it has answered.
  void Reply(size_t i, absl::StatusOr<CdpResponse> response) {
    ASSERT_LT(i, pending.size());
    CdpResponseCallback cb = std::move(pending[i]);
    pending[i] = nullptr;
    ASSERT_TRUE(cb) << "command " << i << " was already answered";
    cb(std::move(response));
  }
};

CdpResponse Ok() { return CdpResponse{}; }
CdpResponse OkWith(nlohmann::json result) {
  CdpResponse r;
  r.result = std::move(result);
  return r;
}
CdpResponse FrameTreeWithLoader(const std::string& loader_id) {
  return OkWith({{"frameTree",
                  {{"frame", {{"id", "frame-1"}, {"loaderId", loader_id}}}}}});
}
CdpResponse Failed() {
  CdpResponse r;
  r.error_message = "boom";
  r.error_code = -32000;
  return r;
}

class EmulateDeviceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    uv_loop_init(&loop_);
    client_.event_loop = &loop_;
    s_->client = &client_;
  }
  void TearDown() override {
    // A pending poll timer holds the session; let it run out first. An
    // unanswered command still holds EmulateDevice's callback, which holds
    // the session: release it so the session dies with the test.
    s_->completed = true;
    uv_run(&loop_, UV_RUN_DEFAULT);
    s_->pending.clear();
    EXPECT_EQ(s_.use_count(), 1) << "the session outlived its test";
    EXPECT_EQ(uv_loop_close(&loop_), 0);
  }
  // Run the poll timer: the next frame-tree read is sent when it fires.
  void Pump() { uv_run(&loop_, UV_RUN_DEFAULT); }
  uv_loop_t loop_{};
  FakeClient client_;
  std::shared_ptr<FakeSession> s_ = std::make_shared<FakeSession>();
  bool continued_ = false;
};

TEST_F(EmulateDeviceTest, SendsMetricsThenTouchThenUserAgentThenContinues) {
  EmulateDevice(s_, 375, 667, [this]() { continued_ = true; });
  ASSERT_EQ(s_->sent.size(), 1u);
  EXPECT_EQ(s_->sent[0].method, "Emulation.setDeviceMetricsOverride");
  EXPECT_EQ(s_->sent[0].params, DeviceMetricsOverrideParams(375, 667));
  EXPECT_EQ(s_->sent[0].session_id, "sess-7");
  EXPECT_FALSE(continued_);
  s_->Reply(0, Ok());
  ASSERT_EQ(s_->sent.size(), 2u);
  EXPECT_EQ(s_->sent[1].method, "Emulation.setTouchEmulationEnabled");
  EXPECT_EQ(s_->sent[1].params, TouchEmulationParams(375));
  EXPECT_EQ(s_->sent[1].params["enabled"], true);
  EXPECT_EQ(s_->sent[1].session_id, "sess-7");
  EXPECT_FALSE(continued_) << "the continuation waits for the touch reply";
  s_->Reply(1, Ok());
  // A phone reports a phone's user agent.
  ASSERT_EQ(s_->sent.size(), 3u);
  EXPECT_EQ(s_->sent[2].method, "Emulation.setUserAgentOverride");
  EXPECT_EQ(s_->sent[2].params, *UserAgentOverrideParams(375));
  EXPECT_EQ(s_->sent[2].params["userAgentMetadata"]["mobile"], true);
  EXPECT_EQ(s_->sent[2].session_id, "sess-7");
  EXPECT_FALSE(continued_) << "the continuation waits for the UA reply";
  s_->Reply(2, Ok());
  EXPECT_TRUE(continued_);
  EXPECT_TRUE(s_->error.empty());
  EXPECT_EQ(s_.use_count(), 1) << "every callback was released once answered";
}

TEST_F(EmulateDeviceTest, TabletSendsTheTabletUserAgent) {
  EmulateDevice(s_, 768, 1024, [this]() { continued_ = true; });
  s_->Reply(0, Ok());
  s_->Reply(1, Ok());
  ASSERT_EQ(s_->sent.size(), 3u);
  EXPECT_EQ(s_->sent[2].method, "Emulation.setUserAgentOverride");
  EXPECT_EQ(s_->sent[2].params, *UserAgentOverrideParams(768));
  EXPECT_EQ(s_->sent[2].params["userAgentMetadata"]["mobile"], false);
  s_->Reply(2, Ok());
  EXPECT_TRUE(continued_);
}

// The desktop window is sent touch (disabled) and NO user agent override:
// its commands are exactly those before User-Agent emulation.
TEST_F(EmulateDeviceTest, DesktopDisablesTouchAndKeepsItsUserAgent) {
  EmulateDevice(s_, 1440, 900, [this]() { continued_ = true; });
  s_->Reply(0, Ok());
  ASSERT_EQ(s_->sent.size(), 2u);
  EXPECT_EQ(s_->sent[1].params["enabled"], false);
  s_->Reply(1, Ok());
  EXPECT_TRUE(continued_);
  EXPECT_EQ(s_->sent.size(), 2u) << "no Emulation.setUserAgentOverride";
}

TEST_F(EmulateDeviceTest, StopsOnFailedMetrics) {
  EmulateDevice(s_, 375, 667, [this]() { continued_ = true; });
  s_->Reply(0, Failed());
  EXPECT_EQ(s_->sent.size(), 1u) << "no touch command after a failed metrics";
  EXPECT_EQ(s_->error, "viewport setup failed");
  EXPECT_FALSE(continued_);
}

TEST_F(EmulateDeviceTest, StopsOnFailedTouch) {
  EmulateDevice(s_, 375, 667, [this]() { continued_ = true; });
  s_->Reply(0, Ok());
  s_->Reply(1, absl::InternalError("transport"));
  EXPECT_EQ(s_->sent.size(), 2u) << "no UA command after a failed touch";
  EXPECT_EQ(s_->error, "touch emulation setup failed");
  EXPECT_FALSE(continued_);
}

TEST_F(EmulateDeviceTest, StopsOnFailedUserAgent) {
  EmulateDevice(s_, 375, 667, [this]() { continued_ = true; });
  s_->Reply(0, Ok());
  s_->Reply(1, Ok());
  s_->Reply(2, Failed());
  EXPECT_EQ(s_->error, "user agent setup failed");
  EXPECT_FALSE(continued_);
}

// LoadFreshBlankDocument: Page.navigate to about:blank on the session, then
// Page.getFrameTree until the main frame's loader is the navigation's.
TEST_F(EmulateDeviceTest, BlankDocumentWaitsForItsCommit) {
  LoadFreshBlankDocument(s_, [this]() { continued_ = true; });
  ASSERT_EQ(s_->sent.size(), 1u);
  EXPECT_EQ(s_->sent[0].method, "Page.navigate");
  EXPECT_EQ(s_->sent[0].params, (nlohmann::json{{"url", "about:blank"}}));
  EXPECT_EQ(s_->sent[0].session_id, "sess-7");
  s_->Reply(0, OkWith({{"frameId", "frame-1"}, {"loaderId", "L2"}}));
  ASSERT_EQ(s_->sent.size(), 2u);
  EXPECT_EQ(s_->sent[1].method, "Page.getFrameTree");
  EXPECT_EQ(s_->sent[1].session_id, "sess-7");
  // Still the target's first document: not committed yet. The next read
  // waits kBlankDocumentPollIntervalMs, on the loop.
  const auto before = std::chrono::steady_clock::now();
  s_->Reply(1, FrameTreeWithLoader("L1"));
  EXPECT_FALSE(continued_);
  EXPECT_EQ(s_->sent.size(), 2u) << "no back-to-back read";
  Pump();
  // libuv counts in whole milliseconds, so allow 1 ms of truncation.
  EXPECT_GE(std::chrono::steady_clock::now() - before,
            std::chrono::milliseconds(kBlankDocumentPollIntervalMs - 1));
  ASSERT_EQ(s_->sent.size(), 3u);
  EXPECT_EQ(s_->sent[2].method, "Page.getFrameTree");
  s_->Reply(2, FrameTreeWithLoader("L2"));
  EXPECT_TRUE(continued_);
  EXPECT_EQ(s_->sent.size(), 3u);
  EXPECT_TRUE(s_->error.empty());
  EXPECT_EQ(s_.use_count(), 1) << "every callback was released once answered";
}

// A clock the tests move by hand.
struct FakeClock {
  using duration = std::chrono::steady_clock::duration;
  using time_point = std::chrono::steady_clock::time_point;
  static time_point now() { return current; }
  static inline time_point current{};
};

// The wait is a time budget (kBlankDocumentCommitTimeout from the
// navigation's reply), not a number of reads: it keeps asking while the
// budget lasts, however many reads that takes, and gives up after it.
TEST_F(EmulateDeviceTest, BlankDocumentThatNeverCommitsEndsTheSession) {
  FakeClock::current = FakeClock::time_point{};
  LoadFreshBlankDocument<FakeClock>(s_, [this]() { continued_ = true; });
  s_->Reply(0, OkWith({{"frameId", "frame-1"}, {"loaderId", "L2"}}));
  // 200 reads inside the budget: still waiting.
  for (int i = 0; i < 200; ++i) {
    FakeClock::current += std::chrono::microseconds(500);
    ASSERT_EQ(s_->sent.size(), static_cast<size_t>(i + 2));
    s_->Reply(i + 1, FrameTreeWithLoader("L1"));
    ASSERT_TRUE(s_->error.empty()) << "gave up after " << i + 1 << " reads";
    Pump();
  }
  EXPECT_FALSE(continued_);
  // Past the budget: the next stale read ends the session.
  FakeClock::current += kBlankDocumentCommitTimeout;
  const size_t sent = s_->sent.size();
  s_->Reply(sent - 1, FrameTreeWithLoader("L1"));
  EXPECT_EQ(s_->sent.size(), sent) << "no read after the budget";
  EXPECT_EQ(s_->error, "blank document did not commit");
  EXPECT_FALSE(continued_);
}

// A commit that arrives late but inside the budget continues the render.
TEST_F(EmulateDeviceTest, BlankDocumentCommittingLateInsideTheBudget) {
  FakeClock::current = FakeClock::time_point{};
  LoadFreshBlankDocument<FakeClock>(s_, [this]() { continued_ = true; });
  s_->Reply(0, OkWith({{"frameId", "frame-1"}, {"loaderId", "L2"}}));
  FakeClock::current += kBlankDocumentCommitTimeout / 2;
  s_->Reply(1, FrameTreeWithLoader("L1"));
  Pump();
  ASSERT_EQ(s_->sent.size(), 3u);
  s_->Reply(2, FrameTreeWithLoader("L2"));
  EXPECT_TRUE(continued_);
  EXPECT_TRUE(s_->error.empty());
}

TEST_F(EmulateDeviceTest, FailedBlankNavigationEndsTheSession) {
  LoadFreshBlankDocument(s_, [this]() { continued_ = true; });
  s_->Reply(0, Failed());
  EXPECT_EQ(s_->error, "blank document navigation failed");
  EXPECT_FALSE(continued_);
  EXPECT_EQ(s_->sent.size(), 1u);
}

// Page.navigate reports a failed navigation as a successful reply that
// carries errorText.
TEST_F(EmulateDeviceTest, BlankNavigationWithErrorTextEndsTheSession) {
  LoadFreshBlankDocument(s_, [this]() { continued_ = true; });
  s_->Reply(0, OkWith({{"frameId", "frame-1"},
                       {"loaderId", "L2"},
                       {"errorText", "net::ERR_ABORTED"}}));
  EXPECT_EQ(s_->error, "blank document navigation failed");
  EXPECT_FALSE(continued_);
}

TEST_F(EmulateDeviceTest, FailedFrameTreeEndsTheSession) {
  LoadFreshBlankDocument(s_, [this]() { continued_ = true; });
  s_->Reply(0, OkWith({{"frameId", "frame-1"}, {"loaderId", "L2"}}));
  s_->Reply(1, absl::InternalError("transport"));
  EXPECT_EQ(s_->error, "blank document: getFrameTree failed");
  EXPECT_FALSE(continued_);
}

// A reply without a loader (or without a result object at all) has no
// document to wait for: the render continues.
TEST_F(EmulateDeviceTest, BlankNavigationWithoutALoaderContinues) {
  LoadFreshBlankDocument(s_, [this]() { continued_ = true; });
  s_->Reply(0, Ok());
  EXPECT_TRUE(continued_);
  EXPECT_EQ(s_->sent.size(), 1u);
  EXPECT_TRUE(s_->error.empty());
}

// The whole setup of a setDocumentContent render, as the five call sites
// chain it: metrics, touch, UA, then the blank document.
TEST_F(EmulateDeviceTest, PhoneSetupOrderEndsWithTheBlankDocument) {
  EmulateDevice(s_, 375, 667, [this]() {
    LoadFreshBlankDocument(s_, [this]() { continued_ = true; });
  });
  s_->Reply(0, Ok());
  s_->Reply(1, Ok());
  s_->Reply(2, Ok());
  s_->Reply(3, OkWith({{"loaderId", "L2"}}));
  s_->Reply(4, FrameTreeWithLoader("L2"));
  ASSERT_EQ(s_->sent.size(), 5u);
  const char* const order[] = {"Emulation.setDeviceMetricsOverride",
                               "Emulation.setTouchEmulationEnabled",
                               "Emulation.setUserAgentOverride",
                               "Page.navigate", "Page.getFrameTree"};
  for (size_t i = 0; i < 5; ++i) EXPECT_EQ(s_->sent[i].method, order[i]) << i;
  EXPECT_TRUE(continued_);
}

}  // namespace
}  // namespace pagespeed
