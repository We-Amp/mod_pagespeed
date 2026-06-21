// Copyright 2026 We-Amp B.V.
// Licensed under the Apache License, Version 2.0 (the "License").
//
// Hermetic unit tests for the SSRF guard of NetFetchKeyDirectory (the network
// trust boundary built in #211, previously untested -- this fills that gap).
// Exercises the pure predicates IsGloballyRoutableIp / IsAllowlistedHttpsUrl and
// the fail-closed branches of GetKey that return BEFORE any network I/O, so no
// real fetcher is needed.

#include "pagespeed/kernel/webbotauth/net_fetch_key_directory.h"

#include "gtest/gtest.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/opt/http/request_context.h"

namespace net_instaweb {
namespace webbotauth {
namespace {

TEST(SsrfGuardTest, RejectsPrivateLoopbackLinkLocalAndMetadata) {
  EXPECT_FALSE(IsGloballyRoutableIp("10.0.0.1"));
  EXPECT_FALSE(IsGloballyRoutableIp("127.0.0.1"));
  EXPECT_FALSE(IsGloballyRoutableIp("169.254.169.254"));  // cloud metadata
  EXPECT_FALSE(IsGloballyRoutableIp("172.16.5.4"));
  EXPECT_FALSE(IsGloballyRoutableIp("192.168.1.1"));
  EXPECT_FALSE(IsGloballyRoutableIp("100.64.0.1"));  // CGNAT
  EXPECT_FALSE(IsGloballyRoutableIp("224.0.0.1"));   // multicast
  EXPECT_FALSE(IsGloballyRoutableIp("0.0.0.0"));
  EXPECT_FALSE(IsGloballyRoutableIp("::1"));              // IPv6 loopback
  EXPECT_FALSE(IsGloballyRoutableIp("fe80::1"));          // IPv6 link-local
  EXPECT_FALSE(IsGloballyRoutableIp("fc00::1"));          // IPv6 unique-local
  EXPECT_FALSE(IsGloballyRoutableIp("::ffff:10.0.0.1"));  // v4-mapped
  EXPECT_FALSE(IsGloballyRoutableIp("not-an-ip"));
  EXPECT_FALSE(IsGloballyRoutableIp(""));
}

TEST(SsrfGuardTest, AcceptsGlobalUnicast) {
  EXPECT_TRUE(IsGloballyRoutableIp("8.8.8.8"));
  EXPECT_TRUE(IsGloballyRoutableIp("1.1.1.1"));
  EXPECT_TRUE(IsGloballyRoutableIp("2606:4700:4700::1111"));
}

TEST(AllowlistTest, OnlyHttpsOriginsInTheAllowlistPass) {
  std::set<GoogleString> allow;
  allow.insert("https://keys.example.com");

  EXPECT_TRUE(IsAllowlistedHttpsUrl(
      "https://keys.example.com/.well-known/jwks.json", allow));
  // Case-insensitive host.
  EXPECT_TRUE(IsAllowlistedHttpsUrl("https://KEYS.EXAMPLE.COM/x", allow));
  // http (not https) rejected even if the host matches.
  EXPECT_FALSE(IsAllowlistedHttpsUrl("http://keys.example.com/x", allow));
  // Off-allowlist origin rejected.
  EXPECT_FALSE(IsAllowlistedHttpsUrl("https://evil.example.com/x", allow));
  // Malformed rejected.
  EXPECT_FALSE(IsAllowlistedHttpsUrl("keys.example.com", allow));
  EXPECT_FALSE(IsAllowlistedHttpsUrl("", allow));
}

TEST(AllowlistTest, EmptyAllowlistRejectsEverything) {
  std::set<GoogleString> empty;
  EXPECT_FALSE(IsAllowlistedHttpsUrl("https://keys.example.com/x", empty));
}

// GetKey must fail closed (kError) WITHOUT touching the network on any of:
// empty allowlist, unmapped host, or an off-allowlist directory URL.
TEST(NetFetchKeyDirectoryTest, FailsClosedBeforeAnyFetch) {
  NullMessageHandler handler;
  RequestContextPtr null_ctx;  // never dereferenced on these paths

  // (1) Empty allowlist => feature disabled.
  {
    NetFetchKeyDirectory::Options opts;
    NetFetchKeyDirectory dir(/*fetcher=*/nullptr, &handler, null_ctx, opts);
    EXPECT_EQ(KeyLookupResult::kError,
              dir.GetKey("keys.example.com", "k1", 0).status);
  }
  // (2) Allowlist set but host not mapped to a directory URL.
  {
    NetFetchKeyDirectory::Options opts;
    opts.host_allowlist.insert("https://keys.example.com");
    NetFetchKeyDirectory dir(nullptr, &handler, null_ctx, opts);
    EXPECT_EQ(KeyLookupResult::kError,
              dir.GetKey("keys.example.com", "k1", 0).status);
  }
  // (3) Host mapped, but to an off-allowlist URL.
  {
    NetFetchKeyDirectory::Options opts;
    opts.host_allowlist.insert("https://keys.example.com");
    opts.host_to_directory_url["keys.example.com"] =
        "https://evil.example.com/j";
    NetFetchKeyDirectory dir(nullptr, &handler, null_ctx, opts);
    EXPECT_EQ(KeyLookupResult::kError,
              dir.GetKey("keys.example.com", "k1", 0).status);
  }
}

}  // namespace
}  // namespace webbotauth
}  // namespace net_instaweb
