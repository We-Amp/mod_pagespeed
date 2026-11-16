// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Direct lint + compile coverage for the 2.0-owned lib/html/compat/ shim
// headers (#1130), mirroring test/lib/js/compat_headers_test.cc (#1104 item
// 3). This TU includes every compat header DIRECTLY, so ci.yml's
// -header-filter picks them up as first-party on every full-mode lint run
// (a compat-header edit forces full mode) independent of what the vendored
// files happen to include, and proves each header is self-contained.
// Keep the include list in sync with the hdrs of //lib/html:compat.
//
// The tests are deliberately trivial smoke assertions over the shims'
// contracts; the real behavioral coverage lives in the sibling
// compat_message_handler_test.cc and compat_google_url_test.cc suites.

#include <cstdint>
#include <string_view>
#include <type_traits>

#include "gtest/gtest.h"
#include "lib/html/compat/google_url.h"
#include "lib/html/compat/html_page_limit.h"
#include "lib/html/compat/logging.h"
#include "lib/html/compat/message_handler.h"
#include "lib/html/compat/sparse_hash_map.h"
#include "lib/html/compat/stl_util.h"
#include "lib/html/compat/string.h"
#include "lib/html/compat/string_hash.h"
#include "lib/html/compat/string_util.h"
#include "lib/html/compat/timer.h"

namespace {

TEST(HtmlCompatHeadersTest, MessageTypeEnumIsCanonicalCompatible) {
  // Unscoped, canonical-ordered enum in net_instaweb (distinct from the
  // 2.0-native pagespeed::MessageType enum class in lib/base).
  static_assert(
      !std::is_same_v<net_instaweb::MessageType,
                      std::underlying_type_t<net_instaweb::MessageType>>);
  EXPECT_EQ(net_instaweb::kInfo, 0);
  EXPECT_EQ(net_instaweb::kWarning, 1);
  EXPECT_EQ(net_instaweb::kError, 2);
  EXPECT_EQ(net_instaweb::kFatal, 3);
}

TEST(HtmlCompatHeadersTest, HandlerSurfaceCompilesStandalone) {
  net_instaweb::NullMessageHandler null_handler;
  null_handler.Message(net_instaweb::kInfo, "self-contained TU smoke");
  net_instaweb::PrintMessageHandler print_handler;
  print_handler.Check(true, "quiet");
}

TEST(HtmlCompatHeadersTest, GoogleUrlSeamCompilesStandalone) {
  net_instaweb::GoogleUrl url(std::string_view("http://example.com/"));
  EXPECT_TRUE(url.IsValid());
  EXPECT_TRUE(url.IsAnyValid());
  EXPECT_EQ(url.Spec(), "http://example.com/");
  net_instaweb::GoogleUrl empty;
  EXPECT_FALSE(empty.IsValid());
}

TEST(HtmlCompatHeadersTest, PageLimitIsCanonical) {
  static_assert(net_instaweb::kHtmlPageLimitBytes == int64_t{64} << 20);
  EXPECT_GT(net_instaweb::kHtmlPageLimitBytes, 0);
}

}  // namespace
