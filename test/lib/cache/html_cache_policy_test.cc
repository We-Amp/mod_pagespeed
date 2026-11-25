// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The HTML cacheability policy: the store verdict and the serve verdict.
//
// The properties these tests exist to hold, in the order the policy depends
// on them:
//
//   1. every store rule refuses for its own reason, and the FIRST failing
//      rule is the reason reported when several would fail;
//   2. nothing is ever stored on a guessed lifetime: a lifetime comes from
//      s-maxage, max-age or Expires, and a response that states none needs a
//      validator and is always revalidated;
//   3. credentials, cookies, a personalising Vary and per-client page markers
//      keep a response out of the cache, and the error direction of every
//      doubtful case is toward not storing;
//   4. a stale entry is never served, in any mode;
//   5. kOff and kReportOnly never enforce, report-only never returns a storing
//      or a serve action, and the verdict functions are pure.

#include "lib/cache/html_cache_policy.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "lib/cache/origin_cache_control.h"
#include "lib/classify/alternate_metadata.h"

namespace pagespeed {
namespace {

using Meta = AlternateMetadata;
using Fields = std::vector<HeaderField>;

// Sun, 06 Nov 1994 08:49:37 GMT.
constexpr uint32_t kNow = 784111777;
constexpr std::string_view kNowDate = "Sun, 06 Nov 1994 08:49:37 GMT";
constexpr std::string_view kPlus60 = "Sun, 06 Nov 1994 08:50:37 GMT";
constexpr std::string_view kMinus60 = "Sun, 06 Nov 1994 08:48:37 GMT";

// One store call, with every input defaulted to a storable response.
struct StoreInput {
  HtmlStoreRequest request{.method = "GET", .url_path = "/index.html"};
  int status = 200;
  Fields headers = {{"Content-Type", "text/html; charset=utf-8"},
                    {"Cache-Control", "max-age=300"}};
  uint64_t body_bytes = 1000;
  bool body_complete = true;
  std::string body = "<html><body>hello</body></html>";
  HtmlStorePolicy policy{.mode = HtmlCacheMode::kServeVariants,
                         .max_body_bytes = uint64_t{5} * 1024 * 1024,
                         .now_seconds = kNow};
  // The caller handed over less of the body than the scan window asks for.
  bool short_prefix = false;

  // Replace the Cache-Control line(s) and keep everything else.
  void SetCacheControl(std::string_view value) {
    Fields out;
    for (const HeaderField& h : headers) {
      if (h.name != "Cache-Control") out.push_back(h);
    }
    out.push_back({"Cache-Control", value});
    headers = out;
  }
  // Drop Cache-Control altogether.
  void DropCacheControl() {
    Fields out;
    for (const HeaderField& h : headers) {
      if (h.name != "Cache-Control") out.push_back(h);
    }
    headers = out;
  }
  void Add(std::string_view name, std::string_view value) {
    headers.push_back({name, value});
  }

  [[nodiscard]] HtmlStoreVerdict Run() const {
    HtmlStoreResponse response;
    response.status = status;
    response.headers = headers;
    response.body_bytes = body_bytes;
    response.body_complete = body_complete;
    // body_bytes is often larger than the sample body; the rest is padding.
    std::string prefix = body;
    const uint64_t wanted =
        std::min<uint64_t>(body_bytes, kPersonalisationScanBytes);
    if (!short_prefix && prefix.size() < wanted) prefix.resize(wanted, ' ');
    response.body_prefix = prefix;
    return ClassifyHtmlForStore(request, response, policy);
  }
};

void ExpectRefused(const StoreInput& in, HtmlStoreReason reason,
                   std::string_view detail = "") {
  const HtmlStoreVerdict v = in.Run();
  EXPECT_EQ(v.action, HtmlStoreAction::kRefuse) << v.reason_code();
  EXPECT_EQ(HtmlStoreReasonName(v.reason), HtmlStoreReasonName(reason));
  EXPECT_EQ(v.detail, detail);
  EXPECT_FALSE(v.stores());
}

void ExpectStored(const StoreInput& in) {
  const HtmlStoreVerdict v = in.Run();
  EXPECT_EQ(v.action, HtmlStoreAction::kStore) << v.reason_code();
  EXPECT_EQ(v.reason, HtmlStoreReason::kNone);
  EXPECT_TRUE(v.stores());
}

// ------------------------------------------------------------- store rules

TEST(HtmlCachePolicyTest, BaselineIsStored) {
  const HtmlStoreVerdict v = StoreInput().Run();
  EXPECT_EQ(v.action, HtmlStoreAction::kStore);
  EXPECT_EQ(v.reason, HtmlStoreReason::kNone);
  EXPECT_EQ(v.reason_code(), "none");
  EXPECT_EQ(v.lifetime_s, 300u);
  EXPECT_TRUE(v.lifetime_explicit);
  EXPECT_FALSE(v.needs_validation);
  EXPECT_FALSE(v.has_validators);
  EXPECT_FALSE(v.explicit_shared);
  EXPECT_TRUE(v.sidecar_evaluated);
  EXPECT_TRUE(v.sidecar.swap_eligible());
  EXPECT_TRUE(v.vary_tokens.empty());
}

TEST(HtmlCachePolicyTest, MethodMustBeGet) {
  StoreInput in;
  in.request.method = "HEAD";
  ExpectRefused(in, HtmlStoreReason::kMethod);
  in.request.method = "POST";
  ExpectRefused(in, HtmlStoreReason::kMethod);
  in.request.method = "get";  // the caller normalises; lowercase is refused
  ExpectRefused(in, HtmlStoreReason::kMethod);
  in.request.method = "";
  ExpectRefused(in, HtmlStoreReason::kMethod);
}

TEST(HtmlCachePolicyTest, StatusMustBe200) {
  for (int status : {0, 204, 206, 301, 304, 404, 500}) {
    StoreInput in;
    in.status = status;
    ExpectRefused(in, HtmlStoreReason::kStatus);
  }
}

TEST(HtmlCachePolicyTest, IncompleteBodyAndContentRangeAreRefused) {
  StoreInput in;
  in.body_complete = false;
  ExpectRefused(in, HtmlStoreReason::kIncomplete);

  StoreInput ranged;
  ranged.Add("Content-Range", "bytes 0-9/100");
  ExpectRefused(ranged, HtmlStoreReason::kIncomplete);

  // 206 is a status refusal first only if the status rule runs first; here the
  // status is 200 with a Content-Range, which is the incomplete rule.
  StoreInput partial;
  partial.status = 206;
  partial.Add("Content-Range", "bytes 0-9/100");
  ExpectRefused(partial, HtmlStoreReason::kStatus);
}

TEST(HtmlCachePolicyTest, UrlMustBeKeySafe) {
  for (std::string_view bad :
       {"", "index.html", "/a?b=1", "/a%20b", "/a#frag", "/?", "/%",
        "/index.html;jsessionid=X", "/a,b;c=d", "/;",
        // The rewritten-URL shape: the optimizer's own output is never an
        // origin page, wherever the marker sits in the path.
        "/img/a.jpg.pagespeed.ce.HASH.jpg", "/xa.jpg.pagespeed.ic.HASH.jpg",
        "/dir.pagespeed.x/index.html", "/a.pagespeed."}) {
    StoreInput in;
    in.request.url_path = bad;
    ExpectRefused(in, HtmlStoreReason::kUrl);
  }
  for (std::string_view good : {"/", "/a/b.html", "/a b", "/a,b",
                                // Near misses of the marker are ordinary paths.
                                "/pagespeed/docs.html", "/a.pagespeed",
                                "/pagespeed.html", "/mod-pagespeed.html"}) {
    StoreInput in;
    in.request.url_path = good;
    ExpectStored(in);
  }
}

TEST(HtmlCachePolicyTest, ContentTypeMustBeTextHtml) {
  StoreInput ok;
  ok.headers = {{"content-type", "TEXT/HTML ; charset=utf-8"},
                {"Cache-Control", "max-age=60"}};
  ExpectStored(ok);

  StoreInput bare;
  bare.headers = {{"Content-Type", "text/html"},
                  {"Cache-Control", "max-age=60"}};
  ExpectStored(bare);

  for (std::string_view type :
       {"application/xhtml+xml", "text/plain", "text/htmlx", "image/png", ""}) {
    StoreInput in;
    in.headers = {{"Content-Type", type}, {"Cache-Control", "max-age=60"}};
    ExpectRefused(in, HtmlStoreReason::kContentType);
  }

  StoreInput missing;
  missing.headers = {{"Cache-Control", "max-age=60"}};
  ExpectRefused(missing, HtmlStoreReason::kContentType);

  StoreInput twice;
  twice.Add("Content-Type", "text/html");
  ExpectRefused(twice, HtmlStoreReason::kContentType);
}

TEST(HtmlCachePolicyTest, ContentEncodingMustBeAbsentOrIdentity) {
  StoreInput gzip;
  gzip.Add("Content-Encoding", "gzip");
  ExpectRefused(gzip, HtmlStoreReason::kEncoding);

  StoreInput br;
  br.Add("Content-Encoding", "br");
  ExpectRefused(br, HtmlStoreReason::kEncoding);

  StoreInput empty;
  empty.Add("Content-Encoding", "");
  ExpectRefused(empty, HtmlStoreReason::kEncoding);

  // `identity` passes this rule; the header itself is then one the sidecar
  // cannot reproduce, which is reported as a header refusal.
  StoreInput identity;
  identity.Add("Content-Encoding", "identity");
  ExpectRefused(identity, HtmlStoreReason::kHeaders, "content-encoding");
}

TEST(HtmlCachePolicyTest, CredentialsAreNeverStored) {
  StoreInput auth;
  auth.request.has_authorization =
      true;  // Authorization or Proxy-Authorization
  ExpectRefused(auth, HtmlStoreReason::kAuthRequest);

  StoreInput user;
  user.request.authenticated_user = true;
  ExpectRefused(user, HtmlStoreReason::kAuthRequest);

  // No response directive overrides it.
  StoreInput overridden;
  overridden.request.has_authorization = true;
  overridden.SetCacheControl("public, s-maxage=600, must-revalidate");
  ExpectRefused(overridden, HtmlStoreReason::kAuthRequest);
}

TEST(HtmlCachePolicyTest, SetCookieIsRefused) {
  StoreInput a;
  a.Add("Set-Cookie", "sid=1");
  ExpectRefused(a, HtmlStoreReason::kSetCookie);
  StoreInput b;
  b.Add("set-cookie2", "sid=1");
  ExpectRefused(b, HtmlStoreReason::kSetCookie);
  StoreInput b2;
  b2.Add("Set-Cookie2", "sid=1; Version=1");
  ExpectRefused(b2, HtmlStoreReason::kSetCookie);
  // Even on an explicitly public response.
  StoreInput c;
  c.SetCacheControl("public, max-age=60");
  c.Add("Set-Cookie", "sid=1");
  ExpectRefused(c, HtmlStoreReason::kSetCookie);
}

TEST(HtmlCachePolicyTest, PrivateInEitherFormIsRefused) {
  for (std::string_view cc :
       {"private", "private, max-age=60", "max-age=60, PRIVATE",
        "private=\"Set-Cookie\", max-age=60", "private=\"\"",
        "max-age=60, private = x"}) {
    StoreInput in;
    in.SetCacheControl(cc);
    ExpectRefused(in, HtmlStoreReason::kPrivate);
  }
  // Two lines, one qualified, one bare.
  StoreInput two;
  two.SetCacheControl("private=\"Set-Cookie\"");
  two.Add("Cache-Control", "private");
  ExpectRefused(two, HtmlStoreReason::kPrivate);
}

TEST(HtmlCachePolicyTest, NoStoreIsRefused) {
  StoreInput in;
  in.SetCacheControl("no-store");
  ExpectRefused(in, HtmlStoreReason::kNoStore);
  StoreInput mixed;
  mixed.SetCacheControl("max-age=60, No-Store");
  ExpectRefused(mixed, HtmlStoreReason::kNoStore);
}

TEST(HtmlCachePolicyTest, CookieRequestNeedsExplicitSharing) {
  StoreInput plain;
  plain.request.has_cookie = true;
  ExpectRefused(plain, HtmlStoreReason::kCookieRequest);

  StoreInput no_cc;
  no_cc.request.has_cookie = true;
  no_cc.DropCacheControl();
  no_cc.Add("ETag", "\"v1\"");
  ExpectRefused(no_cc, HtmlStoreReason::kCookieRequest);

  StoreInput pub;
  pub.request.has_cookie = true;
  pub.SetCacheControl("public, max-age=60");
  const HtmlStoreVerdict v = pub.Run();
  EXPECT_EQ(v.action, HtmlStoreAction::kStore);
  EXPECT_TRUE(v.explicit_shared);

  StoreInput smax;
  smax.request.has_cookie = true;
  smax.SetCacheControl("s-maxage=60");
  const HtmlStoreVerdict s = smax.Run();
  EXPECT_EQ(s.action, HtmlStoreAction::kStore);
  EXPECT_TRUE(s.explicit_shared);
  EXPECT_EQ(s.lifetime_s, 60u);

  // `public` without a cookie is still just explicit_shared.
  StoreInput no_cookie;
  no_cookie.SetCacheControl("public, max-age=60");
  EXPECT_TRUE(no_cookie.Run().explicit_shared);
}

TEST(HtmlCachePolicyTest, VaryAdmitsOnlyAcceptEncoding) {
  StoreInput ok;
  ok.Add("Vary", "Accept-Encoding");
  const HtmlStoreVerdict v = ok.Run();
  EXPECT_EQ(v.action, HtmlStoreAction::kStore);
  ASSERT_EQ(v.vary_tokens.size(), 1u);
  EXPECT_EQ(v.vary_tokens[0], "accept-encoding");

  StoreInput empty;
  empty.Add("Vary", "");
  ExpectStored(empty);

  StoreInput accept;
  accept.Add("Vary", "Accept");
  ExpectRefused(accept, HtmlStoreReason::kVary, "accept");

  StoreInput star;
  star.Add("Vary", "*");
  ExpectRefused(star, HtmlStoreReason::kVary, "*");

  StoreInput cookie;
  cookie.Add("Vary", "Accept-Encoding, Cookie");
  ExpectRefused(cookie, HtmlStoreReason::kVary, "cookie");

  StoreInput ua;
  ua.Add("Vary", "User-Agent");
  ExpectRefused(ua, HtmlStoreReason::kVary, "user-agent");

  // Two lines are one list: each is admissible alone, together they are not.
  StoreInput two;
  two.Add("Vary", "Accept-Encoding");
  two.Add("Vary", "Origin");
  const HtmlStoreVerdict t = two.Run();
  EXPECT_EQ(t.reason, HtmlStoreReason::kVary);
  EXPECT_EQ(t.detail, "origin");
  EXPECT_EQ(t.reason_code(), "vary:origin");
  EXPECT_EQ(t.vary_tokens.size(), 2u);
}

TEST(HtmlCachePolicyTest, UnreproducibleHeadersAreRefusedByName) {
  StoreInput powered;
  powered.Add("X-Request-Id", "abc123");
  const HtmlStoreVerdict v = powered.Run();
  EXPECT_EQ(v.reason, HtmlStoreReason::kHeaders);
  EXPECT_EQ(v.detail, "x-request-id");
  EXPECT_EQ(v.reason_code(), "headers:x-request-id");
  EXPECT_TRUE(v.sidecar_evaluated);
  EXPECT_EQ(v.sidecar.verdict, SidecarVerdict::kFallThrough);

  // Fail closed: a header whose value is chosen per response is not carried.
  StoreInput credentials;
  credentials.Add("Access-Control-Allow-Credentials", "true");
  ExpectRefused(credentials, HtmlStoreReason::kHeaders,
                "access-control-allow-credentials");

  // A static policy or resource header is carried and replayed, so it stores.
  StoreInput carried;
  carried.Add("Strict-Transport-Security", "max-age=31536000");
  carried.Add("Link", "</a.css>; rel=preload");
  carried.Add("X-Powered-By", "PHP/8");
  ExpectStored(carried);

  // A nonce in a Link is per response, like one in a CSP, and shares its code.
  StoreInput link_nonce;
  link_nonce.Add("Link", "</a.css>; rel=preload; nonce=abc");
  ExpectRefused(link_nonce, HtmlStoreReason::kNonceLink);
  EXPECT_EQ(link_nonce.Run().reason_code(), "nonce_link");
  link_nonce.Add("Content-Security-Policy", "script-src 'nonce-a'");
  ExpectRefused(link_nonce, HtmlStoreReason::kNonceCsp);
}

TEST(HtmlCachePolicyTest, DetailIsSanitisedAndBounded) {
  // The detail is token characters only: anything else becomes `_`, so the
  // reason code is always one the decision line's grammar admits.
  StoreInput odd;
  odd.Add("X-Bad\x01Name", "v");
  const HtmlStoreVerdict v = odd.Run();
  EXPECT_EQ(v.reason, HtmlStoreReason::kHeaders);
  EXPECT_EQ(v.detail, "x-bad_name");
  EXPECT_TRUE(IsHttpToken(v.detail));

  StoreInput quoted;
  quoted.Add("Vary", "\"x\"");
  ExpectRefused(quoted, HtmlStoreReason::kVary, "_x_");
  EXPECT_EQ(quoted.Run().reason_code(), "vary:_x_");
  StoreInput spaced;
  spaced.Add("Vary", "a b");
  ExpectRefused(spaced, HtmlStoreReason::kVary, "a_b");
  StoreInput high;
  high.Add("Vary", "caf\xC3\xA9");
  ExpectRefused(high, HtmlStoreReason::kVary, "caf__");

  const std::string long_name(500, 'a');
  StoreInput big;
  big.Add(long_name, "v");
  EXPECT_EQ(big.Run().detail.size(), kMaxHtmlStoreDetailBytes);
}

TEST(HtmlCachePolicyTest, NonceCspIsItsOwnReason) {
  StoreInput in;
  in.Add("Content-Security-Policy", "script-src 'nonce-abc123'");
  const HtmlStoreVerdict v = in.Run();
  EXPECT_EQ(v.action, HtmlStoreAction::kRefuse);
  EXPECT_EQ(v.reason, HtmlStoreReason::kNonceCsp);
  EXPECT_EQ(v.reason_code(), "nonce_csp");
  EXPECT_EQ(v.sidecar.verdict, SidecarVerdict::kNeverOptimized);

  // A CSP without a nonce is carried in the sidecar and the response stores.
  StoreInput plain;
  plain.Add("Content-Security-Policy", "default-src 'self'");
  const HtmlStoreVerdict p = plain.Run();
  EXPECT_EQ(p.action, HtmlStoreAction::kStore);
  EXPECT_EQ(p.sidecar.fields, 1u);
  EXPECT_FALSE(p.sidecar.payload.empty());
}

TEST(HtmlCachePolicyTest, LifetimePrecedence) {
  // s-maxage beats max-age beats Expires.
  StoreInput all;
  all.SetCacheControl("max-age=100, s-maxage=200");
  all.Add("Date", kNowDate);
  all.Add("Expires", kPlus60);
  EXPECT_EQ(all.Run().lifetime_s, 200u);

  StoreInput ma;
  ma.SetCacheControl("max-age=100");
  ma.Add("Date", kNowDate);
  ma.Add("Expires", kPlus60);
  EXPECT_EQ(ma.Run().lifetime_s, 100u);

  StoreInput ex;
  ex.DropCacheControl();
  ex.Add("Date", kNowDate);
  ex.Add("Expires", kPlus60);
  const HtmlStoreVerdict v = ex.Run();
  EXPECT_EQ(v.action, HtmlStoreAction::kStore);
  EXPECT_EQ(v.lifetime_s, 60u);
  EXPECT_TRUE(v.lifetime_explicit);
  EXPECT_FALSE(v.needs_validation);

  // s-maxage=0 wins over a positive max-age.
  StoreInput zero;
  zero.SetCacheControl("max-age=100, s-maxage=0");
  zero.Add("ETag", "\"v\"");
  const HtmlStoreVerdict z = zero.Run();
  EXPECT_EQ(z.lifetime_s, 0u);
  EXPECT_TRUE(z.needs_validation);
}

TEST(HtmlCachePolicyTest, ExpiresUsesDateOrReceiptTime) {
  // Without Date the receipt time stands in.
  StoreInput no_date;
  no_date.DropCacheControl();
  no_date.Add("Expires", kPlus60);
  EXPECT_EQ(no_date.Run().lifetime_s, 60u);

  // A Date that is itself invalid is treated as absent.
  StoreInput bad_date;
  bad_date.DropCacheControl();
  bad_date.Add("Date", "yesterday");
  bad_date.Add("Expires", kPlus60);
  EXPECT_EQ(bad_date.Run().lifetime_s, 60u);

  // Date later than receipt time: Expires is measured against Date.
  StoreInput skew;
  skew.DropCacheControl();
  skew.Add("Date", kPlus60);
  skew.Add("Expires", kPlus60);
  skew.Add("ETag", "\"v\"");
  EXPECT_EQ(skew.Run().lifetime_s, 0u);

  // Expires in the past: lifetime 0.
  StoreInput past;
  past.DropCacheControl();
  past.Add("Expires", kMinus60);
  past.Add("ETag", "\"v\"");
  const HtmlStoreVerdict v = past.Run();
  EXPECT_EQ(v.action, HtmlStoreAction::kStore);
  EXPECT_EQ(v.lifetime_s, 0u);
  EXPECT_TRUE(v.lifetime_explicit);
  EXPECT_TRUE(v.needs_validation);
}

TEST(HtmlCachePolicyTest, InvalidExpiresMeansZero) {
  for (std::string_view bad :
       {"0", "-1", "", "tomorrow", "Sunday, 06-Nov-94 08:49:37 GMT",
        "Sun Nov  6 08:49:37 1994"}) {
    StoreInput without;
    without.DropCacheControl();
    without.Add("Expires", bad);
    ExpectRefused(without, HtmlStoreReason::kNoLifetime);

    StoreInput with;
    with.DropCacheControl();
    with.Add("Expires", bad);
    with.Add("Last-Modified", kMinus60);
    const HtmlStoreVerdict v = with.Run();
    EXPECT_EQ(v.action, HtmlStoreAction::kStore) << bad;
    EXPECT_EQ(v.lifetime_s, 0u) << bad;
    EXPECT_TRUE(v.needs_validation) << bad;
  }
}

TEST(HtmlCachePolicyTest, NoCacheNeedsAValidator) {
  StoreInput bare;
  bare.SetCacheControl("no-cache");
  ExpectRefused(bare, HtmlStoreReason::kNoLifetime);

  StoreInput with_max_age;
  with_max_age.SetCacheControl("no-cache, max-age=600");
  ExpectRefused(with_max_age, HtmlStoreReason::kNoLifetime);

  StoreInput validated;
  validated.SetCacheControl("no-cache, max-age=600");
  validated.Add("ETag", "\"v1\"");
  const HtmlStoreVerdict v = validated.Run();
  EXPECT_EQ(v.action, HtmlStoreAction::kStore);
  EXPECT_TRUE(v.needs_validation);
  EXPECT_TRUE(v.has_validators);
  EXPECT_EQ(v.lifetime_s, 600u);

  // The qualified form is treated as the bare directive.
  StoreInput qualified;
  qualified.SetCacheControl("no-cache=\"Set-Cookie\", max-age=600");
  ExpectRefused(qualified, HtmlStoreReason::kNoLifetime);
  qualified.Add("Last-Modified", kMinus60);
  const HtmlStoreVerdict q = qualified.Run();
  EXPECT_EQ(q.action, HtmlStoreAction::kStore);
  EXPECT_TRUE(q.needs_validation);
}

TEST(HtmlCachePolicyTest, RequestNoStoreIsNeverStored) {
  // RFC 9111 section 5.2.1.5: a cache MUST NOT store a response to a request
  // whose Cache-Control carried no-store.
  StoreInput in;
  in.request.request_no_store = true;
  const HtmlStoreVerdict v = in.Run();
  EXPECT_EQ(v.action, HtmlStoreAction::kRefuse);
  EXPECT_EQ(v.reason, HtmlStoreReason::kNoStore);
  EXPECT_EQ(v.detail, "request");
  EXPECT_EQ(v.reason_code(), "no_store");

  // No response directive overrides it.
  StoreInput shared;
  shared.request.request_no_store = true;
  shared.SetCacheControl("public, s-maxage=600");
  ExpectRefused(shared, HtmlStoreReason::kNoStore, "request");

  // The response's own no-store keeps its empty detail.
  StoreInput own;
  own.SetCacheControl("no-store");
  ExpectRefused(own, HtmlStoreReason::kNoStore);

  // Rule order: credentials come first, a Set-Cookie response second.
  StoreInput auth;
  auth.request.request_no_store = true;
  auth.request.has_authorization = true;
  ExpectRefused(auth, HtmlStoreReason::kAuthRequest);
  StoreInput cookie;
  cookie.request.request_no_store = true;
  cookie.Add("Set-Cookie", "a=b");
  ExpectRefused(cookie, HtmlStoreReason::kNoStore, "request");

  // And a request without it is unaffected.
  ExpectStored(StoreInput());
}

TEST(HtmlCachePolicyTest, ConflictingLifetimeDirectivesCountAsStale) {
  // max-age=0 then max-age=86400, across two lines and in one line, and the
  // s-maxage twins: lifetime 0, explicit, always revalidated.
  StoreInput lines;
  lines.SetCacheControl("max-age=0");
  lines.Add("Cache-Control", "max-age=86400");
  lines.Add("ETag", "\"v1\"");
  StoreInput one;
  one.SetCacheControl("max-age=0, max-age=86400");
  one.Add("ETag", "\"v1\"");
  StoreInput smax_lines;
  smax_lines.SetCacheControl("s-maxage=0");
  smax_lines.Add("Cache-Control", "s-maxage=86400");
  smax_lines.Add("ETag", "\"v1\"");
  StoreInput smax_one;
  smax_one.SetCacheControl("s-maxage=86400, S-Maxage=0");
  smax_one.Add("ETag", "\"v1\"");
  for (const StoreInput* in : {&lines, &one, &smax_lines, &smax_one}) {
    const HtmlStoreVerdict v = in->Run();
    EXPECT_EQ(v.action, HtmlStoreAction::kStore);
    EXPECT_EQ(v.lifetime_s, 0u);
    EXPECT_TRUE(v.lifetime_explicit);
    EXPECT_TRUE(v.needs_validation);
    EXPECT_TRUE(v.has_validators);
  }
  // Without a validator they are refused.
  StoreInput bare;
  bare.SetCacheControl("max-age=0");
  bare.Add("Cache-Control", "max-age=86400");
  ExpectRefused(bare, HtmlStoreReason::kNoLifetime);
  StoreInput same;
  same.SetCacheControl("s-maxage=86400, s-maxage=86400");
  ExpectRefused(same, HtmlStoreReason::kNoLifetime);

  // One of each is not a conflict: s-maxage wins as before.
  StoreInput both;
  both.SetCacheControl("max-age=100, s-maxage=200");
  EXPECT_EQ(both.Run().lifetime_s, 200u);
}

TEST(HtmlCachePolicyTest, NoHeuristicLifetime) {
  // Last-Modified alone would earn a heuristic lifetime under RFC 9111
  // section 4.2.2; here it earns only "store, always revalidate".
  StoreInput in;
  in.DropCacheControl();
  in.Add("Last-Modified", kMinus60);
  const HtmlStoreVerdict v = in.Run();
  EXPECT_EQ(v.action, HtmlStoreAction::kStore);
  EXPECT_EQ(v.lifetime_s, 0u);
  EXPECT_FALSE(v.lifetime_explicit);
  EXPECT_TRUE(v.needs_validation);
  EXPECT_TRUE(v.has_validators);

  // Neither a lifetime nor a validator: refused.
  StoreInput none;
  none.DropCacheControl();
  ExpectRefused(none, HtmlStoreReason::kNoLifetime);

  // An empty validator is not one.
  StoreInput empty;
  empty.DropCacheControl();
  empty.Add("ETag", " ");
  ExpectRefused(empty, HtmlStoreReason::kNoLifetime);
}

TEST(HtmlCachePolicyTest, ZeroLifetimeWithValidatorStoresForRevalidation) {
  StoreInput etag;
  etag.SetCacheControl("max-age=0");
  etag.Add("ETag", "\"v1\"");
  const HtmlStoreVerdict v = etag.Run();
  EXPECT_EQ(v.action, HtmlStoreAction::kStore);
  EXPECT_EQ(v.lifetime_s, 0u);
  EXPECT_TRUE(v.lifetime_explicit);
  EXPECT_TRUE(v.needs_validation);

  StoreInput lm;
  lm.SetCacheControl("max-age=0");
  lm.Add("Last-Modified", kMinus60);
  EXPECT_EQ(lm.Run().action, HtmlStoreAction::kStore);

  StoreInput bare;
  bare.SetCacheControl("max-age=0");
  ExpectRefused(bare, HtmlStoreReason::kNoLifetime);

  // A malformed max-age is a present directive with lifetime 0.
  StoreInput bad;
  bad.SetCacheControl("max-age=soon");
  ExpectRefused(bad, HtmlStoreReason::kNoLifetime);
}

TEST(HtmlCachePolicyTest, SizeCapAndCeiling) {
  StoreInput at;
  at.policy.max_body_bytes = 1000;
  at.body_bytes = 1000;
  ExpectStored(at);
  at.body_bytes = 1001;
  ExpectRefused(at, HtmlStoreReason::kSize);

  StoreInput ceiling;
  ceiling.policy.max_body_bytes = uint64_t{64} * 1024 * 1024;
  ceiling.body_bytes = kHtmlStoreHardMaxBodyBytes;
  ExpectStored(ceiling);
  ceiling.body_bytes = kHtmlStoreHardMaxBodyBytes + 1;
  ExpectRefused(ceiling, HtmlStoreReason::kSize);

  StoreInput default_cap;
  default_cap.body_bytes = uint64_t{5} * 1024 * 1024 + 1;
  ExpectRefused(default_cap, HtmlStoreReason::kSize);
}

TEST(HtmlCachePolicyTest, EveryPersonalisationSignalIsRefused) {
  struct Case {
    std::string_view body;
    std::string_view signal;
  };
  const Case cases[] = {
      {"<meta name=\"csrf-token\" content=\"x\">", "meta_csrf_token"},
      {"<meta name=\"csrf-param\" content=\"authenticity_token\">",
       "meta_csrf_param"},
      {"<input type=\"hidden\" name=\"_token\" value=\"x\">", "hidden_token"},
      {"<input name=\"_token\" type=\"hidden\" value=\"x\">", "hidden_token"},
      {"<input type=hidden name=csrf_value value=x>", "hidden_csrf"},
      {"<input type='hidden' name='CSRF'>", "hidden_csrf"},
      {"<input type=\"hidden\" name=\"authenticity_token\" value=\"x\">",
       "hidden_authenticity"},
      {"<input type=\"hidden\" name=\"__RequestVerificationToken\">",
       "hidden_request_verification"},
      {"<input type=\"hidden\" name=\"csrfmiddlewaretoken\" value=\"x\">",
       "hidden_csrfmiddlewaretoken"},
      {"<form method=\"post\" action=\"/account/login\">", "post_form_login"},
      {"<form method=\"POST\" action=\"/SignIn\">", "post_form_login"},
      {"<form action=\"/shop/checkout\" method=\"post\">",
       "post_form_checkout"},
      {"<form method=post action=/cart/add>", "post_form_checkout"},
  };
  for (const Case& c : cases) {
    StoreInput in;
    in.body = std::string("<html><body>") + std::string(c.body);
    const HtmlStoreVerdict v = in.Run();
    EXPECT_EQ(v.reason, HtmlStoreReason::kPersonalisationSignal) << c.body;
    EXPECT_EQ(v.detail, c.signal) << c.body;
    EXPECT_EQ(v.reason_code(), "personalisation_signal");

    // Case-insensitive on tag names, attribute names and values alike.
    std::string upper(c.body);
    for (char& ch : upper) {
      if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
    }
    EXPECT_TRUE(FindPersonalisationSignal(upper).has_value()) << upper;
    EXPECT_EQ(FindPersonalisationSignal(upper).value_or("none"), c.signal)
        << upper;
  }
}

TEST(HtmlCachePolicyTest, PersonalisationSignalNamesAreTheClosedSet) {
  // The names FindPersonalisationSignal can return are exactly the published
  // set, so the decision line can validate a match it reports.
  const std::vector<std::string_view> expected = {"meta_csrf_token",
                                                  "meta_csrf_param",
                                                  "hidden_token",
                                                  "hidden_csrf",
                                                  "hidden_authenticity",
                                                  "hidden_request_verification",
                                                  "hidden_csrfmiddlewaretoken",
                                                  "hidden_wpnonce",
                                                  "post_form_login",
                                                  "post_form_checkout",
                                                  "oversized_tag"};
  const auto names = HtmlPersonalisationSignalNames();
  ASSERT_EQ(names.size(), expected.size());
  for (size_t i = 0; i < expected.size(); ++i) EXPECT_EQ(names[i], expected[i]);
}

TEST(HtmlCachePolicyTest, PersonalisationNearMissesAreNotSignals) {
  for (
      std::string_view body : {
          "<input type=\"text\" name=\"csrf\">", "<input name=\"csrf_token\">",
          "<input type=\"hidden\" name=\"page\" value=\"2\">",
          "<input type=\"hidden\" name=\"my_token\">",
          "<form method=\"get\" action=\"/login\">",
          "<form method=\"post\" action=\"/subscribe\">",
          "<form action=\"/login\">",
          "<meta name=\"viewport\" content=\"width=device-width\">",
          "<meta name=\"description\" content=\"csrf-token login\">",
          "<inputs type=\"hidden\" name=\"_token\">",
          "<p>csrf-token _token login checkout</p>",
          "<!-- <meta name=\"csrf-token\" -->",  // an unclosed tag is still a tag
      }) {
    const auto signal = FindPersonalisationSignal(body);
    if (body.find("<!--") == 0) {
      // A comment is not parsed: the markup inside it still counts, which is
      // the cheap error.
      EXPECT_TRUE(signal.has_value());
    } else {
      EXPECT_FALSE(signal.has_value()) << body << " -> " << signal.value_or("");
    }
  }
}

TEST(HtmlCachePolicyTest, PersonalisationScanIsBounded) {
  // A signal past the scan window is not seen.
  std::string body(kPersonalisationScanBytes, ' ');
  body += "<meta name=\"csrf-token\" content=\"x\">";
  EXPECT_FALSE(FindPersonalisationSignal(body).has_value());

  // One that ends exactly inside the window is.
  std::string inside(kPersonalisationScanBytes - 40, ' ');
  inside += "<meta name=\"csrf-token\" content=\"x\">";
  EXPECT_TRUE(FindPersonalisationSignal(inside).has_value());

  // Pathological input stays linear enough to finish: many unclosed tags.
  std::string hostile;
  for (int i = 0; i < 20000; ++i) hostile += "<input ";
  EXPECT_EQ(FindPersonalisationSignal(hostile), "oversized_tag");
  EXPECT_FALSE(FindPersonalisationSignal("").has_value());
  EXPECT_FALSE(FindPersonalisationSignal("<").has_value());
  EXPECT_FALSE(FindPersonalisationSignal("<meta").has_value());
}

TEST(HtmlCachePolicyTest, NoTransformStoresWithoutNotify) {
  StoreInput in;
  in.SetCacheControl("max-age=60, no-transform");
  const HtmlStoreVerdict v = in.Run();
  EXPECT_EQ(v.action, HtmlStoreAction::kStoreNoNotify);
  EXPECT_EQ(v.reason, HtmlStoreReason::kNoTransform);
  EXPECT_EQ(v.reason_code(), "no_transform");
  EXPECT_TRUE(v.stores());
}

TEST(HtmlCachePolicyTest, FirstFailingRuleIsTheReason) {
  // Fails everything at once; each fix reveals the next rule, in order.
  StoreInput in;
  in.request.method = "POST";
  in.request.url_path = "/a?b";
  in.request.has_authorization = true;
  in.request.has_cookie = true;
  in.request.request_no_store = true;
  in.status = 500;
  in.body_complete = false;
  in.body_bytes = uint64_t{100} * 1024 * 1024;
  in.body = "<meta name=\"csrf-token\">";
  in.headers = {{"Content-Type", "text/plain"},
                {"Content-Encoding", "gzip"},
                {"Set-Cookie", "a=b"},
                {"Cache-Control", "private, no-store"},
                {"Vary", "*"},
                {"X-Request-Id", "x"},
                {"Content-Security-Policy", "script-src 'nonce-a'"}};

  const auto reason = [&in] { return in.Run().reason; };
  EXPECT_EQ(reason(), HtmlStoreReason::kMethod);
  in.request.method = "GET";
  EXPECT_EQ(reason(), HtmlStoreReason::kStatus);
  in.status = 200;
  EXPECT_EQ(reason(), HtmlStoreReason::kIncomplete);
  in.body_complete = true;
  EXPECT_EQ(reason(), HtmlStoreReason::kUrl);
  in.request.url_path = "/a";
  EXPECT_EQ(reason(), HtmlStoreReason::kContentType);
  in.headers[0] = {"Content-Type", "text/html"};
  EXPECT_EQ(reason(), HtmlStoreReason::kEncoding);
  in.headers.erase(in.headers.begin() + 1);
  EXPECT_EQ(reason(), HtmlStoreReason::kAuthRequest);
  in.request.has_authorization = false;
  EXPECT_EQ(reason(), HtmlStoreReason::kNoStore);
  EXPECT_EQ(in.Run().detail, "request");
  in.request.request_no_store = false;
  EXPECT_EQ(reason(), HtmlStoreReason::kSetCookie);
  in.headers.erase(in.headers.begin() + 1);
  EXPECT_EQ(reason(), HtmlStoreReason::kPrivate);
  in.SetCacheControl("no-store");
  EXPECT_EQ(reason(), HtmlStoreReason::kNoStore);
  in.SetCacheControl("max-age=60");
  EXPECT_EQ(reason(), HtmlStoreReason::kCookieRequest);
  in.request.has_cookie = false;
  EXPECT_EQ(reason(), HtmlStoreReason::kVary);
  in.headers = {{"Content-Type", "text/html"},
                {"Cache-Control", "max-age=60"},
                {"X-Request-Id", "x"},
                {"Content-Security-Policy", "script-src 'nonce-a'"}};
  // The nonce is the decisive fact about a response, so it outranks a
  // reproducibility gap found alongside it.
  EXPECT_EQ(reason(), HtmlStoreReason::kNonceCsp);
  in.headers.pop_back();
  EXPECT_EQ(reason(), HtmlStoreReason::kHeaders);
  in.headers.pop_back();
  EXPECT_EQ(reason(), HtmlStoreReason::kSize);
  in.body_bytes = 10;
  EXPECT_EQ(reason(), HtmlStoreReason::kPersonalisationSignal);
  in.body = "<p>hi</p>";
  EXPECT_EQ(reason(), HtmlStoreReason::kNone);
}

TEST(HtmlCachePolicyTest, NoLifetimeComesAfterHeaders) {
  // A header gap and a missing lifetime: the header gap is reported.
  StoreInput in;
  in.DropCacheControl();
  in.Add("X-Request-Id", "x");
  ExpectRefused(in, HtmlStoreReason::kHeaders, "x-request-id");
}

TEST(HtmlCachePolicyTest, VerdictCarriesTheFactsACallerWrites) {
  StoreInput in;
  in.SetCacheControl("public, max-age=60, must-revalidate, no-transform");
  in.Add("Vary", "Accept-Encoding");
  in.Add("Content-Security-Policy", "default-src 'self'");
  in.Add("ETag", "\"abc\"");
  const HtmlStoreVerdict v = in.Run();
  EXPECT_EQ(v.action, HtmlStoreAction::kStoreNoNotify);
  EXPECT_TRUE(v.explicit_shared);
  EXPECT_TRUE(v.has_validators);
  EXPECT_NE(v.cc_flags & Meta::kCCOriginPublic, 0);
  EXPECT_NE(v.cc_flags & Meta::kCCOriginMustRevalidate, 0);
  EXPECT_NE(v.cc_flags & Meta::kCCOriginNoTransform, 0);
  EXPECT_NE(v.cc_flags & Meta::kCCOriginHeaderPresent, 0);
  EXPECT_EQ(v.cc_flags & Meta::kCCOriginPrivate, 0);
  ASSERT_EQ(v.vary_tokens.size(), 1u);
  EXPECT_EQ(v.sidecar.verdict, SidecarVerdict::kSwapEligible);
  EXPECT_EQ(v.sidecar.fields, 2u);  // Vary and the CSP
}

// ------------------------------------------------------------------ dates

TEST(HtmlCachePolicyTest, HttpDateParsing) {
  auto parse = [](std::string_view v) { return ParseHttpDateSeconds(v, kNow); };
  EXPECT_EQ(parse("Thu, 01 Jan 1970 00:00:00 GMT"), 0);
  EXPECT_EQ(parse(kNowDate), int64_t{kNow});
  EXPECT_EQ(parse("Thu, 29 Feb 2024 12:00:00 GMT"), int64_t{1709208000});
  EXPECT_EQ(parse("Fri, 31 Dec 1999 23:59:59 GMT"), int64_t{946684799});
  EXPECT_EQ(parse("Wed, 31 Dec 1969 23:59:59 GMT"), -1);

  // Optional whitespace around the field value is not part of it.
  EXPECT_EQ(parse("  Sun, 06 Nov 1994 08:49:37 GMT\t "), int64_t{kNow});
  EXPECT_EQ(parse(" Sun, 06 Nov 1994 08:49:37 GMT"), int64_t{kNow});
  EXPECT_FALSE(parse("   ").has_value());

  // The two obsolete forms RFC 9110 section 5.6.7 says a recipient must
  // accept: rfc850-date and asctime-date (with a space-padded or a two-digit
  // day).
  EXPECT_EQ(parse("Sunday, 06-Nov-94 08:49:37 GMT"), int64_t{kNow});
  EXPECT_EQ(parse("Sun Nov  6 08:49:37 1994"), int64_t{kNow});
  EXPECT_EQ(parse("Sun Nov 06 08:49:37 1994"), int64_t{kNow});
  EXPECT_EQ(parse("Thursday, 01-Jan-70 00:00:00 GMT"), 0);
  EXPECT_EQ(parse("Thu Jan  1 00:00:00 1970"), 0);
  EXPECT_EQ(parse("Wednesday, 29-Feb-12 12:00:00 GMT"), int64_t{1330516800});
  EXPECT_EQ(parse(" Sunday, 06-Nov-94 08:49:37 GMT "), int64_t{kNow});

  // The rfc850 two-digit year is the year within 50 years of the receipt
  // time (1994 here): 50 would be 2050, 56 years ahead, so it is 1950; 30 is
  // 2030.  Seen from 2026 the same digits resolve the other way, and 76 is
  // exactly 50 years ahead (not more), so it stays 2076 while 77 is 1977.
  EXPECT_EQ(parse("Sunday, 01-Jan-50 00:00:00 GMT"), int64_t{-631152000});
  EXPECT_EQ(parse("Tuesday, 01-Jan-30 00:00:00 GMT"), int64_t{1893456000});
  constexpr uint32_t kApril2026 = 1776000000;
  EXPECT_EQ(
      ParseHttpDateSeconds("Saturday, 01-Jan-50 00:00:00 GMT", kApril2026),
      int64_t{2524608000});
  EXPECT_EQ(
      ParseHttpDateSeconds("Wednesday, 01-Jan-76 00:00:00 GMT", kApril2026),
      int64_t{3345062400});
  EXPECT_EQ(
      ParseHttpDateSeconds("Saturday, 01-Jan-77 00:00:00 GMT", kApril2026),
      int64_t{220924800});
  // The weekday is checked against the resolved year, so the other reading
  // of the same digits is refused rather than silently shifted a century.
  EXPECT_FALSE(
      ParseHttpDateSeconds("Thursday, 01-Jan-76 00:00:00 GMT", kApril2026)
          .has_value());
  EXPECT_FALSE(parse("Saturday, 01-Jan-50 00:00:00 GMT").has_value());

  for (std::string_view bad : {
           "",
           "Sun, 06 Nov 1994 08:49:37 UTC",     // zone
           "Sun, 06 Nov 1994 08:49:37 gmt",     // case
           "Sun, 06 Nov 1994 08:49:37GMT",      // length
           "Sun,  06 Nov 1994 08:49:37 GMT",    // inner space
           "Sun, 06 Nov 1994 08:49:37 GMT x",   // trailing text
           "Mon, 06 Nov 1994 08:49:37 GMT",     // wrong weekday
           "Sun, 30 Feb 1994 08:49:37 GMT",     // no such day
           "Sun, 29 Feb 1995 08:49:37 GMT",     // not a leap year
           "Sun, 06 Foo 1994 08:49:37 GMT",     // month
           "Sux, 06 Nov 1994 08:49:37 GMT",     // weekday name
           "Sun, 06 Nov 1994 24:49:37 GMT",     // hour
           "Sun, 06 Nov 1994 08:60:37 GMT",     // minute
           "Sun, 06 Nov 1994 08:49:60 GMT",     // second
           "Sun, 00 Nov 1994 08:49:37 GMT",     // day zero
           "Sun, 0x Nov 1994 08:49:37 GMT",     // not digits
           "Sun, 06 Nov 19a4 08:49:37 GMT",     // not digits
           "Sun; 06 Nov 1994 08:49:37 GMT",     // separator
           "Sun, 06-Nov-94 08:49:37 GMT",       // rfc850 with a short day
           "Sunday, 06 Nov 1994 08:49:37 GMT",  // IMF with a long day
           "Sunday, 06-Nov-1994 08:49:37 GMT",  // rfc850 with four digits
           "Sunday, 06-Nov-94 08:49:37 UTC",    // rfc850 zone
           "Monday, 06-Nov-94 08:49:37 GMT",    // rfc850 wrong weekday
           "Sunday 06-Nov-94 08:49:37 GMT",     // rfc850 without the comma
           "Sun Nov 6 08:49:37 1994",           // asctime unpadded day
           "Sun Nov  6 08:49:37 1994 GMT",      // asctime with a zone
           "Mon Nov  6 08:49:37 1994",          // asctime wrong weekday
           "Sun Nov 6  08:49:37 1994",          // asctime padding after
           "Sun Nov  6 08:49:37 94",            // asctime two-digit year
           "Sun Nov  6 08:49:37 19a4",          // asctime not digits
       }) {
    EXPECT_FALSE(parse(bad).has_value()) << bad;
  }
}

TEST(HtmlCachePolicyTest, ExpiresAcceptsTheObsoleteDateForms) {
  for (std::string_view expires :
       {"Sunday, 06-Nov-94 08:50:37 GMT", "Sun Nov  6 08:50:37 1994"}) {
    StoreInput in;
    in.DropCacheControl();
    in.Add("Date", kNowDate);
    in.Add("Expires", expires);
    const HtmlStoreVerdict v = in.Run();
    EXPECT_EQ(v.action, HtmlStoreAction::kStore) << expires;
    EXPECT_EQ(v.lifetime_s, 60u) << expires;
    EXPECT_FALSE(v.needs_validation) << expires;
  }
  // Date in an obsolete form, Expires in the preferred one.
  StoreInput in;
  in.DropCacheControl();
  in.Add("Date", "Sunday, 06-Nov-94 08:49:37 GMT");
  in.Add("Expires", kPlus60);
  EXPECT_EQ(in.Run().lifetime_s, 60u);
}

// ------------------------------------------------------------------ names

TEST(HtmlCachePolicyTest, HttpTokenCharacters) {
  for (char c : std::string_view("abcXYZ019!#$%&'*+-.^_`|~")) {
    EXPECT_TRUE(IsHttpTokenChar(c)) << c;
  }
  for (char c : std::string_view("\"(),/:;<=>?@[\\]{} \t\x7f\x80\xff")) {
    EXPECT_FALSE(IsHttpTokenChar(c)) << static_cast<int>(c);
  }
  EXPECT_FALSE(IsHttpTokenChar('\0'));
  EXPECT_TRUE(IsHttpToken("accept-encoding"));
  EXPECT_TRUE(IsHttpToken("*"));
  EXPECT_FALSE(IsHttpToken(""));
  EXPECT_FALSE(IsHttpToken("a b"));
  EXPECT_FALSE(IsHttpToken("\"x\""));
}

TEST(HtmlCachePolicyTest, NamesAreAClosedSet) {
  const char* const store_reasons[] = {"none",
                                       "method",
                                       "status",
                                       "incomplete",
                                       "auth_request",
                                       "cookie_request",
                                       "set_cookie",
                                       "private",
                                       "no_store",
                                       "no_lifetime",
                                       "vary",
                                       "headers",
                                       "nonce_csp",
                                       "nonce_link",
                                       "content_type",
                                       "encoding",
                                       "size",
                                       "url",
                                       "personalisation_signal",
                                       "no_transform",
                                       "mode"};
  ASSERT_EQ(sizeof(store_reasons) / sizeof(store_reasons[0]),
            size_t{kHtmlStoreReasonCount});
  for (uint8_t i = 0; i < kHtmlStoreReasonCount; ++i) {
    EXPECT_EQ(HtmlStoreReasonName(static_cast<HtmlStoreReason>(i)),
              store_reasons[i]);
  }
  EXPECT_EQ(
      HtmlStoreReasonName(static_cast<HtmlStoreReason>(kHtmlStoreReasonCount)),
      "unknown");

  const char* const serve_reasons[] = {"fresh",
                                       "mode",
                                       "auth_request",
                                       "cookie_request",
                                       "client_no_cache",
                                       "needs_validation",
                                       "stale",
                                       "headers_not_reproducible",
                                       "sidecar_missing",
                                       "purged",
                                       "absent",
                                       "binding_stale",
                                       "parent_stale",
                                       "method",
                                       "range",
                                       "cors",
                                       "no_transform",
                                       "withheld",
                                       "skew"};
  ASSERT_EQ(sizeof(serve_reasons) / sizeof(serve_reasons[0]),
            size_t{kHtmlServeReasonCount});
  for (uint8_t i = 0; i < kHtmlServeReasonCount; ++i) {
    EXPECT_EQ(HtmlServeReasonName(static_cast<HtmlServeReason>(i)),
              serve_reasons[i]);
  }
  EXPECT_EQ(HtmlServeReasonName(static_cast<HtmlServeReason>(200)), "unknown");

  EXPECT_EQ(HtmlStoreActionName(HtmlStoreAction::kRefuse), "refuse");
  EXPECT_EQ(HtmlStoreActionName(HtmlStoreAction::kStore), "store");
  EXPECT_EQ(HtmlStoreActionName(HtmlStoreAction::kStoreNoNotify),
            "store_no_notify");
  EXPECT_EQ(HtmlStoreActionName(static_cast<HtmlStoreAction>(9)), "unknown");
  EXPECT_EQ(HtmlServeActionName(HtmlServeAction::kFallThrough), "fall_through");
  EXPECT_EQ(HtmlServeActionName(HtmlServeAction::kServeHtml), "serve_html");
  EXPECT_EQ(HtmlServeActionName(HtmlServeAction::kServeMarkdown),
            "serve_markdown");
  EXPECT_EQ(HtmlServeActionName(HtmlServeAction::kValidate), "validate");
  EXPECT_EQ(HtmlServeActionName(static_cast<HtmlServeAction>(9)), "unknown");
  EXPECT_EQ(HtmlCacheModeName(HtmlCacheMode::kOff), "off");
  EXPECT_EQ(HtmlCacheModeName(HtmlCacheMode::kReportOnly), "report-only");
  EXPECT_EQ(HtmlCacheModeName(HtmlCacheMode::kServeVariants), "serve-variants");
  EXPECT_EQ(HtmlCacheModeName(HtmlCacheMode::kServeCachedHtml),
            "serve-cached-html");
  EXPECT_EQ(HtmlCacheModeName(static_cast<HtmlCacheMode>(9)), "unknown");
}

// ------------------------------------------------------------ serve rules

constexpr HtmlCacheMode kAllModes[] = {
    HtmlCacheMode::kOff, HtmlCacheMode::kReportOnly,
    HtmlCacheMode::kServeVariants, HtmlCacheMode::kServeCachedHtml};

constexpr uint32_t kStoredAt = 1000;

HtmlServeRequest Req(HtmlCacheMode mode, uint32_t age = 10) {
  HtmlServeRequest r;
  r.mode = mode;
  r.now_seconds = kStoredAt + age;
  return r;
}

StoredHtmlFacts Fresh() {
  StoredHtmlFacts f;
  f.present = true;
  f.stored_at = kStoredAt;
  f.lifetime_s = 100;
  f.variant_present = true;
  f.binding_live = true;
  return f;
}

bool Enforcing(HtmlCacheMode mode) {
  return mode == HtmlCacheMode::kServeVariants ||
         mode == HtmlCacheMode::kServeCachedHtml;
}

void ExpectServe(const HtmlServeRequest& r, const StoredHtmlFacts& f,
                 HtmlServeAction action, HtmlServeReason reason) {
  const HtmlServeVerdict v = ClassifyHtmlForServe(r, f);
  EXPECT_EQ(HtmlServeActionName(v.action), HtmlServeActionName(action))
      << "mode " << HtmlCacheModeName(r.mode);
  EXPECT_EQ(HtmlServeReasonName(v.reason), HtmlServeReasonName(reason))
      << "mode " << HtmlCacheModeName(r.mode);
  EXPECT_EQ(v.enforce, Enforcing(r.mode));
}

// A rule that sends every non-off mode to the origin for one reason.
void ExpectFallThroughInEveryMode(const HtmlServeRequest& base,
                                  const StoredHtmlFacts& f,
                                  HtmlServeReason reason) {
  for (HtmlCacheMode mode : kAllModes) {
    HtmlServeRequest r = base;
    r.mode = mode;
    if (mode == HtmlCacheMode::kOff || mode == HtmlCacheMode::kReportOnly) {
      ExpectServe(r, f, HtmlServeAction::kFallThrough, HtmlServeReason::kMode);
    } else {
      ExpectServe(r, f, HtmlServeAction::kFallThrough, reason);
    }
  }
}

TEST(HtmlCachePolicyTest, ServeOffNeverServes) {
  ExpectServe(Req(HtmlCacheMode::kOff), Fresh(), HtmlServeAction::kFallThrough,
              HtmlServeReason::kMode);
  // Not even an absent entry changes the reason in kOff.
  StoredHtmlFacts absent;
  ExpectServe(Req(HtmlCacheMode::kOff), absent, HtmlServeAction::kFallThrough,
              HtmlServeReason::kMode);
}

TEST(HtmlCachePolicyTest, ServeFreshHtmlByMode) {
  ExpectServe(Req(HtmlCacheMode::kReportOnly), Fresh(),
              HtmlServeAction::kFallThrough, HtmlServeReason::kMode);
  ExpectServe(Req(HtmlCacheMode::kServeVariants), Fresh(),
              HtmlServeAction::kFallThrough, HtmlServeReason::kMode);
  ExpectServe(Req(HtmlCacheMode::kServeCachedHtml), Fresh(),
              HtmlServeAction::kServeHtml, HtmlServeReason::kFresh);
}

TEST(HtmlCachePolicyTest, ServeAbsentEntry) {
  StoredHtmlFacts f;  // present = false
  ExpectFallThroughInEveryMode(Req(HtmlCacheMode::kOff), f,
                               HtmlServeReason::kAbsent);
}

TEST(HtmlCachePolicyTest, ServeCredentialsFallThrough) {
  HtmlServeRequest a = Req(HtmlCacheMode::kOff);
  a.has_authorization = true;
  ExpectFallThroughInEveryMode(a, Fresh(), HtmlServeReason::kAuthRequest);
  HtmlServeRequest u = Req(HtmlCacheMode::kOff);
  u.authenticated_user = true;
  ExpectFallThroughInEveryMode(u, Fresh(), HtmlServeReason::kAuthRequest);
}

TEST(HtmlCachePolicyTest, ServeCookieNeedsAnExplicitlySharedEntry) {
  HtmlServeRequest r = Req(HtmlCacheMode::kOff);
  r.has_cookie = true;
  ExpectFallThroughInEveryMode(r, Fresh(), HtmlServeReason::kCookieRequest);

  StoredHtmlFacts shared = Fresh();
  shared.explicit_shared = true;
  ExpectServe(Req(HtmlCacheMode::kServeCachedHtml), shared,
              HtmlServeAction::kServeHtml, HtmlServeReason::kFresh);
  r.mode = HtmlCacheMode::kServeCachedHtml;
  ExpectServe(r, shared, HtmlServeAction::kServeHtml, HtmlServeReason::kFresh);
}

TEST(HtmlCachePolicyTest, ServeClientNoCache) {
  HtmlServeRequest cc = Req(HtmlCacheMode::kOff);
  cc.has_cache_control = true;
  cc.cache_control_no_cache = true;
  ExpectFallThroughInEveryMode(cc, Fresh(), HtmlServeReason::kClientNoCache);

  // Pragma counts only when the request carries no Cache-Control at all.
  HtmlServeRequest pragma = Req(HtmlCacheMode::kOff);
  pragma.pragma_no_cache = true;
  ExpectFallThroughInEveryMode(pragma, Fresh(),
                               HtmlServeReason::kClientNoCache);

  HtmlServeRequest pragma_with_cc = Req(HtmlCacheMode::kServeCachedHtml);
  pragma_with_cc.pragma_no_cache = true;
  pragma_with_cc.has_cache_control = true;  // and Cache-Control says nothing
  ExpectServe(pragma_with_cc, Fresh(), HtmlServeAction::kServeHtml,
              HtmlServeReason::kFresh);
}

TEST(HtmlCachePolicyTest, ServePurgedAndReplayFacts) {
  StoredHtmlFacts purged = Fresh();
  purged.purged = true;
  ExpectFallThroughInEveryMode(Req(HtmlCacheMode::kOff), purged,
                               HtmlServeReason::kPurged);

  StoredHtmlFacts not_reproducible = Fresh();
  not_reproducible.headers_not_reproducible = true;
  ExpectFallThroughInEveryMode(Req(HtmlCacheMode::kOff), not_reproducible,
                               HtmlServeReason::kHeadersNotReproducible);

  StoredHtmlFacts missing = Fresh();
  missing.sidecar_required = true;
  ExpectFallThroughInEveryMode(Req(HtmlCacheMode::kOff), missing,
                               HtmlServeReason::kSidecarMissing);

  StoredHtmlFacts present = Fresh();
  present.sidecar_required = true;
  present.sidecar_present = true;
  ExpectServe(Req(HtmlCacheMode::kServeCachedHtml), present,
              HtmlServeAction::kServeHtml, HtmlServeReason::kFresh);

  // A sidecar that is present but not required changes nothing.
  StoredHtmlFacts unneeded = Fresh();
  unneeded.sidecar_present = true;
  ExpectServe(Req(HtmlCacheMode::kServeCachedHtml), unneeded,
              HtmlServeAction::kServeHtml, HtmlServeReason::kFresh);
}

TEST(HtmlCachePolicyTest, ServeRuleOrder) {
  StoredHtmlFacts f = Fresh();
  f.purged = true;
  f.headers_not_reproducible = true;
  f.sidecar_required = true;
  f.needs_validation = true;
  HtmlServeRequest r = Req(HtmlCacheMode::kServeCachedHtml);
  r.has_authorization = true;
  r.has_cookie = true;
  r.cache_control_no_cache = true;
  const auto reason = [&] { return ClassifyHtmlForServe(r, f).reason; };
  EXPECT_EQ(reason(), HtmlServeReason::kAuthRequest);
  r.has_authorization = false;
  EXPECT_EQ(reason(), HtmlServeReason::kCookieRequest);
  r.has_cookie = false;
  EXPECT_EQ(reason(), HtmlServeReason::kClientNoCache);
  r.cache_control_no_cache = false;
  EXPECT_EQ(reason(), HtmlServeReason::kPurged);
  f.purged = false;
  EXPECT_EQ(reason(), HtmlServeReason::kHeadersNotReproducible);
  f.headers_not_reproducible = false;
  EXPECT_EQ(reason(), HtmlServeReason::kSidecarMissing);
  f.sidecar_required = false;
  EXPECT_EQ(reason(), HtmlServeReason::kNeedsValidation);
  f.needs_validation = false;
  EXPECT_EQ(reason(), HtmlServeReason::kFresh);
}

TEST(HtmlCachePolicyTest, ServeStaleIsValidatedOrFallsThrough) {
  // Age 100 against lifetime 100: expired.
  const HtmlServeRequest base = Req(HtmlCacheMode::kOff, 100);
  for (HtmlCacheMode mode : kAllModes) {
    HtmlServeRequest r = base;
    r.mode = mode;
    const HtmlServeVerdict v = ClassifyHtmlForServe(r, Fresh());
    EXPECT_EQ(v.enforce, Enforcing(mode));
    switch (mode) {
      case HtmlCacheMode::kOff:
      case HtmlCacheMode::kReportOnly:
        EXPECT_EQ(v.action, HtmlServeAction::kFallThrough);
        EXPECT_EQ(v.reason, HtmlServeReason::kMode);
        break;
      case HtmlCacheMode::kServeCachedHtml:
        EXPECT_EQ(v.action, HtmlServeAction::kValidate);
        EXPECT_EQ(v.reason, HtmlServeReason::kStale);
        break;
      case HtmlCacheMode::kServeVariants:
        EXPECT_EQ(v.action, HtmlServeAction::kFallThrough);
        EXPECT_EQ(v.reason, HtmlServeReason::kStale);
        break;
    }
  }
  // One second short of expiry is still fresh.
  ExpectServe(Req(HtmlCacheMode::kServeCachedHtml, 99), Fresh(),
              HtmlServeAction::kServeHtml, HtmlServeReason::kFresh);
}

TEST(HtmlCachePolicyTest, ServeNeverServesStaleUnderAnyDirective) {
  // stale-while-revalidate, stale-if-error, must-revalidate,
  // proxy-revalidate, public and immutable do not permit stale use here:
  // the stored lifetime is the max-age and nothing else is consulted.  The
  // facts come through the store side, as a front end's would.
  StoreInput in;
  in.SetCacheControl(
      "public, max-age=60, stale-while-revalidate=600, stale-if-error=86400, "
      "must-revalidate, proxy-revalidate, immutable");
  const HtmlStoreVerdict stored = in.Run();
  ASSERT_TRUE(stored.stores()) << stored.reason_code();
  EXPECT_EQ(stored.lifetime_s, 60u);
  EXPECT_FALSE(stored.needs_validation);
  StoredHtmlFacts f = Fresh();
  f.lifetime_s = stored.lifetime_s;
  f.needs_validation = stored.needs_validation;
  f.explicit_shared = stored.explicit_shared;
  for (HtmlCacheMode mode : kAllModes) {
    for (uint32_t age : {60u, 61u, 660u, 86460u}) {
      for (bool markdown : {false, true}) {
        HtmlServeRequest r = Req(mode, age);
        r.wants_markdown = markdown;
        const HtmlServeVerdict v = ClassifyHtmlForServe(r, f);
        EXPECT_NE(v.action, HtmlServeAction::kServeHtml)
            << HtmlCacheModeName(mode) << " age " << age;
        EXPECT_NE(v.action, HtmlServeAction::kServeMarkdown)
            << HtmlCacheModeName(mode) << " age " << age;
      }
    }
  }
  // One second inside the lifetime it is served where the mode allows.
  ExpectServe(Req(HtmlCacheMode::kServeCachedHtml, 59), f,
              HtmlServeAction::kServeHtml, HtmlServeReason::kFresh);
}

TEST(HtmlCachePolicyTest, ServeNeedsValidation) {
  StoredHtmlFacts f = Fresh();
  f.needs_validation = true;
  ExpectServe(Req(HtmlCacheMode::kServeCachedHtml), f,
              HtmlServeAction::kValidate, HtmlServeReason::kNeedsValidation);
  ExpectServe(Req(HtmlCacheMode::kServeVariants), f,
              HtmlServeAction::kFallThrough, HtmlServeReason::kNeedsValidation);

  // Lifetime 0 is the same condition even without the flag.
  StoredHtmlFacts zero = Fresh();
  zero.lifetime_s = 0;
  ExpectServe(Req(HtmlCacheMode::kServeCachedHtml, 0), zero,
              HtmlServeAction::kValidate, HtmlServeReason::kNeedsValidation);

  // Confirmed by a 304 just now: served where there is a validation step.
  // serve-variants has none, so there the flag is not consulted and the
  // entry is still the one that needs validation.
  f.validated_this_request = true;
  ExpectServe(Req(HtmlCacheMode::kServeCachedHtml), f,
              HtmlServeAction::kServeHtml, HtmlServeReason::kFresh);
  ExpectServe(Req(HtmlCacheMode::kReportOnly), f, HtmlServeAction::kFallThrough,
              HtmlServeReason::kMode);
  ExpectServe(Req(HtmlCacheMode::kServeVariants), f,
              HtmlServeAction::kFallThrough, HtmlServeReason::kNeedsValidation);

  StoredHtmlFacts expired = Fresh();
  expired.validated_this_request = true;
  ExpectServe(Req(HtmlCacheMode::kServeCachedHtml, 9999), expired,
              HtmlServeAction::kServeHtml, HtmlServeReason::kFresh);
}

TEST(HtmlCachePolicyTest, ServeHonoursRequestMaxAgeAndMinFresh) {
  HtmlServeRequest r = Req(HtmlCacheMode::kServeCachedHtml, 30);
  r.max_age = 29;
  ExpectServe(r, Fresh(), HtmlServeAction::kValidate, HtmlServeReason::kStale);
  r.max_age = 30;
  ExpectServe(r, Fresh(), HtmlServeAction::kServeHtml, HtmlServeReason::kFresh);
  r.max_age = 0;
  ExpectServe(r, Fresh(), HtmlServeAction::kValidate, HtmlServeReason::kStale);

  // RFC 9111 section 5.2.1.3: the entry must still be fresh min-fresh
  // seconds from now, so age + min-fresh may equal the lifetime but not
  // exceed it.
  HtmlServeRequest m = Req(HtmlCacheMode::kServeCachedHtml, 30);
  m.min_fresh = 69;  // 30 + 69 < 100
  ExpectServe(m, Fresh(), HtmlServeAction::kServeHtml, HtmlServeReason::kFresh);
  m.min_fresh = 70;  // 30 + 70 == 100: still fresh for exactly that long
  ExpectServe(m, Fresh(), HtmlServeAction::kServeHtml, HtmlServeReason::kFresh);
  m.min_fresh = 71;  // 30 + 71 > 100: the entry cannot meet it
  ExpectServe(m, Fresh(), HtmlServeAction::kValidate, HtmlServeReason::kStale);

  // max-stale is never honoured: there is no field for it, and an expired
  // entry stays expired (see ServeStaleIsValidatedOrFallsThrough).
}

TEST(HtmlCachePolicyTest, ServeMinFreshCannotOverflow) {
  // "Fresh for at least forever" must never be served, and the sum age +
  // min_fresh must not wrap.
  for (int64_t huge : {INT64_MAX, INT64_MAX - 5, int64_t{1} << 62,
                       int64_t{UINT32_MAX}, int64_t{UINT32_MAX} + 1}) {
    HtmlServeRequest r = Req(HtmlCacheMode::kServeCachedHtml, 30);
    r.min_fresh = huge;
    ExpectServe(r, Fresh(), HtmlServeAction::kValidate,
                HtmlServeReason::kStale);
    r.mode = HtmlCacheMode::kServeVariants;
    ExpectServe(r, Fresh(), HtmlServeAction::kFallThrough,
                HtmlServeReason::kStale);
    // A huge max-age is simply satisfied.
    HtmlServeRequest m = Req(HtmlCacheMode::kServeCachedHtml, 30);
    m.max_age = huge;
    ExpectServe(m, Fresh(), HtmlServeAction::kServeHtml,
                HtmlServeReason::kFresh);
  }
  // Boundary: min_fresh == lifetime - age is enough, one more is not, and
  // min_fresh == lifetime is met only by an entry of age 0.
  HtmlServeRequest edge = Req(HtmlCacheMode::kServeCachedHtml, 30);
  edge.min_fresh = 71;
  ExpectServe(edge, Fresh(), HtmlServeAction::kValidate,
              HtmlServeReason::kStale);
  edge.min_fresh = 70;
  ExpectServe(edge, Fresh(), HtmlServeAction::kServeHtml,
              HtmlServeReason::kFresh);
  HtmlServeRequest whole = Req(HtmlCacheMode::kServeCachedHtml, 0);
  whole.min_fresh = 100;
  ExpectServe(whole, Fresh(), HtmlServeAction::kServeHtml,
              HtmlServeReason::kFresh);
  whole.min_fresh = 101;
  ExpectServe(whole, Fresh(), HtmlServeAction::kValidate,
              HtmlServeReason::kStale);
}

TEST(HtmlCachePolicyTest, ServeAgeArithmetic) {
  StoredHtmlFacts f = Fresh();
  f.age_at_store = 40;
  HtmlServeVerdict v =
      ClassifyHtmlForServe(Req(HtmlCacheMode::kServeCachedHtml, 25), f);
  EXPECT_EQ(v.age_s, 65u);
  EXPECT_EQ(v.action, HtmlServeAction::kServeHtml);

  // The origin's Age counts against the lifetime.
  v = ClassifyHtmlForServe(Req(HtmlCacheMode::kServeCachedHtml, 60), f);
  EXPECT_EQ(v.age_s, 100u);
  EXPECT_EQ(v.action, HtmlServeAction::kValidate);

  // A clock that stepped back a little is not negative age.
  HtmlServeRequest back = Req(HtmlCacheMode::kServeCachedHtml);
  back.now_seconds = kStoredAt - kHtmlServeSkewSeconds;
  v = ClassifyHtmlForServe(back, Fresh());
  EXPECT_EQ(v.age_s, 0u);
  EXPECT_EQ(v.action, HtmlServeAction::kServeHtml);
  v = ClassifyHtmlForServe(back, f);
  EXPECT_EQ(v.age_s, 40u);
  // Further than the skew bound, the entry's age cannot be known and it is
  // not served.
  back.now_seconds = kStoredAt - 500;
  v = ClassifyHtmlForServe(back, Fresh());
  EXPECT_EQ(v.action, HtmlServeAction::kFallThrough);
  EXPECT_EQ(v.reason, HtmlServeReason::kSkew);

  // Saturates rather than wrapping.
  StoredHtmlFacts huge = Fresh();
  huge.age_at_store = UINT32_MAX;
  v = ClassifyHtmlForServe(Req(HtmlCacheMode::kServeCachedHtml, 500), huge);
  EXPECT_EQ(v.age_s, UINT32_MAX);
  EXPECT_EQ(v.action, HtmlServeAction::kValidate);
}

HtmlServeRequest MarkdownReq(HtmlCacheMode mode, uint32_t age = 10) {
  HtmlServeRequest r = Req(mode, age);
  r.wants_markdown = true;
  return r;
}

TEST(HtmlCachePolicyTest, ServeMarkdown) {
  for (HtmlCacheMode mode :
       {HtmlCacheMode::kServeVariants, HtmlCacheMode::kServeCachedHtml}) {
    ExpectServe(MarkdownReq(mode), Fresh(), HtmlServeAction::kServeMarkdown,
                HtmlServeReason::kFresh);

    StoredHtmlFacts no_copy = Fresh();
    no_copy.variant_present = false;
    ExpectServe(MarkdownReq(mode), no_copy, HtmlServeAction::kFallThrough,
                HtmlServeReason::kAbsent);

    StoredHtmlFacts stale_binding = Fresh();
    stale_binding.binding_live = false;
    ExpectServe(MarkdownReq(mode), stale_binding, HtmlServeAction::kFallThrough,
                HtmlServeReason::kBindingStale);

    // The copy's own lifetime is shorter than the parent's.
    StoredHtmlFacts short_ttl = Fresh();
    short_ttl.render_ttl_s = 20;
    ExpectServe(MarkdownReq(mode, 19), short_ttl,
                HtmlServeAction::kServeMarkdown, HtmlServeReason::kFresh);
    ExpectServe(MarkdownReq(mode, 20), short_ttl, HtmlServeAction::kFallThrough,
                HtmlServeReason::kParentStale);
  }
  ExpectServe(MarkdownReq(HtmlCacheMode::kOff), Fresh(),
              HtmlServeAction::kFallThrough, HtmlServeReason::kMode);
}

TEST(HtmlCachePolicyTest, ServeMarkdownNeedsAFreshParent) {
  // Parent expired: the Markdown outcome is parent_stale, validated first
  // where there is a validation step (kServeCachedHtml), a
  // fall-through in kServeVariants.  Never the HTML request's stale reason.
  ExpectServe(MarkdownReq(HtmlCacheMode::kServeCachedHtml, 200), Fresh(),
              HtmlServeAction::kValidate, HtmlServeReason::kParentStale);
  ExpectServe(MarkdownReq(HtmlCacheMode::kServeVariants, 200), Fresh(),
              HtmlServeAction::kFallThrough, HtmlServeReason::kParentStale);

  // A parent that needs validation is the same outcome.
  StoredHtmlFacts needs = Fresh();
  needs.needs_validation = true;
  ExpectServe(MarkdownReq(HtmlCacheMode::kServeCachedHtml), needs,
              HtmlServeAction::kValidate, HtmlServeReason::kParentStale);
  ExpectServe(MarkdownReq(HtmlCacheMode::kServeVariants), needs,
              HtmlServeAction::kFallThrough, HtmlServeReason::kParentStale);

  // So is a request max-age or min-fresh the parent cannot meet.
  HtmlServeRequest strict = MarkdownReq(HtmlCacheMode::kServeCachedHtml, 30);
  strict.max_age = 10;
  ExpectServe(strict, Fresh(), HtmlServeAction::kValidate,
              HtmlServeReason::kParentStale);
  strict.max_age = -1;
  strict.min_fresh = 80;
  ExpectServe(strict, Fresh(), HtmlServeAction::kValidate,
              HtmlServeReason::kParentStale);

  // Confirmed this request: the copy is served where there is a validation
  // step, bounded by its own lifetime.  kServeVariants has no such step, so
  // the flag is not consulted there and the parent is as fresh as its
  // stored lifetime says.
  StoredHtmlFacts validated = Fresh();
  validated.needs_validation = true;
  validated.validated_this_request = true;
  ExpectServe(MarkdownReq(HtmlCacheMode::kServeCachedHtml, 9999), validated,
              HtmlServeAction::kServeMarkdown, HtmlServeReason::kFresh);
  ExpectServe(MarkdownReq(HtmlCacheMode::kServeVariants, 9999), validated,
              HtmlServeAction::kFallThrough, HtmlServeReason::kParentStale);
  ExpectServe(MarkdownReq(HtmlCacheMode::kServeVariants, 10), validated,
              HtmlServeAction::kFallThrough, HtmlServeReason::kParentStale);
  StoredHtmlFacts confirmed_fresh = Fresh();
  confirmed_fresh.validated_this_request = true;
  ExpectServe(MarkdownReq(HtmlCacheMode::kServeVariants, 10), confirmed_fresh,
              HtmlServeAction::kServeMarkdown, HtmlServeReason::kFresh);
  ExpectServe(MarkdownReq(HtmlCacheMode::kServeVariants, 100), confirmed_fresh,
              HtmlServeAction::kFallThrough, HtmlServeReason::kParentStale);

  // The render bound still applies after validation: the copy is served
  // only while age < render_ttl_s.
  validated.render_ttl_s = 10000;
  ExpectServe(MarkdownReq(HtmlCacheMode::kServeCachedHtml, 9999), validated,
              HtmlServeAction::kServeMarkdown, HtmlServeReason::kFresh);
  validated.render_ttl_s = 9999;  // age == render_ttl_s
  ExpectServe(MarkdownReq(HtmlCacheMode::kServeCachedHtml, 9999), validated,
              HtmlServeAction::kFallThrough, HtmlServeReason::kParentStale);
  validated.render_ttl_s = 100;  // age past render_ttl_s
  ExpectServe(MarkdownReq(HtmlCacheMode::kServeCachedHtml, 9999), validated,
              HtmlServeAction::kFallThrough, HtmlServeReason::kParentStale);
  validated.render_ttl_s = 0;
  ExpectServe(MarkdownReq(HtmlCacheMode::kServeCachedHtml, 9999), validated,
              HtmlServeAction::kFallThrough, HtmlServeReason::kParentStale);
}

TEST(HtmlCachePolicyTest, ServeHtmlOnAConfirmedEntryBySide) {
  // An HTML request on an expired entry the origin confirmed just now: served
  // in kServeCachedHtml, and in
  // kServeVariants (no validation step) a plain stale fall-through.
  StoredHtmlFacts confirmed = Fresh();
  confirmed.validated_this_request = true;
  ExpectServe(Req(HtmlCacheMode::kServeCachedHtml, 500), confirmed,
              HtmlServeAction::kServeHtml, HtmlServeReason::kFresh);
  ExpectServe(Req(HtmlCacheMode::kServeVariants, 500), confirmed,
              HtmlServeAction::kFallThrough, HtmlServeReason::kStale);
}

TEST(HtmlCachePolicyTest, ServeMarkdownNeverDegradesToHtml) {
  // Whatever the Markdown outcome, this function never answers a Markdown
  // request with kServeHtml.
  for (HtmlCacheMode mode : kAllModes) {
    for (int bits = 0; bits < 8; ++bits) {
      StoredHtmlFacts f = Fresh();
      f.variant_present = (bits & 1) != 0;
      f.binding_live = (bits & 2) != 0;
      f.render_ttl_s = (bits & 4) != 0 ? 5 : 86400;
      const HtmlServeVerdict v = ClassifyHtmlForServe(MarkdownReq(mode, 10), f);
      EXPECT_NE(v.action, HtmlServeAction::kServeHtml);
    }
  }
}

TEST(HtmlCachePolicyTest, ReportOnlyNeverEnforces) {
  int checked = 0;
  for (int bits = 0; bits < 1024; ++bits) {
    HtmlServeRequest r =
        Req(HtmlCacheMode::kReportOnly, (bits & 1) != 0 ? 500 : 10);
    r.wants_markdown = (bits & 2) != 0;
    r.has_authorization = (bits & 4) != 0;
    r.has_cookie = (bits & 8) != 0;
    r.cache_control_no_cache = (bits & 16) != 0;
    StoredHtmlFacts f = Fresh();
    f.present = (bits & 32) == 0;
    f.purged = (bits & 64) != 0;
    f.needs_validation = (bits & 128) != 0;
    f.validated_this_request = (bits & 256) != 0;
    f.variant_present = (bits & 512) == 0;
    const HtmlServeVerdict v = ClassifyHtmlForServe(r, f);
    EXPECT_FALSE(v.enforce) << bits;
    r.mode = HtmlCacheMode::kOff;
    EXPECT_FALSE(ClassifyHtmlForServe(r, f).enforce) << bits;
    ++checked;
  }
  EXPECT_EQ(checked, 1024);
}

TEST(HtmlCachePolicyTest, ReportOnlyReturnsNoServeAction) {
  for (int bits = 0; bits < 64; ++bits) {
    HtmlServeRequest r = Req(HtmlCacheMode::kReportOnly, (bits & 1) ? 500 : 10);
    r.wants_markdown = (bits & 2) != 0;
    r.has_cookie = (bits & 4) != 0;
    StoredHtmlFacts f = Fresh();
    f.needs_validation = (bits & 8) != 0;
    f.binding_live = (bits & 16) == 0;
    f.validated_this_request = (bits & 32) != 0;
    const HtmlServeVerdict v = ClassifyHtmlForServe(r, f);
    EXPECT_EQ(v.action, HtmlServeAction::kFallThrough) << bits;
    EXPECT_EQ(v.reason, HtmlServeReason::kMode) << bits;
    EXPECT_FALSE(v.enforce) << bits;
  }
}

TEST(HtmlCachePolicyTest, StoreModeOffRefusesEverything) {
  StoreInput in;
  in.policy.mode = HtmlCacheMode::kOff;
  ExpectRefused(in, HtmlStoreReason::kMode);
  // The default policy is off, so a caller that forgets the mode stores nothing.
  EXPECT_EQ(HtmlStorePolicy{}.mode, HtmlCacheMode::kOff);
}

TEST(HtmlCachePolicyTest, StoreReportOnlyNeverReturnsAStoringAction) {
  StoreInput in;
  in.policy.mode = HtmlCacheMode::kReportOnly;
  HtmlStoreVerdict v = in.Run();
  EXPECT_EQ(v.action, HtmlStoreAction::kWouldStore);
  EXPECT_EQ(v.reason, HtmlStoreReason::kNone);
  EXPECT_FALSE(v.stores());

  in.SetCacheControl("max-age=60, no-transform");
  v = in.Run();
  EXPECT_EQ(v.action, HtmlStoreAction::kWouldStoreNoNotify);
  EXPECT_EQ(v.reason, HtmlStoreReason::kNoTransform);
  EXPECT_FALSE(v.stores());

  in.SetCacheControl("private, max-age=60");
  ExpectRefused(in, HtmlStoreReason::kPrivate);
}

TEST(HtmlCachePolicyTest, TagOpenAtTheBoundIsASignal) {
  const std::string attr = "<input data-x=\"" + std::string(5000, 'a');
  EXPECT_EQ(FindPersonalisationSignal(attr + "\" type=hidden name=_token>"),
            "oversized_tag");
  // The same for the other tags of interest, and for no quote at all.
  EXPECT_EQ(FindPersonalisationSignal("<form " + std::string(5000, 'a')),
            "oversized_tag");
  EXPECT_EQ(FindPersonalisationSignal("<meta " + std::string(5000, 'a')),
            "oversized_tag");
  // A long tag that closes within the bound is read normally.
  const std::string fits = "<input data-x=\"" + std::string(3000, 'a') +
                           "\" type=hidden name=_token>";
  EXPECT_EQ(FindPersonalisationSignal(fits), "hidden_token");
  const std::string benign =
      "<input data-x=\"" + std::string(3000, 'a') + "\" type=text>";
  EXPECT_FALSE(FindPersonalisationSignal(benign).has_value());
  // A tag that closes on its last permitted byte was read whole.
  std::string exact = "<input data-x=\"";
  exact += std::string(4096 - exact.size() - 2, 'a');
  exact += "\">";
  ASSERT_EQ(exact.size(), 4096u);
  EXPECT_FALSE(FindPersonalisationSignal(exact).has_value());
  // Other long tags are not of interest.
  EXPECT_FALSE(
      FindPersonalisationSignal("<div " + std::string(5000, 'a')).has_value());

  StoreInput in;
  in.body = attr + "\" type=hidden name=_token>";
  in.body_bytes = in.body.size();
  ExpectRefused(in, HtmlStoreReason::kPersonalisationSignal, "oversized_tag");
}

TEST(HtmlCachePolicyTest, TagOpenAtTheWindowEdgeOfALongerBodyIsASignal) {
  const std::string edge = std::string(kPersonalisationScanBytes - 100, ' ') +
                           "<input data-x=\"" + std::string(85, 'a');
  ASSERT_EQ(edge.size(), kPersonalisationScanBytes);

  // The prefix stops at the window and the body goes on.
  StoreInput cut;
  cut.body = edge;
  cut.body_bytes = uint64_t{1} * 1024 * 1024;
  ExpectRefused(cut, HtmlStoreReason::kPersonalisationSignal, "oversized_tag");

  // The prefix itself runs past the window.
  StoreInput longer;
  longer.body = edge + "\" type=hidden name=_token>";
  longer.body_bytes = longer.body.size();
  ExpectRefused(longer, HtmlStoreReason::kPersonalisationSignal,
                "oversized_tag");
  EXPECT_EQ(FindPersonalisationSignal(longer.body), "oversized_tag");

  // The body ends there too: nothing went unread, so no signal.
  StoreInput whole;
  whole.body = edge;
  whole.body_bytes = edge.size();
  ExpectStored(whole);

  // A tag that closes inside the window is read normally in a longer body.
  StoreInput closed;
  closed.body =
      std::string(kPersonalisationScanBytes - 100, ' ') + "<input type=text>";
  closed.body_bytes = uint64_t{1} * 1024 * 1024;
  ExpectStored(closed);
}

TEST(HtmlCachePolicyTest, RepeatedDateFallsBackToReceiptTime) {
  StoreInput in;
  in.DropCacheControl();
  in.Add("Date", "Thu, 01 Jan 1970 00:00:00 GMT");
  in.Add("Date", kNowDate);
  in.Add("Expires", kPlus60);
  const HtmlStoreVerdict v = in.Run();
  EXPECT_TRUE(v.stores()) << v.reason_code();
  EXPECT_EQ(v.lifetime_s, 60u);
  // Order does not matter.
  StoreInput swapped;
  swapped.DropCacheControl();
  swapped.Add("Date", kNowDate);
  swapped.Add("Date", "Thu, 01 Jan 1970 00:00:00 GMT");
  swapped.Add("Expires", kPlus60);
  EXPECT_EQ(swapped.Run().lifetime_s, 60u);
}

TEST(HtmlCachePolicyTest, VerdictSaysWhatWasEvaluated) {
  StoreInput method;
  method.request.method = "HEAD";
  const HtmlStoreVerdict early = method.Run();
  EXPECT_FALSE(early.cc_evaluated);
  EXPECT_FALSE(early.sidecar_evaluated);

  StoreInput priv;
  priv.SetCacheControl("private, max-age=60");
  const HtmlStoreVerdict mid = priv.Run();
  EXPECT_TRUE(mid.cc_evaluated);
  EXPECT_FALSE(mid.sidecar_evaluated);

  StoreInput ok;
  const HtmlStoreVerdict full = ok.Run();
  EXPECT_TRUE(full.cc_evaluated);
  EXPECT_TRUE(full.sidecar_evaluated);
}

TEST(HtmlCachePolicyTest, StoreReportOnlyMeasuresAPersonalisationMatch) {
  StoreInput in;
  in.body = "<meta name=\"csrf-token\" content=\"x\">";
  ExpectRefused(in, HtmlStoreReason::kPersonalisationSignal, "meta_csrf_token");

  // Report-only keeps the refusal; the reason and detail measure the match.
  in.policy.mode = HtmlCacheMode::kReportOnly;
  const HtmlStoreVerdict v = in.Run();
  EXPECT_EQ(v.action, HtmlStoreAction::kRefuse);
  EXPECT_EQ(v.reason, HtmlStoreReason::kPersonalisationSignal);
  EXPECT_EQ(v.detail, "meta_csrf_token");
  EXPECT_FALSE(v.stores());
}

TEST(HtmlCachePolicyTest, ShortBodyPrefixIsRefusedNotStored) {
  StoreInput in;
  in.short_prefix = true;
  in.body = "";
  in.body_bytes = 4096;
  ExpectRefused(in, HtmlStoreReason::kIncomplete, "body_prefix");

  // One byte short of the body, and one byte short of the window.
  in.body = std::string(4095, 'x');
  ExpectRefused(in, HtmlStoreReason::kIncomplete, "body_prefix");
  in.body = std::string(4096, 'x');
  ExpectStored(in);

  in.body_bytes = uint64_t{1} * 1024 * 1024;
  in.body = std::string(kPersonalisationScanBytes - 1, 'x');
  ExpectRefused(in, HtmlStoreReason::kIncomplete, "body_prefix");
  in.body = std::string(kPersonalisationScanBytes, 'x');
  ExpectStored(in);
}

TEST(HtmlCachePolicyTest, MalformedSMaxageIsNotAShareableAssertion) {
  for (std::string_view line :
       {"s-maxage=abc", "s-maxage", "s-maxage=", "s-maxage=-1", "s-maxage=1x",
        "s-maxage=0", "S-MaxAge = 0", "s-maxage=60, s-maxage=60",
        "s-maxage=0, s-maxage=86400"}) {
    StoreInput in;
    in.Add("ETag", "\"a\"");
    in.SetCacheControl(line);
    const HtmlStoreVerdict stored = in.Run();
    EXPECT_FALSE(stored.explicit_shared) << line;
    in.request.has_cookie = true;
    ExpectRefused(in, HtmlStoreReason::kCookieRequest);
  }
  for (std::string_view line : {"s-maxage=60", "S-MaxAge = 60"}) {
    StoreInput in;
    in.Add("ETag", "\"a\"");
    in.SetCacheControl(line);
    in.request.has_cookie = true;
    const HtmlStoreVerdict v = in.Run();
    EXPECT_TRUE(v.explicit_shared) << line;
    EXPECT_TRUE(v.stores()) << line;
  }
}

TEST(HtmlCachePolicyTest, OriginPrivacyDirectivesHoldInEveryLegalForm) {
  struct Case {
    std::string_view cache_control;
    HtmlStoreReason reason;
  };
  const Case cases[] = {
      {"max-age=60, private", HtmlStoreReason::kPrivate},
      {"max-age=60,private", HtmlStoreReason::kPrivate},
      {"max-age=60,\tprivate", HtmlStoreReason::kPrivate},
      {"max-age=60,private\t", HtmlStoreReason::kPrivate},
      {"max-age=60, no-store", HtmlStoreReason::kNoStore},
      {"max-age=60,no-store", HtmlStoreReason::kNoStore},
      {"max-age=60,\tno-store", HtmlStoreReason::kNoStore},
      {"max-age=60,no-store\t", HtmlStoreReason::kNoStore},
  };
  for (HtmlCacheMode mode :
       {HtmlCacheMode::kReportOnly, HtmlCacheMode::kServeVariants}) {
    for (const Case& c : cases) {
      StoreInput in;
      in.policy.mode = mode;
      in.SetCacheControl(c.cache_control);
      ExpectRefused(in, c.reason);
    }
  }
}

TEST(HtmlCachePolicyTest, RepeatedExpiresCountsAsExpired) {
  StoreInput in;
  in.DropCacheControl();
  in.Add("Date", kNowDate);
  in.Add("Expires", kPlus60);
  in.Add("Expires", kMinus60);
  ExpectRefused(in, HtmlStoreReason::kNoLifetime);

  in.Add("ETag", "\"a\"");
  const HtmlStoreVerdict v = in.Run();
  EXPECT_TRUE(v.stores());
  EXPECT_EQ(v.lifetime_s, 0u);
  EXPECT_TRUE(v.needs_validation);

  // One line still counts.
  StoreInput one;
  one.DropCacheControl();
  one.Add("Date", kNowDate);
  one.Add("Expires", kPlus60);
  EXPECT_EQ(one.Run().lifetime_s, 60u);
}

TEST(HtmlCachePolicyTest, FrameworkTokenNamesAreSignals) {
  EXPECT_EQ(
      FindPersonalisationSignal("<input type=hidden name=_wpnonce value=a>"),
      "hidden_wpnonce");
  EXPECT_EQ(FindPersonalisationSignal("<input type=hidden name=_csrf value=a>"),
            "hidden_csrf");
  EXPECT_EQ(FindPersonalisationSignal("<meta name=\"_csrf\" content=\"a\">"),
            "meta_csrf_token");
  EXPECT_EQ(
      FindPersonalisationSignal("<meta name=\"_csrf_header\" content=\"a\">"),
      "meta_csrf_param");
  // A visible field of the same name is not a signal.
  EXPECT_FALSE(
      FindPersonalisationSignal("<input type=text name=_wpnonce>").has_value());
}

// ----------------------------------------------------------------- purity

TEST(HtmlCachePolicyTest, StoreVerdictIsPure) {
  // Own the bytes so "byte-identical afterwards" can be checked.
  std::vector<std::pair<std::string, std::string>> storage = {
      {"Content-Type", "text/html"},
      {"Cache-Control", "public, max-age=60, no-transform"},
      {"Vary", "Accept-Encoding"},
      {"Content-Security-Policy", "default-src 'self'"},
      {"ETag", "\"abc\""},
  };
  Fields headers;
  for (const auto& [n, v] : storage) headers.push_back({n, v});
  const auto snapshot = storage;
  std::string url = "/page.html";
  std::string body = "<html><body>x</body></html>";
  const std::string url_before = url;
  // A real copy on purpose: the test asserts `body` is unchanged, which a
  // reference to `body` could never catch.
  // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
  const std::string body_before = body;

  HtmlStoreRequest request{.method = "GET", .url_path = url};
  HtmlStoreResponse response;
  response.status = 200;
  response.headers = headers;
  response.body_bytes = 10;
  response.body_prefix = body;
  HtmlStorePolicy policy{.mode = HtmlCacheMode::kServeVariants,
                         .now_seconds = kNow};

  const HtmlStoreVerdict a = ClassifyHtmlForStore(request, response, policy);
  const HtmlStoreVerdict b = ClassifyHtmlForStore(request, response, policy);
  EXPECT_EQ(a.action, b.action);
  EXPECT_EQ(a.reason, b.reason);
  EXPECT_EQ(a.detail, b.detail);
  EXPECT_EQ(a.lifetime_s, b.lifetime_s);
  EXPECT_EQ(a.lifetime_explicit, b.lifetime_explicit);
  EXPECT_EQ(a.needs_validation, b.needs_validation);
  EXPECT_EQ(a.has_validators, b.has_validators);
  EXPECT_EQ(a.explicit_shared, b.explicit_shared);
  EXPECT_EQ(a.cc_flags, b.cc_flags);
  EXPECT_EQ(a.vary_tokens, b.vary_tokens);
  EXPECT_EQ(a.sidecar.payload, b.sidecar.payload);
  EXPECT_EQ(a.sidecar.reasons, b.sidecar.reasons);
  EXPECT_EQ(a.action, HtmlStoreAction::kStoreNoNotify);

  EXPECT_EQ(storage, snapshot);
  EXPECT_EQ(url, url_before);
  EXPECT_EQ(body, body_before);
  ASSERT_EQ(headers.size(), snapshot.size());
  for (size_t i = 0; i < headers.size(); ++i) {
    EXPECT_EQ(headers[i].name.data(), storage[i].first.data());
    EXPECT_EQ(headers[i].value.data(), storage[i].second.data());
  }
}

TEST(HtmlCachePolicyTest, ServeVerdictIsPure) {
  HtmlServeRequest r = Req(HtmlCacheMode::kServeCachedHtml, 33);
  r.max_age = 500;
  r.min_fresh = 3;
  StoredHtmlFacts f = Fresh();
  f.age_at_store = 4;
  const HtmlServeRequest r_before = r;
  const StoredHtmlFacts f_before = f;
  const HtmlServeVerdict a = ClassifyHtmlForServe(r, f);
  const HtmlServeVerdict b = ClassifyHtmlForServe(r, f);
  EXPECT_EQ(a.action, b.action);
  EXPECT_EQ(a.reason, b.reason);
  EXPECT_EQ(a.enforce, b.enforce);
  EXPECT_EQ(a.age_s, b.age_s);
  EXPECT_EQ(a.age_s, 37u);
  EXPECT_EQ(a.reason_code(), "fresh");
  EXPECT_EQ(r.mode, r_before.mode);
  EXPECT_EQ(r.now_seconds, r_before.now_seconds);
  EXPECT_EQ(r.max_age, r_before.max_age);
  EXPECT_EQ(r.min_fresh, r_before.min_fresh);
  EXPECT_EQ(f.stored_at, f_before.stored_at);
  EXPECT_EQ(f.age_at_store, f_before.age_at_store);
  EXPECT_EQ(f.lifetime_s, f_before.lifetime_s);
  EXPECT_EQ(f.render_ttl_s, f_before.render_ttl_s);
}

TEST(HtmlCachePolicyTest, CookieRulePredicate) {
  EXPECT_TRUE(HtmlCookieRequestMayShare(false, false));
  EXPECT_TRUE(HtmlCookieRequestMayShare(false, true));
  EXPECT_TRUE(HtmlCookieRequestMayShare(true, true));
  EXPECT_FALSE(HtmlCookieRequestMayShare(true, false));
}

TEST(HtmlCachePolicyTest, ExplicitlySharedNeedsPublicOrAPositiveSMaxage) {
  constexpr uint16_t kCc = Meta::kCCOriginHeaderPresent;
  EXPECT_FALSE(HtmlExplicitlyShared(0, 0));
  EXPECT_FALSE(HtmlExplicitlyShared(kCc, 0));
  EXPECT_FALSE(HtmlExplicitlyShared(kCc | Meta::kCCOriginMustRevalidate, 0));
  EXPECT_TRUE(HtmlExplicitlyShared(kCc | Meta::kCCOriginPublic, 0));
  EXPECT_TRUE(HtmlExplicitlyShared(kCc | Meta::kCCOriginSMaxagePresent, 60));
  // s-maxage=0, or a malformed value (which parses to 0), is not a mark.
  EXPECT_FALSE(HtmlExplicitlyShared(kCc | Meta::kCCOriginSMaxagePresent, 0));
  // A value without the directive cannot occur; it is not a mark either.
  EXPECT_FALSE(HtmlExplicitlyShared(kCc, 60));
  // A repeated s-maxage is no mark, whatever the kept value; public still is.
  constexpr uint16_t kRepeated =
      kCc | Meta::kCCOriginSMaxagePresent | Meta::kCCOriginSMaxageRepeated;
  EXPECT_FALSE(HtmlExplicitlyShared(kRepeated, 60));
  EXPECT_TRUE(HtmlExplicitlyShared(kRepeated | Meta::kCCOriginPublic, 60));
}

TEST(HtmlCachePolicyTest, OriginStatesLifetime) {
  constexpr uint16_t kCc = Meta::kCCOriginHeaderPresent;
  EXPECT_FALSE(OriginStatesLifetime(0, 0, 0));
  EXPECT_FALSE(OriginStatesLifetime(0, 600, 0));  // no header: a default
  EXPECT_FALSE(OriginStatesLifetime(kCc, 0, 0));
  EXPECT_FALSE(OriginStatesLifetime(kCc | Meta::kCCOriginSMaxagePresent, 0, 0));
  EXPECT_TRUE(OriginStatesLifetime(kCc, 600, 0));
  EXPECT_TRUE(
      OriginStatesLifetime(kCc | Meta::kCCOriginSMaxagePresent, 0, 600));
}

TEST(HtmlCachePolicyTest, CookieShareClassForContentType) {
  using C = CookieShareClass;
  for (std::string_view ct :
       {"text/css", "TEXT/CSS", "text/css; charset=utf-8",
        "application/javascript", "Application/JavaScript;charset=UTF-8",
        "text/javascript", "application/x-javascript", "image/png",
        "IMAGE/WebP", " image/svg+xml ; q=1"}) {
    EXPECT_EQ(C::kStaticAsset, CookieShareClassForContentType(ct)) << ct;
  }
  for (std::string_view ct :
       {"", "text/html", "Text/HTML", "text/html; charset=utf-8",
        "application/xhtml+xml", "application/json", "text/plain",
        "text/markdown", "application/octet-stream", "image/", "image",
        "text/css-not", "font/woff2", "garbage"}) {
    EXPECT_EQ(C::kPage, CookieShareClassForContentType(ct)) << ct;
  }
}

TEST(HtmlCachePolicyTest, CookieRequestMayStoreByClass) {
  using C = CookieShareClass;
  // Without a cookie nothing changes.
  EXPECT_TRUE(CookieRequestMayStore(false, C::kPage, false, false));
  EXPECT_TRUE(CookieRequestMayStore(false, C::kStaticAsset, false, false));
  // A page needs the sharing mark; a stated lifetime is not enough.
  EXPECT_FALSE(CookieRequestMayStore(true, C::kPage, false, true));
  EXPECT_TRUE(CookieRequestMayStore(true, C::kPage, true, false));
  // A static asset needs a stated lifetime; no class default applies.
  EXPECT_FALSE(CookieRequestMayStore(true, C::kStaticAsset, false, false));
  EXPECT_FALSE(CookieRequestMayStore(true, C::kStaticAsset, true, false));
  EXPECT_TRUE(CookieRequestMayStore(true, C::kStaticAsset, false, true));
}

TEST(HtmlCachePolicyTest, CookieRequestMayServeByClass) {
  using C = CookieShareClass;
  EXPECT_TRUE(CookieRequestMayServe(false, C::kPage, false));
  EXPECT_FALSE(CookieRequestMayServe(true, C::kPage, false));
  EXPECT_TRUE(CookieRequestMayServe(true, C::kPage, true));
  EXPECT_TRUE(CookieRequestMayServe(true, C::kStaticAsset, false));
}

TEST(HtmlCachePolicyTest, StoredFieldsAgreeWithTheStoreVerdict) {
  // The policy's own explicit_shared is HtmlExplicitlyShared over the parsed
  // Cache-Control, so a front end holding only the stored flags and s-maxage
  // answers the cookie rule exactly as the policy does.
  for (const char* line :
       {"max-age=60", "public, max-age=60", "max-age=60, s-maxage=120",
        "s-maxage=0", "s-maxage=abc", "s-maxage=60, s-maxage=60",
        "public, s-maxage=60, s-maxage=60", "max-age=60, must-revalidate",
        "no-cache", "public"}) {
    StoreInput in;
    in.headers = {{"Content-Type", "text/html; charset=utf-8"},
                  {"Cache-Control", line},
                  {"ETag", "\"a\""}};
    const HtmlStoreVerdict v = in.Run();
    ASSERT_TRUE(v.cc_evaluated) << line;
    OriginCacheControl cc;
    AccumulateOriginCacheControl(line, &cc);
    EXPECT_EQ(v.explicit_shared, HtmlExplicitlyShared(cc.cc_flags, cc.s_maxage))
        << line;
  }
}

// ---- C API 1.14 serve rules -----------------------------------------------

HtmlServeRequest FreshMarkdownRequest() {
  HtmlServeRequest r;
  r.mode = HtmlCacheMode::kServeVariants;
  r.wants_markdown = true;
  r.now_seconds = 1000;
  return r;
}

StoredHtmlFacts LiveCopy() {
  StoredHtmlFacts f;
  f.present = true;
  f.stored_at = 900;
  f.lifetime_s = 300;
  f.variant_present = true;
  f.binding_live = true;
  return f;
}

TEST(HtmlServePolicy113Test, RequestShapeFallsThroughBeforeTheEntry) {
  StoredHtmlFacts absent;
  HtmlServeRequest r = FreshMarkdownRequest();
  r.method_cacheable = false;
  EXPECT_EQ(ClassifyHtmlForServe(r, absent).reason, HtmlServeReason::kMethod);
  r = FreshMarkdownRequest();
  r.has_range = true;
  EXPECT_EQ(ClassifyHtmlForServe(r, LiveCopy()).reason,
            HtmlServeReason::kRange);
  r = FreshMarkdownRequest();
  r.has_origin = true;
  EXPECT_EQ(ClassifyHtmlForServe(r, LiveCopy()).reason, HtmlServeReason::kCors);
  // In a mode that does not serve, the mode is the reason.
  r.mode = HtmlCacheMode::kReportOnly;
  EXPECT_EQ(ClassifyHtmlForServe(r, LiveCopy()).reason, HtmlServeReason::kMode);
}

TEST(HtmlServePolicy113Test, FutureDatedEntryIsSkew) {
  StoredHtmlFacts f = LiveCopy();
  f.stored_at = 1000 + kHtmlServeSkewSeconds + 1;
  EXPECT_EQ(ClassifyHtmlForServe(FreshMarkdownRequest(), f).reason,
            HtmlServeReason::kSkew);
  f.stored_at = 1000 + kHtmlServeSkewSeconds;
  EXPECT_EQ(ClassifyHtmlForServe(FreshMarkdownRequest(), f).action,
            HtmlServeAction::kServeMarkdown);
}

TEST(HtmlServePolicy113Test, NoTransformAndWithheldNeverServeTheCopy) {
  StoredHtmlFacts f = LiveCopy();
  f.no_transform = true;
  EXPECT_EQ(ClassifyHtmlForServe(FreshMarkdownRequest(), f).reason,
            HtmlServeReason::kNoTransform);
  f = LiveCopy();
  f.withheld = true;
  EXPECT_EQ(ClassifyHtmlForServe(FreshMarkdownRequest(), f).reason,
            HtmlServeReason::kWithheld);
  // A request that asks for no transformation is not given the copy.
  HtmlServeRequest no_transform = FreshMarkdownRequest();
  ApplyHtmlRequestCacheControl("No-Transform", &no_transform);
  EXPECT_TRUE(no_transform.request_no_transform);
  EXPECT_EQ(ClassifyHtmlForServe(no_transform, LiveCopy()).reason,
            HtmlServeReason::kNoTransform);
  // Neither touches HTML serving.
  HtmlServeRequest html = FreshMarkdownRequest();
  html.wants_markdown = false;
  html.mode = HtmlCacheMode::kServeCachedHtml;
  f.no_transform = true;
  EXPECT_EQ(ClassifyHtmlForServe(html, f).action, HtmlServeAction::kServeHtml);
}

TEST(HtmlServePolicy113Test, RequestCacheControlParsing) {
  const auto parse = [](std::string_view cc) {
    HtmlServeRequest r;
    ApplyHtmlRequestCacheControl(cc, &r);
    return r;
  };
  HtmlServeRequest r = parse("No-Cache");
  EXPECT_TRUE(r.has_cache_control);
  EXPECT_TRUE(r.cache_control_no_cache);
  r = parse("max-age=60, min-fresh=\"10\"");
  EXPECT_EQ(r.max_age, 60);
  EXPECT_EQ(r.min_fresh, 10);
  EXPECT_FALSE(r.cache_control_no_cache);
  r = parse("max-age=60, max-age=30");
  EXPECT_EQ(r.max_age, 30);
  r = parse("max-age=abc");
  EXPECT_EQ(r.max_age, 0);
  r = parse("min-fresh=");
  EXPECT_EQ(r.max_age, 0);
  r = parse("max-age=99999999999999999999");
  EXPECT_EQ(r.max_age, INT32_MAX);
  r = parse("max-stale=100, no-store");
  EXPECT_EQ(r.max_age, -1);
  EXPECT_EQ(r.min_fresh, -1);
  EXPECT_TRUE(r.has_cache_control);
  r = parse("");
  EXPECT_TRUE(r.has_cache_control);
  EXPECT_FALSE(r.cache_control_no_cache);
}

}  // namespace
}  // namespace pagespeed
