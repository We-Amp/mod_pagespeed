// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/net/curl_fetcher.h"

#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace pagespeed {
namespace {

using Argv = std::vector<std::string>;

bool Contains(const Argv& a, const std::string& s) {
  for (const std::string& x : a) {
    if (x == s) return true;
  }
  return false;
}

// True iff `flag` appears immediately followed by `value`.
bool HasFlagValue(const Argv& a, const std::string& flag,
                  const std::string& value) {
  for (std::size_t i = 0; i + 1 < a.size(); ++i) {
    if (a[i] == flag && a[i + 1] == value) return true;
  }
  return false;
}

bool ContainsSubstr(const Argv& a, const std::string& needle) {
  for (const std::string& x : a) {
    if (x.find(needle) != std::string::npos) return true;
  }
  return false;
}

CurlFetchOptions Opts() { return CurlFetchOptions{}; }

TEST(CurlFetcher, HostnamePinnedAndHardened) {
  const Argv a = BuildPinnedGetArgv("https://example.com/page", "example.com",
                                    443, "93.184.216.34", Opts());
  ASSERT_FALSE(a.empty());
  EXPECT_EQ(a.front(), "curl");
  // the IP pin (TLS SNI + Host stay example.com; connection goes to the IP)
  EXPECT_TRUE(HasFlagValue(a, "--resolve", "example.com:443:93.184.216.34"));
  // scheme allowlist (initial + redirects)
  EXPECT_TRUE(HasFlagValue(a, "--proto", "=http,https"));
  EXPECT_TRUE(HasFlagValue(a, "--proto-redir", "=http,https"));
  // never auto-follow — the fetcher re-adjudicates each hop
  EXPECT_TRUE(HasFlagValue(a, "--max-redirs", "0"));
  EXPECT_FALSE(Contains(a, "-L"));
  EXPECT_FALSE(Contains(a, "--location"));
  // GET only
  EXPECT_TRUE(HasFlagValue(a, "-X", "GET"));
  // the URL is passed as an explicit option value (no '-'-leading flag injection)
  EXPECT_TRUE(HasFlagValue(a, "--url", "https://example.com/page"));
  // bounded
  EXPECT_TRUE(Contains(a, "--max-time"));
  EXPECT_TRUE(Contains(a, "--max-filesize"));
}

TEST(CurlFetcher, NoCredentialsOrBodyForwarded) {
  const Argv a = BuildPinnedGetArgv("https://example.com/", "example.com", 443,
                                    "93.184.216.34", Opts());
  // no cookies, no auth, no request body, no form data
  EXPECT_FALSE(Contains(a, "-b"));
  EXPECT_FALSE(Contains(a, "--cookie"));
  EXPECT_FALSE(Contains(a, "-c"));
  EXPECT_FALSE(Contains(a, "--cookie-jar"));
  EXPECT_FALSE(Contains(a, "-u"));
  EXPECT_FALSE(Contains(a, "--user"));
  EXPECT_FALSE(Contains(a, "-d"));
  EXPECT_FALSE(Contains(a, "--data"));
  EXPECT_FALSE(Contains(a, "--data-binary"));
  EXPECT_FALSE(Contains(a, "-T"));
  EXPECT_FALSE(Contains(a, "--upload-file"));
  // no Cookie/Authorization header smuggled into a -H value
  EXPECT_FALSE(ContainsSubstr(a, "Cookie:"));
  EXPECT_FALSE(ContainsSubstr(a, "Authorization:"));
}

TEST(CurlFetcher, HeadOptionSendsHeadOnly) {
  CurlFetchOptions opts;
  opts.head = true;
  const Argv a = BuildPinnedGetArgv("https://example.com/a.css", "example.com",
                                    443, "93.184.216.34", opts);
  ASSERT_FALSE(a.empty());
  EXPECT_TRUE(Contains(a, "--head"));
  EXPECT_FALSE(Contains(a, "-X"));
  EXPECT_FALSE(Contains(a, "GET"));
  // The same pin, anonymity and redirect rules as a GET.
  EXPECT_TRUE(HasFlagValue(a, "--resolve", "example.com:443:93.184.216.34"));
  EXPECT_TRUE(HasFlagValue(a, "--max-redirs", "0"));
  EXPECT_TRUE(HasFlagValue(a, "--url", "https://example.com/a.css"));
  EXPECT_FALSE(ContainsSubstr(a, "Cookie:"));
  EXPECT_FALSE(ContainsSubstr(a, "Authorization:"));
  // The default stays GET.
  EXPECT_FALSE(
      Contains(BuildPinnedGetArgv("https://example.com/", "example.com", 443,
                                  "93.184.216.34", Opts()),
               "--head"));
}

// A HEAD asks whether a resource is served, not for its bytes. curl checks
// --max-filesize against the advertised Content-Length even for a HEAD
// ("Maximum file size exceeded", exit 63), so a cap on a HEAD refuses the
// answer for every resource larger than the cap: a 230 KB stylesheet asked
// about with a 64 KiB cap never got one.
TEST(CurlFetcher, HeadCarriesNoFileSizeCap) {
  CurlFetchOptions opts;
  opts.head = true;
  opts.max_response_bytes = 64u << 10;
  const Argv a = BuildPinnedGetArgv("https://example.com/big.css",
                                    "example.com", 443, "93.184.216.34", opts);
  ASSERT_FALSE(a.empty());
  EXPECT_TRUE(Contains(a, "--head"));
  EXPECT_FALSE(Contains(a, "--max-filesize"));
  // Still bounded in time.
  EXPECT_TRUE(Contains(a, "--max-time"));
}

// Negative control: a GET with the same options keeps its transfer cap.
TEST(CurlFetcher, GetKeepsFileSizeCap) {
  CurlFetchOptions opts;
  opts.max_response_bytes = 64u << 10;
  const Argv a = BuildPinnedGetArgv("https://example.com/big.css",
                                    "example.com", 443, "93.184.216.34", opts);
  ASSERT_FALSE(a.empty());
  EXPECT_FALSE(Contains(a, "--head"));
  EXPECT_TRUE(HasFlagValue(a, "--max-filesize", "65536"));
}

TEST(CurlFetcher, IpLiteralHostNeedsNoResolve) {
  // host == pinned_ip → the URL already pins the target; --resolve is redundant.
  const Argv a = BuildPinnedGetArgv("http://127.0.0.1:3000/api", "127.0.0.1",
                                    3000, "127.0.0.1", Opts());
  ASSERT_FALSE(a.empty());
  EXPECT_FALSE(Contains(a, "--resolve"));
  EXPECT_TRUE(HasFlagValue(a, "--url", "http://127.0.0.1:3000/api"));
  EXPECT_TRUE(HasFlagValue(a, "-X", "GET"));
}

TEST(CurlFetcher, LeadingDashUrlIsSafe) {
  // A hostile URL that looks like a flag must still ride behind --url.
  const Argv a =
      BuildPinnedGetArgv("http://-oProxyCmd.example/", "-oproxycmd.example", 80,
                         "93.184.216.34", Opts());
  ASSERT_FALSE(a.empty());
  EXPECT_TRUE(HasFlagValue(a, "--url", "http://-oProxyCmd.example/"));
}

TEST(CurlFetcher, FailClosedOnBadInput) {
  EXPECT_TRUE(
      BuildPinnedGetArgv("", "example.com", 443, "1.2.3.4", Opts()).empty());
  EXPECT_TRUE(
      BuildPinnedGetArgv("https://x/", "", 443, "1.2.3.4", Opts()).empty());
  EXPECT_TRUE(BuildPinnedGetArgv("https://x/", "x", 443, "", Opts()).empty());
  EXPECT_TRUE(
      BuildPinnedGetArgv("https://x/", "x", 0, "1.2.3.4", Opts()).empty());
  EXPECT_TRUE(
      BuildPinnedGetArgv("https://x/", "x", 99999, "1.2.3.4", Opts()).empty());
}

// MF-1: a URL containing a byte that ParseHttpUrl normalizes/strips but curl does
// not (backslash, tab, LF, CR) lets curl parse a different authority than the one
// the policy validated => the --resolve pin is bypassed. Fail closed.
TEST(CurlFetcher, FailClosedOnParserDivergentBytes) {
  // The empirically-confirmed bypass: curl reads host 127.0.0.1 from `\@`, while
  // ParseHttpUrl read host example.com — so this must NEVER reach curl.
  EXPECT_TRUE(BuildPinnedGetArgv("http://example.com\\@127.0.0.1/",
                                 "example.com", 80, "93.184.216.34", Opts())
                  .empty());
  EXPECT_TRUE(
      BuildPinnedGetArgv("https://x/\tpath", "x", 443, "1.2.3.4", Opts())
          .empty());
  EXPECT_TRUE(
      BuildPinnedGetArgv("https://x/\npath", "x", 443, "1.2.3.4", Opts())
          .empty());
  EXPECT_TRUE(
      BuildPinnedGetArgv("https://x/\rpath", "x", 443, "1.2.3.4", Opts())
          .empty());
}

// H2 — curl must ignore ~/.curlrc (proxy/insecure injection) and ~/.netrc creds.
TEST(CurlFetcher, ConfigAndNetrcDisabled) {
  const Argv a = BuildPinnedGetArgv("https://example.com/", "example.com", 443,
                                    "93.184.216.34", Opts());
  ASSERT_GE(a.size(), 2u);
  EXPECT_EQ(a.at(1),
            "-q");  // MUST be the first option to disable curlrc parsing
  EXPECT_TRUE(Contains(a, "--no-netrc"));
}

// A proxy (http_proxy/https_proxy/all_proxy from env or curlrc) would tunnel the
// request past --resolve to an un-pinned host (SSRF). --noproxy "*" disables it for
// every host; the fetcher additionally scrubs *_proxy from the spawn env.
TEST(CurlFetcher, ProxyDisabledForAllHosts) {
  const Argv a = BuildPinnedGetArgv("https://example.com/", "example.com", 443,
                                    "93.184.216.34", Opts());
  EXPECT_TRUE(HasFlagValue(a, "--noproxy", "*"));
}

// Origin responses are fetched compressed and decoded by curl, so the caller's
// output cap (applied to curl's stdout) bounds the decoded size.
TEST(CurlFetcher, RequestsCompressedResponses) {
  const Argv a = BuildPinnedGetArgv("https://example.com/", "example.com", 443,
                                    "93.184.216.34", Opts());
  EXPECT_TRUE(Contains(a, "--compressed"));
  // Decoding is curl's job: no hand-written Accept-Encoding header that would
  // leave the body undecoded.
  EXPECT_FALSE(ContainsSubstr(a, "Accept-Encoding"));
}

// H1 — globbing off, and a {}/[] URL must ride verbatim (no glob fan-out past the pin).
TEST(CurlFetcher, GlobbingOffAndUrlVerbatim) {
  const Argv a = BuildPinnedGetArgv("http://{a,b}.test/p", "site.test", 80,
                                    "93.184.216.34", Opts());
  EXPECT_TRUE(Contains(a, "-g"));
  EXPECT_TRUE(
      HasFlagValue(a, "--url", "http://{a,b}.test/p"));  // one arg, no fan-out
}

// N1 — an IPv6 pinned IP is bracketed in --resolve (curl's documented form).
TEST(CurlFetcher, Ipv6PinnedIpBracketed) {
  const Argv a = BuildPinnedGetArgv("https://example.com/", "example.com", 443,
                                    "2001:db8::1", Opts());
  EXPECT_TRUE(HasFlagValue(a, "--resolve", "example.com:443:[2001:db8::1]"));
}

TEST(CurlFetcher, OptionsReflectedInArgv) {
  CurlFetchOptions o;
  o.max_time_sec = 9;
  o.max_response_bytes = 1234;
  const Argv a = BuildPinnedGetArgv("https://x/", "x", 443, "1.2.3.4", o);
  EXPECT_TRUE(HasFlagValue(a, "--max-time", "9"));
  EXPECT_TRUE(HasFlagValue(a, "--max-filesize", "1234"));
}

}  // namespace
}  // namespace pagespeed
