// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/iis/iis_local_request.h"

#include <set>
#include <string>

#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

// Direct requests: no forwarding headers.
TEST(IisLocalRequestTest, DirectLoopbackIsLocalUnderBothOptionValues) {
  EXPECT_TRUE(IisIsLocalRequest(true, "127.0.0.1", "10.0.0.5", false, false));
  EXPECT_TRUE(IisIsLocalRequest(true, "127.0.0.1", "10.0.0.5", false, true));
  EXPECT_TRUE(IisIsLocalRequest(true, "::1", "::1", false, false));
}

TEST(IisLocalRequestTest, DirectRequestFromServerAddressIsLocal) {
  EXPECT_TRUE(IisIsLocalRequest(false, "10.0.0.5", "10.0.0.5", false, false));
  EXPECT_TRUE(IisIsLocalRequest(false, "10.0.0.5", "10.0.0.5", false, true));
}

TEST(IisLocalRequestTest, DirectRemoteRequestIsNotLocal) {
  EXPECT_FALSE(IisIsLocalRequest(false, "203.0.113.9", "10.0.0.5", false, false));
  EXPECT_FALSE(IisIsLocalRequest(false, "203.0.113.9", "10.0.0.5", false, true));
}

TEST(IisLocalRequestTest, UnknownAddressesAreNotLocal) {
  EXPECT_FALSE(IisIsLocalRequest(false, "", "", false, false));
  EXPECT_FALSE(IisIsLocalRequest(false, "", "", false, true));
}

// Requests relayed by a proxy on the same server.
TEST(IisLocalRequestTest, ProxiedLoopbackIsNotLocalByDefault) {
  EXPECT_FALSE(IisIsLocalRequest(true, "127.0.0.1", "10.0.0.5", true, false));
  EXPECT_FALSE(IisIsLocalRequest(true, "::1", "::1", true, false));
}

TEST(IisLocalRequestTest, ProxiedServerAddressIsNotLocalByDefault) {
  EXPECT_FALSE(IisIsLocalRequest(false, "10.0.0.5", "10.0.0.5", true, false));
}

TEST(IisLocalRequestTest, ProxiedLoopbackIsLocalWhenOperatorOptsIn) {
  EXPECT_TRUE(IisIsLocalRequest(true, "127.0.0.1", "10.0.0.5", true, true));
  EXPECT_TRUE(IisIsLocalRequest(false, "10.0.0.5", "10.0.0.5", true, true));
}

TEST(IisLocalRequestTest, OptInNeverMakesARemotePeerLocal) {
  EXPECT_FALSE(IisIsLocalRequest(false, "203.0.113.9", "10.0.0.5", true, true));
}

// Forwarding header detection.
bool HasAnyOf(const std::set<std::string>& present) {
  return IisHasForwardingHeaders(
      [&present](const char* name) { return present.count(name) > 0; });
}

TEST(IisLocalRequestTest, NoForwardingHeaders) {
  EXPECT_FALSE(HasAnyOf({}));
  EXPECT_FALSE(HasAnyOf({"User-Agent", "Accept", "Host"}));
}

TEST(IisLocalRequestTest, EachForwardingHeaderIsDetected) {
  for (int i = 0; i < kIisForwardingHeaderCount; ++i) {
    EXPECT_TRUE(HasAnyOf({kIisForwardingHeaders[i]})) << kIisForwardingHeaders[i];
  }
}

TEST(IisLocalRequestTest, CommonProxyHeadersAreCovered) {
  EXPECT_TRUE(HasAnyOf({"X-Forwarded-For"}));
  EXPECT_TRUE(HasAnyOf({"Forwarded"}));
  EXPECT_TRUE(HasAnyOf({"X-ARR-ClientIP"}));
  EXPECT_TRUE(HasAnyOf({"X-Real-IP"}));
  EXPECT_TRUE(HasAnyOf({"X-Original-URL"}));
  EXPECT_TRUE(HasAnyOf({"X-Forwarded-Port"}));
  EXPECT_TRUE(HasAnyOf({"CF-Ray"}));
}

// Access decision.
TEST(IisAdminAccessTest, LocalOnlyServesLocalRequestsOnly) {
  EXPECT_TRUE(IisAdminAccessAllowed(true, true, false, false));
  EXPECT_FALSE(IisAdminAccessAllowed(true, false, false, false));
}

TEST(IisAdminAccessTest, HostOnAllowlistIsServedWhateverTheClient) {
  EXPECT_TRUE(IisAdminAccessAllowed(true, false, true, true));
  EXPECT_TRUE(IisAdminAccessAllowed(true, true, true, true));
  EXPECT_TRUE(IisAdminAccessAllowed(false, false, true, true));
}

TEST(IisAdminAccessTest, HostOffAllowlistFallsBackToTheLocalRule) {
  EXPECT_FALSE(IisAdminAccessAllowed(true, false, true, false));
  EXPECT_TRUE(IisAdminAccessAllowed(true, true, true, false));
}

TEST(IisAdminAccessTest, LocalOnlyOffServesEveryoneUnlessAllowlisted) {
  EXPECT_TRUE(IisAdminAccessAllowed(false, false, false, false));
  EXPECT_FALSE(IisAdminAccessAllowed(false, false, true, false));
}

// The combination operators rely on: a proxied request is served only when
// its host is on the allowlist.
TEST(IisAdminAccessTest, ProxiedRequestNeedsAnAllowlistedHost) {
  const bool proxied_is_local =
      IisIsLocalRequest(true, "127.0.0.1", "10.0.0.5", true, false);
  EXPECT_FALSE(IisAdminAccessAllowed(true, proxied_is_local, false, false));
  EXPECT_FALSE(IisAdminAccessAllowed(true, proxied_is_local, true, false));
  EXPECT_TRUE(IisAdminAccessAllowed(true, proxied_is_local, true, true));
}

}  // namespace
}  // namespace net_instaweb
