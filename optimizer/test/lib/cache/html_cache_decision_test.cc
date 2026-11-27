// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The HTML cache decision line formatter.
//
// The properties these tests exist to hold:
//
//   1. the line is one JSON object whose keys come out in the canonical order
//      whatever order the caller gave them in;
//   2. the key set is closed, and has no key through which a body, a cookie,
//      a header value, a client address or a user agent could be logged;
//   3. every value is checked against its key's grammar (fixed codes and
//      counts only), so a header value or a cookie cannot pass even under a
//      legitimate key, and one refusal per key class is pinned here;
//   4. every value is escaped, and a value that could split the line (CR, LF,
//      NUL) is refused rather than written;
//   5. the bounds (path length, host present) hold.

#include "lib/cache/html_cache_decision.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"
#include "lib/cache/headers_sidecar.h"
#include "lib/cache/html_cache_policy.h"

namespace pagespeed {
namespace {

using Fields = std::vector<HtmlCacheDecisionField>;

std::optional<std::string> Format(const Fields& fields) {
  return FormatHtmlCacheDecisionLine(fields);
}

TEST(HtmlCacheDecisionTest, KeysAreTheClosedCanonicalSet) {
  const std::vector<std::string_view> expected = {
      "ts_ms",  "host",    "path",    "mode",       "action",
      "reason", "status",  "size",    "lifetime_s", "validators",
      "cc",     "vary",    "sidecar", "request",    "personalisation",
      "before", "variant", "binding", "age_s",      "vhost_added"};
  const auto keys = HtmlCacheDecisionKeys();
  ASSERT_EQ(keys.size(), expected.size());
  for (size_t i = 0; i < expected.size(); ++i) EXPECT_EQ(keys[i], expected[i]);
}

TEST(HtmlCacheDecisionTest, NoKeyCanCarryAPrivateValue) {
  for (std::string_view key : HtmlCacheDecisionKeys()) {
    for (std::string_view banned :
         {"body", "cookie", "user_agent", "remote_addr", "value", "header",
          "headers", "ip", "client", "agent", "referer", "authorization",
          "query", "content"}) {
      EXPECT_EQ(key.find(banned), std::string_view::npos)
          << key << " contains " << banned;
    }
  }
  // And the formatter enforces it: those names are refused.
  for (std::string_view banned :
       {"body", "cookie", "user_agent", "remote_addr", "value"}) {
    const Fields f = {{"host", "a.example"}, {banned, "x"}};
    EXPECT_FALSE(Format(f).has_value()) << banned;
  }
}

TEST(HtmlCacheDecisionTest, CanonicalOrderWhateverTheInputOrder) {
  const Fields shuffled = {{"age_s", "7"},           {"reason", "none"},
                           {"host", "a.example"},    {"ts_ms", "1700000000000"},
                           {"path", "/x"},           {"mode", "report-only"},
                           {"action", "would_store"}};
  EXPECT_EQ(Format(shuffled).value(),
            "{\"ts_ms\":1700000000000,\"host\":\"a.example\",\"path\":\"/x\","
            "\"mode\":\"report-only\",\"action\":\"would_store\","
            "\"reason\":\"none\",\"age_s\":7}");
}

TEST(HtmlCacheDecisionTest, OnlyTheGivenKeysAreWritten) {
  const Fields one = {{"host", "a.example"}};
  EXPECT_EQ(Format(one).value(), "{\"host\":\"a.example\"}");
  const Fields none;
  EXPECT_EQ(Format(none).value(), "{}");
}

TEST(HtmlCacheDecisionTest, EveryKeyRoundTrips) {
  const Fields all = {{"ts_ms", "1700000000000"},
                      {"host", "h.example"},
                      {"path", "/p"},
                      {"mode", "serve-variants"},
                      {"action", "served_markdown"},
                      {"reason", "fresh"},
                      {"status", "200"},
                      {"size", "1234"},
                      {"lifetime_s", "300"},
                      {"validators", "etag"},
                      {"cc", "max-age=300,public"},
                      {"vary", "accept-encoding"},
                      {"sidecar", "eligible_block"},
                      {"request", "accept_markdown"},
                      {"personalisation", "hidden_token"},
                      {"before", "cbf29ce484222325"},
                      {"variant", "0x7C"},
                      {"binding", "live"},
                      {"age_s", "7"},
                      {"vhost_added", "x-frame-options"}};
  ASSERT_EQ(all.size(), HtmlCacheDecisionKeys().size());
  const std::string line = Format(all).value();
  EXPECT_EQ(line.front(), '{');
  EXPECT_EQ(line.back(), '}');
  EXPECT_EQ(line.find('\n'), std::string::npos);
  for (std::string_view key : HtmlCacheDecisionKeys()) {
    EXPECT_NE(line.find("\"" + std::string(key) + "\":"), std::string::npos)
        << key;
  }
}

TEST(HtmlCacheDecisionTest, NumbersAreCanonicalIntegersWrittenAsNumbers) {
  const Fields digits = {{"host", "h"},         {"ts_ms", "1"},
                         {"status", "200"},     {"size", "0"},
                         {"lifetime_s", "300"}, {"age_s", "12"}};
  EXPECT_EQ(Format(digits).value(),
            "{\"ts_ms\":1,\"host\":\"h\",\"status\":200,\"size\":0,"
            "\"lifetime_s\":300,\"age_s\":12}");
  // The largest count a uint64_t holds is still a number.
  EXPECT_EQ(Format({{"host", "h"}, {"size", "18446744073709551615"}}).value(),
            "{\"host\":\"h\",\"size\":18446744073709551615}");

  // Anything that is not a canonical integer is refused for a numeric key:
  // a count never becomes a string.
  for (std::string_view bad : {"12ab", "", "-5", "007", "1.5", "+1", " 1",
                               "123456789012345678901", "0x10"}) {
    for (std::string_view key :
         {"ts_ms", "status", "size", "lifetime_s", "age_s"}) {
      EXPECT_FALSE(Format({{"host", "h"}, {key, bad}}).has_value())
          << key << "=" << bad;
      EXPECT_FALSE(HtmlCacheDecisionValueFits(key, bad)) << key << "=" << bad;
    }
  }

  // A digits-only value for a non-numeric key is a string, where its grammar
  // admits digits at all.
  const Fields text = {{"host", "h"}, {"before", "1234"}};
  EXPECT_EQ(Format(text).value(), "{\"host\":\"h\",\"before\":\"1234\"}");
}

// One acceptance and one refusal per key class: the value grammar.

TEST(HtmlCacheDecisionTest, ClosedNameSetsAreEnforced) {
  struct Row {
    std::string_view key;
    std::vector<std::string_view> good;
    std::vector<std::string_view> bad;
  };
  const std::vector<Row> rows = {
      {"mode",
       {"off", "report-only", "serve-variants", "serve-cached-html"},
       {"on", "Report-Only", "serve", "", "off,report-only"}},
      {"action",
       {"would_store", "stored", "skipped", "would_serve", "served_html",
        "served_markdown", "validated", "purged", "invalidated", "refuse",
        "store", "store_no_notify", "fall_through", "serve_html",
        "serve_markdown", "validate"},
       {"served", "Stored", "store ", "", "unknown"}},
      {"reason",
       {"none", "method", "no_store", "personalisation_signal", "no_transform",
        "fresh", "mode", "parent_stale", "validated", "validation_failed",
        "sidecar_write_failed", "invalidated", "headers:x-powered-by",
        "vary:cookie", "vary:*"},
       {"", "unknown", "headers:", "vary:", "headers:x y", "headers:a\"b",
        "no_store:request", "personalisation_signal:meta_csrf_token",
        "vary:a,b", "Headers:x"}},
      {"sidecar",
       {"eligible_block", "eligible_empty", "fall_through",
        "fall_through:x-powered-by", "never:nonce_csp"},
       {"", "swap", "eligible", "fall_through:", "fall_through:a b",
        "never:", "never:other"}},
      {"personalisation",
       {"meta_csrf_token", "post_form_checkout"},
       {"", "csrf", "meta_csrf_token,hidden_token", "token=abc"}},
      {"variant", {"0x7C", "markdown"}, {"", "0x7c", "html", "0x3C"}},
      {"binding", {"live", "stale"}, {"", "Live", "dead", "live,stale"}},
  };
  for (const Row& row : rows) {
    for (std::string_view good : row.good) {
      EXPECT_TRUE(Format({{"host", "h"}, {row.key, good}}).has_value())
          << row.key << "=" << good;
      EXPECT_TRUE(HtmlCacheDecisionValueFits(row.key, good))
          << row.key << "=" << good;
    }
    for (std::string_view bad : row.bad) {
      EXPECT_FALSE(Format({{"host", "h"}, {row.key, bad}}).has_value())
          << row.key << "=" << bad;
      EXPECT_FALSE(HtmlCacheDecisionValueFits(row.key, bad))
          << row.key << "=" << bad;
    }
  }
  // Every store and serve reason name the policy can return is a reason.
  for (uint8_t i = 0; i < kHtmlStoreReasonCount; ++i) {
    EXPECT_TRUE(HtmlCacheDecisionValueFits(
        "reason", HtmlStoreReasonName(static_cast<HtmlStoreReason>(i))));
  }
  for (uint8_t i = 0; i < kHtmlServeReasonCount; ++i) {
    EXPECT_TRUE(HtmlCacheDecisionValueFits(
        "reason", HtmlServeReasonName(static_cast<HtmlServeReason>(i))));
  }
  for (std::string_view signal : HtmlPersonalisationSignalNames()) {
    EXPECT_TRUE(HtmlCacheDecisionValueFits("personalisation", signal));
  }
  // The detail of a headers or vary reason is a token of at most 128 bytes.
  const std::string long_name(128, 'a');
  EXPECT_TRUE(HtmlCacheDecisionValueFits("reason", "headers:" + long_name));
  EXPECT_FALSE(
      HtmlCacheDecisionValueFits("reason", "headers:" + long_name + "a"));
  EXPECT_TRUE(
      HtmlCacheDecisionValueFits("sidecar", "fall_through:" + long_name));
  EXPECT_FALSE(
      HtmlCacheDecisionValueFits("sidecar", "fall_through:" + long_name + "a"));
  EXPECT_FALSE(HtmlCacheDecisionValueFits("bogus", "x"));
}

TEST(HtmlCacheDecisionTest, BooleanGrammarsAreOrderedSubsets) {
  for (std::string_view good :
       {"none", "etag", "last_modified", "etag,last_modified"}) {
    EXPECT_TRUE(Format({{"host", "h"}, {"validators", good}}).has_value())
        << good;
  }
  for (std::string_view bad :
       {"", "last_modified,etag", "etag,etag", "etag,", ",etag", "etag=1",
        "true", "none,etag", "ETag", "etag, last_modified"}) {
    EXPECT_FALSE(Format({{"host", "h"}, {"validators", bad}}).has_value())
        << bad;
  }
  for (std::string_view good :
       {"none", "authorization", "cookie", "no_cache", "accept_markdown",
        "authorization,cookie", "cookie,accept_markdown",
        "authorization,cookie,no_cache,accept_markdown"}) {
    EXPECT_TRUE(Format({{"host", "h"}, {"request", good}}).has_value()) << good;
  }
  for (std::string_view bad :
       {"", "cookie,authorization", "cookie,cookie", "cookie=sid", "no-cache",
        "accept_markdown,cookie", "cookie,"}) {
    EXPECT_FALSE(Format({{"host", "h"}, {"request", bad}}).has_value()) << bad;
  }
}

TEST(HtmlCacheDecisionTest, CacheControlIsDirectiveNamesAndNumbersOnly) {
  for (std::string_view good :
       {"public", "max-age=300", "max-age=0,public", "s-maxage=60,max-age=0",
        "max-age", "s-maxage,public",
        // Spaces around a comma are optional (RFC 9110 section 5.6.1).
        "max-age=300, public", "max-age=300 ,public", " public", "public ",
        "no-cache,no-store,no-transform,must-revalidate,proxy-revalidate",
        "immutable,stale-while-revalidate=600,stale-if-error=86400",
        "private"}) {
    EXPECT_TRUE(Format({{"host", "h"}, {"cc", good}}).has_value()) << good;
  }
  for (std::string_view bad :
       {"", "42", "max-age=", "max-age=abc", "max-age=0300", "public=1",
        "no-cache=\"Set-Cookie\"", "private=\"Set-Cookie\"",
        "max-age=300,max-age=60", "x-session=abc", "public,", ",public",
        "public, ,private", "max-age = 300", "max-age= 300", "Public",
        // A tab is a control byte: refused before the grammar is consulted.
        "max-age=300\t,\tpublic", "max-age=300;public"}) {
    EXPECT_FALSE(Format({{"host", "h"}, {"cc", bad}}).has_value()) << bad;
  }
  // A directive's text value never passes: `max-age` takes a number, the
  // others take nothing.
  EXPECT_FALSE(Format({{"host", "h"}, {"cc", "max-age=300,private=sid-abc"}})
                   .has_value());
}

TEST(HtmlCacheDecisionTest, VaryIsAListOfTokens) {
  for (std::string_view good :
       {"accept-encoding", "Accept-Encoding", "*", "accept-encoding,cookie",
        "accept-encoding, cookie", "accept-encoding ,cookie", " cookie ",
        "a!#$%&'*+-.^_`|~z"}) {
    EXPECT_TRUE(Format({{"host", "h"}, {"vary", good}}).has_value()) << good;
  }
  for (std::string_view bad :
       {"", ",", " ", "accept-encoding,", ",cookie", "accept-encoding, ,cookie",
        "accept-encoding\t,\tcookie", "a b", "a\"b", "a(b)", "a/b", "a;b",
        "a=b", "a:b", "caf\xC3\xA9"}) {
    EXPECT_FALSE(Format({{"host", "h"}, {"vary", bad}}).has_value()) << bad;
  }
}

TEST(HtmlCacheDecisionTest, HostIsHostnameCharacters) {
  for (std::string_view good :
       {"a.example", "A.EXAMPLE", "a-b_c.example:8080", "[::1]", "[::1]:443",
        "10.0.0.1", "xn--bcher-kva.example"}) {
    EXPECT_TRUE(Format({{"host", good}}).has_value()) << good;
  }
  const std::string longest(255, 'a');
  EXPECT_TRUE(Format({{"host", longest}}).has_value());
  for (std::string_view bad :
       {"", "a example", "a/b", "a?b", "a@b", "user:pw@a", "a\"b",
        "caf\xC3\xA9", "a,b", "http://a.example"}) {
    EXPECT_FALSE(Format({{"host", bad}}).has_value()) << bad;
  }
  EXPECT_FALSE(Format({{"host", longest + "a"}}).has_value());
}

TEST(HtmlCacheDecisionTest, PathNeverCarriesAQueryOrFragment) {
  EXPECT_TRUE(Format({{"host", "h"}, {"path", "/a/b.html"}}).has_value());
  EXPECT_TRUE(Format({{"host", "h"}, {"path", "/a b"}}).has_value());
  for (std::string_view bad : {"", "/a?token=1", "/a#frag", "/?", "/a?", "?",
                               "/a#", "/a%20b", "/x;y=1"}) {
    EXPECT_FALSE(Format({{"host", "h"}, {"path", bad}}).has_value()) << bad;
  }
}

TEST(HtmlCacheDecisionTest, BeforeIsAHashInDigits) {
  for (std::string_view good :
       {"0", "18446744073709551615", "cbf29ce484222325", "af63dc4c8601ec8c"}) {
    EXPECT_TRUE(Format({{"host", "h"}, {"before", good}}).has_value()) << good;
  }
  for (std::string_view bad :
       {"", "CBF29CE484222325", "0x1", "-1", "123456789012345678901", "g"}) {
    EXPECT_FALSE(Format({{"host", "h"}, {"before", bad}}).has_value()) << bad;
  }
}

TEST(HtmlCacheDecisionTest, PolicyReasonCodesAlwaysFit) {
  // Whatever an origin puts in a header name or a Vary token, the reason
  // code the policy builds from it (and the sidecar code a front end builds
  // from the same detail) is inside the grammar: no decision line is lost
  // to a refusal on the policy's own output.
  const std::string quotes(500, '"');
  const std::vector<std::vector<HeaderField>> cases = {
      {{"Content-Type", "text/html"}, {"Cache-Control", "max-age=60"}},
      {{"Content-Type", "text/html"},
       {"Cache-Control", "max-age=60"},
       {"Vary", "\"x\""}},
      {{"Content-Type", "text/html"},
       {"Cache-Control", "max-age=60"},
       {"Vary", "a b, (c)"}},
      {{"Content-Type", "text/html"},
       {"Cache-Control", "max-age=60"},
       {"Vary", "caf\xC3\xA9"}},
      {{"Content-Type", "text/html"},
       {"Cache-Control", "max-age=60"},
       {"X-Bad\x01Name", "v"}},
      {{"Content-Type", "text/html"},
       {"Cache-Control", "max-age=60"},
       {"X-Request-Id", "x"}},
      {{"Content-Type", "text/html"},
       {"Cache-Control", "max-age=60"},
       {quotes, "v"}},
      {{"Content-Type", "text/html"},
       {"Cache-Control", "max-age=60"},
       {"Content-Security-Policy", "script-src 'nonce-abc'"}},
      {{"Content-Type", "text/html"}},
      {{"Content-Type", "text/html"}, {"Cache-Control", "private"}},
  };
  for (const auto& headers : cases) {
    HtmlStoreRequest request{.method = "GET", .url_path = "/p"};
    HtmlStoreResponse response;
    response.status = 200;
    response.headers = headers;
    response.body_bytes = 8;
    response.body_prefix = "<p>x</p>";
    const HtmlStoreVerdict v = ClassifyHtmlForStore(
        request, response,
        HtmlStorePolicy{.mode = HtmlCacheMode::kServeVariants});
    const std::string code = v.reason_code();
    EXPECT_TRUE(HtmlCacheDecisionValueFits("reason", code)) << code;
    EXPECT_TRUE(Format({{"host", "h"}, {"reason", code}}).has_value()) << code;
    if (v.reason == HtmlStoreReason::kHeaders && !v.detail.empty()) {
      EXPECT_TRUE(
          HtmlCacheDecisionValueFits("sidecar", "fall_through:" + v.detail))
          << v.detail;
    }
  }
}

TEST(HtmlCacheDecisionTest, AMeasuredPersonalisationMatchFormats) {
  // The report-only outcome for a personalisation match: a refusal, with the
  // signal that caused it.
  const Fields f = {{"host", "a.example"},
                    {"path", "/account"},
                    {"mode", "report-only"},
                    {"action", "refuse"},
                    {"reason", "personalisation_signal"},
                    {"personalisation", "meta_csrf_token"}};
  EXPECT_EQ(Format(f).value(),
            "{\"host\":\"a.example\",\"path\":\"/account\","
            "\"mode\":\"report-only\",\"action\":\"refuse\","
            "\"reason\":\"personalisation_signal\","
            "\"personalisation\":\"meta_csrf_token\"}");
}

TEST(HtmlCacheDecisionTest, PathRefusesEncodedBytesAndParameters) {
  EXPECT_FALSE(HtmlCacheDecisionValueFits("path", "/x;session=abc"));
  EXPECT_FALSE(HtmlCacheDecisionValueFits("path", "/a%20b"));
  EXPECT_TRUE(HtmlCacheDecisionValueFits("path", "/a/b.html"));
}

TEST(HtmlCacheDecisionTest, VaryValueIsBounded) {
  std::string at_bound = "a";
  while (at_bound.size() + 2 <= kMaxHtmlCacheDecisionVaryBytes)
    at_bound += ",a";
  EXPECT_TRUE(HtmlCacheDecisionValueFits("vary", at_bound));
  EXPECT_FALSE(HtmlCacheDecisionValueFits("vary", at_bound + ",a,a"));
}

TEST(HtmlCacheDecisionTest, StoreSideRefusesServeRequestFacts) {
  const auto store_side = [](const Fields& f) {
    return HtmlCacheDecisionIsStoreSide(f);
  };
  EXPECT_TRUE(store_side({{"request", "authorization,cookie"}}));
  EXPECT_TRUE(store_side({{"request", "none"}}));
  EXPECT_FALSE(store_side({{"request", "no_cache"}}));
  EXPECT_FALSE(store_side({{"request", "cookie,accept_markdown"}}));
}

TEST(HtmlCacheDecisionTest, ReportOnlyLineHasNoServeOutcome) {
  for (std::string_view action :
       {"would_serve", "served_html", "served_markdown", "validated", "stored",
        "store", "store_no_notify", "fall_through", "serve_html",
        "serve_markdown", "validate"}) {
    const Fields report = {{"mode", "report-only"}, {"action", action}};
    EXPECT_FALSE(Format(report).has_value()) << action;
  }
  for (std::string_view action :
       {"would_store", "would_store_no_notify", "skipped", "refuse"}) {
    const Fields report = {{"mode", "report-only"}, {"action", action}};
    EXPECT_TRUE(Format(report).has_value()) << action;
  }
  // The same action is admitted in a mode that serves.
  const Fields serving = {{"mode", "serve-variants"},
                          {"action", "would_serve"}};
  EXPECT_TRUE(Format(serving).has_value());
}

TEST(HtmlCacheDecisionTest, StoreSideRefusesServeFacts) {
  const auto store_side = [](const Fields& f) {
    return HtmlCacheDecisionIsStoreSide(f);
  };
  EXPECT_TRUE(store_side({{"mode", "off"}, {"action", "refuse"}}));
  EXPECT_TRUE(store_side({{"reason", "headers:x-request-id"}}));
  EXPECT_TRUE(store_side({{"reason", "nonce_link"}}));
  EXPECT_FALSE(store_side({{"mode", "serve-variants"}}));
  EXPECT_FALSE(store_side({{"action", "would_serve"}}));
  EXPECT_FALSE(store_side({{"reason", "fresh"}}));
  EXPECT_FALSE(store_side({{"age_s", "1"}}));
  EXPECT_FALSE(store_side({{"variant", "markdown"}}));
  EXPECT_FALSE(store_side({{"binding", "live"}}));
}

TEST(HtmlCacheDecisionTest, NonceLinkSidecarCodeFits) {
  EXPECT_TRUE(HtmlCacheDecisionValueFits("sidecar", "never:nonce_link"));
}

TEST(HtmlCacheDecisionTest, NoValueIsEverEmpty) {
  for (std::string_view key : HtmlCacheDecisionKeys()) {
    EXPECT_FALSE(Format({{"host", "h"}, {key, ""}}).has_value()) << key;
    EXPECT_FALSE(HtmlCacheDecisionValueFits(key, "")) << key;
  }
}

TEST(HtmlCacheDecisionTest, ValuesAreEscaped) {
  // `path` is the one key whose grammar admits a quote, a backslash or a
  // byte outside ASCII; they come out escaped.
  const Fields f = {{"host", "h"}, {"path", "/a\"b\\c</script>\x7e"}};
  EXPECT_EQ(Format(f).value(),
            "{\"host\":\"h\",\"path\":\"/a\\\"b\\\\c</script>~\"}");
}

TEST(HtmlCacheDecisionTest, HighBytesAreEscapedAsCodePoints) {
  // Each byte from 0x80 up is written as \u00XX, so the line is ASCII and
  // strict JSON whatever bytes the caller held.
  const Fields f = {{"host", "h"}, {"path", "/caf\xC3\xA9 \xFF"}};
  const std::string line = Format(f).value();
  EXPECT_EQ(line, "{\"host\":\"h\",\"path\":\"/caf\\u00c3\\u00a9 \\u00ff\"}");
  for (char c : line) EXPECT_LT(static_cast<unsigned char>(c), 0x80);
  // A lone 0xC3 (invalid UTF-8) is escaped too.
  EXPECT_EQ(Format({{"host", "h"}, {"path", "/\xC3"}}).value(),
            "{\"host\":\"h\",\"path\":\"/\\u00c3\"}");
}

TEST(HtmlCacheDecisionTest, EveryControlByteIsRefused) {
  for (int c = 0; c <= 0x1F; ++c) {
    const std::string value = std::string("/a") + static_cast<char>(c) + "b";
    EXPECT_FALSE(Format({{"host", "h"}, {"path", value}}).has_value()) << c;
    EXPECT_FALSE(Format({{"host", value}}).has_value()) << c;
  }
  // The dedup separator in particular, and DEL.
  EXPECT_FALSE(Format({{"host", "h"}, {"path", "/a\x1f"}}).has_value());
  EXPECT_FALSE(Format({{"host", "h"}, {"path", "/a\x7f"}}).has_value());
  // Space and the rest of the printable range are fine in a path.
  EXPECT_TRUE(Format({{"host", "h"}, {"path", "/ ~"}}).has_value());
}

TEST(HtmlCacheDecisionTest, UnknownAndDuplicateKeysAreRefused) {
  EXPECT_FALSE(Format({{"host", "h"}, {"bogus", "x"}}).has_value());
  EXPECT_FALSE(Format({{"host", "h"}, {"HOST", "x"}}).has_value());
  EXPECT_FALSE(Format({{"host", "h"}, {"", "x"}}).has_value());
  EXPECT_FALSE(
      Format({{"host", "h"}, {"path", "/a"}, {"path", "/b"}}).has_value());
  EXPECT_FALSE(Format({{"host", "h"}, {"host", "h"}}).has_value());
}

TEST(HtmlCacheDecisionTest, LineBreakingBytesAreRefused) {
  EXPECT_FALSE(Format({{"host", "h"}, {"path", "/a\nb"}}).has_value());
  EXPECT_FALSE(Format({{"host", "h"}, {"path", "/a\rb"}}).has_value());
  EXPECT_FALSE(Format({{"host", "h"}, {"path", std::string_view("/a\0b", 4)}})
                   .has_value());
  EXPECT_FALSE(Format({{"host", "a\nb"}}).has_value());
  EXPECT_FALSE(Format({{"host", "h"}, {"path", "/a\r\n"}}).has_value());
  // A tab is a control byte too.
  EXPECT_FALSE(Format({{"host", "h"}, {"path", "/a\tb"}}).has_value());
}

TEST(HtmlCacheDecisionTest, HostMustNotBeEmpty) {
  EXPECT_FALSE(Format({{"host", ""}}).has_value());
  EXPECT_FALSE(Format({{"host", ""}, {"path", "/"}}).has_value());
  EXPECT_TRUE(Format({{"host", "x"}}).has_value());
}

TEST(HtmlCacheDecisionTest, PathIsBoundedAt300Bytes) {
  const std::string ok(300, 'a');
  const std::string too_long(301, 'a');
  EXPECT_TRUE(Format({{"host", "h"}, {"path", ok}}).has_value());
  EXPECT_FALSE(Format({{"host", "h"}, {"path", too_long}}).has_value());
  // The bound is on the raw value, not on the escaped one.
  const std::string control(300, '"');
  EXPECT_TRUE(Format({{"host", "h"}, {"path", control}}).has_value());
}

TEST(HtmlCacheDecisionTest, Fnv1a64Vectors) {
  EXPECT_EQ(HtmlCacheFnv1a64(""), 0xcbf29ce484222325ULL);
  EXPECT_EQ(HtmlCacheFnv1a64("a"), 0xaf63dc4c8601ec8cULL);
  EXPECT_EQ(HtmlCacheFnv1a64("foobar"), 0x85944171f73967e8ULL);
  EXPECT_NE(HtmlCacheFnv1a64("a"), HtmlCacheFnv1a64("b"));
  // Bytes at and above 0x80 hash as unsigned bytes.
  EXPECT_EQ(HtmlCacheFnv1a64(std::string_view("\xff", 1)),
            0xaf64724c8602eb6eULL);
  // NUL bytes are hashed, not terminators.
  EXPECT_NE(HtmlCacheFnv1a64(std::string_view("a\0", 2)),
            HtmlCacheFnv1a64("a"));
}

TEST(HtmlCacheDecisionTest, DedupKey) {
  const Fields f = {{"ts_ms", "5"},
                    {"age_s", "9"},
                    {"host", "h.example"},
                    {"path", "/p"},
                    {"mode", "off"},
                    {"action", "refuse"},
                    {"reason", "vary:accept"},
                    {"sidecar", "eligible_empty"},
                    {"before", "abcd"},
                    {"status", "200"}};
  EXPECT_EQ(HtmlCacheDecisionDedupKey(f),
            std::string("h.example\x1f/p\x1foff\x1frefuse\x1fvary:accept\x1f"
                        "eligible_empty\x1f"
                        "abcd"));
  // Order of the input does not matter; the ignored fields do not either.
  const Fields g = {{"before", "abcd"},        {"sidecar", "eligible_empty"},
                    {"reason", "vary:accept"}, {"action", "refuse"},
                    {"mode", "off"},           {"path", "/p"},
                    {"host", "h.example"}};
  EXPECT_EQ(HtmlCacheDecisionDedupKey(f), HtmlCacheDecisionDedupKey(g));
  // A missing component is empty, so the separators still count.
  const Fields sparse = {{"host", "h"}};
  EXPECT_EQ(HtmlCacheDecisionDedupKey(sparse),
            std::string("h\x1f\x1f\x1f\x1f\x1f\x1f"));
  const Fields changed = {
      {"host", "h.example"},     {"path", "/p"},
      {"mode", "off"},           {"action", "refuse"},
      {"reason", "vary:accept"}, {"sidecar", "eligible_empty"},
      {"before", "ffff"}};
  EXPECT_NE(HtmlCacheDecisionDedupKey(f), HtmlCacheDecisionDedupKey(changed));
}

TEST(HtmlCacheDecisionTest, VhostAddedIsNamesOnlyAndLast) {
  EXPECT_TRUE(HtmlCacheDecisionValueFits("vhost_added", "x-t"));
  EXPECT_TRUE(HtmlCacheDecisionValueFits(
      "vhost_added", "x-frame-options, strict-transport-security"));
  EXPECT_FALSE(HtmlCacheDecisionValueFits("vhost_added", "x-t: 1"));
  EXPECT_FALSE(HtmlCacheDecisionValueFits("vhost_added", "x t"));
  EXPECT_FALSE(HtmlCacheDecisionValueFits("vhost_added", ""));
  EXPECT_EQ(Format({{"vhost_added", "x-t"}, {"age_s", "1"}}).value(),
            "{\"age_s\":1,\"vhost_added\":\"x-t\"}");
}

TEST(HtmlCacheDecisionTest, FrontEndEngineReasonsFit) {
  for (std::string_view reason :
       {"purge_race", "budget", "sidecar_unpaired", "url_disagree",
        "transport_conflict", "method", "range", "cors", "withheld", "skew",
        "no_transform"}) {
    EXPECT_TRUE(HtmlCacheDecisionValueFits("reason", reason)) << reason;
  }
}

TEST(HtmlCacheDecisionTest, CApiGateAdmitsServeLinesAndKeepsReportLinesNarrow) {
  const auto admits = [](const Fields& f) {
    return HtmlCacheDecisionCApiAdmits(f);
  };
  // Every store-side line the old gate admitted is still admitted.
  EXPECT_TRUE(admits({{"mode", "off"}, {"action", "refuse"}}));
  EXPECT_TRUE(admits({{"reason", "headers:x-request-id"}}));
  EXPECT_TRUE(admits({{"request", "authorization,cookie"}}));
  // Report-only may now say the request asked for Markdown, and carry a
  // front-end store refusal.
  EXPECT_TRUE(
      admits({{"mode", "report-only"}, {"request", "accept_markdown"}}));
  EXPECT_TRUE(admits({{"mode", "report-only"}, {"reason", "url_disagree"}}));
  EXPECT_TRUE(admits({{"reason", "budget"}}));
  EXPECT_TRUE(admits({{"reason", "purge_race"}}));
  EXPECT_TRUE(admits({{"vhost_added", "x-t"}}));
  // Report-only still states no serve fact.
  EXPECT_FALSE(admits({{"mode", "report-only"}, {"reason", "fresh"}}));
  EXPECT_FALSE(admits({{"reason", "sidecar_unpaired"}}));
  EXPECT_FALSE(admits({{"reason", "transport_conflict"}}));
  EXPECT_FALSE(admits({{"request", "no_cache"}}));
  EXPECT_FALSE(admits({{"action", "would_serve"}}));
  EXPECT_FALSE(admits({{"age_s", "1"}}));
  EXPECT_FALSE(admits({{"variant", "markdown"}}));
  // A serve mode admits the serve side.
  for (std::string_view mode : {"serve-variants", "serve-cached-html"}) {
    EXPECT_TRUE(admits({{"mode", mode},
                        {"action", "served_markdown"},
                        {"reason", "sidecar_unpaired"},
                        {"request", "no_cache,accept_markdown"},
                        {"variant", "markdown"},
                        {"binding", "stale"},
                        {"age_s", "3"}}))
        << mode;
  }
  // The C++ store-side gate itself is unchanged.
  EXPECT_FALSE(
      HtmlCacheDecisionIsStoreSide(Fields{{"request", "accept_markdown"}}));
}

}  // namespace
}  // namespace pagespeed
