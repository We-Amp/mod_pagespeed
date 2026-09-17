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

// The serve arm: which stored entry this module hands back for one request,
// and the response state that goes with it.
//
// THE PEER IS NOT MOCKED IN PROCESS, for the same reason the record arm's
// suite says so: every case binds the REAL client library through the REAL
// loader, against a shared object that reproduces the peer's published
// behaviour -- including the one behaviour this arm exists to work around.
// The stand-in's `read_best` skips the durable-originals class exactly as the
// peer's does, and its selector copies the peer's scoring arithmetic (the
// peer's deterministic tie-break is not modelled -- no case here scores a
// positive tie).
//
// THE SCORER PLAYS BOTH GENERATIONS OF THE PEER'S FORMAT DISQUALIFY AND BOTH
// ARE EXERCISED HERE, which is
// what changed when the peer landed its own format disqualify
// (We-Amp/pagespeed-optimizer#1334) and made this module's disqualify defense in
// depth.  The DEFAULT is the older scorer, deliberately: a stand-in that
// always disqualified would make the arm's own disqualify unreachable and
// every assertion over it green by construction.  `PS_STUB_FORMAT_DISQUALIFY`
// switches to the current one, and the cases that set it pin what a real
// family now does -- an incompatible variant scores 0, the selection returns
// nothing rather than something undecodable, and the arm reaches the durable
// original by id.  That shape is unobservable on the parity rig at this pin
// precisely because nothing there can any longer hand the module a format the
// request did not advertise.

#include "pagespeed/system/daemon_serve_arm.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/option_context.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/util/platform.h"
#include "pagespeed/kernel/util/simple_stats.h"
#include "pagespeed/system/daemon_abi.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {

namespace {

const char kUrl[] = "/a/photo.png";
const char kHost[] = "example.com";
const char kScheme[] = "http";

// The two requests every selection case is written against.  Both are
// desktop / 1x / identity; only the advertised image format differs, which
// is the axis the arm's disqualify is about.
const char kUaDesktop[] =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/126.0 Safari/537.36";

// Capability masks, in the peer's numbering: bits 0-1 format, 2-3 viewport,
// 4 density, 5 Save-Data, 6-7 encoding.  0x08 is desktop/original/identity.
constexpr uint32_t kMaskOriginalDesktop = 0x08;
constexpr uint32_t kMaskWebpDesktop = 0x09;
constexpr uint32_t kMaskAvifDesktop = 0x0A;

class DaemonServeArmTest : public testing::Test {
 public:
  DaemonServeArmTest() : thread_system_(Platform::CreateThreadSystem()) {
    RewriteOptions::Initialize();
  }
  ~DaemonServeArmTest() override { RewriteOptions::Terminate(); }

 protected:
  void SetUp() override {
    log_path_ = StrCat(GTestTempDir(), "/daemon_serve_stub_calls.log");
    remove(log_path_.c_str());
    setenv("PS_STUB_LOG", log_path_.c_str(), 1);
    unsetenv("PS_STUB_ALTERNATES");
    unsetenv("PS_STUB_ORIGIN_ETAG");
    unsetenv("PS_STUB_ORIGIN_HASH_BYTE");
    unsetenv("PS_STUB_SERVE_STATS_ABSENT");
    unsetenv("PS_STUB_FORMAT_DISQUALIFY");
    unsetenv("PS_STUB_CLASSIFY_VIEWPORT");
    unsetenv("PS_STUB_NOTIFY_FAIL");
    unsetenv("PS_STUB_BODY_MAGIC");
    unsetenv("PS_STUB_ORIGIN_CCFLAGS");
    unsetenv("PS_STUB_ORIGIN_CONTENT_LENGTH");
    setenv("PS_STUB_INSERTED_AT", "1000", 1);
    setenv("PS_STUB_ORIGIN_MAXAGE", "600", 1);
    setenv("PS_STUB_ORIGIN_CT", "image/png", 1);
    setenv("PS_STUB_READ_CONTENT_TYPE", "3", 1);  // image

    GoogleString error;
    abi_.reset(LoadDaemonAbi(
        StrCat(GTestSrcDir(), "/test/pagespeed/system/libdaemon_stub.so"),
        &error));
    ASSERT_TRUE(abi_ != nullptr) << error;
    ASSERT_EQ(kPsOk, abi_->CacheOpen(nullptr, &cache_));

    // The context an unmodified configuration renders, computed once by this
    // module's own producer so the re-notify cases carry a context that is
    // genuinely valid rather than a literal that could drift from one.
    RewriteOptions options(thread_system_.get());
    ASSERT_EQ(OptionContextStatus::kOk,
              OptionContext::Compute(options, &default_context_,
                                     &default_signature_));
  }

  void TearDown() override {
    unsetenv("PS_STUB_LOG");
    unsetenv("PS_STUB_ALTERNATES");
    unsetenv("PS_STUB_ORIGIN_ETAG");
    unsetenv("PS_STUB_ORIGIN_HASH_BYTE");
    unsetenv("PS_STUB_ORIGIN_MAXAGE");
    unsetenv("PS_STUB_ORIGIN_CT");
    unsetenv("PS_STUB_READ_CONTENT_TYPE");
    unsetenv("PS_STUB_INSERTED_AT");
    unsetenv("PS_STUB_SERVE_STATS_ABSENT");
    unsetenv("PS_STUB_FORMAT_DISQUALIFY");
    unsetenv("PS_STUB_CLASSIFY_VIEWPORT");
    unsetenv("PS_STUB_NOTIFY_FAIL");
    unsetenv("PS_STUB_BODY_MAGIC");
    unsetenv("PS_STUB_ORIGIN_CCFLAGS");
    unsetenv("PS_STUB_ORIGIN_CONTENT_LENGTH");
  }

  GoogleString CallLog() const {
    FILE* f = fopen(log_path_.c_str(), "r");
    if (f == nullptr) {
      return GoogleString();
    }
    GoogleString out;
    char buffer[4096];
    size_t n = 0;
    while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0) {
      out.append(buffer, n);
    }
    fclose(f);
    return out;
  }

  void SetAlternates(const char* spec) {
    setenv("PS_STUB_ALTERNATES", spec, 1);
  }

  DaemonServeRequest Request(StringPiece accept) const {
    DaemonServeRequest request;
    request.url = kUrl;
    request.hostname = kHost;
    request.scheme = kScheme;
    request.accept = accept;
    request.user_agent = kUaDesktop;
    request.accept_encoding = "identity";
    return request;
  }

  // The rig's own sniff, reproduced here so a case can assert the PAIR --
  // the format on the wire and the media type emitted for it -- against a
  // reading of the bytes that is not the code under test.
  static GoogleString SniffFormat(StringPiece body) {
    if (body.starts_with(StringPiece("\x89PNG\r\n\x1a\n", 8))) return "png";
    if (body.starts_with(StringPiece("\xff\xd8\xff", 3))) return "jpeg";
    if (body.starts_with("GIF87a") || body.starts_with("GIF89a")) return "gif";
    if (body.size() >= 12 && body.starts_with("RIFF") &&
        body.substr(8, 4) == "WEBP") {
      return "webp";
    }
    if (body.size() >= 12 && body.substr(4, 4) == "ftyp" &&
        (body.substr(8, 4) == "avif" || body.substr(8, 4) == "avis")) {
      return "avif";
    }
    return "none";
  }

  DaemonServeDecision Serve(const DaemonServeRequest& request) {
    DaemonServeReader reader(abi_.get(), cache_);
    return reader.Serve(request, 1100 /* now_seconds */);
  }

  // The same call with the reader KEPT ALIVE, for the cases that read the
  // served BYTES.
  //
  // `decision.body` borrows from the read result the reader holds, and the
  // helper above destroys the reader on the way out -- so its decision
  // carries a length that is still right and a pointer that is not. Every
  // case that predates this one asks only whether the body is empty or how
  // long it is, which survives that; a case that looks at the bytes needs
  // the owner to outlive the decision, exactly as the header says.
  DaemonServeDecision ServeHoldingBytes(
      const DaemonServeRequest& request,
      std::unique_ptr<DaemonServeReader>* reader) {
    reader->reset(new DaemonServeReader(abi_.get(), cache_));
    return (*reader)->Serve(request, 1100 /* now_seconds */);
  }

  // The re-notify the Apache seam issues after a served fallback hit, with
  // this fixture's valid option context.  Everything the stand-in was asked
  // is in the call log, so the cases assert on sends rather than on returns
  // alone.
  bool Renotify(const DaemonServeRequest& request,
                const DaemonServeDecision& decision) {
    return RenotifyCounted(request, decision, nullptr, nullptr);
  }

  // The same call with the seam's two counters attached, so the counter
  // cases can assert which one moved.
  bool RenotifyCounted(const DaemonServeRequest& request,
                       const DaemonServeDecision& decision, Variable* notified,
                       Variable* notify_failed) {
    return DaemonServeFallbackRenotify(*abi_, "/tmp/ps-worker.sock", request,
                                       decision, default_context_,
                                       default_signature_, notified,
                                       notify_failed);
  }

  // The origin-refreshed sentinel the Apache seam issues on an age-expired
  // variant fall-through, with this fixture's valid option context, and the
  // same assert-on-sends arrangement as the re-notify pair above.
  bool RefreshNotifyCounted(const DaemonServeRequest& request,
                            const DaemonServeDecision& decision,
                            Variable* notified, Variable* notify_failed) {
    return DaemonServeOriginRefreshedNotify(
        *abi_, "/tmp/ps-worker.sock", request, decision, default_context_,
        default_signature_, notified, notify_failed);
  }

  int NotifyCount() const {
    return CountSubstring(CallLog(), "notify socket=");
  }

  std::unique_ptr<ThreadSystem> thread_system_;
  std::unique_ptr<DaemonAbi> abi_;
  void* cache_ = nullptr;
  GoogleString log_path_;
  GoogleString default_context_;
  GoogleString default_signature_;
};

// ---------------------------------------------------------------------------
// The pure half: no peer, no cache.
// ---------------------------------------------------------------------------

TEST_F(DaemonServeArmTest, FormatDisqualifyIsExactOrOriginal) {
  // The predicate itself, independent of any peer generation: exact format or
  // the ORIGINAL fallback is servable and nothing else is.  It is the answer
  // the ABI does not export, which is why the module carries it whether or
  // not the peer's scorer happens to agree today.
  EXPECT_TRUE(ServeFormatIsAdvertised(kMaskWebpDesktop, kMaskWebpDesktop));
  EXPECT_TRUE(ServeFormatIsAdvertised(kMaskWebpDesktop, kMaskOriginalDesktop));
  EXPECT_FALSE(ServeFormatIsAdvertised(kMaskWebpDesktop, kMaskAvifDesktop));
  EXPECT_FALSE(ServeFormatIsAdvertised(kMaskAvifDesktop, kMaskWebpDesktop));
  EXPECT_TRUE(ServeFormatIsAdvertised(kMaskAvifDesktop, kMaskAvifDesktop));
  // kSvg is refused for every client, because no request can advertise it.
  EXPECT_FALSE(ServeFormatIsAdvertised(kMaskWebpDesktop, 0x0B));
  EXPECT_FALSE(ServeFormatIsAdvertised(0x0B, 0x0B));
}

TEST_F(DaemonServeArmTest, OnlyIdentityEncodingIsServable) {
  EXPECT_TRUE(ServeEncodingIsServable(kMaskOriginalDesktop));
  EXPECT_FALSE(ServeEncodingIsServable(kMaskOriginalDesktop | 0x40));  // gzip
  EXPECT_FALSE(ServeEncodingIsServable(kMaskOriginalDesktop | 0x80));  // br
}

TEST_F(DaemonServeArmTest, MaskHelpersMatchThePeersLayout) {
  EXPECT_EQ(kPsImageFormatAvif, ServeImageFormat(kMaskAvifDesktop));
  EXPECT_EQ(kPsImageFormatOriginal, ServeImageFormat(kMaskOriginalDesktop));
  EXPECT_EQ(kPsImageFormatWebp, ServeImageFormat(kMaskWebpDesktop));
  EXPECT_EQ(kPsTransferEncodingIdentity,
            ServeTransferEncoding(kMaskOriginalDesktop));
  EXPECT_EQ(2u, ServeTransferEncoding(kMaskOriginalDesktop | 0x80));
  // THE ID IS THE MASK'S LOW BYTE, and nothing above it: the peer derives the
  // stored mask back from the id, so a helper that let a high bit through
  // would name an alternate the peer cannot have written.
  EXPECT_EQ(0x0Au, ServeAlternateIdForMask(0x1230Au));
}

TEST_F(DaemonServeArmTest, TheSentinelSpaceIsTheReservedViewport) {
  // The peer's own alternate-id namespace, pinned as a predicate: every id
  // whose viewport field carries the reserved value 3 is a sentinel, and no
  // capability vector ever is.  This is the invariant the by-id retry's guard
  // rests on, stated where the layout is stated rather than only inside the
  // guard.
  EXPECT_TRUE(ServeAlternateIdIsSentinel(0x0C));  // the originals class
  EXPECT_TRUE(ServeAlternateIdIsSentinel(0x1C));  // early hints
  EXPECT_TRUE(ServeAlternateIdIsSentinel(0x5C));  // browser profile
  EXPECT_TRUE(ServeAlternateIdIsSentinel(0x0D));  // any viewport-3 id
  EXPECT_TRUE(ServeAlternateIdIsSentinel(0xFC));
  EXPECT_FALSE(ServeAlternateIdIsSentinel(0x00));  // mobile / original
  EXPECT_FALSE(ServeAlternateIdIsSentinel(0x08));  // desktop, canonical
  EXPECT_FALSE(ServeAlternateIdIsSentinel(0x09));  // webp
  EXPECT_FALSE(ServeAlternateIdIsSentinel(0x0B));  // svg -- still a vector
  EXPECT_FALSE(ServeAlternateIdIsSentinel(0x10));  // mobile 2x
  EXPECT_FALSE(ServeAlternateIdIsSentinel(0xFB));  // everything but viewport
}

TEST_F(DaemonServeArmTest, TheMediaTypeIsReadOffTheBytes) {
  const GoogleString png("\x89PNG\r\n\x1a\n....", 12);
  const GoogleString jpeg("\xff\xd8\xff\xe0 jfif", 9);
  const GoogleString gif87("GIF87a....", 10);
  const GoogleString gif89("GIF89a....", 10);
  const GoogleString webp("RIFF\x20\x00\x00\x00WEBPVP8 ", 16);
  const GoogleString avif(GoogleString("\x00\x00\x00\x20", 4) + "ftypavif....");
  const GoogleString avis(GoogleString("\x00\x00\x00\x20", 4) + "ftypavis....");

  EXPECT_EQ("image/png", ServeMediaTypeForServedBytes(kPsContentImage, png));
  EXPECT_EQ("image/jpeg", ServeMediaTypeForServedBytes(kPsContentImage, jpeg));
  EXPECT_EQ("image/gif", ServeMediaTypeForServedBytes(kPsContentImage, gif87));
  EXPECT_EQ("image/gif", ServeMediaTypeForServedBytes(kPsContentImage, gif89));
  EXPECT_EQ("image/webp", ServeMediaTypeForServedBytes(kPsContentImage, webp));
  EXPECT_EQ("image/avif", ServeMediaTypeForServedBytes(kPsContentImage, avif));
  // The animated-sequence brand is the same media type.
  EXPECT_EQ("image/avif", ServeMediaTypeForServedBytes(kPsContentImage, avis));

  // A format this module cannot name: the origin's field is the only source
  // and the caller keeps it.
  EXPECT_TRUE(
      ServeMediaTypeForServedBytes(kPsContentImage, "<svg xmlns=").empty());
  EXPECT_TRUE(ServeMediaTypeForServedBytes(kPsContentImage, "\x00\x00\x01\x00")
                  .empty());
  EXPECT_TRUE(ServeMediaTypeForServedBytes(kPsContentImage, "").empty());

  // A TRUNCATED MAGIC IS NOT A MATCH, and this is the case a length-free
  // comparison reads off the end for: every prefix below is a proper prefix
  // of a signature the rule names.
  EXPECT_TRUE(
      ServeMediaTypeForServedBytes(kPsContentImage, StringPiece(png.data(), 7))
          .empty());
  EXPECT_TRUE(ServeMediaTypeForServedBytes(kPsContentImage, "GIF8").empty());
  EXPECT_TRUE(ServeMediaTypeForServedBytes(kPsContentImage,
                                           StringPiece(webp.data(), 11))
                  .empty());
  EXPECT_TRUE(ServeMediaTypeForServedBytes(kPsContentImage,
                                           StringPiece(avif.data(), 11))
                  .empty());

  // AND THE CLASS GATE, which is not the sniff saying no: these bytes ARE a
  // format the sniff names, and a stylesheet or a script that happens to
  // start with them is still not an image.
  EXPECT_TRUE(ServeMediaTypeForServedBytes(kPsContentCss, png).empty());
  EXPECT_TRUE(ServeMediaTypeForServedBytes(kPsContentJs, webp).empty());
  EXPECT_TRUE(ServeMediaTypeForServedBytes(kPsContentOther, avif).empty());
  EXPECT_TRUE(ServeMediaTypeForServedBytes(kPsContentHtml, gif89).empty());
}

TEST_F(DaemonServeArmTest, DerivedETagIsWeakAndPerRepresentation) {
  const GoogleString webp =
      ServeDerivedETag(kMaskWebpDesktop, 0, nullptr, "\"origin\"", 1234, 100);
  const GoogleString avif =
      ServeDerivedETag(kMaskAvifDesktop, 0, nullptr, "\"origin\"", 1234, 100);
  EXPECT_TRUE(StringCaseStartsWith(webp, "W/\"ps-"));
  // Two encodings of one source are different representations.
  EXPECT_NE(webp, avif);
  // The flag byte separates an entry stored with the origin-negotiates
  // marker from one stored without it: the two are served under different
  // cache keying, so they must not validate against each other.
  EXPECT_NE(webp, ServeDerivedETag(kMaskWebpDesktop, kPsFlagOriginVariesAccept,
                                   nullptr, "\"origin\"", 1234, 100));
  // A source revision moves it; the served length moves it.
  EXPECT_NE(webp, ServeDerivedETag(kMaskWebpDesktop, 0, nullptr, "\"other\"",
                                   1234, 100));
  EXPECT_NE(webp, ServeDerivedETag(kMaskWebpDesktop, 0, nullptr, "\"origin\"",
                                   1234, 101));
}

TEST_F(DaemonServeArmTest, DerivedETagFallsBackToTheIdentityFreeShape) {
  // No source binding and no origin validator: the legacy shape, which is
  // the one the optimizer emits for the same entry, so those clients keep
  // their 304s.
  const GoogleString legacy =
      ServeDerivedETag(kMaskWebpDesktop, 0, nullptr, "", 0, 100);
  const GoogleString identity =
      ServeDerivedETag(kMaskWebpDesktop, 0, nullptr, "\"o\"", 0, 100);
  EXPECT_NE(legacy, identity);
  // The identity form carries one more field, and therefore one more
  // separator, than the legacy one.
  EXPECT_EQ(2, CountSubstring(legacy, "-"));
  EXPECT_EQ(3, CountSubstring(identity, "-"));
}

TEST_F(DaemonServeArmTest, ConditionalIsMatchedOnTheDerivedPlaneOnly) {
  const GoogleString derived =
      ServeDerivedETag(kMaskWebpDesktop, 0, nullptr, "\"origin\"", 1234, 100);
  EXPECT_TRUE(ServeDerivedValidatorMatches(derived, derived));
  // Weak comparison: the client may echo it without the weakness marker.
  GoogleString unmarked = derived;
  GlobalReplaceSubstring("W/", "", &unmarked);
  EXPECT_TRUE(ServeDerivedValidatorMatches(unmarked, derived));
  // A list, with ours in it.
  EXPECT_TRUE(
      ServeDerivedValidatorMatches(StrCat("\"other\", ", derived), derived));
  // THE ORIGIN PLANE IS NEVER CROSSED: a client holding the origin's own tag
  // holds a validator for a representation this arm did not issue.
  EXPECT_FALSE(ServeDerivedValidatorMatches("\"origin\"", derived));
  // `*` is not a validator on any plane.
  EXPECT_FALSE(ServeDerivedValidatorMatches("*", derived));
  EXPECT_FALSE(ServeDerivedValidatorMatches("", derived));
}

// ---------------------------------------------------------------------------
// Selection, against the real loader and the real stand-in.
// ---------------------------------------------------------------------------

TEST_F(DaemonServeArmTest, ServesTheFormatExactVariant) {
  SetAlternates("09:00:1,0a:00:1,0c:00:0");
  const DaemonServeDecision decision = Serve(Request("image/webp,*/*"));
  EXPECT_EQ(DaemonServeVerdict::kServeOptimized, decision.verdict);
  EXPECT_EQ(0x09, decision.alternate_id);
  EXPECT_EQ(kPsServeClassOptimized, decision.serve_class);
  EXPECT_TRUE(decision.worker_processed);
  EXPECT_FALSE(decision.body.empty());
}

TEST_F(DaemonServeArmTest, RefusesAVariantTheRequestDidNotAdvertise) {
  // ONLY an AVIF variant exists and the client advertised WebP.  Against the
  // PRE-FIX peer -- which is what this stand-in plays by default -- the
  // selection returns the AVIF one, because it scores 200, and the arm must
  // not emit it.  This is We-Amp/pagespeed-optimizer#1331 not being inherited,
  // and it is the case the peer's own #1334 has since removed at the source;
  // the module's refusal is kept and exercised here because a scorer is not
  // published contract and a by-id read never goes through one.
  SetAlternates("0a:00:1");
  const DaemonServeDecision decision = Serve(Request("image/webp"));
  EXPECT_EQ(DaemonServeVerdict::kFallThrough, decision.verdict);
  EXPECT_STREQ(daemon_serve_reason::kNoCompatibleVariant, decision.reason);
  EXPECT_EQ(kPsServeClassOriginalPending, decision.serve_class);
  // And the peer really did offer it, so the refusal is the module's.
  EXPECT_TRUE(CallLog().find("read_best") != GoogleString::npos);
  EXPECT_TRUE(CallLog().find("selected=10") != GoogleString::npos);
}

TEST_F(DaemonServeArmTest,
       TheOriginalFormatVariantIsPreferredWithNoSecondRead) {
  // The arithmetic the refusal rests on, exercised rather than asserted in a
  // comment: an ORIGINAL-format variant at the request's own normalized id
  // scores 300 -- every axis but format matches by construction -- and beats
  // the incompatible AVIF variant's 200 inside the PEER's selection.  So the
  // compatible entry is returned by the first read and there is no second
  // one; a "recovery" read here would be unreachable code.
  SetAlternates("0a:00:1,08:00:1");
  const DaemonServeDecision decision = Serve(Request("image/webp"));
  EXPECT_EQ(DaemonServeVerdict::kServeOptimized, decision.verdict);
  EXPECT_EQ(0x08, decision.alternate_id);
  // EXACTLY ONE by-id read, and it is the rule-6 probe of the originals
  // sentinel (id 12) rather than a recovery read for a variant.  Stated as a
  // count plus an id: the probe is unconditional now, so "no by-id read at
  // all" stopped being the way to say "no recovery read happened".
  EXPECT_EQ(1, CountSubstring(CallLog(), "read_alternate"));
  EXPECT_TRUE(CallLog().find("read_alternate url=/a/photo.png id=12") !=
              GoogleString::npos);
}

TEST_F(DaemonServeArmTest, TheCostOfTheRefusalIsAFallThroughAndIsNamed) {
  // The case the refusal DOES cost, pinned so it is a measured property
  // rather than a claim.  The client asks as webp/desktop/1x/identity (mask
  // 0x09).  0x0A is AVIF and matches every OTHER axis, so the peer scores it
  // 200 with no format penalty at all; 0x34 is ORIGINAL format but misses on
  // viewport, density and Save-Data, so it scores 160.  The peer hands back
  // the AVIF one, this arm refuses it, and the request falls through although
  // a servable entry existed.  Recovering it from here would need a
  // module-side re-selection over the whole alternate set.
  //
  // Both stored variants are IDENTITY on purpose: a pre-compressed one would
  // be refused a step earlier, by the encoding floor, and the case would then
  // prove something about that floor instead of about the format axis.
  SetAlternates("0a:00:1,34:00:1");
  const DaemonServeDecision decision = Serve(Request("image/webp"));
  EXPECT_EQ(DaemonServeVerdict::kFallThrough, decision.verdict);
  EXPECT_STREQ(daemon_serve_reason::kNoCompatibleVariant, decision.reason);

  // AND THE COST IS GONE AT THE CURRENT PEER, asserted rather than promised.
  // Same family, same request, the peer's own format disqualify in place: the
  // AVIF variant scores 0 instead of 200, so the ORIGINAL-format one at 160
  // is what the selection returns and the serve happens.
  setenv("PS_STUB_FORMAT_DISQUALIFY", "1", 1);
  const DaemonServeDecision fixed = Serve(Request("image/webp"));
  EXPECT_EQ(DaemonServeVerdict::kServeOptimized, fixed.verdict);
  EXPECT_EQ(0x34, fixed.alternate_id);
}

TEST_F(DaemonServeArmTest, ARealBrowserGetsTheOptimizedIdentityVariant) {
  // THE DEFECT THIS PINS WAS INVISIBLE TO EVERY IDENTITY-ONLY TEST, which is
  // why it is written against a real browser's header rather than a
  // convenient one.  `Accept-Encoding: gzip, deflate, br` is what a browser
  // sends; the peer's classifier returns a BROTLI mask; the peer's scorer
  // then prefers the stored brotli variant (exact encoding match, +60) over
  // its identity sibling (identity fallback, +5). An arm that refuses
  // pre-compressed bytes without normalizing the encoding axis first
  // therefore refuses the ONLY thing it was offered and serves the
  // unoptimized durable original -- to every real client, while the
  // fall-through counter never moves because a serve did happen.
  //
  // With the selection mask normalized, the peer hard-disqualifies the
  // brotli variant itself and hands back the identity sibling.
  SetAlternates("89:00:1,09:00:1,0c:00:0");
  DaemonServeRequest request = Request("image/webp");
  request.accept_encoding = "gzip, deflate, br";
  const DaemonServeDecision decision = Serve(request);
  EXPECT_EQ(DaemonServeVerdict::kServeOptimized, decision.verdict);
  EXPECT_EQ(0x09, decision.alternate_id);
  EXPECT_TRUE(decision.worker_processed);
  // The classifier's own answer is still reported; only the mask the read was
  // taken at is normalized.
  EXPECT_EQ(2u, ServeTransferEncoding(decision.client_mask));
  EXPECT_EQ(kPsTransferEncodingIdentity,
            ServeTransferEncoding(decision.selection_mask));
}

TEST_F(DaemonServeArmTest, EncodingNormalizationLeavesEveryOtherAxisAlone) {
  EXPECT_EQ(0x09u, ServeMaskWithIdentityEncoding(0x89u));
  EXPECT_EQ(0x09u, ServeMaskWithIdentityEncoding(0x49u));
  EXPECT_EQ(0x09u, ServeMaskWithIdentityEncoding(0x09u));
  EXPECT_EQ(0x3Au, ServeMaskWithIdentityEncoding(0xBAu));
  EXPECT_EQ(0x08u, ServeMaskWithOriginalFormat(0x0Au));
  EXPECT_EQ(0x48u, ServeMaskWithOriginalFormat(0x4Au));
}

TEST_F(DaemonServeArmTest, AnSvgVariantDoesNotHideItsServableSiblings) {
  // THE ARITHMETIC THE RETRY EXISTS FOR.  The peer scores a stored SVG
  // variant +1200 REGARDLESS of the client's format, plus the viewport,
  // density and Save-Data bonuses unconditionally -- about 1400 -- so on a
  // vectorized URL it outranks the WebP sibling sitting right beside it.  A
  // refusal with no recovery would make every optimized variant of that URL
  // unreachable and serve the durable original instead.  0x0B is the SVG
  // variant; 0x09 is the WebP one this client can actually use.
  SetAlternates("0b:00:1,09:00:1,0c:00:0");
  const DaemonServeDecision decision = Serve(Request("image/webp"));
  EXPECT_EQ(DaemonServeVerdict::kServeOptimized, decision.verdict);
  EXPECT_EQ(0x09, decision.alternate_id);
  // The peer really did offer the SVG one, so the recovery is the module's.
  EXPECT_TRUE(CallLog().find("selected=11") != GoogleString::npos);
  EXPECT_TRUE(CallLog().find("read_alternate url=/a/photo.png id=9") !=
              GoogleString::npos);
}

TEST_F(DaemonServeArmTest, TheSecondRetryFindsTheOriginalFormatSibling) {
  // Same regime, one rung down: the SVG variant outranks everything and the
  // client's own format is not stored, so the first retry misses and the
  // format-normalized id is what is left.
  SetAlternates("0b:00:1,08:00:1,0c:00:0");
  const DaemonServeDecision decision = Serve(Request("image/webp"));
  EXPECT_EQ(DaemonServeVerdict::kServeOptimized, decision.verdict);
  EXPECT_EQ(0x08, decision.alternate_id);
}

TEST_F(DaemonServeArmTest, FallsThroughOnAPreCompressedVariant) {
  // A family with NOTHING BUT pre-compressed variants -- no identity sibling
  // and no original -- which is the only shape left that can reach the
  // encoding floor now that the selection mask is normalized.  The realistic
  // shape, where an identity sibling does exist, is
  // ARealBrowserGetsTheOptimizedIdentityVariant above; this case used to
  // stand in for it and could not, because a family with one brotli entry is
  // not what a worker builds.
  //
  // The peer scores 0x88 against an identity-normalized client mask as a
  // mismatched non-identity encoding, i.e. a hard disqualify, so nothing is
  // selected at all and the arm falls through with no entry rather than with
  // a refusal.  Either way it does not emit bytes it cannot label -- and the
  // floor stays in the code because a by-id read bypasses the peer's scoring
  // entirely.
  SetAlternates("88:00:1");
  DaemonServeRequest request = Request("image/webp");
  request.accept_encoding = "br";
  const DaemonServeDecision decision = Serve(request);
  EXPECT_EQ(DaemonServeVerdict::kFallThrough, decision.verdict);
  EXPECT_TRUE(decision.body.empty());
}

TEST_F(DaemonServeArmTest, ReadsTheDurableOriginalByIdWhenNoVariantFits) {
  // THE TRAP.  Only the durable original exists.  `read_best` cannot return
  // it -- the peer's selector skips the class unconditionally -- so an arm
  // built on the selector alone would report a cold cache.  The by-id read
  // is what finds it.
  SetAlternates("0c:00:0");
  const DaemonServeDecision decision = Serve(Request("image/webp"));
  EXPECT_EQ(DaemonServeVerdict::kServeOriginal, decision.verdict);
  EXPECT_EQ(kPsSentinelOriginal, decision.alternate_id);
  EXPECT_STREQ(daemon_serve_reason::kDurableOriginal, decision.reason);
  // ORIGIN BYTES, not an optimized serve.
  EXPECT_EQ(kPsServeClassOriginalPending, decision.serve_class);
  EXPECT_FALSE(decision.worker_processed);
  const GoogleString log = CallLog();
  EXPECT_TRUE(log.find("read_alternate url=/a/photo.png id=12") !=
              GoogleString::npos);
}

TEST_F(DaemonServeArmTest, TheCurrentPeerSelectsNothingAndTheOriginalIsById) {
  // THE SHAPE A REAL FAMILY NOW PRESENTS, pinned here because it is exactly
  // what stops being observable elsewhere.  A durable original with WebP and
  // AVIF variants beside it, asked for by a client that advertised no image
  // format at all: the originals class is skipped by the selector and both
  // variants are hard-disqualified by the current peer, so the selection
  // returns NOTHING and the arm goes straight to the by-id read.
  //
  // Both generations of the format disqualify are driven, and the served
  // answer is the same
  // either way -- which is the whole content of "defense in depth" and the
  // reason a seeded defect in the module's disqualify can no longer be caught
  // by watching what gets served.  What differs is the ROUTE, and that is
  // what the call log is asserted on.
  SetAlternates("09:00:1,0a:00:1,0c:00:0");

  // The pre-fix peer: something incompatible is handed over, the module
  // refuses it, both by-id retries miss, and the original is reached.
  const DaemonServeDecision inherited = Serve(Request("text/html"));
  EXPECT_EQ(DaemonServeVerdict::kServeOriginal, inherited.verdict);
  EXPECT_EQ(kPsSentinelOriginal, inherited.alternate_id);
  EXPECT_TRUE(CallLog().find("read_best") != GoogleString::npos);
  EXPECT_TRUE(CallLog().find("selected=") != GoogleString::npos);

  // The current peer: nothing is selected at all, so the module's disqualify
  // is never reached and the same entry is served for a different reason.
  remove(log_path_.c_str());
  setenv("PS_STUB_FORMAT_DISQUALIFY", "1", 1);
  const DaemonServeDecision fixed = Serve(Request("text/html"));
  EXPECT_EQ(DaemonServeVerdict::kServeOriginal, fixed.verdict);
  EXPECT_EQ(kPsSentinelOriginal, fixed.alternate_id);
  EXPECT_STREQ(daemon_serve_reason::kDurableOriginal, fixed.reason);
  EXPECT_EQ(kPsServeClassOriginalPending, fixed.serve_class);
  // The selection was taken and came back empty -- the stand-in logs a
  // read_best only when it selected something.
  EXPECT_TRUE(CallLog().find("read_best") == GoogleString::npos);
  EXPECT_TRUE(CallLog().find("read_alternate url=/a/photo.png id=12") !=
              GoogleString::npos);
}

TEST_F(DaemonServeArmTest, AReservedViewportNeverNamesTheOriginalAsAVariant) {
  // THE SENTINEL GUARD, exercised on the one input it exists for.  A
  // selection mask whose viewport field carries the RESERVED value 3 is a
  // shape no classifier produces today -- which is exactly why it needs a
  // guard rather than an assumption, the #747 review round having shown that
  // "unreachable by a peer invariant" is reasoning that can be wrong.  With
  // such a mask the by-id retry's format-normalized candidate lands on 0x0C,
  // the durable original's id, and WITHOUT the guard the original is served
  // as kServeOptimized -- the negotiated `Vary: Accept`, the optimized serve
  // class, origin bytes wearing a variant's clothes.
  //
  // The stand-in is steered to the defect shape on purpose
  // (PS_STUB_CLASSIFY_VIEWPORT), and the family is the SVG regime that makes
  // the retry fire at all: the vector variant outranks everything the client
  // can use, the format refusal branch runs, and the retry is where an
  // unguarded id would have been named.  With the guard the retry names
  // NOTHING (both candidates share the viewport field), and the request
  // reaches the durable original only through the origin-mode read that
  // names it honestly.
  setenv("PS_STUB_CLASSIFY_VIEWPORT", "3", 1);
  SetAlternates("0b:00:1,0c:00:0");
  const DaemonServeDecision decision = Serve(Request("image/webp"));
  EXPECT_EQ(DaemonServeVerdict::kServeOriginal, decision.verdict);
  EXPECT_EQ(kPsSentinelOriginal, decision.alternate_id);
  EXPECT_STREQ(daemon_serve_reason::kDurableOriginal, decision.reason);
  EXPECT_EQ(kPsServeClassOriginalPending, decision.serve_class);
  EXPECT_FALSE(decision.vary_accept_negotiated);
  // The selection really did hand back the SVG variant (score 1400) and the
  // refusal branch really did run -- the retry is on the path taken.
  const GoogleString log = CallLog();
  EXPECT_TRUE(log.find("read_best") != GoogleString::npos);
  EXPECT_TRUE(log.find("selected=11") != GoogleString::npos);
  // AND THE RETRY NAMED NOTHING: no read at the request's own id 13, and the
  // only id-12 reads are the rule-6 probe and the origin-mode serve -- two
  // exactly, pinned so a third reader cannot slip in unnoticed.
  EXPECT_TRUE(log.find("id=13") == GoogleString::npos);
  EXPECT_EQ(2, CountSubstring(log, "read_alternate url=/a/photo.png id=12"));
}

TEST_F(DaemonServeArmTest, ColdKeyIsDistinguishedFromAnUnusableEntry) {
  SetAlternates("");
  const DaemonServeDecision cold = Serve(Request("image/webp"));
  EXPECT_EQ(DaemonServeVerdict::kFallThrough, cold.verdict);
  EXPECT_STREQ(daemon_serve_reason::kNoEntry, cold.reason);
  EXPECT_EQ(kPsServeClassOriginalCold, cold.serve_class);

  SetAlternates("0a:00:1");
  const DaemonServeDecision pending = Serve(Request("image/webp"));
  EXPECT_EQ(kPsServeClassOriginalPending, pending.serve_class);
}

// ---------------------------------------------------------------------------
// D6.4 rule 6: the origin sent a header no serve from the entry can put back.
// ---------------------------------------------------------------------------

TEST_F(DaemonServeArmTest, AMarkedOriginalDeclinesTheWholeSubstrate) {
  // The optimized variant is PRESENT, servable, and exactly what the client
  // asked for -- and it is declined, because serving it is what would drop the
  // origin's other headers.  A check that only guarded the ORIGINAL serve
  // would leave this the loudest hole in the rule: every optimized URL is
  // served with the header missing and nothing anywhere errors.
  SetAlternates("09:00:1,0c:08:0");
  const DaemonServeDecision decision = Serve(Request("image/webp,*/*"));
  EXPECT_EQ(DaemonServeVerdict::kFallThrough, decision.verdict);
  EXPECT_STREQ(daemon_serve_reason::kOriginHeadersNotReproducible,
               decision.reason);
  // DECLINED, not cold and not pending: the entry exists and is being refused
  // on purpose, and the peer publishes a class for exactly that.
  EXPECT_EQ(kPsServeClassOriginalDeclined, decision.serve_class);
  EXPECT_TRUE(decision.body.empty());
  EXPECT_FALSE(decision.selected);
}

TEST_F(DaemonServeArmTest, TheMarkedOriginalIsDeclinedToo) {
  SetAlternates("0c:08:0");
  const DaemonServeDecision decision = Serve(Request("image/webp"));
  EXPECT_EQ(DaemonServeVerdict::kFallThrough, decision.verdict);
  EXPECT_STREQ(daemon_serve_reason::kOriginHeadersNotReproducible,
               decision.reason);
}

TEST_F(DaemonServeArmTest, TheFlagIsReadOffTheByIdOriginalAndNowhereElse) {
  // THE STRUCTURAL FALSE ALL-CLEAR THIS PINS.  The bit lives only on the entry
  // the writer stamped; the optimizer builds its variants with a clear flags
  // byte whatever the original says.  So a marked ORIGINAL beside a variant
  // that carries the bit's value zero -- which is every variant there has ever
  // been -- must still decline, and an arm that took the reading from the
  // selection result would serve every marked URL for ever without a single
  // failing assertion anywhere.
  SetAlternates("09:00:1,0a:00:1,0c:08:0");
  EXPECT_STREQ(daemon_serve_reason::kOriginHeadersNotReproducible,
               Serve(Request("image/webp,*/*")).reason);
  // And the read really was the by-id one on the originals sentinel, taken
  // BEFORE any selection: read off the peer's own call log rather than
  // inferred from the verdict.
  const GoogleString log = CallLog();
  const size_t by_id = log.find("read_alternate url=/a/photo.png id=12");
  ASSERT_NE(GoogleString::npos, by_id) << log;
  const size_t best = log.find("read_best");
  EXPECT_TRUE(best == GoogleString::npos || by_id < best) << log;
}

TEST_F(DaemonServeArmTest, AnUnmarkedOriginalChangesNothing) {
  // The other half of the discrimination, and the one a "decline everything"
  // implementation would fail: the same family with a CLEAR flags byte is
  // served exactly as before -- the optimized variant, by the ordinary route.
  SetAlternates("09:00:1,0c:00:0");
  const DaemonServeDecision decision = Serve(Request("image/webp,*/*"));
  EXPECT_EQ(DaemonServeVerdict::kServeOptimized, decision.verdict);
  EXPECT_EQ(0x09, decision.alternate_id);
  EXPECT_STREQ(daemon_serve_reason::kOptimizedVariant, decision.reason);
}

TEST_F(DaemonServeArmTest, OtherFlagBitsOnTheOriginalAreNotThisOne) {
  // The flags byte carries more than one statement and the module sets two of
  // them.  A check that tested the byte for non-zero rather than for the bit
  // would decline every `Accept`-negotiating origin as well -- a whole class
  // of resource silently losing in-place optimization, for a rule that says
  // nothing about it.
  //
  // 0x04 ALONE IS THE ORDINARY STATE for that class, not a contrived one: an
  // origin that negotiates on `Accept` earns the Vary-Accept marker and does
  // NOT earn rule 6's, because `Accept` is a handled `Vary` axis -- the serve
  // arm puts the header back from this very marker.  So this case is what the
  // record arm actually writes for such an origin.
  //
  // WHAT THIS TEST IS AND IS NOT ABOUT: it asserts that the 0x04 marker does
  // not trip rule 6's decline.  It is NOT the assertion that clause (ii)
  // emits `Vary: Accept` -- this fixture is an image, so clause (i) would
  // reach the same emission by a different route, which is exactly why the
  // clause-(ii) tests use a non-image entry.  The origin-declared field is
  // asserted here anyway, since it is the field this marker feeds and it
  // costs one line to say which of the two is being read.
  SetAlternates("09:00:1,0c:04:0");  // 0x04 is the Vary-Accept marker
  const DaemonServeDecision decision = Serve(Request("image/webp,*/*"));
  EXPECT_EQ(DaemonServeVerdict::kServeOptimized, decision.verdict);
  EXPECT_TRUE(decision.vary_accept_origin_declared);
  // ... and the two bits together still decline, on the one that says so.
  SetAlternates("09:00:1,0c:0c:0");
  EXPECT_STREQ(daemon_serve_reason::kOriginHeadersNotReproducible,
               Serve(Request("image/webp,*/*")).reason);
}

TEST_F(DaemonServeArmTest, TheRuleSixReasonIsNotTheColdOne) {
  // 6b's whole content: a fall-through for rule 6 and a fall-through for a
  // cold key are different facts about the substrate, and only a distinct
  // reason keeps the partition able to say which happened.  Under one shared
  // `no-entry` a substrate working exactly as ruled is indistinguishable from
  // an empty cache.
  SetAlternates("0c:08:0");
  const DaemonServeDecision marked = Serve(Request("image/webp"));
  SetAlternates("");
  const DaemonServeDecision cold = Serve(Request("image/webp"));
  EXPECT_EQ(DaemonServeVerdict::kFallThrough, marked.verdict);
  EXPECT_EQ(DaemonServeVerdict::kFallThrough, cold.verdict);
  EXPECT_STRNE(marked.reason, cold.reason);
  EXPECT_STREQ(daemon_serve_reason::kNoEntry, cold.reason);
  EXPECT_NE(marked.serve_class, cold.serve_class);
}

TEST_F(DaemonServeArmTest, RefusesUrlsNeitherArmCanNameAnEntryWith) {
  SetAlternates("09:00:1");
  DaemonServeRequest query = Request("image/webp");
  query.url = "/a/photo.png?v=2";
  EXPECT_STREQ(daemon_serve_reason::kUrlNotKeySafe, Serve(query).reason);

  DaemonServeRequest rewritten = Request("image/webp");
  rewritten.url = "/a/photo.png.pagespeed.ic.0.png";
  EXPECT_STREQ(daemon_serve_reason::kRewrittenUrlShape,
               Serve(rewritten).reason);
}

// ---------------------------------------------------------------------------
// The response state.
// ---------------------------------------------------------------------------

TEST_F(DaemonServeArmTest, VaryAccentIsEmittedForEitherReasonAndKeptDistinct) {
  // (i) THIS ARM negotiated: an image variant chosen from the client's
  //     Accept.
  SetAlternates("09:00:1");
  DaemonServeDecision negotiated = Serve(Request("image/webp"));
  EXPECT_TRUE(negotiated.emit_vary_accept);
  EXPECT_TRUE(negotiated.vary_accept_negotiated);
  EXPECT_FALSE(negotiated.vary_accept_origin_declared);

  // (ii) the ORIGIN negotiates on Accept itself.  A durable original, so no
  //      variant selection happened and clause (i) is false -- which is what
  //      makes the two distinguishable rather than one boolean.
  SetAlternates("0c:04:0");
  DaemonServeDecision origin_declared = Serve(Request("image/webp"));
  EXPECT_EQ(DaemonServeVerdict::kServeOriginal, origin_declared.verdict);
  EXPECT_TRUE(origin_declared.emit_vary_accept);
  EXPECT_FALSE(origin_declared.vary_accept_negotiated);
  EXPECT_TRUE(origin_declared.vary_accept_origin_declared);

  // Neither: a non-image entry cannot have been format-negotiated, because
  // the peer's variants for one carry the original format by construction.
  // The durable original here is UNMARKED, which is what makes this the
  // negative case rather than the defect below.
  setenv("PS_STUB_READ_CONTENT_TYPE", "1", 1);  // css
  SetAlternates("08:00:1,0c:00:0");
  DaemonServeDecision css = Serve(Request("text/css,*/*"));
  EXPECT_EQ(DaemonServeVerdict::kServeOptimized, css.verdict);
  EXPECT_FALSE(css.emit_vary_accept);
  EXPECT_FALSE(css.vary_accept_origin_declared);
}

TEST_F(DaemonServeArmTest, ClauseTwoSurvivesServingAnOptimizedVariant) {
  // THE CASE CLAUSE (ii) IS ACTUALLY FOR, and the one a durable-original test
  // cannot reach.  A NON-IMAGE resource whose origin negotiates on `Accept`:
  // clause (i) is image-gated and false, so clause (ii) is the only thing that
  // can emit the header -- and once the optimizer has produced a variant, the
  // entry being SERVED is that variant, whose flags byte is clear by
  // construction.  Read the marker off the served entry and the origin's
  // `Vary: Accept` disappears from every optimized response, silently, for
  // exactly the class the record arm admits BECAUSE this clause reproduces it.
  //
  // So the marker is read off the DURABLE ORIGINAL, by id, on both paths.
  setenv("PS_STUB_READ_CONTENT_TYPE", "1", 1);  // css
  SetAlternates("08:00:1,0c:04:0");
  const DaemonServeDecision decision = Serve(Request("text/css,*/*"));
  ASSERT_EQ(DaemonServeVerdict::kServeOptimized, decision.verdict);
  EXPECT_EQ(0x08, decision.alternate_id);
  // The VARIANT's own byte is clear -- stated so the test is visibly about
  // where the answer came from rather than about the answer alone.
  EXPECT_EQ(0, decision.stored_flags & kPsFlagOriginVariesAccept);
  EXPECT_TRUE(decision.vary_accept_origin_declared);
  EXPECT_FALSE(decision.vary_accept_negotiated);
  EXPECT_TRUE(decision.emit_vary_accept);
}

TEST_F(DaemonServeArmTest, ClauseTwoOnAnImageVariantIsNotJustClauseOne) {
  // The same read, on the path where clause (i) would have masked it: an
  // IMAGE whose origin also negotiates on `Accept`.  Both clauses are true
  // here and `emit_vary_accept` cannot tell them apart, so the two are
  // asserted separately -- which is the whole reason the decision carries
  // them as two fields rather than one boolean.
  SetAlternates("09:00:1,0c:04:0");
  const DaemonServeDecision decision = Serve(Request("image/webp"));
  ASSERT_EQ(DaemonServeVerdict::kServeOptimized, decision.verdict);
  EXPECT_EQ(0, decision.stored_flags & kPsFlagOriginVariesAccept);
  EXPECT_TRUE(decision.vary_accept_negotiated);
  EXPECT_TRUE(decision.vary_accept_origin_declared);
  EXPECT_TRUE(decision.emit_vary_accept);
}

TEST_F(DaemonServeArmTest, AVariantWithNoDurableOriginalDeclaresNothing) {
  // The honest limit of sourcing clause (ii) from the by-id read: a variant
  // whose durable original is not in this cache at all.  Nothing here knows
  // what the origin said, so the clause answers false rather than guessing.
  //
  // THE VARIANT CARRIES 0x04 DELIBERATELY, and that is what makes this case
  // discriminating rather than decorative.  A variant with a clear byte
  // answers false under the correct code AND under a regression to reading
  // the served entry, so it would pass either way and prove nothing.  With
  // the bit SET on the variant, the two implementations disagree: reading the
  // durable original still answers false (there is no original to read), and
  // reading the entry being served answers true off a byte that is not the
  // origin's statement at all.  A variant carrying that bit is not a shape
  // the peer produces -- which is precisely why it is safe to use here to
  // separate the two reads.
  setenv("PS_STUB_READ_CONTENT_TYPE", "1", 1);  // css
  SetAlternates("08:04:1");
  const DaemonServeDecision decision = Serve(Request("text/css,*/*"));
  ASSERT_EQ(DaemonServeVerdict::kServeOptimized, decision.verdict);
  EXPECT_FALSE(decision.vary_accept_origin_declared);
  EXPECT_FALSE(decision.emit_vary_accept);
}

TEST_F(DaemonServeArmTest, TheMediaTypeAndTheServedBytesAgree) {
  // THE PAIR IS THE ASSERTION, and it is stated as one because the two
  // halves used to be independently correct and add up to a wrong response:
  // an AVIF variant, selected from an `Accept` that advertised AVIF, stamped
  // with `Vary: Accept` -- and emitted with the origin's `image/png`.  A
  // client decodes by the media type, so the response failed in the consumer
  // past every check this path makes, and a shared cache stored the
  // mislabelling with it.
  setenv("PS_STUB_ORIGIN_CT", "image/png", 1);
  setenv("PS_STUB_BODY_MAGIC", "0000002066747970617669660000", 1);  // ftypavif
  SetAlternates("0a:00:1");
  std::unique_ptr<DaemonServeReader> avif_reader;
  const DaemonServeDecision avif =
      ServeHoldingBytes(Request("image/avif,image/webp"), &avif_reader);
  ASSERT_EQ(DaemonServeVerdict::kServeOptimized, avif.verdict);
  EXPECT_EQ("avif", SniffFormat(avif.body));
  EXPECT_EQ("image/avif", avif.content_type);

  setenv("PS_STUB_BODY_MAGIC", "5249464620000000574542505650382000", 1);
  SetAlternates("09:00:1");
  std::unique_ptr<DaemonServeReader> webp_reader;
  const DaemonServeDecision webp =
      ServeHoldingBytes(Request("image/webp"), &webp_reader);
  ASSERT_EQ(DaemonServeVerdict::kServeOptimized, webp.verdict);
  EXPECT_EQ("webp", SniffFormat(webp.body));
  EXPECT_EQ("image/webp", webp.content_type);
}

TEST_F(DaemonServeArmTest, AConvertedSlotHoldingUnconvertedBytesIsNotRelabelled) {
  // THE CASE THAT MAKES THE MASK THE WRONG SOURCE, and it is not
  // hypothetical: seven corpus units are exactly this.  The peer fills a
  // CONVERTED-format slot with the ORIGINAL bytes when the conversion does
  // not pay for itself -- an animated GIF among them -- so an entry sitting
  // at the AVIF id can hold GIF.  A rule that read the id's format field
  // would label these `image/avif`, which is the same defect this whole
  // rule exists to close, in the opposite direction, on responses that were
  // correct before it.
  setenv("PS_STUB_ORIGIN_CT", "image/gif", 1);
  setenv("PS_STUB_BODY_MAGIC", "474946383961", 1);  // GIF89a
  SetAlternates("0a:00:1");
  std::unique_ptr<DaemonServeReader> reader;
  const DaemonServeDecision decision =
      ServeHoldingBytes(Request("image/avif,image/webp"), &reader);
  ASSERT_EQ(DaemonServeVerdict::kServeOptimized, decision.verdict);
  // The entry really is at the AVIF id -- so this is the disagreement, not
  // its absence.
  EXPECT_EQ(kPsImageFormatAvif, ServeImageFormat(decision.stored_mask));
  EXPECT_EQ("gif", SniffFormat(decision.body));
  EXPECT_EQ("image/gif", decision.content_type);
}

TEST_F(DaemonServeArmTest, AnUnconvertedServeStillCarriesTheOriginsOwnField) {
  // The other half of the rule, and the reason it is not "always derive".
  // A format the sniff does not name has exactly one source -- the origin's
  // field -- and a rule that answered anyway would relabel every SVG, every
  // ICO and every format added after this list was written.
  setenv("PS_STUB_ORIGIN_CT", "image/svg+xml", 1);
  setenv("PS_STUB_BODY_MAGIC", "3c7376672078", 1);  // "<svg x"
  SetAlternates("0c:00:0");
  const DaemonServeDecision original = Serve(Request("image/webp"));
  ASSERT_EQ(DaemonServeVerdict::kServeOriginal, original.verdict);
  EXPECT_EQ("image/svg+xml", original.content_type);

  // And a same-format re-encode, where the origin's field and the bytes
  // already agree and the rule has nothing to change.
  setenv("PS_STUB_ORIGIN_CT", "image/jpeg", 1);
  setenv("PS_STUB_BODY_MAGIC", "ffd8ffe0", 1);
  SetAlternates("08:00:1");
  const DaemonServeDecision reencode = Serve(Request("image/webp"));
  ASSERT_EQ(DaemonServeVerdict::kServeOptimized, reencode.verdict);
  EXPECT_EQ(kPsImageFormatOriginal, ServeImageFormat(reencode.stored_mask));
  EXPECT_EQ("image/jpeg", reencode.content_type);
}

TEST_F(DaemonServeArmTest, ANonImageVariantIsNeverRelabelled) {
  // The class gate, exercised where it is discriminating rather than where
  // it is free: this stylesheet's stored bytes begin with a PNG signature,
  // so the sniff alone would answer `image/png` for it.  A css unit cannot
  // become an image because of what its first eight bytes happen to be.
  setenv("PS_STUB_READ_CONTENT_TYPE", "1", 1);  // css
  setenv("PS_STUB_ORIGIN_CT", "text/css", 1);
  setenv("PS_STUB_BODY_MAGIC", "89504e470d0a1a0a", 1);
  SetAlternates("08:00:1");
  std::unique_ptr<DaemonServeReader> reader;
  const DaemonServeDecision decision =
      ServeHoldingBytes(Request("text/css,*/*"), &reader);
  ASSERT_EQ(DaemonServeVerdict::kServeOptimized, decision.verdict);
  EXPECT_EQ("png", SniffFormat(decision.body));
  EXPECT_EQ("text/css", decision.content_type);
}

// ---------------------------------------------------------------------------
// `Vary`, composed once for both legs of the response.
// ---------------------------------------------------------------------------

namespace {

// A decision carrying only the fields the `Vary` composition reads.  Built
// by hand rather than served, because the property under test is about the
// two LEGS of one response and a reader hands back one leg at a time.
//
// `body` is the caller's and outlives the decision, which borrows it -- the
// same ownership the real decision has.
DaemonServeDecision VaryDecision(bool emit_vary_accept, bool not_modified,
                                 const GoogleString& body) {
  DaemonServeDecision decision;
  decision.emit_vary_accept = emit_vary_accept;
  decision.not_modified = not_modified;
  decision.body = StringPiece(body);
  return decision;
}

}  // namespace

TEST_F(DaemonServeArmTest, TheTwoLegsOfOneResponseStateTheSameVary) {
  // THE DEFECT, STATED AS THE PROPERTY IT BROKE.  A 200 went out with
  // `Vary: Accept, Accept-Encoding` -- `Accept` from this arm, the encoding
  // token from the compressor the seam hands the body to -- and the 304 that
  // revalidated the very same entry seconds later went out with
  // `Vary: Accept`.  A cache updates its stored header fields from a 304
  // (RFC 9111 4.3.4), so it was told the response varies on a NARROWER set
  // than the one it had keyed on.
  //
  // The 200's set is what this arm composes PLUS what the compressor stamps;
  // the 304's is what this arm composes alone.  The two must be equal.
  const GoogleString big(kServeCompressorPassThroughBytes + 1, 'a');

  const GoogleString two_hundred =
      ServeVaryFieldValue(VaryDecision(true, false, big), true);
  const GoogleString not_modified =
      ServeVaryFieldValue(VaryDecision(true, true, big), true);
  // The 200 leg leaves the encoding token to the compressor and says so by
  // not carrying it; the 304 leg is the only emitter left and carries it.
  EXPECT_EQ("Accept", two_hundred);
  EXPECT_EQ("Accept, Accept-Encoding", not_modified);
  EXPECT_EQ(StrCat(two_hundred, ", Accept-Encoding"), not_modified);
}

TEST_F(DaemonServeArmTest, TheEncodingTokenTracksTheCompressorsOwnDecision) {
  // The compressor passes a response through uncompressed when it can
  // already see the body is smaller than what compressing it would add, and
  // it takes that shortcut BEFORE the line that states the axis -- so a
  // small 200 carries no `Vary: Accept-Encoding` at all.  A 304 leg that
  // stated the axis unconditionally would then disagree with its own 200 in
  // the OTHER direction, which is the same defect with the sign flipped.
  const GoogleString over(kServeCompressorPassThroughBytes + 1, 'a');
  const GoogleString at(kServeCompressorPassThroughBytes, 'a');
  const GoogleString empty;

  EXPECT_EQ("Accept-Encoding",
            ServeVaryFieldValue(VaryDecision(false, true, over), true));
  EXPECT_EQ("", ServeVaryFieldValue(VaryDecision(false, true, at), true));

  // A media type the seam does not hand to the compressor -- every image --
  // has no encoding axis on either leg.
  EXPECT_EQ("", ServeVaryFieldValue(VaryDecision(false, true, over), false));
  EXPECT_EQ("Accept",
            ServeVaryFieldValue(VaryDecision(true, true, over), false));

  // And a response that varies on nothing says nothing, on either leg.
  EXPECT_EQ("", ServeVaryFieldValue(VaryDecision(false, false, over), true));
  EXPECT_EQ("", ServeVaryFieldValue(VaryDecision(false, true, empty), true));
}

TEST_F(DaemonServeArmTest, TheCompressorFloorIsTheCompressorsOwnArithmetic) {
  // THE CONSTANT ITSELF, ABSOLUTELY, and not a case that computes its inputs
  // from it: every other case here builds a body at
  // `kServeCompressorPassThroughBytes` plus or minus one, so the pair moves
  // WITH the constant and any value at all satisfies them. A mirrored number
  // whose only test is written in terms of itself is not mirrored, it is
  // copied and unchecked.
  //
  // The arithmetic is the compressor's, at the line where it decides a
  // response is too small to be worth compressing: the gzip header it would
  // prepend (10), the gzip trailer it would append (8), and the 50 bytes it
  // budgets for the `Content-Encoding` and `Vary` fields and the validator
  // suffix that go with them. It compresses when the body is LARGER than
  // that sum, so the largest body it passes through is the sum itself.
  static_assert(kServeCompressorPassThroughBytes == 10 + 8 + 50,
                "the compressor's small-response arithmetic moved");
  EXPECT_EQ(68u, kServeCompressorPassThroughBytes);

  // AND THE BOUNDARY IN LITERALS, on both sides, so the pair is a statement
  // about 68 and 69 rather than about whatever the constant happens to say.
  const GoogleString at_the_floor(68, 'a');
  const GoogleString over_the_floor(69, 'a');
  EXPECT_EQ("", ServeVaryFieldValue(VaryDecision(false, true, at_the_floor),
                                    true));
  EXPECT_EQ("Accept-Encoding",
            ServeVaryFieldValue(VaryDecision(false, true, over_the_floor),
                                true));
}

TEST_F(DaemonServeArmTest, TheCompressorIsAskedAboutTheTypeGoingOut) {
  // THE TWO SIDES OF THE SELECTION ARE NOT INTERCHANGEABLE, and the case is
  // built on a pair that falls on OPPOSITE sides of the question the caller
  // asks with the answer -- whether the serving layer hands this response to
  // its compressor at all. A stylesheet is handed to it; a converted image
  // is not. So collapsing the selection to the request's inherited type does
  // not merely name a different string, it states the encoding axis for a
  // response the compressor never sees, or omits it for one it does.
  DaemonServeDecision decision;

  // The substrate's own case: the URL maps to one type, the bytes going out
  // are another, and the decision's is the one that will be written and
  // therefore the one the compressor is given.
  decision.content_type = "image/avif";
  EXPECT_STREQ("image/avif", ServeCompressorMediaType(decision, "text/css"));

  // And the other direction, so the case is about the SELECTION rather than
  // about images.
  decision.content_type = "text/css";
  EXPECT_STREQ("text/css", ServeCompressorMediaType(decision, "image/png"));

  // No decision type is the one case where the inherited type is the right
  // answer: nothing writes a `Content-Type`, so the compressor really does
  // see the one the request came in with.
  decision.content_type.clear();
  EXPECT_STREQ("image/png", ServeCompressorMediaType(decision, "image/png"));

  // Null is an ordinary value on that side and is passed through, because
  // the predicate downstream answers false for it and that is the correct
  // answer for a response with no media type at all.
  EXPECT_EQ(nullptr, ServeCompressorMediaType(decision, nullptr));
}

TEST_F(DaemonServeArmTest, A304StillCarriesTheRepresentationItRevalidates) {
  // THE FACT THE 304 LEG'S ANSWER RESTS ON, pinned because it is a silent
  // dependency: the encoding token is composed against the LENGTH of the
  // representation, and on a 304 that length is only knowable because the
  // decision still carries the entry's bytes.  Clear the body on a
  // not-modified decision as an "it is not sent anyway" tidy-up and the 304
  // silently stops declaring the axis for every representation, with no
  // test between that change and the wire.
  SetAlternates("09:00:1");
  const DaemonServeDecision first = Serve(Request("image/webp"));
  ASSERT_FALSE(first.body.empty());

  DaemonServeRequest conditional = Request("image/webp");
  conditional.if_none_match = first.etag;
  const DaemonServeDecision second = Serve(conditional);
  ASSERT_TRUE(second.not_modified);
  EXPECT_EQ(first.body.size(), second.body.size());
}

TEST_F(DaemonServeArmTest, CacheControlComesFromTheStoredOriginState) {
  SetAlternates("09:00:1");
  const DaemonServeDecision decision = Serve(Request("image/webp"));
  // The directive ORDER is the real library's, which the stand-in was
  // corrected to match: pinning the stand-in's own spelling would have been a
  // test of the harness rather than of the emitter.
  EXPECT_EQ("must-revalidate, max-age=600", decision.cache_control);
  // SWR IS NEVER SYNTHESIZED.  The peer's SAFE mode cannot produce one, and
  // this arm asks for no other mode.
  EXPECT_TRUE(decision.cache_control.find("stale-while-revalidate") ==
              GoogleString::npos);
}

TEST_F(DaemonServeArmTest, OriginNoCacheIsRelayedRatherThanDropped) {
  // 0x2200 = header present + bare no-cache.
  setenv("PS_STUB_ORIGIN_CCFLAGS", "8704", 1);
  SetAlternates("09:00:1");
  const DaemonServeDecision decision = Serve(Request("image/webp"));
  EXPECT_TRUE(decision.cache_control.find("no-cache") != GoogleString::npos);
  unsetenv("PS_STUB_ORIGIN_CCFLAGS");
}

TEST_F(DaemonServeArmTest, AStaleEntryFallsThroughInsteadOfBeingServed) {
  setenv("PS_STUB_INSERTED_AT", "1", 1);  // now is 1100, max-age 600
  SetAlternates("09:00:1");
  const DaemonServeDecision decision = Serve(Request("image/webp"));
  EXPECT_EQ(DaemonServeVerdict::kFallThrough, decision.verdict);
  EXPECT_STREQ(daemon_serve_reason::kNeedsRevalidation, decision.reason);
  EXPECT_TRUE(decision.body.empty());
}

TEST_F(DaemonServeArmTest, AZeroLifetimeEntryFallsThroughRatherThanServes) {
  // THE PREMISE `Expires` RESTS ON, made a test instead of a comment.  Nothing
  // on the record path parses `Expires` into the stored lifetime, so an origin
  // that expressed its lifetime ONLY that way stores a zero max-age and no
  // recorded `Cache-Control` at all.  What keeps `Expires` in the reproducible
  // set is that such an entry is REFUSED rather than served with a freshness
  // nobody stated -- a fall-through, never a wrong header.
  //
  // WHAT THIS DOES AND DOES NOT PROVE, because the difference matters: it
  // pins the behaviour of the in-tree MODEL of the peer's evaluator.  The
  // peer's own arithmetic for a zero lifetime with no recorded
  // `Cache-Control` is not observable from this tree, and if it applied a
  // content-type default there instead the entry would be served fresh.  That
  // is the one step of the `Expires` argument this suite cannot close.
  setenv("PS_STUB_ORIGIN_MAXAGE", "0", 1);
  setenv("PS_STUB_ORIGIN_CCFLAGS", "0", 1);  // no Cache-Control was recorded
  SetAlternates("09:00:1");
  const DaemonServeDecision decision = Serve(Request("image/webp"));
  EXPECT_EQ(DaemonServeVerdict::kFallThrough, decision.verdict);
  EXPECT_STREQ(daemon_serve_reason::kNeedsRevalidation, decision.reason);
  EXPECT_TRUE(decision.body.empty());
  unsetenv("PS_STUB_ORIGIN_CCFLAGS");
  setenv("PS_STUB_ORIGIN_MAXAGE", "600", 1);
}

TEST_F(DaemonServeArmTest, AClientForcedReloadFallsThrough) {
  SetAlternates("09:00:1");
  DaemonServeRequest request = Request("image/webp");
  request.force_revalidate = true;
  EXPECT_EQ(DaemonServeVerdict::kFallThrough, Serve(request).verdict);
}

TEST_F(DaemonServeArmTest, AnAgeExpiredVariantIsMarkedForTheOriginRefresh) {
  // THE DEFECT, at the one point the arm can still see it.  The stored
  // variant's stamped cache_inserted_at / origin_max_age make it older than
  // its effective lifetime at request time, so the arm declines it on
  // freshness -- and because the expiry is genuinely age-based, the decision
  // carries the marker the seam answers with the origin-refreshed sentinel.
  // Without the sentinel the worker's processed-set dedup answers every
  // later plain notification "already processed, skipping", and the URL
  // re-fetches from the origin on every request, for ever.
  setenv("PS_STUB_INSERTED_AT", "1", 1);  // now is 1100, max-age 600
  SetAlternates("09:00:1,0c:00:0");
  const DaemonServeDecision decision = Serve(Request("image/webp"));
  EXPECT_EQ(DaemonServeVerdict::kFallThrough, decision.verdict);
  EXPECT_STREQ(daemon_serve_reason::kNeedsRevalidation, decision.reason);
  EXPECT_TRUE(decision.stale_variant_expired_by_age);
}

TEST_F(DaemonServeArmTest, AFreshVariantIsServedAndMarkedForNothing) {
  // The steady state: served, and the marker stays clear.
  SetAlternates("09:00:1,0c:00:0");
  const DaemonServeDecision decision = Serve(Request("image/webp"));
  ASSERT_EQ(DaemonServeVerdict::kServeOptimized, decision.verdict);
  EXPECT_FALSE(decision.stale_variant_expired_by_age);
}

TEST_F(DaemonServeArmTest, AClientForcedReloadIsNotAnExpiry) {
  // THE FIRST ABI GUARANTEE, pinned where the arm relies on it.  A forced
  // reload reaches the SAME revalidate verdict, but the peer's evaluator
  // reports expired_by_age = false: the client demanded revalidation, which
  // says nothing about whether the stored variants are outdated.  Ungated,
  // any anonymous client could purge a URL's entire optimized variant set
  // with a plain reload (Cache-Control: max-age=0) -- the abuse case the
  // peer added the distinction for.
  //
  // THE ENTRY IS ALREADY AGE-EXPIRED, and that is what makes the case
  // discriminating: inserted at 1, requested at 1100, max-age 600, so
  // is_stale is set and ONLY the evaluator's force-revalidate clause keeps
  // expired_by_age clear.  On a fresh entry the flag would be clear either
  // way and the pin would be vacuous -- a stand-in missing that clause would
  // stay green.
  setenv("PS_STUB_INSERTED_AT", "1", 1);
  SetAlternates("09:00:1,0c:00:0");
  DaemonServeRequest request = Request("image/webp");
  request.force_revalidate = true;
  const DaemonServeDecision decision = Serve(request);
  EXPECT_EQ(DaemonServeVerdict::kFallThrough, decision.verdict);
  EXPECT_STREQ(daemon_serve_reason::kNeedsRevalidation, decision.reason);
  EXPECT_FALSE(decision.stale_variant_expired_by_age);
}

TEST_F(DaemonServeArmTest, AnOriginNoCacheEntryIsNotAnExpiry) {
  // THE SECOND ABI GUARANTEE, and the discriminating half of the pin: the
  // entry here IS age-expired (inserted at 1, now 1100, max-age 600), so
  // what keeps the marker clear is the no-cache half of the peer's rule and
  // nothing else.  Origin no-cache content is permanently "stale" by
  // definition -- every use revalidates -- and counting that as expiry
  // would churn the worker's purge -> re-optimize cycle on every request
  // for a very common CMS default.  The stand-in is taught the peer's exact
  // rule (expired_by_age = !no_cache && age > lifetime) for this case.
  //
  // 0x2201 = Cache-Control header present (0x0200) + bare no-cache (0x2000)
  // + the no-cache bit itself (0x0001), which is the combination the peer's
  // parser stamps for a bare `no-cache` line.
  setenv("PS_STUB_INSERTED_AT", "1", 1);
  setenv("PS_STUB_ORIGIN_CCFLAGS", "8705", 1);
  SetAlternates("09:00:1,0c:00:0");
  const DaemonServeDecision decision = Serve(Request("image/webp"));
  EXPECT_EQ(DaemonServeVerdict::kFallThrough, decision.verdict);
  EXPECT_STREQ(daemon_serve_reason::kNeedsRevalidation, decision.reason);
  EXPECT_FALSE(decision.stale_variant_expired_by_age);
  // PS_STUB_ORIGIN_CCFLAGS is cleared by TearDown, so a fatal failure above
  // cannot leak the flags into a later test.
}

TEST_F(DaemonServeArmTest, AnAgeExpiredOriginalIsNotMarkedForTheRefresh) {
  // negotiated == false: the durable original's own freshness decline never
  // carries the marker, however age-expired it is.  The sentinel exists to
  // purge a stale VARIANT set; an original-only entry has none, and its
  // fall-through converges by the ordinary record -> notify path -- the
  // same one a cold key takes.
  setenv("PS_STUB_INSERTED_AT", "1", 1);  // now is 1100, max-age 600
  SetAlternates("0c:00:0");
  const DaemonServeDecision decision = Serve(Request("image/webp"));
  EXPECT_EQ(DaemonServeVerdict::kFallThrough, decision.verdict);
  EXPECT_STREQ(daemon_serve_reason::kNeedsRevalidation, decision.reason);
  EXPECT_FALSE(decision.stale_variant_expired_by_age);
}

TEST_F(DaemonServeArmTest, ConditionalOnTheDerivedTagYields304) {
  SetAlternates("09:00:1");
  const DaemonServeDecision first = Serve(Request("image/webp"));
  ASSERT_FALSE(first.etag.empty());

  DaemonServeRequest conditional = Request("image/webp");
  conditional.if_none_match = first.etag;
  const DaemonServeDecision second = Serve(conditional);
  EXPECT_TRUE(second.not_modified);
  // The serve still HAPPENED and still has its class: a 304 is a serve.
  EXPECT_EQ(kPsServeClassOptimized, second.serve_class);

  // The origin's own tag does not validate the derived plane.
  setenv("PS_STUB_ORIGIN_ETAG", "\"origin-tag\"", 1);
  DaemonServeRequest cross = Request("image/webp");
  cross.if_none_match = "\"origin-tag\"";
  EXPECT_FALSE(Serve(cross).not_modified);
}

TEST_F(DaemonServeArmTest, EveryHitReportsHowOldTheEntryIs) {
  // `Age` is not optional and it is not cosmetic.  What goes out carries the
  // entry's full remaining lifetime, so a cache downstream told
  // `max-age=600` and not told the response is already 100 seconds old keeps
  // it for 700.  The value was being computed by the peer's freshness
  // evaluator and discarded.
  setenv("PS_STUB_INSERTED_AT", "1000", 1);  // now is 1100
  SetAlternates("09:00:1");
  EXPECT_EQ(100u, Serve(Request("image/webp")).age_seconds);
  setenv("PS_STUB_INSERTED_AT", "1090", 1);
  EXPECT_EQ(10u, Serve(Request("image/webp")).age_seconds);
}

TEST_F(DaemonServeArmTest, IfModifiedSinceIsHonouredOnTheOriginPlaneOnly) {
  setenv("PS_STUB_ORIGIN_LM", "1000000000", 1);  // 2001-09-09T01:46:40Z
  const char kAtTheStamp[] = "Sun, 09 Sep 2001 01:46:40 GMT";
  const char kBeforeIt[] = "Sat, 08 Sep 2001 01:46:40 GMT";

  // A DURABLE-ORIGINAL serve is the one response this arm puts the origin's
  // own `Last-Modified` on, so it is the one response an `If-Modified-Since`
  // can be answered over.
  SetAlternates("0c:00:0");
  DaemonServeRequest fresh_enough = Request("image/webp");
  fresh_enough.if_modified_since = kAtTheStamp;
  const DaemonServeDecision matched = Serve(fresh_enough);
  EXPECT_EQ(DaemonServeVerdict::kServeOriginal, matched.verdict);
  EXPECT_TRUE(matched.not_modified);

  DaemonServeRequest older = Request("image/webp");
  older.if_modified_since = kBeforeIt;
  EXPECT_FALSE(Serve(older).not_modified);

  // An unparseable date is IGNORED, which means serving -- never a 304 over
  // a validator we could not read.
  DaemonServeRequest nonsense = Request("image/webp");
  nonsense.if_modified_since = "not a date";
  EXPECT_FALSE(Serve(nonsense).not_modified);

  // AND NOT ON AN OPTIMIZED VARIANT.  That response carries no
  // `Last-Modified` -- it is not the representation the origin's validator
  // describes -- so answering 304 to a conditional over it would validate
  // bytes the client has never seen.
  SetAlternates("09:00:1");
  DaemonServeRequest on_a_variant = Request("image/webp");
  on_a_variant.if_modified_since = kAtTheStamp;
  const DaemonServeDecision variant = Serve(on_a_variant);
  EXPECT_EQ(DaemonServeVerdict::kServeOptimized, variant.verdict);
  EXPECT_FALSE(variant.not_modified);
  unsetenv("PS_STUB_ORIGIN_LM");
}

TEST_F(DaemonServeArmTest, ADeadSubstrateIsSkewRatherThanCold) {
  DaemonServeReader reader(abi_.get(), nullptr /* cache */);
  const DaemonServeDecision decision =
      reader.Serve(Request("image/webp"), 1100);
  EXPECT_EQ(DaemonServeVerdict::kFallThrough, decision.verdict);
  EXPECT_STREQ(daemon_serve_reason::kSubstrateUnavailable, decision.reason);
  EXPECT_EQ(kPsServeClassOriginalSkew, decision.serve_class);
}

// ---------------------------------------------------------------------------
// The invariant obligation booked against the option-context ruling: the
// capability-mask chain reads REQUEST HEADERS and nothing else.
// ---------------------------------------------------------------------------

TEST_F(DaemonServeArmTest, TheCapabilityMaskIsAFunctionOfRequestHeadersAlone) {
  SetAlternates("08:00:1,09:00:1,0a:00:1");

  // Two requests that differ only in `Accept` must classify differently and
  // select differently.
  const DaemonServeDecision webp = Serve(Request("image/webp"));
  const DaemonServeDecision avif = Serve(Request("image/avif,image/webp,*/*"));
  EXPECT_NE(webp.client_mask, avif.client_mask);
  EXPECT_EQ(0x09, webp.alternate_id);
  EXPECT_EQ(0x0A, avif.alternate_id);

  // AND NOTHING ELSE MOVES IT.  A resolved configuration is built and
  // deliberately never offered to the arm -- DaemonServeRequest has no field
  // that could carry one, which is the property being pinned: an option
  // context or signature reaching selection would make the entry a function
  // of server configuration, which the shared cache's key cannot express.
  // The pin is structural rather than behavioural on purpose; a value-based
  // one could only ever assert that a knob nobody wired changed nothing.
  RewriteOptions options(thread_system_.get());
  options.set_image_recompress_quality(42);
  const DaemonServeDecision after = Serve(Request("image/webp"));
  EXPECT_EQ(webp.client_mask, after.client_mask);
  EXPECT_EQ(webp.alternate_id, after.alternate_id);
  EXPECT_EQ(webp.etag, after.etag);
}

// ---------------------------------------------------------------------------
// Serve-class accounting.
// ---------------------------------------------------------------------------

TEST_F(DaemonServeArmTest, EveryServeClassIsRecordedExactlyOnce) {
  DaemonServeStats stats(abi_.get(), "/run/parity/cache");
  stats.Record(kPsServeClassOptimized);
  stats.Record(kPsServeClassOriginalCold);
  const GoogleString log = CallLog();
  EXPECT_TRUE(log.find("serve_class class=1 flags=0") != GoogleString::npos);
  EXPECT_TRUE(log.find("serve_class class=2 flags=0") != GoogleString::npos);
  // Back pressure is observe-only: the suppressed-notify flag is never set,
  // because this arm has no cooldown of its own to report on that channel.
  EXPECT_TRUE(log.find("flags=1") == GoogleString::npos);
}

TEST_F(DaemonServeArmTest, AnAbsentServeStatsFileIsNotAnError) {
  setenv("PS_STUB_SERVE_STATS_ABSENT", "1", 1);
  DaemonServeStats stats(abi_.get(), "/run/parity/cache");
  stats.Record(kPsServeClassOptimized);
  EXPECT_FALSE(stats.open());
  EXPECT_TRUE(CallLog().find("serve_class") == GoogleString::npos);
}

// ---------------------------------------------------------------------------
// Serve-hit accounting.  In the module-serves topology the daemon never
// answers a request -- it only writes alternates -- so its serve_savings
// counters can move only if the serving side records the hit.
// The gate (optimized class, worker-produced entry, recorded origin length)
// lives in the Apache seam and is pinned there; what is pinned HERE is the
// arm half: the decision carries the three facts the gate reads, and the
// recorder hands the peer exactly the bytes and the mask it was given.
// ---------------------------------------------------------------------------

TEST_F(DaemonServeArmTest, AnOptimizedServeDecisionCarriesTheHitGateInputs) {
  setenv("PS_STUB_ORIGIN_CONTENT_LENGTH", "4096", 1);
  SetAlternates("09:00:1,0c:00:0");
  const DaemonServeDecision decision = Serve(Request("image/webp"));
  ASSERT_EQ(DaemonServeVerdict::kServeOptimized, decision.verdict);
  EXPECT_EQ(kPsServeClassOptimized, decision.serve_class);
  EXPECT_TRUE(decision.worker_processed);
  EXPECT_EQ(4096u, decision.origin_content_length);
  EXPECT_EQ(kPsContentImage, decision.ps_content_type);
  EXPECT_EQ(kMaskWebpDesktop, decision.stored_mask);
}

TEST_F(DaemonServeArmTest, AnUnrecordedOriginLengthStaysZeroOnTheDecision) {
  // The default: the entry records no origin length.  Zero is "not recorded",
  // never "the origin sent nothing" -- the seam's gate declines it.
  SetAlternates("09:00:1,0c:00:0");
  const DaemonServeDecision decision = Serve(Request("image/webp"));
  ASSERT_EQ(DaemonServeVerdict::kServeOptimized, decision.verdict);
  EXPECT_TRUE(decision.worker_processed);
  EXPECT_EQ(0u, decision.origin_content_length);
}

TEST_F(DaemonServeArmTest, ANotWorkerProcessedEntryIsMarkedAsSuch) {
  SetAlternates("09:00:0,0c:00:0");
  const DaemonServeDecision decision = Serve(Request("image/webp"));
  ASSERT_EQ(DaemonServeVerdict::kServeOptimized, decision.verdict);
  EXPECT_FALSE(decision.worker_processed);
}

TEST_F(DaemonServeArmTest, AServeHitIsRecordedWithItsBytesAndMask) {
  DaemonServeStats stats(abi_.get(), "/run/parity/cache");
  stats.RecordHit(kPsContentImage, 4096, 1024, kMaskWebpDesktop);
  const GoogleString log = CallLog();
  EXPECT_NE(GoogleString::npos,
            log.find("serve_hit type=3 original=4096 optimized=1024 mask=9"));
}

TEST_F(DaemonServeArmTest, AServeHitIsNotRecordedWhileTheFileIsAbsent) {
  setenv("PS_STUB_SERVE_STATS_ABSENT", "1", 1);
  DaemonServeStats stats(abi_.get(), "/run/parity/cache");
  stats.RecordHit(kPsContentImage, 4096, 1024, kMaskWebpDesktop);
  EXPECT_FALSE(stats.open());
  EXPECT_EQ(GoogleString::npos, CallLog().find("serve_hit"));
}

// ---------------------------------------------------------------------------
// The fallback re-notify.  A fallback hit is SERVED, and a serve records
// nothing -- so the serve itself is the only signal that can ask the worker
// for the variant the client's mask names.  One notification per fallback
// hit, fire and forget, carrying the raw client mask; suppressed by the
// durable original's kPsFlagOriginVariesAccept bit and by an unnameable
// option context.  The seam that calls this is pinned in
// test/pagespeed/apache/daemon_seam_test.cc.
// ---------------------------------------------------------------------------

TEST_F(DaemonServeArmTest,
       AFallbackHitIsServedAndRenotifiesOnceWithTheClientMask) {
  // 0x29 is webp/desktop/1x/identity WITH Save-Data; this client sent no
  // Save-Data header, so its mask is 0x09 and the stored variant is a
  // fallback on the Save-Data axis.  The serve happens -- the variant is one
  // this client can decode, merely not the one its mask names -- and the
  // decision says so.
  SetAlternates("29:00:1,0c:00:0");
  const DaemonServeRequest request = Request("image/webp");
  const DaemonServeDecision decision = Serve(request);
  ASSERT_EQ(DaemonServeVerdict::kServeOptimized, decision.verdict);
  EXPECT_EQ(0x29, decision.alternate_id);
  EXPECT_TRUE(decision.fallback_hit);
  EXPECT_EQ(0x09u, decision.client_mask);

  const int before = NotifyCount();
  SimpleStats stats(thread_system_.get());
  Variable* notified = stats.AddVariable("ipro_daemon_fallback_notified");
  Variable* failed = stats.AddVariable("ipro_daemon_fallback_notify_failed");
  EXPECT_TRUE(RenotifyCounted(request, decision, notified, failed));
  // EXACTLY ONE notification, carrying the client mask the family should
  // converge on.  Matched on the notify line's own shape -- a bare "mask=9"
  // would also match the read_best line the selection already logged.
  EXPECT_EQ(before + 1, NotifyCount());
  EXPECT_NE(GoogleString::npos, CallLog().find("content_type=3 mask=9 "));
  // And the SEND moved the success counter and only it.
  EXPECT_EQ(1, notified->Get());
  EXPECT_EQ(0, failed->Get());
}

TEST_F(DaemonServeArmTest, AFailedSendMovesTheFailureCounterOnly) {
  // The send is fire-and-forget, but it is not SILENT.  A failed delivery
  // moves the failure counter and only it, so at rollout a dead notify
  // socket reads as the failure counter climbing while the success one stays
  // flat -- distinguishable from a quiet converged family (both flat) and
  // from a suppression (both flat BY DESIGN, asserted in the gate cases
  // above).  The stand-in logs the attempt before failing it, so "attempted
  // and failed" is asserted rather than "never asked".
  setenv("PS_STUB_NOTIFY_FAIL", "1", 1);
  SetAlternates("29:00:1,0c:00:0");
  const DaemonServeRequest request = Request("image/webp");
  const DaemonServeDecision decision = Serve(request);
  ASSERT_EQ(DaemonServeVerdict::kServeOptimized, decision.verdict);
  ASSERT_TRUE(decision.fallback_hit);

  SimpleStats stats(thread_system_.get());
  Variable* notified = stats.AddVariable("ipro_daemon_fallback_notified");
  Variable* failed = stats.AddVariable("ipro_daemon_fallback_notify_failed");
  const int before = NotifyCount();
  EXPECT_FALSE(RenotifyCounted(request, decision, notified, failed));
  EXPECT_EQ(before + 1, NotifyCount());
  EXPECT_EQ(0, notified->Get());
  EXPECT_EQ(1, failed->Get());
}

TEST_F(DaemonServeArmTest, AnExactMatchServeDoesNotRenotify) {
  // The steady state of a converged family: the stored variant IS the one
  // this client's mask names, so there is nothing to ask for.
  SetAlternates("09:00:1,0c:00:0");
  const DaemonServeRequest request = Request("image/webp");
  const DaemonServeDecision decision = Serve(request);
  ASSERT_EQ(DaemonServeVerdict::kServeOptimized, decision.verdict);
  EXPECT_FALSE(decision.fallback_hit);

  const int before = NotifyCount();
  EXPECT_FALSE(Renotify(request, decision));
  EXPECT_EQ(before, NotifyCount());
}

TEST_F(DaemonServeArmTest, TheEncodingAxisIsNotAFallback) {
  // THE COMPARISON IS AGAINST THE SELECTION MASK, and this case is what pins
  // it.  A real browser advertises brotli, so the classifier's answer (0x89)
  // differs from the identity variant this arm serves (0x09) on the
  // transfer-encoding axis -- permanently, for every browser, BY DESIGN: the
  // arm normalized that axis away before selecting.  Compared against
  // `client_mask` the predicate would fire on every optimized serve and the
  // re-notify would be permanent per-request traffic; compared against the
  // mask the read was taken at, there is no fallback here and nothing to ask.
  SetAlternates("89:00:1,09:00:1,0c:00:0");
  DaemonServeRequest request = Request("image/webp");
  request.accept_encoding = "gzip, deflate, br";
  const DaemonServeDecision decision = Serve(request);
  ASSERT_EQ(DaemonServeVerdict::kServeOptimized, decision.verdict);
  EXPECT_EQ(0x09, decision.alternate_id);
  // Stated so the case is visibly discriminating: under a comparison against
  // `client_mask` this decision WOULD be a fallback hit.
  EXPECT_NE(decision.stored_mask, decision.client_mask);
  EXPECT_FALSE(decision.fallback_hit);

  const int before = NotifyCount();
  EXPECT_FALSE(Renotify(request, decision));
  EXPECT_EQ(before, NotifyCount());
}

TEST_F(DaemonServeArmTest, TheRenotifySendsTheRawClientMask) {
  // The mask the worker is asked to build for is the classifier's own answer,
  // encoding bits included -- the value the record arm notifies at and the
  // peer's own front end sends.  The worker normalizes it for dedup, so
  // sending the arm's identity-normalized selection mask would only hide what
  // the client advertised.
  SetAlternates("29:00:1,0c:00:0");
  DaemonServeRequest request = Request("image/webp");
  request.accept_encoding = "gzip, deflate, br";
  const DaemonServeDecision decision = Serve(request);
  ASSERT_EQ(DaemonServeVerdict::kServeOptimized, decision.verdict);
  ASSERT_TRUE(decision.fallback_hit);
  ASSERT_EQ(0x89u, decision.client_mask);

  EXPECT_TRUE(Renotify(request, decision));
  EXPECT_NE(GoogleString::npos, CallLog().find("content_type=3 mask=137 "));  // 0x89
}

TEST_F(DaemonServeArmTest, TheZeroFourGateSuppressesTheRenotify) {
  // The SAME fallback hit as the first case, but the durable original carries
  // kPsFlagOriginVariesAccept: at the current pin such a URL never acquires a
  // variant family, so every re-notify would ask for work the worker will
  // never do -- permanent per-request notify traffic.  The detection still
  // fires; it is the NOTIFY that is suppressed, off the byte the rule-6
  // probe already read.
  SetAlternates("29:00:1,0c:04:0");
  const DaemonServeRequest request = Request("image/webp");
  const DaemonServeDecision decision = Serve(request);
  ASSERT_EQ(DaemonServeVerdict::kServeOptimized, decision.verdict);
  EXPECT_TRUE(decision.fallback_hit);
  EXPECT_TRUE(decision.vary_accept_origin_declared);

  const int before = NotifyCount();
  EXPECT_FALSE(Renotify(request, decision));
  EXPECT_EQ(before, NotifyCount());
}

TEST_F(DaemonServeArmTest, AnUnnameableContextSkipsTheNotify) {
  // A request whose configuration cannot be stated as a value asks for
  // nothing, mirroring the record gate's kNoOptionContext outcome: the
  // notification would be processed under the DEFAULT context, which is the
  // outcome the context exists to prevent.
  SetAlternates("29:00:1,0c:00:0");
  const DaemonServeRequest request = Request("image/webp");
  const DaemonServeDecision decision = Serve(request);
  ASSERT_EQ(DaemonServeVerdict::kServeOptimized, decision.verdict);
  ASSERT_TRUE(decision.fallback_hit);

  const int before = NotifyCount();
  EXPECT_FALSE(DaemonServeFallbackRenotify(
      *abi_, "/tmp/ps-worker.sock", request, decision, StringPiece(),
      StringPiece(), nullptr, nullptr));
  EXPECT_EQ(before, NotifyCount());
}

TEST_F(DaemonServeArmTest, TheSvgRegimeProducesNoFallbackRenotify) {
  // THE INHERITED CARVE-OUT, shown unreachable rather than asserted present.
  // The peer's front end suppresses its re-notify for a stored SVG variant
  // because SVG is derived from nothing a client sends and therefore
  // mismatches EVERY raster client permanently.  On this arm the SVG variant
  // is refused (ServeFormatIsAdvertised) and the client's own sibling is
  // recovered by exact id, so the served mask IS the client's own and there
  // is nothing to re-notify: the permanent-mismatch loop has no serve to hang
  // on.  Any FUTURE peer variant class that is permanently client-unmatchable
  // must be re-evaluated against this case.
  SetAlternates("0b:00:1,09:00:1,0c:00:0");
  const DaemonServeRequest request = Request("image/webp");
  const DaemonServeDecision decision = Serve(request);
  ASSERT_EQ(DaemonServeVerdict::kServeOptimized, decision.verdict);
  EXPECT_EQ(0x09, decision.alternate_id);
  EXPECT_FALSE(decision.fallback_hit);

  const int before = NotifyCount();
  EXPECT_FALSE(Renotify(request, decision));
  EXPECT_EQ(before, NotifyCount());
}

TEST_F(DaemonServeArmTest,
       AnOriginalFormatFallbackRenotifiesForTheClientsFormat) {
  // The second retry's serve: an SVG variant outranked everything, the
  // client's own format is not stored, and the ORIGINAL-format sibling at the
  // client's other axes is what was recovered -- a fallback on the format
  // axis alone.  That is a fallback hit like any other and it re-notifies;
  // the loop self-terminates because once the client's format sibling exists
  // the selection returns it (an exact format scores 1000 against the
  // original-format variant's 100) and the masks compare equal.
  SetAlternates("0b:00:1,08:00:1,0c:00:0");
  const DaemonServeRequest request = Request("image/webp");
  const DaemonServeDecision decision = Serve(request);
  ASSERT_EQ(DaemonServeVerdict::kServeOptimized, decision.verdict);
  EXPECT_EQ(0x08, decision.alternate_id);
  EXPECT_TRUE(decision.fallback_hit);

  EXPECT_TRUE(Renotify(request, decision));
  EXPECT_NE(GoogleString::npos, CallLog().find("content_type=3 mask=9 "));
}

// ---------------------------------------------------------------------------
// The origin-refreshed sentinel: the seam's answer to an age-expired variant
// fall-through.  The flag cases above pin WHEN the decision carries
// the marker; these pin what the helper does with it -- one fire-and-forget
// notification carrying the sentinel mask, with the same send-outcome
// accounting as the fallback re-notify.
// ---------------------------------------------------------------------------

TEST_F(DaemonServeArmTest,
       AnAgeExpiredFallThroughSendsOneOriginRefreshedSentinel) {
  // EXACTLY ONE notification, carrying the sentinel mask the worker reads by
  // name, and the SEND moving the success counter and only it.  The worker
  // answers by purging the stale variant set, clearing the dedup entry and
  // rebuilding inline -- the heal the plain re-record notification cannot
  // reach, because the processed set swallows it.
  setenv("PS_STUB_INSERTED_AT", "1", 1);  // now is 1100, max-age 600
  SetAlternates("09:00:1,0c:00:0");
  const DaemonServeRequest request = Request("image/webp");
  const DaemonServeDecision decision = Serve(request);
  ASSERT_EQ(DaemonServeVerdict::kFallThrough, decision.verdict);
  ASSERT_TRUE(decision.stale_variant_expired_by_age);

  const int before = NotifyCount();
  SimpleStats stats(thread_system_.get());
  Variable* notified = stats.AddVariable("ipro_daemon_refresh_notified");
  Variable* failed = stats.AddVariable("ipro_daemon_refresh_notify_failed");
  EXPECT_TRUE(RefreshNotifyCounted(request, decision, notified, failed));
  EXPECT_EQ(before + 1, NotifyCount());
  // 4294967292 is 0xFFFFFFFC, kOriginRefreshedSentinel.  Matched on the
  // notify line's own shape, with the trailing space, so a mask field in
  // some other line cannot satisfy it.
  EXPECT_NE(GoogleString::npos, CallLog().find("mask=4294967292 "));
  EXPECT_EQ(1, notified->Get());
  EXPECT_EQ(0, failed->Get());
}

TEST_F(DaemonServeArmTest, AFailedRefreshNotifyMovesTheFailureCounterOnly) {
  // Fire-and-forget is not SILENT, on the same terms as the fallback pair:
  // attempted-and-failed moves the failure counter and only it, so a dead
  // notify socket reads as the failure counter climbing rather than as a
  // substrate with nothing to heal.  The stand-in logs the attempt before
  // failing it, so "attempted" is asserted rather than "never asked".
  setenv("PS_STUB_NOTIFY_FAIL", "1", 1);
  setenv("PS_STUB_INSERTED_AT", "1", 1);
  SetAlternates("09:00:1,0c:00:0");
  const DaemonServeRequest request = Request("image/webp");
  const DaemonServeDecision decision = Serve(request);
  ASSERT_EQ(DaemonServeVerdict::kFallThrough, decision.verdict);
  ASSERT_TRUE(decision.stale_variant_expired_by_age);

  const int before = NotifyCount();
  SimpleStats stats(thread_system_.get());
  Variable* notified = stats.AddVariable("ipro_daemon_refresh_notified");
  Variable* failed = stats.AddVariable("ipro_daemon_refresh_notify_failed");
  EXPECT_FALSE(RefreshNotifyCounted(request, decision, notified, failed));
  EXPECT_EQ(before + 1, NotifyCount());
  EXPECT_EQ(0, notified->Get());
  EXPECT_EQ(1, failed->Get());
}

TEST_F(DaemonServeArmTest, AnUnflaggedFallThroughSendsNoRefreshNotify) {
  // SAME VERDICT, NO EXPIRY -- a client forced reload -- and no sentinel:
  // the variants are not known to be outdated, so there is nothing to
  // purge, and the re-record's plain notification is the right ask.  A
  // suppression moves NEITHER counter, which is what keeps the pair
  // readable at rollout.
  //
  // The entry is ALREADY AGE-EXPIRED (inserted at 1, requested at 1100,
  // max-age 600), so is_stale is set and only the evaluator's
  // force-revalidate clause keeps the marker -- and therefore the sentinel
  // -- off.  On a fresh entry this case would pass with the clause deleted.
  setenv("PS_STUB_INSERTED_AT", "1", 1);
  SetAlternates("09:00:1,0c:00:0");
  DaemonServeRequest request = Request("image/webp");
  request.force_revalidate = true;
  const DaemonServeDecision decision = Serve(request);
  ASSERT_EQ(DaemonServeVerdict::kFallThrough, decision.verdict);
  ASSERT_FALSE(decision.stale_variant_expired_by_age);

  const int before = NotifyCount();
  SimpleStats stats(thread_system_.get());
  Variable* notified = stats.AddVariable("ipro_daemon_refresh_notified");
  Variable* failed = stats.AddVariable("ipro_daemon_refresh_notify_failed");
  EXPECT_FALSE(RefreshNotifyCounted(request, decision, notified, failed));
  EXPECT_EQ(before, NotifyCount());
  EXPECT_EQ(0, notified->Get());
  EXPECT_EQ(0, failed->Get());
}

TEST_F(DaemonServeArmTest, AServedFallbackHitSendsNoRefreshNotify) {
  // The OTHER decision that can carry worker-bound traffic: a served
  // fallback hit re-notifies for the family's convergence, but it is not a
  // fall-through and it must not purge -- the stored variants are fresh.
  SetAlternates("29:00:1,0c:00:0");
  const DaemonServeRequest request = Request("image/webp");
  const DaemonServeDecision decision = Serve(request);
  ASSERT_EQ(DaemonServeVerdict::kServeOptimized, decision.verdict);
  ASSERT_TRUE(decision.fallback_hit);

  const int before = NotifyCount();
  EXPECT_FALSE(RefreshNotifyCounted(request, decision, nullptr, nullptr));
  EXPECT_EQ(before, NotifyCount());
}

TEST_F(DaemonServeArmTest, AnUnnameableContextSkipsTheRefreshNotify) {
  // Both halves or neither, mirroring the record gate's kNoOptionContext
  // outcome: a notification with no context would be processed under the
  // DEFAULT one, which is the outcome the context exists to prevent -- and
  // a purge under the wrong context is worse than no purge, because the
  // next age-expired fall-through asks again anyway.
  setenv("PS_STUB_INSERTED_AT", "1", 1);
  SetAlternates("09:00:1,0c:00:0");
  const DaemonServeRequest request = Request("image/webp");
  const DaemonServeDecision decision = Serve(request);
  ASSERT_EQ(DaemonServeVerdict::kFallThrough, decision.verdict);
  ASSERT_TRUE(decision.stale_variant_expired_by_age);

  const int before = NotifyCount();
  EXPECT_FALSE(DaemonServeOriginRefreshedNotify(
      *abi_, "/tmp/ps-worker.sock", request, decision, StringPiece(),
      StringPiece(), nullptr, nullptr));
  EXPECT_EQ(before, NotifyCount());
}

// ---------------------------------------------------------------------------
// Concurrency.  The header claims Record is thread-safe ("the open happens
// once per object however many threads race for it") and that one reader
// serves one request -- which is the Apache seam's shape, many worker
// threads each with their own reader over one cache handle and one loaded
// library.  No test here ever spawned a thread, so the TSan CI legs were
// vacuous for the mutex: these are the shipped equivalents of the review
// probes that ran clean under --config=clang-tsan without landing.
// ---------------------------------------------------------------------------
TEST_F(DaemonServeArmTest, RecordIsSafeUnderConcurrentCalls) {
  // 8 threads x 200 records, the review probe's shape.  The discriminating
  // power is two-layered: under TSan, a Record without its mutex reports the
  // race here, a racy second open included.  Without TSan, the counts below
  // catch a DROPPED RECORD and nothing else: against this stub a double-open
  // is invisible -- every open hands back the same placeholder and close is
  // a no-op, so both records still log -- and the leaked-handle direction of
  // that race is visible only to TSan or against the real peer, never in
  // this log.
  DaemonServeStats stats(abi_.get(), "/run/parity/cache");
  constexpr int kThreads = 8;
  constexpr int kRecords = 200;
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&stats] {
      for (int i = 0; i < kRecords; ++i) {
        // The three classes THESE probes exercise: 1, 2 and 4 in the
        // peer's numbering (optimized, original-cold, original-pending).
        // Declined (8) and skew (16) are landable too -- a response can be
        // refused on reproducibility or served over a dead substrate -- and
        // are exercised elsewhere in this file.
        stats.Record((i % 3 == 2) ? 4 : (i % 3) + 1);
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  EXPECT_TRUE(stats.open());
  const GoogleString log = CallLog();
  // Every record landed: 1600 serve_class lines, with the exact per-class
  // counts the loop produces (67/67/66 per thread).
  EXPECT_EQ(kThreads * kRecords, CountSubstring(log, "serve_class class="));
  EXPECT_EQ(kThreads * 67, CountSubstring(log, "serve_class class=1"));
  EXPECT_EQ(kThreads * 67, CountSubstring(log, "serve_class class=2"));
  EXPECT_EQ(kThreads * 66, CountSubstring(log, "serve_class class=4"));
}

TEST_F(DaemonServeArmTest, ConcurrentServeProbesStayIndependent) {
  // 8 threads x 50 serve probes, one reader per request over the shared
  // cache handle, each serve recorded through ONE shared DaemonServeStats --
  // the seam's exact shape.  Every probe must come back the same decision a
  // single-threaded serve produces: the verdict, the variant, and the body
  // length the stand-in derives from the id.
  SetAlternates("09:00:1");
  DaemonServeStats stats(abi_.get(), "/run/parity/cache");
  constexpr int kThreads = 8;
  constexpr int kProbes = 50;
  std::atomic<int> mismatches(0);
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([this, &stats, &mismatches] {
      for (int i = 0; i < kProbes; ++i) {
        DaemonServeReader reader(abi_.get(), cache_);
        const DaemonServeDecision decision =
            reader.Serve(Request("image/webp"), 1100);
        if (decision.verdict != DaemonServeVerdict::kServeOptimized ||
            decision.alternate_id != 0x09 ||
            decision.body.size() != 16u + 0x09) {
          ++mismatches;
        }
        stats.Record(decision.serve_class);
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(0, mismatches.load());
  // Every response was classified and recorded exactly once.
  EXPECT_EQ(kThreads * kProbes,
            CountSubstring(CallLog(), "serve_class class=1"));
}

}  // namespace

}  // namespace net_instaweb
