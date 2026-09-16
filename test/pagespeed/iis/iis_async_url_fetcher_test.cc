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

// Regression test for IisAsyncUrlFetcher::DeriveHost lifetime contract.
//
// The previous inline implementation of host derivation in
// IisAsyncUrlFetcher::Fetch was:
//
//   const char* host = fetch->request_headers()->Lookup1(kHost);
//   if (host == NULL) {
//     GoogleUrl gurl(url);
//     host = gurl.HostAndPort().as_string().c_str();   // dangling
//   }
//   message_handler->Message(..., "%s", host);
//   worker->GetUrl(url, host);
//
// `gurl.HostAndPort().as_string()` returned a temporary GoogleString. Calling
// `.c_str()` on that temporary and stashing the pointer in `host` produced a
// pointer into a buffer that was freed at the next semicolon. The reads via
// `%s` in Message() and via `std::string(host)` in worker->GetUrl() were
// reading freed memory.
//
// Under AppVerifier's PageHeap, freed allocations land on a guard page. The
// reads tripped STATUS_ACCESS_VIOLATION, which AppVerifier raises as a
// FIRST_CHANCE_ACCESS_VIOLATION stop (Heaps provider, stop code 0x13). On the
// Windows AppVerif CI job this surfaced as `vrfcore.dll 0x80000003`
// Application Error events during every test that triggers a background
// image rewrite — `test_config_isolation` (the unlucky one without
// fetch_until retry), `test_lazyload`, `test_query_params_resource`, etc.
//
// The fix extracts the host-derivation to a static helper that returns by
// value, so the caller holds the std::string for the duration of the
// subsequent reads. This test exercises that contract on any platform:
//
// 1. With an explicit Host header, DeriveHost returns a copy of it.
// 2. Without an explicit Host header, DeriveHost returns HostAndPort
//    derived from the URL.
// 3. The returned string is independent of the inputs and survives even if
//    the inputs are freed or overwritten (the central lifetime property
//    that distinguishes the fix from the dangling-pointer original).

#include "pagespeed/iis/iis_async_url_fetcher.h"

#include "pagespeed/iis/asyncwinhttp.h"

#include <string>

#include "pagespeed/kernel/base/string.h"
#include "gtest/gtest.h"

namespace net_instaweb {
namespace {

TEST(IisAsyncUrlFetcherDeriveHostTest, ExplicitHostHeaderUsed) {
  // When an explicit Host header is supplied (non-null), DeriveHost should
  // return a copy of it — even if the URL would derive a different host.
  GoogleString result =
      IisAsyncUrlFetcher::DeriveHost("client.example.com",
                                     "http://other.example.org:8080/path");
  EXPECT_EQ("client.example.com", result);
}

TEST(IisAsyncUrlFetcherDeriveHostTest, DerivesFromUrlWhenHeaderMissing) {
  // When no explicit Host header is supplied, DeriveHost should fall back to
  // extracting HostAndPort from the URL.
  GoogleString result =
      IisAsyncUrlFetcher::DeriveHost(nullptr,
                                     "http://example.com:8080/img.jpg");
  EXPECT_EQ("example.com:8080", result);
}

TEST(IisAsyncUrlFetcherDeriveHostTest, DerivesFromUrlWithDefaultPort) {
  // Default port should be omitted from HostAndPort.
  GoogleString result =
      IisAsyncUrlFetcher::DeriveHost(nullptr, "http://example.com/img.jpg");
  EXPECT_EQ("example.com", result);
}

TEST(IisAsyncUrlFetcherDeriveHostTest,
     ReturnedStringSurvivesInputMutation) {
  // Lifetime regression: callers used to assign the c_str() of a temporary
  // GoogleString to a const char*, leaving them with a dangling pointer once
  // the temporary went out of scope at the end of the statement. The fix
  // returns a fresh GoogleString by value. Verify the returned string
  // remains valid (and uncorrupted) after the input URL is mutated, proving
  // we are not aliasing into the caller's storage.
  GoogleString url = "http://example.com:8080/img.jpg";
  GoogleString result = IisAsyncUrlFetcher::DeriveHost(nullptr, url);
  // Mutate the input. If the result were aliasing into url's buffer, this
  // would corrupt it.
  url = "http://attacker.example.org/";
  EXPECT_EQ("example.com:8080", result);
}

TEST(IisAsyncUrlFetcherDeriveHostTest,
     ReturnedStringSurvivesHeaderMutation) {
  // Same lifetime regression for the explicit-Host path. Use a heap-allocated
  // C string that we destroy after the call to ensure the result is a
  // standalone copy.
  std::string header = "client.example.com";
  GoogleString result =
      IisAsyncUrlFetcher::DeriveHost(header.c_str(),
                                     "http://other.example.org/path");
  header = "scribble-over-original-bytes";
  EXPECT_EQ("client.example.com", result);
}


// The IIS native fetcher's WinHTTP client resolves "localhost"
// through the OS resolver, which can prefer the IPv6 loopback while the
// origin site only answers on IPv4. The async connect then fails, PSOL
// remembers the fetch failure, and rewrites of local sub-resources never
// converge (the Windows CI lane surfaced this as rewrite-convergence
// timeouts under Application Verifier). WinHTTP::LoopbackConnectHost is the
// pure decision helper for the fix: it picks the address handed to
// WinHttpConnect for a loopback host per attempt, while the Host request
// header keeps naming the original authority. These tests pin the mapping.

typedef ::ASyncWinHTTP::WinHTTP WinHttpClient;

TEST(WinHttpLoopbackConnectHostTest, LocalhostPinsIpv4ThenIpv6) {
  // First attempt connects to the IPv4 loopback; the single retry attempt
  // falls back to the IPv6 loopback. The IPv6 loopback must be BRACKETED:
  // WinHttpConnect rejects a bare "::1" synchronously (error 12005,
  // ERROR_WINHTTP_INVALID_URL), which completes the fetch as failed before
  // the retry can engage.
  EXPECT_EQ(std::wstring(L"127.0.0.1"),
            WinHttpClient::LoopbackConnectHost(L"localhost", false));
  EXPECT_EQ(std::wstring(L"[::1]"),
            WinHttpClient::LoopbackConnectHost(L"localhost", true));
}

TEST(WinHttpLoopbackConnectHostTest, LocalhostMatchIsCaseInsensitive) {
  EXPECT_EQ(std::wstring(L"127.0.0.1"),
            WinHttpClient::LoopbackConnectHost(L"LOCALHOST", false));
  EXPECT_EQ(std::wstring(L"[::1]"),
            WinHttpClient::LoopbackConnectHost(L"LocalHost", true));
}

TEST(WinHttpLoopbackConnectHostTest, Ipv6LoopbackTargetRetriesIpv4) {
  // A URL that already names the IPv6 loopback keeps it for the first
  // attempt but retries on IPv4 (the observed failure was a native fetch of
  // http://[::1]:8080/... against a binding that only answered on IPv4).
  // The first-attempt address must stay bracketed -- WinHttpCrackUrl keeps
  // the brackets, and WinHttpConnect requires them for IPv6 literals.
  EXPECT_EQ(std::wstring(L"[::1]"),
            WinHttpClient::LoopbackConnectHost(L"::1", false));
  EXPECT_EQ(std::wstring(L"127.0.0.1"),
            WinHttpClient::LoopbackConnectHost(L"::1", true));
  EXPECT_EQ(std::wstring(L"[::1]"),
            WinHttpClient::LoopbackConnectHost(L"[::1]", false));
  EXPECT_EQ(std::wstring(L"127.0.0.1"),
            WinHttpClient::LoopbackConnectHost(L"[::1]", true));
}

TEST(WinHttpLoopbackConnectHostTest, NonLoopbackHostsAreNotPinned) {
  EXPECT_EQ(std::wstring(),
            WinHttpClient::LoopbackConnectHost(L"example.com", false));
  EXPECT_EQ(std::wstring(),
            WinHttpClient::LoopbackConnectHost(L"example.com", true));
  // An explicit 127.0.0.1 already names the answering family; leave it be.
  EXPECT_EQ(std::wstring(),
            WinHttpClient::LoopbackConnectHost(L"127.0.0.1", false));
  // No prefix confusion with hostnames that merely start with "localhost".
  EXPECT_EQ(std::wstring(),
            WinHttpClient::LoopbackConnectHost(L"localhost.example.com", false));
}

}  // namespace
}  // namespace net_instaweb
