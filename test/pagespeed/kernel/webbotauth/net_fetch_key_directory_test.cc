// Copyright 2026 We-Amp B.V.
// Licensed under the Apache License, Version 2.0 (the "License").
//
// Hermetic unit tests for the network trust boundary of
// NetFetchKeyDirectory, which guards key-directory fetches.
// Exercises the pure predicates IsGloballyRoutableIp / IsAllowlistedHttpsUrl,
// the fail-closed branches of GetKey that return BEFORE any network I/O, and
// the fetch path itself through a recording fake fetcher: a non-200 response
// is rejected after the single fetch, its body unparsed.

#include "pagespeed/kernel/webbotauth/net_fetch_key_directory.h"

#include <memory>
#include <utility>

#include "gtest/gtest.h"
#include "net/instaweb/http/public/async_fetch.h"
#include "net/instaweb/http/public/url_async_fetcher.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "pagespeed/kernel/util/platform.h"
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

// base64url (no pad) of 32 zero bytes; the same trick as
// static_key_directory_test.cc.
const char kX[] = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

GoogleString OneKeyDoc() {
  return StrCat(
      "{\"keys\":[{\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"kid\":\"only\",\"x\":"
      "\"",
      kX, "\"}]}");
}

NetFetchKeyDirectory::Options MappedOptions() {
  NetFetchKeyDirectory::Options opts;
  opts.host_allowlist.insert("https://keys.example.com");
  opts.host_to_directory_url["keys.example.com"] =
      "https://keys.example.com/.well-known/jwks.json";
  return opts;
}

// Serves a canned response inline and records how many fetches it is handed.
class RecordingFetcher : public UrlAsyncFetcher {
 public:
  explicit RecordingFetcher(GoogleString body) : body_(std::move(body)) {}
  void set_status(int status) { status_ = status; }
  void set_location(GoogleString location) { location_ = std::move(location); }

  void Fetch(const GoogleString& /*url*/, MessageHandler* handler,
             AsyncFetch* fetch) override {
    ++calls_;
    ResponseHeaders* headers = fetch->response_headers();
    headers->SetStatusAndReason(static_cast<HttpStatus::Code>(status_));
    if (!location_.empty()) {
      headers->Add("Location", location_);
    }
    fetch->Write(body_, handler);
    fetch->Done(true);
  }

  int calls() const { return calls_; }

 private:
  GoogleString body_;
  int status_ = HttpStatus::kOK;
  GoogleString location_;
  int calls_ = 0;
};

// A non-200 response is rejected after the single fetch: its body, even a
// well-formed JWKS, is never parsed, and no second fetch is issued.
TEST(NetFetchKeyDirectoryTest, NonOkResponseIsRejectedWithoutRetry) {
  std::unique_ptr<ThreadSystem> thread_system(Platform::CreateThreadSystem());
  NullMessageHandler handler;
  RecordingFetcher fetcher(OneKeyDoc());
  fetcher.set_status(HttpStatus::kInternalServerError);

  RequestContextPtr ctx =
      RequestContext::NewTestRequestContext(thread_system.get());
  NetFetchKeyDirectory dir(&fetcher, &handler, ctx, MappedOptions());
  EXPECT_EQ(KeyLookupResult::kError,
            dir.GetKey("keys.example.com", "only", 0).status);
  EXPECT_EQ(1, fetcher.calls());
}

// A redirect status is never chased and its body never parsed, even when that
// body is a well-formed JWKS: the lookup fails closed after a single fetch.
TEST(NetFetchKeyDirectoryTest, RedirectResponseIsRejectedNotFollowed) {
  std::unique_ptr<ThreadSystem> thread_system(Platform::CreateThreadSystem());
  NullMessageHandler handler;
  RecordingFetcher fetcher(OneKeyDoc());
  fetcher.set_status(HttpStatus::kFound);
  fetcher.set_location("https://other.example.com/jwks");

  RequestContextPtr ctx =
      RequestContext::NewTestRequestContext(thread_system.get());
  NetFetchKeyDirectory dir(&fetcher, &handler, ctx, MappedOptions());
  EXPECT_EQ(KeyLookupResult::kError,
            dir.GetKey("keys.example.com", "only", 0).status);
  EXPECT_EQ(1, fetcher.calls());  // no second fetch to the redirect target
}

}  // namespace
}  // namespace webbotauth
}  // namespace net_instaweb
