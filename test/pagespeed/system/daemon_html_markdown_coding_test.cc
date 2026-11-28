// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

// The HTML cache engine's coded Markdown answers (C API 1.16), against the
// injected-verdict library: which coded copy the engine reads for the order
// the library chose from Accept-Encoding, the request it sends, and the
// answer it builds from the verdict.  Every policy outcome (the order, the
// binding, the verdict) is injected; these cases assert only that the engine
// asks for the right thing and obeys the answer.

#include <memory>
#include <vector>

#include "net/instaweb/rewriter/public/rewrite_stats.h"
#include "pagespeed/kernel/base/null_mutex.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/cache/purge_set.h"
#include "pagespeed/kernel/util/platform.h"
#include "pagespeed/kernel/util/simple_stats.h"
#include "pagespeed/system/daemon_abi.h"
#include "pagespeed/system/daemon_html_engine.h"
#include "pagespeed/system/daemon_serve_arm.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/base/mock_message_handler.h"
#include "test/pagespeed/kernel/base/mock_timer.h"
#include "test/pagespeed/system/fake_daemon_volume.h"
#include "test/pagespeed/system/injected_verdict_daemon_abi.h"

namespace net_instaweb {
namespace {

const int64 kNowMs = 1790000000000LL;
const char kSite[] =
    "2007f4b98af3e43f6c81272c8606ea77a35978dd7a5c0ee623f15036edac5831";
const char kUrl[] = "https://www.example.test/page";
const char kPlain[] = "# Page\n\nThe plain Markdown copy.\n";
const char kBr[] = "BR-CODED-BYTES";
const char kGzip[] = "GZIP-CODED-BYTES-LONGER";
const char kPlainTag[] = "W/\"psm-0123456789abcdef\"";
const char kGzipTag[] = "W/\"psm-0123456789abcdeg\"";
const char kBrTag[] = "W/\"psm-0123456789abcdeb\"";

class Source : public DaemonHtmlPurgeSource {
 public:
  bool IsUrlValidIgnoringGlobal(const GoogleString& url,
                                int64 time_ms) const override {
    return purge_set.IsValidIgnoringGlobal(url, time_ms);
  }
  int64 GlobalInvalidationTimestampMs() const override {
    return purge_set.global_invalidation_timestamp_ms();
  }
  bool FlushFileMtimeMs(int64*) const override { return false; }
  PurgeSet purge_set{1 << 16};
};

// Keeps what the engine asked it to emit, and whether the no-compress mark
// came first.
class RecordingPort : public DaemonHtmlPort {
 public:
  bool ClearAndEmitAllowlisted(const DaemonHtmlServeResult& result,
                               bool head) override {
    emitted.push_back(result);
    heads.push_back(head);
    marked_before_emit.push_back(marked);
    return true;
  }
  void MarkNoCompress() override { marked = true; }
  void RemoveClientConditionals() override {}
  void RunBlocking(DaemonHtmlCommit commit) override { commit.Run(); }
  bool CodedAnswerReplacedByPlain() const override { return replaced; }
  bool MaySendPlainInstead() const override { return standby; }

  bool marked = false;
  // Answer CodedAnswerReplacedByPlain and MaySendPlainInstead.
  bool replaced = false;
  bool standby = false;
  std::vector<DaemonHtmlServeResult> emitted;
  std::vector<bool> heads;
  std::vector<bool> marked_before_emit;
};

class DaemonHtmlMarkdownCodingTest : public testing::Test {
 protected:
  DaemonHtmlMarkdownCodingTest()
      : thread_system_(Platform::CreateThreadSystem()),
        timer_(new NullMutex, kNowMs),
        stats_(thread_system_.get()),
        limiter_(1),
        abi_(&volume_) {
    RewriteStats::InitStats(&stats_);
    config_.mode = DaemonHtmlMode::kServeVariants;
    config_.agent_optimize = true;
    config_.host_names.primary = "www.example.test";
    config_.site = kSite;
    HtmlServeInjection* in = abi_.mutable_injection();
    in->has_set_15 = true;
    in->has_set_16 = true;
    in->wants_markdown = 1;
    // A page stored under this configuration's credentialed policy, as a
    // port's configuration load stamps it.
    config_.policy_stamp = kStamp;
    in->facts.flags = kPsHtmlFactsPresent | kPsHtmlFactsBindingLive |
                      kPsHtmlFactsStoreV2 | kPsHtmlFactsAnonymousRecorded;
    in->facts.entry_policy_stamp = kStamp;
    in->facts.stored_at = 1789999990;
    in->facts.lifetime_s = 300;
    memset(in->facts.binding_hash, 0x11, sizeof(in->facts.binding_hash));
    memset(in->facts.markdown_hash, 0x22, sizeof(in->facts.markdown_hash));
    in->etag = kPlainTag;
    in->coded_etag[kPsMarkdownEncodingGzip] = kGzipTag;
    in->coded_etag[kPsMarkdownEncodingBr] = kBrTag;
    in->inm_compare = true;
    Put(kPsSentinelOriginal, "<html><body>page</body></html>");
    Put(kPsSentinelAgentMarkdown, kPlain);
  }

  void Put(uint8_t id, const char* body) {
    FakeVolumeEntry e;
    e.body = body;
    e.alternate_id = id;
    e.inserted_at = 1789999990;
    e.max_age = 300;
    volume_.entries()[{FakeDaemonVolume::Key(kSite, kUrl), id}] = e;
  }

  std::unique_ptr<DaemonHtmlEngine> NewEngine() {
    DaemonHtmlEngine::Deps deps;
    deps.substrate.abi = &abi_;
    deps.substrate.cache = &volume_;
    deps.substrate.cache_path = "/cache";
    deps.purge_source = &source_;
    deps.statistics = &stats_;
    deps.timer = &timer_;
    deps.handler = &handler_;
    deps.limiter = &limiter_;
    deps.budget = &budget_;
    deps.compress_demands = &demands_;
    return std::make_unique<DaemonHtmlEngine>(config_, deps);
  }

  DaemonHtmlRequestFacts Request(std::vector<GoogleString> accept_encoding) {
    DaemonHtmlRequestFacts f;
    f.method = "GET";
    f.scheme = "https";
    f.host = "www.example.test";
    f.path = "/page";
    f.site = kSite;
    f.url_agreed = true;
    f.options_enabled_and_allowed = true;
    f.accept_lines = {"text/markdown"};
    f.accept_encoding_lines = std::move(accept_encoding);
    f.capture_start_ms = kNowMs;
    return f;
  }

  // The library's order for the request, whatever its lines.
  void Order(std::vector<int> order) {
    abi_.mutable_injection()->select_rc = static_cast<int>(order.size());
    abi_.mutable_injection()->select_order = std::move(order);
  }

  // A verdict to serve the copy in `coding`.
  static PsHtmlServeVerdict ServeIn(int coding) {
    PsHtmlServeVerdict v = {};
    v.struct_size = sizeof(v);
    v.action = kPsHtmlServeActionServeMarkdown;
    v.wants_markdown = 1;
    v.age_s = 7;
    v.markdown_encoding = coding;
    return v;
  }
  // A fall-through for the reason at `index` of the fake's names.
  static PsHtmlServeVerdict FallThrough(int index) {
    PsHtmlServeVerdict v = {};
    v.struct_size = sizeof(v);
    v.action = kPsHtmlServeActionFallThrough;
    v.reason = index;
    v.wants_markdown = 1;
    return v;
  }
  static constexpr int kNoAcceptableEncoding = 27;
  static constexpr uint64_t kStamp = 0x5A5A;

  void Verdicts(std::vector<PsHtmlServeVerdict> verdicts) {
    abi_.mutable_injection()->verdicts = std::move(verdicts);
  }

  int64 Counter(const char* name) { return stats_.GetVariable(name)->Get(); }

  // The _coded facts calls, as "coded-or-null/coding".
  std::vector<GoogleString> CodedFactsCalls() const {
    std::vector<GoogleString> out;
    for (const InjectedHtmlCall& c : abi_.html_calls()) {
      if (c.fn == "ps_html_stored_facts_from_reads_coded") {
        out.push_back(StrCat(c.args[5], "/", c.args[6]));
      }
    }
    return out;
  }

  int Calls(const char* fn) const {
    int n = 0;
    for (const GoogleString& name : abi_.CallNames()) n += name == fn;
    return n;
  }

  // The compress demands that reached the worker.
  int CompressDemands() const {
    int n = 0;
    for (const RecordedNotify& r : abi_.notifies()) {
      n += (r.notify_flags & kPsNotifyFlagMarkdownCompress) != 0;
    }
    return n;
  }

  static DaemonHtmlHeaderLines Coded(const char* coding, size_t length,
                                     const char* tag) {
    DaemonHtmlHeaderLines h = {
        {"Content-Type", "text/markdown; charset=utf-8"},
        {"Content-Length", IntegerToString(static_cast<int>(length))}};
    if (coding != nullptr) h.emplace_back("Content-Encoding", coding);
    h.emplace_back("Cache-Control", "private");
    h.emplace_back("Vary", "Accept, Accept-Encoding");
    h.emplace_back("X-Robots-Tag", "noindex");
    h.emplace_back("Age", "7");
    h.emplace_back("ETag", tag);
    return h;
  }

  std::unique_ptr<ThreadSystem> thread_system_;
  MockTimer timer_;
  SimpleStats stats_;
  MockMessageHandler handler_{new NullMutex};
  DaemonHealNotifyLimiter limiter_;
  DaemonHtmlByteBudget budget_;
  DaemonMarkdownCompressDemands demands_;
  Source source_;
  FakeDaemonVolume volume_;
  InjectedVerdictDaemonAbi abi_;
  DaemonHtmlEngine::Config config_;
};

// md-enc-br-selected
TEST_F(DaemonHtmlMarkdownCodingTest, BrSelected) {
  Put(kPsSentinelMarkdownBr, kBr);
  abi_.mutable_injection()->coded_bits[kPsMarkdownEncodingBr] =
      kPsHtmlFactsBrCopyBound;
  Order({kPsMarkdownEncodingBr, kPsMarkdownEncodingGzip,
         kPsMarkdownEncodingIdentity});
  Verdicts({ServeIn(kPsMarkdownEncodingBr)});
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  RecordingPort port;
  const DaemonHtmlServeResult r = engine->Serve(Request({"br, gzip"}), &port);
  ASSERT_EQ(DaemonHtmlServeResult::kFinal, r.kind);
  EXPECT_EQ(200, r.status);
  EXPECT_EQ(Coded("br", sizeof(kBr) - 1, kBrTag), r.headers);
  EXPECT_EQ(kBr, r.body);
  ASSERT_EQ(1u, port.emitted.size());
  EXPECT_TRUE(port.marked_before_emit[0]);
  // The library was handed the lines as received, asked for br with
  // identity acceptable, and one facts call carried the br read.
  ASSERT_EQ(1, Calls("ps_markdown_select_encoding"));
  EXPECT_EQ(std::vector<GoogleString>({"coded/2"}), CodedFactsCalls());
  ASSERT_EQ(1u, abi_.serve_requests().size());
  EXPECT_EQ(2u, abi_.serve_requests()[0].markdown_encoding);
  EXPECT_EQ(sizeof(PsHtmlServeVerdict), abi_.serve_requests()[0].verdict_size);
  EXPECT_NE(0u, abi_.serve_requests()[0].request_flags &
                    kPsHtmlServeRequestIdentityOk);
  EXPECT_EQ(0u, abi_.serve_requests()[0].request_flags &
                    kPsHtmlServeRequestEncodingNone);
  EXPECT_NE(0u, abi_.serve_facts()[0].flags & kPsHtmlFactsBrCopyBound);
  EXPECT_EQ(1, Counter("daemon_markdown_served_br"));
  EXPECT_EQ(1, Counter("daemon_markdown_served"));
  EXPECT_EQ(0, CompressDemands());
}

// The coded copy's facts carry the entry's header block, so the library
// compares the entry's policy stamp for a coded answer as for the plain one.
TEST_F(DaemonHtmlMarkdownCodingTest, CodedFactsCarryTheEntryHeaderBlock) {
  abi_.mutable_injection()->sidecar_read_rc = kPsOk;
  Put(kPsSentinelMarkdownBr, kBr);
  abi_.mutable_injection()->coded_bits[kPsMarkdownEncodingBr] =
      kPsHtmlFactsBrCopyBound;
  Order({kPsMarkdownEncodingBr, kPsMarkdownEncodingIdentity});
  Verdicts({ServeIn(kPsMarkdownEncodingBr)});
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  RecordingPort port;
  engine->Serve(Request({"br"}), &port);
  int coded_calls = 0;
  for (const InjectedHtmlCall& c : abi_.html_calls()) {
    if (c.fn != "ps_html_stored_facts_from_reads_coded") continue;
    ++coded_calls;
    EXPECT_EQ("block", c.args[4]);
  }
  EXPECT_EQ(1, coded_calls);
}

// An entry stored under another policy stamp never answers with a coded
// copy, whatever the verdict says.
TEST_F(DaemonHtmlMarkdownCodingTest, AStampMismatchedEntryNeverServesCoded) {
  Put(kPsSentinelMarkdownBr, kBr);
  abi_.mutable_injection()->coded_bits[kPsMarkdownEncodingBr] =
      kPsHtmlFactsBrCopyBound;
  abi_.mutable_injection()->facts.entry_policy_stamp = kStamp + 1;
  Order({kPsMarkdownEncodingBr, kPsMarkdownEncodingIdentity});
  Verdicts({ServeIn(kPsMarkdownEncodingBr)});
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  RecordingPort port;
  const DaemonHtmlServeResult r = engine->Serve(Request({"br"}), &port);
  EXPECT_EQ(DaemonHtmlServeResult::kFallThrough, r.kind);
  EXPECT_TRUE(port.emitted.empty());
  EXPECT_EQ(1, Counter("daemon_html_served_credentialed_refused"));
  EXPECT_EQ(0, Counter("daemon_markdown_served_br"));
}

// md-enc-gzip-selected
TEST_F(DaemonHtmlMarkdownCodingTest, GzipSelected) {
  Put(kPsSentinelMarkdownGzip, kGzip);
  abi_.mutable_injection()->coded_bits[kPsMarkdownEncodingGzip] =
      kPsHtmlFactsGzipCopyBound;
  Order({kPsMarkdownEncodingGzip, kPsMarkdownEncodingIdentity});
  Verdicts({ServeIn(kPsMarkdownEncodingGzip)});
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  RecordingPort port;
  const DaemonHtmlServeResult r = engine->Serve(Request({"gzip"}), &port);
  ASSERT_EQ(DaemonHtmlServeResult::kFinal, r.kind);
  EXPECT_EQ(Coded("gzip", sizeof(kGzip) - 1, kGzipTag), r.headers);
  EXPECT_EQ(kGzip, r.body);
  EXPECT_EQ(std::vector<GoogleString>({"coded/1"}), CodedFactsCalls());
  EXPECT_EQ(1, Counter("daemon_markdown_served_gzip"));
}

// md-enc-identity-vary-lists-accept-encoding: no Accept-Encoding at all;
// the plain copy, no coded read, and Vary lists Accept-Encoding too (after
// the origin's own tokens, without a repeat).
TEST_F(DaemonHtmlMarkdownCodingTest, IdentityVaryListsAcceptEncoding) {
  Order({kPsMarkdownEncodingIdentity});
  Verdicts({ServeIn(kPsMarkdownEncodingIdentity)});
  abi_.mutable_injection()->sidecar_read_rc = kPsOk;
  abi_.mutable_injection()->block.lines = {{"Vary", "accept-encoding, Cookie"}};
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  RecordingPort port;
  const DaemonHtmlServeResult r = engine->Serve(Request({}), &port);
  ASSERT_EQ(DaemonHtmlServeResult::kFinal, r.kind);
  DaemonHtmlHeaderLines want = Coded(nullptr, sizeof(kPlain) - 1, kPlainTag);
  for (auto& line : want) {
    if (line.first == "Vary") line.second = "Cookie, Accept, Accept-Encoding";
  }
  EXPECT_EQ(want, r.headers);
  EXPECT_EQ(kPlain, r.body);
  EXPECT_EQ(std::vector<GoogleString>({"null/0"}), CodedFactsCalls());
  EXPECT_EQ(0u, abi_.serve_requests()[0].markdown_encoding);
  EXPECT_EQ(1, Counter("daemon_markdown_served_identity"));
  EXPECT_EQ(0, Counter("daemon_markdown_encoded_copy_missing"));
  EXPECT_EQ(0, CompressDemands());
}

// md-enc-copy-missing-plain: br preferred, no br copy: the plain copy, the
// missing copy counted, one compress demand; a second request sends none.
TEST_F(DaemonHtmlMarkdownCodingTest, CopyMissingPlain) {
  Order({kPsMarkdownEncodingBr, kPsMarkdownEncodingIdentity});
  Verdicts({ServeIn(kPsMarkdownEncodingIdentity),
            ServeIn(kPsMarkdownEncodingIdentity)});
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  RecordingPort port;
  DaemonHtmlServeResult r = engine->Serve(Request({"br"}), &port);
  ASSERT_EQ(DaemonHtmlServeResult::kFinal, r.kind);
  EXPECT_EQ(Coded(nullptr, sizeof(kPlain) - 1, kPlainTag), r.headers);
  EXPECT_EQ(kPlain, r.body);
  EXPECT_EQ(std::vector<GoogleString>({"null/2"}), CodedFactsCalls());
  EXPECT_EQ(1, Counter("daemon_markdown_encoded_copy_missing"));
  EXPECT_EQ(1, Counter("daemon_markdown_compress_demand_sent"));
  ASSERT_EQ(1, CompressDemands());
  const RecordedNotify& n = abi_.notifies().back();
  EXPECT_EQ(kPsNotifyFlagFromModule | kPsNotifyFlagHostVouched |
                kPsNotifyFlagHtmlStored | kPsNotifyFlagMarkdownCompress,
            n.notify_flags);
  EXPECT_EQ(kPsContentHtml, n.content_type);
  EXPECT_EQ(kSite, n.site);

  r = engine->Serve(Request({"br"}), &port);
  ASSERT_EQ(DaemonHtmlServeResult::kFinal, r.kind);
  EXPECT_EQ(2, Counter("daemon_markdown_encoded_copy_missing"));
  EXPECT_EQ(1, CompressDemands());
  EXPECT_EQ(1, Counter("daemon_markdown_compress_demand_sent"));
}

// md-enc-not-smaller-no-demand
TEST_F(DaemonHtmlMarkdownCodingTest, NotSmallerNoDemand) {
  Put(kPsSentinelMarkdownBr, "");
  abi_.mutable_injection()->coded_bits[kPsMarkdownEncodingBr] =
      kPsHtmlFactsBrNotSmaller;
  Order({kPsMarkdownEncodingBr, kPsMarkdownEncodingIdentity});
  Verdicts({ServeIn(kPsMarkdownEncodingIdentity)});
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  RecordingPort port;
  const DaemonHtmlServeResult r = engine->Serve(Request({"br"}), &port);
  ASSERT_EQ(DaemonHtmlServeResult::kFinal, r.kind);
  EXPECT_EQ(kPlain, r.body);
  EXPECT_EQ(0, CompressDemands());
  EXPECT_EQ(0, Counter("daemon_markdown_encoded_copy_missing"));
  EXPECT_EQ(0, Counter("daemon_markdown_encoded_copy_unbound"));
}

// md-enc-worker-no-support-no-demand: a worker that publishes no codings,
// and one that has the coding asked for disabled, are never asked.
TEST_F(DaemonHtmlMarkdownCodingTest, WorkerNoSupportNoDemand) {
  Order({kPsMarkdownEncodingBr, kPsMarkdownEncodingIdentity});
  abi_.mutable_injection()->markdown_codings = -1;
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  RecordingPort port;
  Verdicts({ServeIn(kPsMarkdownEncodingIdentity),
            ServeIn(kPsMarkdownEncodingIdentity)});
  ASSERT_EQ(DaemonHtmlServeResult::kFinal,
            engine->Serve(Request({"br"}), &port).kind);
  EXPECT_EQ(0, CompressDemands());
  // Such a worker writes no coded copy: none is read or counted.
  EXPECT_EQ(std::vector<GoogleString>({"null/0"}), CodedFactsCalls());
  EXPECT_EQ(0, Counter("daemon_markdown_encoded_copy_missing"));
  EXPECT_EQ(0u, abi_.serve_requests()[0].markdown_encoding);
  EXPECT_NE(0u, abi_.serve_requests()[0].request_flags &
                    kPsHtmlServeRequestIdentityOk);

  // Only gzip enabled: a missing br copy is never asked for.  Read on the
  // shared-config timer, not per request.
  abi_.mutable_injection()->markdown_codings = 1 << kPsMarkdownEncodingGzip;
  engine->OnPurgePollTick();
  ASSERT_EQ(DaemonHtmlServeResult::kFinal,
            engine->Serve(Request({"br"}), &port).kind);
  EXPECT_EQ(0, CompressDemands());
  EXPECT_EQ(0, Counter("daemon_markdown_compress_demand_sent"));
}

// md-enc-copy-unbound-plain: a br copy of another plain copy is never sent.
TEST_F(DaemonHtmlMarkdownCodingTest, CopyUnboundPlain) {
  Put(kPsSentinelMarkdownBr, kBr);  // present, but the facts say unbound
  Order({kPsMarkdownEncodingBr, kPsMarkdownEncodingIdentity});
  Verdicts({ServeIn(kPsMarkdownEncodingIdentity)});
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  RecordingPort port;
  const DaemonHtmlServeResult r = engine->Serve(Request({"br"}), &port);
  ASSERT_EQ(DaemonHtmlServeResult::kFinal, r.kind);
  EXPECT_EQ(Coded(nullptr, sizeof(kPlain) - 1, kPlainTag), r.headers);
  EXPECT_EQ(kPlain, r.body);
  EXPECT_EQ(std::vector<GoogleString>({"coded/2"}), CodedFactsCalls());
  EXPECT_EQ(1, Counter("daemon_markdown_encoded_copy_unbound"));
  EXPECT_EQ(0, Counter("daemon_markdown_encoded_copy_missing"));
  EXPECT_EQ(1, CompressDemands());
}

// md-enc-second-coding-read: br missing, gzip bound, `br, gzip`: the gzip
// copy, after two coded reads, one facts call each.
TEST_F(DaemonHtmlMarkdownCodingTest, SecondCodingRead) {
  Put(kPsSentinelMarkdownGzip, kGzip);
  abi_.mutable_injection()->coded_bits[kPsMarkdownEncodingGzip] =
      kPsHtmlFactsGzipCopyBound;
  Order({kPsMarkdownEncodingBr, kPsMarkdownEncodingGzip,
         kPsMarkdownEncodingIdentity});
  Verdicts(
      {ServeIn(kPsMarkdownEncodingIdentity), ServeIn(kPsMarkdownEncodingGzip)});
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  RecordingPort port;
  const DaemonHtmlServeResult r = engine->Serve(Request({"br, gzip"}), &port);
  ASSERT_EQ(DaemonHtmlServeResult::kFinal, r.kind);
  EXPECT_EQ(Coded("gzip", sizeof(kGzip) - 1, kGzipTag), r.headers);
  EXPECT_EQ(kGzip, r.body);
  EXPECT_EQ(std::vector<GoogleString>({"null/2", "coded/1"}),
            CodedFactsCalls());
  ASSERT_EQ(2u, abi_.serve_requests().size());
  EXPECT_EQ(2u, abi_.serve_requests()[0].markdown_encoding);
  EXPECT_EQ(1u, abi_.serve_requests()[1].markdown_encoding);
  EXPECT_EQ(1, Counter("daemon_markdown_encoded_copy_missing"));
  // A coded copy went out: nothing is asked for.
  EXPECT_EQ(0, CompressDemands());
  EXPECT_EQ(1, Counter("daemon_markdown_served_gzip"));
}

// md-enc-second-coding-identity-refused: `br, gzip, identity;q=0`, br
// missing, gzip bound: the library refuses br without identity, and the
// engine asks again for gzip instead of falling through.
TEST_F(DaemonHtmlMarkdownCodingTest, SecondCodingIdentityRefused) {
  Put(kPsSentinelMarkdownGzip, kGzip);
  abi_.mutable_injection()->coded_bits[kPsMarkdownEncodingGzip] =
      kPsHtmlFactsGzipCopyBound;
  Order({kPsMarkdownEncodingBr, kPsMarkdownEncodingGzip});
  Verdicts(
      {FallThrough(kNoAcceptableEncoding), ServeIn(kPsMarkdownEncodingGzip)});
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  RecordingPort port;
  const DaemonHtmlServeResult r =
      engine->Serve(Request({"br, gzip, identity;q=0"}), &port);
  ASSERT_EQ(DaemonHtmlServeResult::kFinal, r.kind);
  EXPECT_EQ(Coded("gzip", sizeof(kGzip) - 1, kGzipTag), r.headers);
  ASSERT_EQ(2u, abi_.serve_requests().size());
  for (const RecordedServeRequest& q : abi_.serve_requests()) {
    EXPECT_EQ(0u, q.request_flags & kPsHtmlServeRequestIdentityOk);
  }
  EXPECT_EQ(0, Counter("daemon_markdown_fallthrough_no_acceptable_encoding"));
}

// md-enc-encoding-none-bit: `identity;q=0` alone: ENCODING_NONE, no coded
// read, and the library's no_acceptable_encoding fall-through.
TEST_F(DaemonHtmlMarkdownCodingTest, EncodingNoneBit) {
  Order({});
  Verdicts({FallThrough(kNoAcceptableEncoding)});
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  RecordingPort port;
  const DaemonHtmlServeResult r =
      engine->Serve(Request({"identity;q=0"}), &port);
  EXPECT_EQ(DaemonHtmlServeResult::kFallThrough, r.kind);
  EXPECT_EQ("no_acceptable_encoding", r.reason);
  ASSERT_EQ(1u, abi_.serve_requests().size());
  const RecordedServeRequest& q = abi_.serve_requests()[0];
  EXPECT_NE(0u, q.request_flags & kPsHtmlServeRequestEncodingNone);
  EXPECT_EQ(0u, q.request_flags & kPsHtmlServeRequestIdentityOk);
  EXPECT_EQ(0u, q.markdown_encoding);
  EXPECT_EQ(std::vector<GoogleString>({"null/0"}), CodedFactsCalls());
  EXPECT_EQ(1, Counter("daemon_markdown_fallthrough_no_acceptable_encoding"));
  EXPECT_TRUE(port.emitted.empty());
  EXPECT_EQ(0, CompressDemands());
}

// md-enc-verdict-zeroed: every classification gets a zeroed verdict at the
// 1.16 size, the second one too (after the first was written).
TEST_F(DaemonHtmlMarkdownCodingTest, VerdictZeroed) {
  Order({kPsMarkdownEncodingBr, kPsMarkdownEncodingGzip,
         kPsMarkdownEncodingIdentity});
  PsHtmlServeVerdict first = ServeIn(kPsMarkdownEncodingIdentity);
  memset(first.etag, 'x', sizeof(first.etag) - 1);
  first.reserved_1_16 = 0x5A5A;
  Verdicts({first, ServeIn(kPsMarkdownEncodingIdentity)});
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  RecordingPort port;
  engine->Serve(Request({"br, gzip"}), &port);
  ASSERT_EQ(2u, abi_.serve_requests().size());
  for (const RecordedServeRequest& q : abi_.serve_requests()) {
    EXPECT_EQ(sizeof(PsHtmlServeVerdict), q.verdict_size);
    EXPECT_TRUE(q.verdict_zeroed);
  }
}

// md-enc-none-acceptable-falls-through: `br;q=1, identity;q=0` with no br
// copy: the request goes to the origin, counted, nothing emitted, and the
// worker is asked for the copy once.
TEST_F(DaemonHtmlMarkdownCodingTest, NoneAcceptableFallsThrough) {
  Order({kPsMarkdownEncodingBr});
  Verdicts({FallThrough(kNoAcceptableEncoding)});
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  RecordingPort port;
  const DaemonHtmlServeResult r =
      engine->Serve(Request({"br;q=1, identity;q=0"}), &port);
  EXPECT_EQ(DaemonHtmlServeResult::kFallThrough, r.kind);
  EXPECT_EQ("no_acceptable_encoding", r.reason);
  EXPECT_TRUE(port.emitted.empty());
  EXPECT_FALSE(port.marked);
  EXPECT_EQ(1, Counter("daemon_markdown_fallthrough_no_acceptable_encoding"));
  EXPECT_EQ(1, Counter("daemon_markdown_encoded_copy_missing"));
  EXPECT_EQ(1, CompressDemands());
}

// md-enc-head-coded-headers-no-body
TEST_F(DaemonHtmlMarkdownCodingTest, HeadCodedHeadersNoBody) {
  Put(kPsSentinelMarkdownBr, kBr);
  abi_.mutable_injection()->coded_bits[kPsMarkdownEncodingBr] =
      kPsHtmlFactsBrCopyBound;
  Order({kPsMarkdownEncodingBr, kPsMarkdownEncodingIdentity});
  Verdicts({ServeIn(kPsMarkdownEncodingBr)});
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  RecordingPort port;
  DaemonHtmlRequestFacts f = Request({"br"});
  f.method = "HEAD";
  const DaemonHtmlServeResult r = engine->Serve(f, &port);
  ASSERT_EQ(DaemonHtmlServeResult::kFinal, r.kind);
  EXPECT_EQ(200, r.status);
  EXPECT_EQ(Coded("br", sizeof(kBr) - 1, kBrTag), r.headers);
  EXPECT_TRUE(r.body.empty());
  ASSERT_EQ(1u, port.heads.size());
  EXPECT_TRUE(port.heads[0]);
}

// md-enc-304-per-coding: the br tag on a br request is a 304 with that tag
// and the same Vary; the plain copy's tag on a br request is a 200.
TEST_F(DaemonHtmlMarkdownCodingTest, NotModifiedPerCoding) {
  Put(kPsSentinelMarkdownBr, kBr);
  abi_.mutable_injection()->coded_bits[kPsMarkdownEncodingBr] =
      kPsHtmlFactsBrCopyBound;
  Order({kPsMarkdownEncodingBr, kPsMarkdownEncodingIdentity});
  Verdicts({ServeIn(kPsMarkdownEncodingBr), ServeIn(kPsMarkdownEncodingBr)});
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  RecordingPort port;
  DaemonHtmlRequestFacts f = Request({"br"});
  f.has_if_none_match = true;
  f.if_none_match = kBrTag;
  DaemonHtmlServeResult r = engine->Serve(f, &port);
  ASSERT_EQ(DaemonHtmlServeResult::kFinal, r.kind);
  EXPECT_EQ(304, r.status);
  EXPECT_EQ(DaemonHtmlHeaderLines({{"ETag", kBrTag},
                                   {"Cache-Control", "private"},
                                   {"Vary", "Accept, Accept-Encoding"}}),
            r.headers);
  EXPECT_TRUE(r.body.empty());

  f.if_none_match = kPlainTag;
  r = engine->Serve(f, &port);
  ASSERT_EQ(DaemonHtmlServeResult::kFinal, r.kind);
  EXPECT_EQ(200, r.status);
  EXPECT_EQ(kBr, r.body);
  EXPECT_EQ(1, Counter("daemon_markdown_not_modified"));
  EXPECT_EQ(1, Counter("daemon_markdown_served_br"));
}

// md-enc-library-1.15-plain: without the 1.16 set the plain copy goes out
// exactly as before: no coding asked, no coded read, the 1.15 verdict size,
// no Content-Encoding and Vary: Accept.
TEST_F(DaemonHtmlMarkdownCodingTest, Library115Plain) {
  abi_.mutable_injection()->has_set_16 = false;
  Put(kPsSentinelMarkdownBr, kBr);
  abi_.mutable_injection()->verdict = ServeIn(kPsMarkdownEncodingIdentity);
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  RecordingPort port;
  const DaemonHtmlServeResult r = engine->Serve(Request({"br, gzip"}), &port);
  ASSERT_EQ(DaemonHtmlServeResult::kFinal, r.kind);
  DaemonHtmlHeaderLines want = Coded(nullptr, sizeof(kPlain) - 1, kPlainTag);
  for (auto& line : want) {
    if (line.first == "Vary") line.second = "Accept";
  }
  EXPECT_EQ(want, r.headers);
  EXPECT_EQ(kPlain, r.body);
  EXPECT_EQ(0, Calls("ps_markdown_select_encoding"));
  EXPECT_EQ(0, Calls("ps_html_stored_facts_from_reads_coded"));
  EXPECT_EQ(0, Calls("ps_html_markdown_etag_coded"));
  EXPECT_EQ(0, Calls("ps_read_shared_config_markdown_codings"));
  // The 1.15 form, which reads the entry's stamp from its header block.
  EXPECT_EQ(1, Calls("ps_html_stored_facts_from_reads_ex"));
  ASSERT_EQ(1u, abi_.serve_requests().size());
  EXPECT_EQ(kPsHtmlServeVerdictSize15, abi_.serve_requests()[0].verdict_size);
  EXPECT_EQ(0u, abi_.serve_requests()[0].markdown_encoding);
  EXPECT_EQ(0u, abi_.serve_requests()[0].request_flags &
                    (kPsHtmlServeRequestIdentityOk |
                     kPsHtmlServeRequestEncodingNone));
  EXPECT_EQ(0, Counter("daemon_markdown_served_identity"));
  EXPECT_EQ(1, Counter("daemon_markdown_served"));
  EXPECT_EQ(0, CompressDemands());
}

// A verdict naming a coding the engine did not ask for, or a coded copy it
// never read, is never sent.
TEST_F(DaemonHtmlMarkdownCodingTest, AnUnaskedCodingIsNeverSent) {
  Put(kPsSentinelMarkdownBr, kBr);
  Order({kPsMarkdownEncodingIdentity});
  Verdicts({ServeIn(kPsMarkdownEncodingBr)});
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  RecordingPort port;
  const DaemonHtmlServeResult r = engine->Serve(Request({"br"}), &port);
  EXPECT_EQ(DaemonHtmlServeResult::kFallThrough, r.kind);
  EXPECT_EQ("other", r.reason);
  EXPECT_TRUE(port.emitted.empty());
}

// A copy the library did not admit (a stale or purged page) asks for no
// coded copy and counts none missing, whatever the request preferred.
TEST_F(DaemonHtmlMarkdownCodingTest, NotAdmittedNoDemand) {
  Order({kPsMarkdownEncodingBr, kPsMarkdownEncodingIdentity});
  Verdicts({FallThrough(6 /* stale */), FallThrough(9 /* purged */)});
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  RecordingPort port;
  DaemonHtmlServeResult r = engine->Serve(Request({"br"}), &port);
  EXPECT_EQ(DaemonHtmlServeResult::kFallThrough, r.kind);
  EXPECT_EQ("stale", r.reason);
  r = engine->Serve(Request({"br"}), &port);
  EXPECT_EQ("purged", r.reason);
  EXPECT_EQ(0, CompressDemands());
  EXPECT_EQ(0, Counter("daemon_markdown_encoded_copy_missing"));
  EXPECT_EQ(1, Counter("daemon_markdown_fallthrough_stale"));
  EXPECT_EQ(1, Counter("daemon_markdown_fallthrough_purged"));
}

// A verdict naming another coding than the copy read is never sent: br read
// and bound, the verdict says gzip (br bytes would go out labelled gzip).
TEST_F(DaemonHtmlMarkdownCodingTest, AVerdictForAnotherCodingIsNeverSent) {
  Put(kPsSentinelMarkdownBr, kBr);
  abi_.mutable_injection()->coded_bits[kPsMarkdownEncodingBr] =
      kPsHtmlFactsBrCopyBound;
  Order({kPsMarkdownEncodingBr, kPsMarkdownEncodingIdentity});
  Verdicts({ServeIn(kPsMarkdownEncodingGzip)});
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  RecordingPort port;
  const DaemonHtmlServeResult r = engine->Serve(Request({"br, gzip"}), &port);
  EXPECT_EQ(DaemonHtmlServeResult::kFallThrough, r.kind);
  EXPECT_EQ("other", r.reason);
  EXPECT_TRUE(port.emitted.empty());
  EXPECT_EQ(0, Counter("daemon_markdown_served"));
}

// The coding asked for, but its copy was never read (missing): never sent.
TEST_F(DaemonHtmlMarkdownCodingTest, AVerdictForAnUnreadCopyIsNeverSent) {
  Order({kPsMarkdownEncodingBr, kPsMarkdownEncodingIdentity});
  Verdicts({ServeIn(kPsMarkdownEncodingBr)});
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  RecordingPort port;
  const DaemonHtmlServeResult r = engine->Serve(Request({"br"}), &port);
  EXPECT_EQ(DaemonHtmlServeResult::kFallThrough, r.kind);
  EXPECT_EQ("other", r.reason);
  EXPECT_TRUE(port.emitted.empty());
}

// A demand whose send failed is asked again by the next request, and once
// it went out, never again.
TEST_F(DaemonHtmlMarkdownCodingTest, AFailedDemandIsAskedAgain) {
  Order({kPsMarkdownEncodingBr, kPsMarkdownEncodingIdentity});
  abi_.mutable_injection()->verdict = ServeIn(kPsMarkdownEncodingIdentity);
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  RecordingPort port;
  volume_.notify_rc = 2;  // PS_ERR_IO
  engine->Serve(Request({"br"}), &port);
  EXPECT_EQ(1, CompressDemands());  // tried
  EXPECT_EQ(0, Counter("daemon_markdown_compress_demand_sent"));
  EXPECT_EQ(0u, demands_.size());
  volume_.notify_rc = kPsOk;
  engine->Serve(Request({"br"}), &port);
  EXPECT_EQ(2, CompressDemands());
  EXPECT_EQ(1, Counter("daemon_markdown_compress_demand_sent"));
  engine->Serve(Request({"br"}), &port);
  EXPECT_EQ(2, CompressDemands());
}

// Identity refused, and only a coding this module does not read accepted (a
// value a later library might return): ENCODING_NONE, never the plain copy.
TEST_F(DaemonHtmlMarkdownCodingTest, AnUnknownCodingAloneIsEncodingNone) {
  Order({7});
  Verdicts({FallThrough(kNoAcceptableEncoding)});
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  RecordingPort port;
  const DaemonHtmlServeResult r =
      engine->Serve(Request({"zstd, identity;q=0"}), &port);
  EXPECT_EQ("no_acceptable_encoding", r.reason);
  ASSERT_EQ(1u, abi_.serve_requests().size());
  EXPECT_NE(0u, abi_.serve_requests()[0].request_flags &
                    kPsHtmlServeRequestEncodingNone);
}

// A coded answer carries the plain copy's tag and bytes for a port that
// must send the plain copy instead, and such an answer counts as plain.
TEST_F(DaemonHtmlMarkdownCodingTest, APortMaySendThePlainCopyInstead) {
  Put(kPsSentinelMarkdownGzip, kGzip);
  abi_.mutable_injection()->coded_bits[kPsMarkdownEncodingGzip] =
      kPsHtmlFactsGzipCopyBound;
  Order({kPsMarkdownEncodingGzip, kPsMarkdownEncodingIdentity});
  Verdicts({ServeIn(kPsMarkdownEncodingGzip)});
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  RecordingPort port;
  port.replaced = true;
  port.standby = true;
  const DaemonHtmlServeResult r = engine->Serve(Request({"gzip"}), &port);
  ASSERT_EQ(DaemonHtmlServeResult::kFinal, r.kind);
  EXPECT_EQ(kGzip, r.body);
  EXPECT_EQ(kPlain, r.plain_body);
  EXPECT_EQ(kPlainTag, r.plain_etag);
  EXPECT_EQ(1, Counter("daemon_markdown_coded_fallback"));
  EXPECT_EQ(1, Counter("daemon_markdown_served_identity"));
  EXPECT_EQ(0, Counter("daemon_markdown_served_gzip"));
}

// A port that may send the plain copy instead gets no coded answer without
// one; a port that may not never has the plain copy read or carried.
TEST_F(DaemonHtmlMarkdownCodingTest, ThePlainStandbyIsReadOnlyWhenUsable) {
  volume_.entries().erase(
      {FakeDaemonVolume::Key(kSite, kUrl), kPsSentinelAgentMarkdown});
  Put(kPsSentinelMarkdownBr, kBr);
  abi_.mutable_injection()->coded_bits[kPsMarkdownEncodingBr] =
      kPsHtmlFactsBrCopyBound;
  Order({kPsMarkdownEncodingBr, kPsMarkdownEncodingIdentity});
  abi_.mutable_injection()->verdict = ServeIn(kPsMarkdownEncodingBr);
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  RecordingPort standby_port;
  standby_port.standby = true;
  DaemonHtmlServeResult r = engine->Serve(Request({"br"}), &standby_port);
  EXPECT_EQ(DaemonHtmlServeResult::kFallThrough, r.kind);
  EXPECT_EQ("absent", r.reason);
  EXPECT_TRUE(standby_port.emitted.empty());
  RecordingPort port;
  r = engine->Serve(Request({"br"}), &port);
  ASSERT_EQ(DaemonHtmlServeResult::kFinal, r.kind);
  EXPECT_EQ(kBr, r.body);
  EXPECT_TRUE(r.plain_body.empty());
  EXPECT_TRUE(r.plain_etag.empty());
}

// The demand set: each key once, the oldest forgotten first.
TEST(DaemonMarkdownCompressDemandsTest, OncePerKeyBounded) {
  DaemonMarkdownCompressDemands demands(2);
  EXPECT_TRUE(demands.Admit("a"));
  EXPECT_FALSE(demands.Admit("a"));
  EXPECT_TRUE(demands.Admit("b"));
  EXPECT_TRUE(demands.Admit("c"));  // "a" is dropped
  EXPECT_EQ(2u, demands.size());
  EXPECT_TRUE(demands.Admit("a"));
  EXPECT_FALSE(demands.Admit("c"));
  demands.Forget("c");
  EXPECT_EQ(1u, demands.size());
  EXPECT_TRUE(demands.Admit("c"));
  // A long key costs a digest, not its length.
  EXPECT_TRUE(demands.Admit(GoogleString(100000, 'x')));
  EXPECT_FALSE(demands.Admit(GoogleString(100000, 'x')));
  EXPECT_EQ(4096u, DaemonMarkdownCompressDemands::kDefaultCapacity);
}

}  // namespace
}  // namespace net_instaweb
