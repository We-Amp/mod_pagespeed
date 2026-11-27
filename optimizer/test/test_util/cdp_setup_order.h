// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The setup order every browser-analysis render that writes its document
// with Page.setDocumentContent shares (src/browser/device_emulation.h),
// checked against the commands a fake CDP peer
// received:
//
//   Emulation.setDeviceMetricsOverride
//   Emulation.setTouchEmulationEnabled
//   Emulation.setUserAgentOverride        (phone and tablet only)
//   ... Page.addScriptToEvaluateOnNewDocument (where the render has one)
//   Page.enable
//   Page.navigate {url: about:blank}       (LoadFreshBlankDocument)
//   Page.setLifecycleEventsEnabled
//   Page.setDocumentContent
//
// each on the render's session, with the params the helpers in
// device_emulation.h produce for the viewport.

#ifndef PAGESPEED_TEST_TEST_UTIL_CDP_SETUP_ORDER_H_
#define PAGESPEED_TEST_TEST_UTIL_CDP_SETUP_ORDER_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"
#include "nlohmann/json.hpp"
#include "src/browser/device_emulation.h"

namespace pagespeed::test {

// Index of the first command with `method` at or after `from`, or -1.
inline int IndexOfCommand(const std::vector<nlohmann::json>& commands,
                          std::string_view method, size_t from = 0) {
  for (size_t i = from; i < commands.size(); ++i) {
    if (commands[i].value("method", "") == method) return static_cast<int>(i);
  }
  return -1;
}

// How many commands have `method`.
inline int CountCommands(const std::vector<nlohmann::json>& commands,
                         std::string_view method) {
  int n = 0;
  for (const auto& cmd : commands) {
    if (cmd.value("method", "") == method) ++n;
  }
  return n;
}

// Expects the setup above for a `width` x `height` render on `session_id`.
// The render must have got as far as Page.setDocumentContent.
inline void ExpectFreshWindowRenderSetup(
    const std::vector<nlohmann::json>& commands, const std::string& session_id,
    uint32_t width, uint32_t height) {
  SCOPED_TRACE(testing::Message() << "render " << width << "x" << height);
  const int metrics =
      IndexOfCommand(commands, "Emulation.setDeviceMetricsOverride");
  const int touch =
      IndexOfCommand(commands, "Emulation.setTouchEmulationEnabled");
  const int ua = IndexOfCommand(commands, "Emulation.setUserAgentOverride");
  const int script =
      IndexOfCommand(commands, "Page.addScriptToEvaluateOnNewDocument");
  const int enable = IndexOfCommand(commands, "Page.enable");
  const int navigate = IndexOfCommand(commands, "Page.navigate");
  const int lifecycle =
      IndexOfCommand(commands, "Page.setLifecycleEventsEnabled");
  const int content = IndexOfCommand(commands, "Page.setDocumentContent");
  ASSERT_GE(metrics, 0) << "no device metrics";
  ASSERT_GE(touch, 0) << "no touch emulation";
  ASSERT_GE(enable, 0) << "no Page.enable";
  ASSERT_GE(navigate, 0) << "no blank document: the render writes its "
                            "document into the window it started with";
  ASSERT_GE(lifecycle, 0) << "no lifecycle events";
  ASSERT_GE(content, 0) << "the render did not reach setDocumentContent";

  const std::optional<nlohmann::json> ua_params =
      UserAgentOverrideParams(width);
  int last_emulation = touch;
  EXPECT_LT(metrics, touch) << "touch follows the device metrics";
  if (ua_params.has_value()) {
    ASSERT_GE(ua, 0) << "a phone or a tablet reports its user agent";
    EXPECT_LT(touch, ua) << "the user agent follows touch";
    EXPECT_EQ(commands[ua].value("params", nlohmann::json()), *ua_params);
    EXPECT_EQ(commands[ua].value("sessionId", ""), session_id);
    last_emulation = ua;
  } else {
    EXPECT_EQ(ua, -1) << "a desktop window keeps the browser's user agent";
  }
  EXPECT_EQ(CountCommands(commands, "Emulation.setUserAgentOverride"),
            ua_params.has_value() ? 1 : 0);
  EXPECT_LT(last_emulation, enable) << "the emulation precedes Page.enable";
  EXPECT_LT(enable, navigate) << "Page is enabled before the blank document";
  if (script >= 0) {
    EXPECT_LT(script, navigate)
        << "the new-document script is registered before the blank document, "
           "so it runs in the window the document is written into";
  }
  EXPECT_LT(navigate, lifecycle)
      << "lifecycle events are enabled after the blank document loaded";
  EXPECT_LT(lifecycle, content);
  EXPECT_EQ(CountCommands(commands, "Page.navigate"), 1)
      << "one navigation, to the blank document";

  EXPECT_EQ(commands[metrics].value("params", nlohmann::json()),
            DeviceMetricsOverrideParams(width, height));
  EXPECT_EQ(commands[touch].value("params", nlohmann::json()),
            TouchEmulationParams(width));
  EXPECT_EQ(commands[navigate].value("params", nlohmann::json()),
            (nlohmann::json{{"url", "about:blank"}}));
  EXPECT_EQ(commands[metrics].value("sessionId", ""), session_id);
  EXPECT_EQ(commands[touch].value("sessionId", ""), session_id);
  EXPECT_EQ(commands[navigate].value("sessionId", ""), session_id);
}

}  // namespace pagespeed::test

#endif  // PAGESPEED_TEST_TEST_UTIL_CDP_SETUP_ORDER_H_
