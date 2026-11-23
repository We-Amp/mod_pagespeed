// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "src/worker/subresource_gate.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"
#include "src/browser/agent_fetcher.h"

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <thread>
#endif
#include "lib/classify/alternate_metadata.h"
#include "lib/net/upstream_pin.h"

namespace pagespeed {
namespace {

// A server whose answers the test sets per absolute URL; a URL it has no
// answer for gets no response at all.
class FakeServer {
 public:
  SubresourceGate::HeadFn Head() {
    return [this](const std::string& origin,
                  const std::string& url) -> std::optional<int> {
      origins.push_back(origin);
      asked.push_back(url);
      auto it = status.find(url);
      if (it == status.end()) return std::nullopt;
      return it->second;
    };
  }
  std::map<std::string, int> status;
  std::vector<std::string> origins;
  std::vector<std::string> asked;
};

class SubresourceGateTest : public ::testing::Test {
 protected:
  SubresourceGateTest()
      : gate_(server_.Head(), [this] { return now_ms_; }, Options()) {}

  static SubresourceGate::Options Options() {
    SubresourceGate::Options o;
    o.admitted_ttl_ms = 300 * 1000;
    o.refused_ttl_ms = 60 * 1000;
    return o;
  }

  // The read holds views, so the URL must outlive it: a string_view of a
  // literal does, a std::string temporary bound to a const reference does not.
  static SubresourceRead Read(std::string_view url) {
    SubresourceRead r;
    r.site = "";
    r.scheme = "https";
    r.host = "example.com";
    r.url = url;
    r.page_host = "example.com";
    r.host_fetch_allowed = true;
    return r;
  }

  FakeServer server_;
  std::int64_t now_ms_ = 1000;
  SubresourceGate gate_;
};

TEST_F(SubresourceGateTest, ServedToAnonymousClientIsAdmitted) {
  server_.status["https://example.com/open/a.css"] = 200;
  EXPECT_EQ(gate_.Check(Read("/open/a.css"), true),
            SubresourceVerdict::kAdmitted);
  ASSERT_EQ(server_.asked.size(), 1u);
  EXPECT_EQ(server_.origins[0], "https://example.com");
  EXPECT_EQ(server_.asked[0], "https://example.com/open/a.css");
  EXPECT_EQ(gate_.heads_sent(), 1u);
  EXPECT_EQ(gate_.refusals(), 0u);
}

TEST_F(SubresourceGateTest, RefusedToAnonymousClientIsRefused) {
  for (int code : {401, 403, 404, 500, 204, 302}) {
    SubresourceGate gate(server_.Head(), [this] { return now_ms_; });
    const std::string url = "/held/" + std::to_string(code) + ".css";
    server_.status["https://example.com" + url] = code;
    EXPECT_EQ(gate.Check(Read(url), true), SubresourceVerdict::kRefused)
        << code;
    EXPECT_FALSE(gate.Admitted(Read(url), true)) << code;
  }
}

TEST_F(SubresourceGateTest, NoAnswerIsUnknownAndNotAdmitted) {
  EXPECT_EQ(gate_.Check(Read("/silent.css"), true),
            SubresourceVerdict::kUnknown);
  EXPECT_FALSE(gate_.Admitted(Read("/silent.css"), true));
  // Remembered like a refusal: no second request inside the period.
  EXPECT_EQ(server_.asked.size(), 1u);
  EXPECT_EQ(gate_.refusals(), 1u);
}

TEST_F(SubresourceGateTest, AnswersAreRememberedPerUrlUntilTheyExpire) {
  server_.status["https://example.com/a.css"] = 200;
  server_.status["https://example.com/b.css"] = 403;
  ASSERT_TRUE(gate_.Admitted(Read("/a.css"), true));
  ASSERT_FALSE(gate_.Admitted(Read("/b.css"), true));
  ASSERT_EQ(server_.asked.size(), 2u);

  // Within both periods: no new request, same answers, even when the server
  // has changed its mind.
  server_.status["https://example.com/a.css"] = 403;
  server_.status["https://example.com/b.css"] = 200;
  now_ms_ += 59 * 1000;
  EXPECT_TRUE(gate_.Admitted(Read("/a.css"), true));
  EXPECT_FALSE(gate_.Admitted(Read("/b.css"), true));
  EXPECT_EQ(server_.asked.size(), 2u);

  // The refusal expires first and is asked again.
  now_ms_ += 2 * 1000;
  EXPECT_TRUE(gate_.Admitted(Read("/b.css"), true));
  EXPECT_EQ(server_.asked.size(), 3u);
  EXPECT_TRUE(gate_.Admitted(Read("/a.css"), true));
  EXPECT_EQ(server_.asked.size(), 3u);

  // Then the admission.
  now_ms_ += 300 * 1000;
  EXPECT_FALSE(gate_.Admitted(Read("/a.css"), true));
  EXPECT_EQ(server_.asked.size(), 4u);
}

TEST_F(SubresourceGateTest, MemoIsKeyedBySiteSchemeHostAndUrl) {
  server_.status["https://example.com/a.css"] = 200;
  ASSERT_TRUE(gate_.Admitted(Read("/a.css"), true));
  SubresourceRead other_site = Read("/a.css");
  other_site.site = "0123456789abcdef";
  EXPECT_FALSE(gate_.Admitted(other_site, /*may_fetch=*/false));
  SubresourceRead other_scheme = Read("/a.css");
  other_scheme.scheme = "http";
  EXPECT_FALSE(gate_.Admitted(other_scheme, /*may_fetch=*/false));
  SubresourceRead other_query = Read("/a.css?v=2");
  EXPECT_FALSE(gate_.Admitted(other_query, /*may_fetch=*/false));
  // Host case and a default port name the same host.
  SubresourceRead same_host = Read("/a.css");
  same_host.host = "Example.COM:443";
  EXPECT_TRUE(gate_.Admitted(same_host, /*may_fetch=*/false));
  EXPECT_EQ(server_.asked.size(), 1u);
}

TEST_F(SubresourceGateTest, MemoOnlyCallerNeverAsks) {
  server_.status["https://example.com/a.css"] = 200;
  EXPECT_EQ(gate_.Check(Read("/a.css"), /*may_fetch=*/false),
            SubresourceVerdict::kUnknown);
  EXPECT_TRUE(server_.asked.empty());
  ASSERT_TRUE(gate_.Admitted(Read("/a.css"), /*may_fetch=*/true));
  EXPECT_TRUE(gate_.Admitted(Read("/a.css"), /*may_fetch=*/false));
  EXPECT_EQ(server_.asked.size(), 1u);
}

TEST_F(SubresourceGateTest, AsksOnlyThePageHostForAPermittedSender) {
  server_.status["https://example.com/a.css"] = 200;
  server_.status["https://cdn.example.net/a.css"] = 200;

  SubresourceRead not_permitted = Read("/a.css");
  not_permitted.host_fetch_allowed = false;
  EXPECT_EQ(gate_.Check(not_permitted, true), SubresourceVerdict::kUnknown);

  SubresourceRead other_host = Read("/a.css");
  other_host.host = "cdn.example.net";
  EXPECT_EQ(gate_.Check(other_host, true), SubresourceVerdict::kUnknown);

  SubresourceRead no_page_host = Read("/a.css");
  no_page_host.page_host = "";
  EXPECT_EQ(gate_.Check(no_page_host, true), SubresourceVerdict::kUnknown);

  SubresourceRead no_host = Read("/a.css");
  no_host.host = "";
  no_host.page_host = "";
  EXPECT_EQ(gate_.Check(no_host, true), SubresourceVerdict::kUnknown);

  EXPECT_TRUE(server_.asked.empty());
  // Not remembered: a permitted sender asks afterwards.
  EXPECT_TRUE(gate_.Admitted(Read("/a.css"), true));
  EXPECT_EQ(server_.asked.size(), 1u);
}

TEST_F(SubresourceGateTest, OnlyPathKeysAreAsked) {
  for (const char* url : {"", "a.css", "//evil.example/a.css",
                          "https://example.com/a.css", "javascript:x"}) {
    EXPECT_EQ(gate_.Check(Read(url), true), SubresourceVerdict::kUnknown)
        << url;
  }
  SubresourceRead bad_scheme = Read("/a.css");
  bad_scheme.scheme = "ftp";
  EXPECT_EQ(gate_.Check(bad_scheme, true), SubresourceVerdict::kUnknown);
  EXPECT_TRUE(server_.asked.empty());
}

TEST_F(SubresourceGateTest, EmptySchemeIsHttpsAndPortIsKept) {
  server_.status["https://example.com:8443/a.css"] = 200;
  SubresourceRead r = Read("/a.css");
  r.scheme = "";
  r.host = "example.com:8443";
  r.page_host = "EXAMPLE.com:8443";
  EXPECT_TRUE(gate_.Admitted(r, true));
  ASSERT_EQ(server_.origins.size(), 1u);
  EXPECT_EQ(server_.origins[0], "https://example.com:8443");
}

TEST_F(SubresourceGateTest, FullMemoDropsExpiredThenEverything) {
  SubresourceGate::Options o;
  o.max_entries = 2;
  o.refused_ttl_ms = 10;
  SubresourceGate gate(server_.Head(), [this] { return now_ms_; }, o);
  server_.status["https://example.com/a.css"] = 200;
  server_.status["https://example.com/b.css"] = 200;
  server_.status["https://example.com/c.css"] = 200;
  ASSERT_TRUE(gate.Admitted(Read("/a.css"), true));
  ASSERT_TRUE(gate.Admitted(Read("/b.css"), true));
  ASSERT_TRUE(gate.Admitted(Read("/c.css"), true));  // full: cleared
  EXPECT_FALSE(gate.Admitted(Read("/a.css"), false));
  EXPECT_TRUE(gate.Admitted(Read("/c.css"), false));
}

TEST_F(SubresourceGateTest, TestOverrideDecidesForEveryGate) {
  SubresourceGate::SetVerdictOverrideForTest(
      [](const SubresourceRead& r) -> std::optional<SubresourceVerdict> {
        if (r.url == "/forced.css") return SubresourceVerdict::kAdmitted;
        return std::nullopt;
      });
  SubresourceRead r = Read("/forced.css");
  r.host_fetch_allowed = false;
  EXPECT_TRUE(gate_.Admitted(r, false));
  EXPECT_EQ(gate_.Check(Read("/other.css"), false),
            SubresourceVerdict::kUnknown);
  SubresourceGate::SetVerdictOverrideForTest(nullptr);
  EXPECT_FALSE(gate_.Admitted(r, false));
  EXPECT_TRUE(server_.asked.empty());
}

// An entry the front end recorded from an anonymous request, read from the
// page's own site keyspace.
SubresourceRead StampedRead(SubresourceRead r, std::uint8_t extra_flags = 0) {
  r.entry_read = true;
  r.entry_flags = AlternateMetadata::kFlagAnonymousRecorded | extra_flags;
  r.entry_site = r.site;
  return r;
}

TEST_F(SubresourceGateTest, StampedEntryUnderUnconfirmedHostIsAdmitted) {
  server_.status["https://example.com/a.css"] = 403;  // never asked
  SubresourceRead r =
      StampedRead(Read("/a.css"), AlternateMetadata::kFlagHeadersClassified);
  r.site = "0123456789abcdef";
  r.entry_site = r.site;
  r.host_fetch_allowed = false;
  EXPECT_EQ(gate_.Check(r, /*may_fetch=*/true), SubresourceVerdict::kAdmitted);
  // A memo-only caller gets the same answer: the entry is the evidence.
  EXPECT_EQ(gate_.Check(r, /*may_fetch=*/false), SubresourceVerdict::kAdmitted);
  EXPECT_TRUE(server_.asked.empty());
  EXPECT_EQ(gate_.heads_sent(), 0u);
  EXPECT_EQ(gate_.refusals(), 0u);
  // Nothing is remembered for the URL: the same URL without the entry is
  // still unknown.
  SubresourceRead bare = Read("/a.css");
  bare.site = r.site;
  bare.host_fetch_allowed = false;
  EXPECT_EQ(gate_.Check(bare, true), SubresourceVerdict::kUnknown);
}

TEST_F(SubresourceGateTest, UnstampedEntryUnderUnconfirmedHostIsAbsent) {
  server_.status["https://example.com/a.css"] = 200;
  SubresourceRead r = Read("/a.css");
  r.host_fetch_allowed = false;
  r.entry_read = true;
  r.entry_flags = AlternateMetadata::kFlagHeadersClassified |
                  AlternateMetadata::kFlagWorkerProcessed;
  r.entry_site = r.site;
  EXPECT_EQ(gate_.Check(r, true), SubresourceVerdict::kUnknown);
  EXPECT_FALSE(gate_.Admitted(r, true));
  EXPECT_TRUE(server_.asked.empty());
}

TEST_F(SubresourceGateTest, StampedEntryFromAnotherSiteKeyspaceIsNotAdmitted) {
  SubresourceRead r = StampedRead(Read("/a.css"));
  r.site = "0123456789abcdef";
  r.entry_site = "fedcba9876543210";
  r.host_fetch_allowed = false;
  EXPECT_EQ(gate_.Check(r, true), SubresourceVerdict::kUnknown);
  EXPECT_TRUE(server_.asked.empty());
  // The default keyspace and a named one are different keyspaces too.
  SubresourceRead to_default = StampedRead(Read("/a.css"));
  to_default.entry_site = "0123456789abcdef";
  to_default.host_fetch_allowed = false;
  EXPECT_EQ(gate_.Check(to_default, true), SubresourceVerdict::kUnknown);
  // Under a confirmed host such an entry is asked about like any other.
  server_.status["https://example.com/a.css"] = 403;
  r.host_fetch_allowed = true;
  EXPECT_EQ(gate_.Check(r, true), SubresourceVerdict::kRefused);
  EXPECT_EQ(server_.asked.size(), 1u);
}

TEST_F(SubresourceGateTest, StampedEntryUnderConfirmedHostIsAskedAsBefore) {
  // The host's answer decides, stamp or not.
  server_.status["https://example.com/a.css"] = 403;
  server_.status["https://example.com/b.css"] = 200;
  SubresourceRead r = StampedRead(Read("/a.css"));
  ASSERT_TRUE(r.host_fetch_allowed);
  EXPECT_EQ(gate_.Check(r, /*may_fetch=*/true), SubresourceVerdict::kRefused);
  SubresourceRead b = StampedRead(Read("/b.css"));
  EXPECT_EQ(gate_.Check(b, /*may_fetch=*/true), SubresourceVerdict::kAdmitted);
  ASSERT_EQ(server_.asked.size(), 2u);
  EXPECT_EQ(server_.asked[0], "https://example.com/a.css");
  EXPECT_EQ(server_.asked[1], "https://example.com/b.css");
  EXPECT_EQ(gate_.heads_sent(), 2u);
  // A memo-only caller gets the remembered answer, not the stamp.
  SubresourceRead c = StampedRead(Read("/c.css"));
  EXPECT_EQ(gate_.Check(c, /*may_fetch=*/false), SubresourceVerdict::kUnknown);
  EXPECT_EQ(gate_.Check(r, /*may_fetch=*/false), SubresourceVerdict::kRefused);
  EXPECT_EQ(server_.asked.size(), 2u);
}

// ---- shared-copy reads (the agent render's document, /llms.txt) ------------

struct FakeSharedHead {
  SubresourceGate::SharedCopyHeadFn Head() {
    return [this](const std::string& origin, const std::string& url,
                  const std::vector<PinnedUpstream>& private_origins)
               -> std::optional<int> {
      asked.push_back(url);
      origins_seen = private_origins.size();
      (void)origin;
      auto it = status.find(url);
      if (it == status.end()) return std::nullopt;
      return it->second;
    };
  }
  std::map<std::string, int> status;
  std::vector<std::string> asked;
  std::size_t origins_seen = 0;
};

TEST_F(SubresourceGateTest, SharedCopyReadAsksTheSharedHeadWithTheOrigins) {
  FakeSharedHead shared;
  gate_.SetSharedCopyHead(shared.Head());
  shared.status["https://example.com/a.css"] = 200;
  std::vector<PinnedUpstream> named = {*ParseUpstream("https://example.com")};
  SubresourceRead r = Read("/a.css");
  r.shared_copy = true;
  r.private_address_origins = &named;
  EXPECT_EQ(gate_.Check(r, true), SubresourceVerdict::kAdmitted);
  ASSERT_EQ(shared.asked.size(), 1u);
  EXPECT_EQ(shared.origins_seen, 1u);
  EXPECT_TRUE(server_.asked.empty()) << "the ordinary HEAD is not used";
}

TEST_F(SubresourceGateTest, SharedCopyReadWithoutASharedHeadIsNotAdmitted) {
  server_.status["https://example.com/a.css"] = 200;
  SubresourceRead r = Read("/a.css");
  r.shared_copy = true;
  EXPECT_EQ(gate_.Check(r, true), SubresourceVerdict::kUnknown);
  EXPECT_TRUE(server_.asked.empty());
}

// The ordinary path admits what the shared-copy HEAD refuses (a private
// answer, an internal address): each keeps its own verdict.
TEST_F(SubresourceGateTest, SharedCopyAndOrdinaryVerdictsAreKeptApart) {
  FakeSharedHead shared;
  gate_.SetSharedCopyHead(shared.Head());
  server_.status["https://example.com/a.css"] = 200;  // ordinary: 200
  // shared: no answer (refused by the shared-copy rules)
  SubresourceRead plain = Read("/a.css");
  SubresourceRead strict = Read("/a.css");
  strict.shared_copy = true;
  EXPECT_TRUE(gate_.Admitted(plain, true));
  EXPECT_FALSE(gate_.Admitted(strict, true));
  // Memo only: each answers from its own entry.
  EXPECT_TRUE(gate_.Admitted(plain, false));
  EXPECT_FALSE(gate_.Admitted(strict, false));
  EXPECT_EQ(server_.asked.size(), 1u);
  EXPECT_EQ(shared.asked.size(), 1u);
  // And the other way round: an ordinary refusal does not leak into a
  // shared-copy admission or back.
  shared.status["https://example.com/b.css"] = 200;
  SubresourceRead strict_b = Read("/b.css");
  strict_b.shared_copy = true;
  EXPECT_TRUE(gate_.Admitted(strict_b, true));
  EXPECT_FALSE(gate_.Admitted(Read("/b.css"), false));  // never asked: unknown
}

// A stamped entry is admitted without asking for a shared-copy read only
// when its recorded Cache-Control is neither no-store nor private; ordinary
// reads keep admitting it.
TEST_F(SubresourceGateTest, StampedEntryRules) {
  for (std::uint16_t cc :
       {AlternateMetadata::kCCOriginNoStore,
        AlternateMetadata::kCCOriginPrivate,
        static_cast<std::uint16_t>(AlternateMetadata::kCCOriginPrivate |
                                   AlternateMetadata::kCCOriginPublic)}) {
    SubresourceRead r = Read("/stamped.css");
    r.host_fetch_allowed = false;
    r.entry_read = true;
    r.entry_flags = AlternateMetadata::kFlagAnonymousRecorded;
    r.entry_site = r.site;
    r.entry_cc_flags = cc;
    EXPECT_NE(gate_.Check(r, true), SubresourceVerdict::kAdmitted) << cc;
    r.shared_copy = true;
    EXPECT_NE(gate_.Check(r, true), SubresourceVerdict::kAdmitted) << cc;
  }
  SubresourceRead clean = Read("/stamped.css");
  clean.host_fetch_allowed = false;
  clean.entry_read = true;
  clean.entry_flags = AlternateMetadata::kFlagAnonymousRecorded;
  clean.entry_site = clean.site;
  clean.entry_cc_flags = AlternateMetadata::kCCOriginPublic;
  clean.shared_copy = true;
  EXPECT_EQ(gate_.Check(clean, true), SubresourceVerdict::kAdmitted);
  EXPECT_TRUE(server_.asked.empty());
}

// The production shared-copy HEAD refuses an origin at a loopback or private
// address (also inside an IPv6 address) before anything is sent, and logs it
// once per origin naming the flag.
TEST(SharedCopyHead, AddressRulesAndLog) {
  std::vector<std::string> logged;
  auto head = MakeSharedCopyHeadFn(
      [&logged](const std::string& m) { logged.push_back(m); });
  for (const char* origin : {"http://127.0.0.1:9", "http://[64:ff9b::7f00:1]:9",
                             "http://10.0.0.1:9"}) {
    const std::string url = std::string(origin) + "/a.css";
    // A refusal, not "no answer".
    EXPECT_EQ(head(origin, url, {}), std::optional<int>(kRefusedStatus))
        << origin;
    EXPECT_EQ(head(origin, url, {}), std::optional<int>(kRefusedStatus))
        << origin;
  }
  ASSERT_EQ(logged.size(), 3u);
  for (const std::string& m : logged) {
    EXPECT_NE(m.find("--agent-render-private-origins"), std::string::npos) << m;
  }
}

// ---- the gate's HEAD: answer rules on every call ---------------------------

// A fake curl answering per URL path with the given header block.
SpawnCurlFn FakeCurl(std::map<std::string, std::string> by_path,
                     std::vector<std::string>* urls) {
  return [by_path, urls](const std::vector<std::string>& argv,
                         std::size_t) -> CurlSpawnOutcome {
    std::string url;
    for (std::size_t i = 0; i + 1 < argv.size(); ++i) {
      if (argv[i] == "--url") url = argv[i + 1];
    }
    if (urls != nullptr) urls->push_back(url);
    CurlSpawnOutcome o;
    o.spawned = true;
    o.exit_code = 0;
    for (const auto& [path, headers] : by_path) {
      if (url.size() >= path.size() &&
          url.compare(url.size() - path.size(), path.size(), path) == 0) {
        o.output = "HTTP/1.1 200 OK\r\n" + headers + "\r\n";
      }
    }
    return o;
  };
}

ResolveHostFn Loopback() {
  return [](std::string_view) { return std::vector<std::string>{"127.0.0.1"}; };
}

// The ordinary HEAD: an answer marked private or no-store, or setting a
// cookie, does not admit; a cacheable answer does, also from an origin on
// the same host (loopback).
TEST(AnonymousHead, AnswerRules) {
  std::vector<std::string> urls;
  auto head = MakeAnonymousHeadFn(
      FakeCurl({{"/public.css", "Cache-Control: public, max-age=600\r\n"},
                {"/plain.css", "Content-Type: text/css\r\n"},
                {"/private.css", "Cache-Control: private, max-age=600\r\n"},
                {"/nostore.css", "Cache-Control: no-store\r\n"},
                {"/cookie.css", "Set-Cookie: lb=1; Path=/\r\n"}},
               &urls),
      Loopback());
  const std::string o = "https://example.com";
  EXPECT_EQ(head(o, o + "/public.css"), std::optional<int>(200));
  EXPECT_EQ(head(o, o + "/plain.css"), std::optional<int>(200));
  EXPECT_EQ(head(o, o + "/private.css"), std::optional<int>(kRefusedStatus));
  EXPECT_EQ(head(o, o + "/nostore.css"), std::optional<int>(kRefusedStatus));
  EXPECT_EQ(head(o, o + "/cookie.css"), std::optional<int>(kRefusedStatus));
  EXPECT_EQ(urls.size(), 5u) << "every request was sent (no address refusal)";
}

// Through the gate: only the cacheable answers admit.
TEST(AnonymousHead, GateAdmitsOnlyCacheableAnswers) {
  SubresourceGate gate(
      MakeAnonymousHeadFn(FakeCurl({{"/a.css", "Cache-Control: max-age=60\r\n"},
                                    {"/b.css", "Cache-Control: private\r\n"},
                                    {"/c.css", "Set-Cookie: s=1\r\n"}},
                                   nullptr),
                          Loopback()),
      [] { return std::int64_t{0}; });
  auto read = [](std::string_view url) {
    SubresourceRead r;
    r.scheme = "https";
    r.host = "example.com";
    r.url = url;
    r.page_host = "example.com";
    r.host_fetch_allowed = true;
    return r;
  };
  EXPECT_TRUE(gate.Admitted(read("/a.css"), true));
  EXPECT_FALSE(gate.Admitted(read("/b.css"), true));
  EXPECT_FALSE(gate.Admitted(read("/c.css"), true));
}

// A refused answer is a refusal (kRefused, counted), not "no answer"; the
// first refusal per origin is logged with its reason; the per-reason
// counters move.
TEST(AnonymousHead, RefusalVerdictLogAndCounters) {
  std::vector<std::string> logged;
  const SharedCopyRefusalCounts before = GetSharedCopyRefusalCounts();
  SubresourceGate gate(
      MakeAnonymousHeadFn(
          FakeCurl({{"/v.css", "Vary: Cookie\r\n"},
                    {"/w.css", "Vary: Accept-Encoding\r\n"},
                    {"/k.css", "Set-Cookie: AWSALB=x\r\n"},
                    {"/n.css", "Cache-Control: no-store\r\n"},
                    {"/p.css", "Cache-Control: private\r\n"}},
                   nullptr),
          Loopback(), [&logged](const std::string& m) { logged.push_back(m); }),
      [] { return std::int64_t{0}; });
  auto read = [](std::string_view url, std::string_view host) {
    SubresourceRead r;
    r.scheme = "https";
    r.host = host;
    r.url = url;
    r.page_host = host;
    r.host_fetch_allowed = true;
    return r;
  };
  EXPECT_EQ(gate.Check(read("/v.css", "refusal-a.example"), true),
            SubresourceVerdict::kRefused);
  EXPECT_EQ(gate.Check(read("/w.css", "refusal-a.example"), true),
            SubresourceVerdict::kAdmitted);
  EXPECT_EQ(gate.Check(read("/k.css", "refusal-a.example"), true),
            SubresourceVerdict::kRefused);
  EXPECT_EQ(gate.Check(read("/n.css", "refusal-b.example"), true),
            SubresourceVerdict::kRefused);
  EXPECT_EQ(gate.Check(read("/p.css", "refusal-b.example"), true),
            SubresourceVerdict::kRefused);
  EXPECT_EQ(gate.refusals(), 4u);
  ASSERT_EQ(logged.size(), 2u);  // once per origin
  EXPECT_NE(logged[0].find("vary"), std::string::npos) << logged[0];
  EXPECT_NE(logged[1].find("no-store"), std::string::npos) << logged[1];
  const SharedCopyRefusalCounts after = GetSharedCopyRefusalCounts();
  EXPECT_EQ(after.vary - before.vary, 1u);
  EXPECT_EQ(after.cookie - before.cookie, 1u);
  EXPECT_EQ(after.no_store - before.no_store, 1u);
  EXPECT_EQ(after.private_answer - before.private_answer, 1u);
}

#ifndef _WIN32
// The production HEAD (real curl) against a server on this host: the
// loopback origin is asked, and only its cacheable answer admits.
TEST(AnonymousHead, RealCurlAgainstLoopbackServer) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(fd, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  ASSERT_EQ(bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
  ASSERT_EQ(listen(fd, 8), 0);
  socklen_t len = sizeof(addr);
  ASSERT_EQ(getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len), 0);
  const int port = ntohs(addr.sin_port);
  std::atomic<int> served{0};
  std::thread server([fd, &served] {
    for (int i = 0; i < 3; ++i) {
      int c = accept(fd, nullptr, nullptr);
      if (c < 0) return;
      char buf[2048];
      ssize_t n = read(c, buf, sizeof(buf) - 1);
      std::string req(buf, n > 0 ? static_cast<std::size_t>(n) : 0);
      std::string h = "Cache-Control: max-age=60\r\n";
      if (req.find("/private.css") != std::string::npos) {
        h = "Cache-Control: private\r\n";
      } else if (req.find("/cookie.css") != std::string::npos) {
        h = "Set-Cookie: s=1\r\n";
      }
      std::string resp = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n" + h +
                         "Connection: close\r\n\r\n";
      (void)!write(c, resp.data(), resp.size());
      close(c);
      ++served;
    }
  });
  auto head = MakeAnonymousHeadFn();
  const std::string o = "http://127.0.0.1:" + std::to_string(port);
  EXPECT_EQ(head(o, o + "/public.css"), std::optional<int>(200));
  EXPECT_EQ(head(o, o + "/private.css"), std::optional<int>(kRefusedStatus));
  EXPECT_EQ(head(o, o + "/cookie.css"), std::optional<int>(kRefusedStatus));
  server.join();
  close(fd);
  EXPECT_EQ(served.load(), 3);
}
#endif

}  // namespace
}  // namespace pagespeed
