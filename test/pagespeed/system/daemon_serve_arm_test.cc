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

#include "net/instaweb/rewriter/public/option_context.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/rewrite_stats.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/util/platform.h"
#include "pagespeed/kernel/util/simple_stats.h"
#include "pagespeed/system/daemon_abi.h"
#include "pagespeed/system/siphash.h"
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

// A stylesheet URL, for the cases about the stored compressed copies: the
// optimizer writes those for stylesheets, scripts and SVG images.
const char kCssUrl[] = "/a/site.css";

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
    unsetenv("PS_STUB_GZIP_ISIZE");
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
    unsetenv("PS_STUB_GZIP_ISIZE");
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

  // A stylesheet request from a desktop browser that sends the given
  // Accept-Encoding.  `Accept` carries */*, as a browser's stylesheet fetch
  // does, so the stand-in classifies the image-format field as webp -- a
  // field the arm ignores for a stylesheet.
  DaemonServeRequest CssRequest(StringPiece accept_encoding) const {
    DaemonServeRequest request;
    request.url = kCssUrl;
    request.hostname = kHost;
    request.scheme = kScheme;
    request.accept = "text/css,*/*;q=0.1";
    request.user_agent = kUaDesktop;
    request.accept_encoding = accept_encoding;
    return request;
  }

  // A stylesheet family as the worker writes it: the identity copy (0x08),
  // its gzip (0x48) and brotli (0x88) siblings -- same capability vector,
  // the coding in bits 6-7 -- and the durable original (0x0c).  The
  // stand-in's bodies are 16 + id bytes: 24, 88 and 152.
  void UseStylesheetFamily() {
    setenv("PS_STUB_READ_CONTENT_TYPE", "1", 1);  // kPsContentCss
    setenv("PS_STUB_ORIGIN_CT", "text/css", 1);
    SetAlternates("08:00:1,48:00:1,88:00:1,0c:00:0");
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

  // The same call through a reader whose switch for the stored compressed
  // copies is ON, as a seam sets it from its port's directive.
  DaemonServeDecision ServeEncoded(const DaemonServeRequest& request) {
    DaemonServeReader reader(abi_.get(), cache_);
    reader.set_serve_stored_encodings(true);
    return reader.Serve(request, 1100 /* now_seconds */);
  }

  // A stylesheet entry as it looks after its optimized copy went missing, or
  // after the optimizer judged it already minimal -- the two have the same
  // alternates: the gzip (0x48) and brotli (0x88) copies and the durable
  // original (0x0c), no identity copy (0x08).  What tells them apart is what
  // the gzip copy says it compresses (`gzip_isize`) against the original's
  // length the optimizer recorded (`origin_length`).
  void UseStylesheetWithoutAnIdentityCopy(const char* origin_length,
                                          const char* gzip_isize) {
    setenv("PS_STUB_READ_CONTENT_TYPE", "1", 1);  // kPsContentCss
    setenv("PS_STUB_ORIGIN_CT", "text/css", 1);
    SetAlternates("48:00:1,88:00:1,0c:00:0");
    setenv("PS_STUB_ORIGIN_CONTENT_LENGTH", origin_length, 1);
    setenv("PS_STUB_GZIP_ISIZE", gzip_isize, 1);
  }

  // The serve a port that opted in performs: the reader is given a limiter.
  DaemonServeDecision ServeWithLimiter(const DaemonServeRequest& request,
                                       DaemonHealNotifyLimiter* limiter,
                                       int64 now_seconds = 1100) {
    DaemonServeReader reader(abi_.get(), cache_);
    reader.set_heal_notify_limiter(limiter);
    return reader.Serve(request, now_seconds);
  }

  // How often the stand-in handed back the gzip copy (id 0x48 = 72): the
  // probe's read, and nothing else in these cases, selects it.
  int GzipCopyReads() const {
    return CountSubstring(CallLog(), "selected=72 ");
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
    return DaemonServeFallbackRenotify(
        *abi_, "/tmp/ps-worker.sock", request, decision, default_context_,
        default_signature_, notified, notify_failed);
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

// ServeVaryFieldValueOnBothLegs, the composition for a port whose
// compressor's eligibility cannot be read from the arm.  Every case asserts
// the same value on the 200 and the 304 leg: that the two legs can never
// disagree is the whole point of the function.
TEST_F(DaemonServeArmTest, BothLegsVaryIsTheFullSetForACompressibleType) {
  DaemonServeDecision decision;
  decision.emit_vary_accept = true;
  decision.not_modified = false;
  EXPECT_EQ("Accept, Accept-Encoding",
            ServeVaryFieldValueOnBothLegs(decision, true));
  decision.not_modified = true;
  EXPECT_EQ("Accept, Accept-Encoding",
            ServeVaryFieldValueOnBothLegs(decision, true));
}

TEST_F(DaemonServeArmTest, BothLegsVaryIsTheEncodingAloneWithoutTheAcceptAxis) {
  DaemonServeDecision decision;
  decision.emit_vary_accept = false;
  decision.not_modified = false;
  EXPECT_EQ("Accept-Encoding", ServeVaryFieldValueOnBothLegs(decision, true));
  decision.not_modified = true;
  EXPECT_EQ("Accept-Encoding", ServeVaryFieldValueOnBothLegs(decision, true));
}

TEST_F(DaemonServeArmTest,
       BothLegsVaryIsTheAcceptAxisAloneForAnUncompressible) {
  DaemonServeDecision decision;
  decision.emit_vary_accept = true;
  decision.not_modified = false;
  EXPECT_EQ("Accept", ServeVaryFieldValueOnBothLegs(decision, false));
  decision.not_modified = true;
  EXPECT_EQ("Accept", ServeVaryFieldValueOnBothLegs(decision, false));
}

TEST_F(DaemonServeArmTest, BothLegsVaryIsEmptyWhenNeitherAxisApplies) {
  DaemonServeDecision decision;
  decision.emit_vary_accept = false;
  decision.not_modified = false;
  EXPECT_EQ("", ServeVaryFieldValueOnBothLegs(decision, false));
  decision.not_modified = true;
  EXPECT_EQ("", ServeVaryFieldValueOnBothLegs(decision, false));
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

TEST_F(DaemonServeArmTest,
       AConvertedSlotHoldingUnconvertedBytesIsNotRelabelled) {
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
  EXPECT_EQ("",
            ServeVaryFieldValue(VaryDecision(false, true, at_the_floor), true));
  EXPECT_EQ(
      "Accept-Encoding",
      ServeVaryFieldValue(VaryDecision(false, true, over_the_floor), true));
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

TEST_F(DaemonServeArmTest, AServeHitCarriesItsHostWhenTheOptimizerTakesOne) {
  DaemonServeStats stats(abi_.get(), "/run/parity/cache");
  stats.RecordHit(kPsContentImage, 4096, 1024, kMaskWebpDesktop,
                  "www.example.test");
  const GoogleString log = CallLog();
  EXPECT_NE(GoogleString::npos,
            log.find("serve_hit_host type=3 original=4096 optimized=1024 "
                     "mask=9 host=www.example.test"));
  // One record per serve: the host-less recorder is not called as well.
  EXPECT_EQ(GoogleString::npos, log.find("serve_hit type=3"));
}

TEST_F(DaemonServeArmTest, AServeHitWithoutAHostUsesTheHostlessRecorder) {
  DaemonServeStats stats(abi_.get(), "/run/parity/cache");
  stats.RecordHit(kPsContentImage, 4096, 1024, kMaskWebpDesktop, "");
  const GoogleString log = CallLog();
  EXPECT_NE(GoogleString::npos,
            log.find("serve_hit type=3 original=4096 optimized=1024 mask=9"));
  EXPECT_EQ(GoogleString::npos, log.find("serve_hit_host"));
}

TEST_F(DaemonServeArmTest, AnOptimizerWithoutTheHostRecorderStillRecordsTheHit) {
  // An optimizer package from before per-host serve savings: the entry point
  // is absent, the library still loads, and the hit is recorded as before.
  GoogleString error;
  std::unique_ptr<DaemonAbi> older(LoadDaemonAbi(
      StrCat(GTestSrcDir(),
             "/test/pagespeed/system/libdaemon_stub_no_generation.so"),
      &error));
  ASSERT_TRUE(older != nullptr) << error;
  DaemonServeStats stats(older.get(), "/run/parity/cache");
  stats.RecordHit(kPsContentImage, 4096, 1024, kMaskWebpDesktop,
                  "www.example.test");
  const GoogleString log = CallLog();
  EXPECT_NE(GoogleString::npos,
            log.find("serve_hit type=3 original=4096 optimized=1024 mask=9"));
  EXPECT_EQ(GoogleString::npos, log.find("serve_hit_host"));
}

TEST_F(DaemonServeArmTest, EveryServedContentClassIsRecordedAsItself) {
  // Every served class is recorded as itself: an optimized, worker-produced
  // entry of ANY class is served, and its hit is recorded under the entry's
  // own class -- nothing here filters by class or declares one
  // "not applicable".  Pinned for the four classes the optimizer reports,
  // through the same decision fields every port hands the recorder.
  setenv("PS_STUB_ORIGIN_CONTENT_LENGTH", "4096", 1);
  SetAlternates("08:00:1,0c:00:0");
  DaemonServeStats stats(abi_.get(), "/run/parity/cache");
  const int kClasses[] = {kPsContentHtml, kPsContentCss, kPsContentJs,
                          kPsContentImage};
  for (const int content_class : kClasses) {
    SCOPED_TRACE(content_class);
    setenv("PS_STUB_READ_CONTENT_TYPE", IntegerToString(content_class).c_str(),
           1);
    const DaemonServeDecision decision = Serve(Request("*/*"));
    ASSERT_EQ(DaemonServeVerdict::kServeOptimized, decision.verdict);
    EXPECT_EQ(kPsServeClassOptimized, decision.serve_class);
    EXPECT_TRUE(decision.worker_processed);
    EXPECT_EQ(4096u, decision.origin_content_length);
    EXPECT_EQ(content_class, decision.ps_content_type);
    stats.RecordHit(decision.ps_content_type, decision.origin_content_length,
                    decision.body.size(), decision.stored_mask);
    EXPECT_NE(GoogleString::npos,
              CallLog().find(StrCat("serve_hit type=",
                                    IntegerToString(content_class),
                                    " original=4096")));
  }
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

TEST_F(DaemonServeArmTest, AStylesheetServeIsNotAFallbackForTheFormatAxis) {
  // A stylesheet variant is stored with the image-format field at original,
  // because format is meaningless for it, while a browser's selection mask
  // carries an image format.  Comparing that field for non-image content
  // would class every stylesheet serve as a permanent mismatch and re-notify
  // the worker for ever; the encoding axis was normalized away for exactly
  // this reason, and the format axis for a stylesheet is the same non-fact.
  // So the fallback comparison excludes the image-format field when the
  // served entry is not an image.
  setenv("PS_STUB_READ_CONTENT_TYPE", "1", 1);  // kPsContentCss
  SetAlternates("08:00:1,0c:00:0");

  // A webp client and an avif client: both differ from the stored original
  // format alone, and neither is a fallback.
  for (const char* accept : {"image/webp,*/*", "image/avif,image/webp,*/*"}) {
    const DaemonServeRequest request = Request(accept);
    const DaemonServeDecision decision = Serve(request);
    ASSERT_EQ(DaemonServeVerdict::kServeOptimized, decision.verdict);
    // Stated so the case is visibly discriminating: the stored and
    // selection masks differ, on the format field, and that difference is
    // not a fallback for a stylesheet.
    ASSERT_NE(decision.stored_mask, decision.selection_mask);
    EXPECT_FALSE(decision.fallback_hit) << accept;

    SimpleStats stats(thread_system_.get());
    Variable* notified = stats.AddVariable("ipro_daemon_fallback_notified");
    Variable* failed = stats.AddVariable("ipro_daemon_fallback_notify_failed");
    const int before = NotifyCount();
    EXPECT_FALSE(RenotifyCounted(request, decision, notified, failed));
    EXPECT_EQ(before, NotifyCount()) << accept;
    EXPECT_EQ(0, notified->Get()) << accept;
    EXPECT_EQ(0, failed->Get()) << accept;
  }
}

TEST_F(DaemonServeArmTest, AScriptServeIsNotAFallbackForTheFormatAxis) {
  // The same rule for a script: the stored variant's format field is
  // original, the client's carries webp, and that alone is not a fallback.
  setenv("PS_STUB_READ_CONTENT_TYPE", "2", 1);  // kPsContentJs
  SetAlternates("08:00:1,0c:00:0");
  const DaemonServeRequest request = Request("image/webp,*/*");
  const DaemonServeDecision decision = Serve(request);
  ASSERT_EQ(DaemonServeVerdict::kServeOptimized, decision.verdict);
  ASSERT_NE(decision.stored_mask, decision.selection_mask);
  EXPECT_FALSE(decision.fallback_hit);

  const int before = NotifyCount();
  EXPECT_FALSE(Renotify(request, decision));
  EXPECT_EQ(before, NotifyCount());
}

TEST_F(DaemonServeArmTest,
       AStylesheetFallbackOnTheSaveDataAxisStillRenotifies) {
  // THE OTHER AXES ARE STILL IN THE COMPARISON, for a stylesheet exactly as
  // for an image: the convergence the re-notify exists for is viewport,
  // density and Save-Data.  The stored variant is a stylesheet's own shape
  // -- original format -- WITH Save-Data (0x28), and this client sent none
  // (0x09): with the format field out of the comparison the ONLY difference
  // left is Save-Data, so this case fails if the comparison ever drops that
  // axis too.  The serve IS a fallback and one re-notify goes out.
  setenv("PS_STUB_READ_CONTENT_TYPE", "1", 1);  // kPsContentCss
  SetAlternates("28:00:1,0c:00:0");
  const DaemonServeRequest request = Request("image/webp");
  const DaemonServeDecision decision = Serve(request);
  ASSERT_EQ(DaemonServeVerdict::kServeOptimized, decision.verdict);
  EXPECT_EQ(0x28, decision.alternate_id);
  EXPECT_TRUE(decision.fallback_hit);
  EXPECT_EQ(0x09u, decision.client_mask);

  SimpleStats stats(thread_system_.get());
  Variable* notified = stats.AddVariable("ipro_daemon_fallback_notified");
  Variable* failed = stats.AddVariable("ipro_daemon_fallback_notify_failed");
  const int before = NotifyCount();
  EXPECT_TRUE(RenotifyCounted(request, decision, notified, failed));
  EXPECT_EQ(before + 1, NotifyCount());
  EXPECT_EQ(1, notified->Get());
  EXPECT_EQ(0, failed->Get());
}

// The image half of the rule -- a stored original-format IMAGE served to a
// webp client IS a fallback and re-notifies -- is already pinned by
// AnOriginalFormatFallbackRenotifiesForTheClientsFormat below, whose content
// class is the stub's image default; this change must not disturb it.

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
  EXPECT_NE(GoogleString::npos,
            CallLog().find("content_type=3 mask=137 "));  // 0x89
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
  EXPECT_FALSE(DaemonServeFallbackRenotify(*abi_, "/tmp/ps-worker.sock",
                                           request, decision, StringPiece(),
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

// --- per-class split helpers -------------------------------------------------

TEST_F(DaemonServeArmTest, TheServedClassSplitMovesOnlyKnownClasses) {
  SimpleStats stats(thread_system_.get());
  RewriteStats::InitStats(&stats);
  RewriteStats rewrite_stats(false, &stats, thread_system_.get(), nullptr);

  RecordDaemonServedClass(&rewrite_stats, kPsContentCss);
  RecordDaemonServedClass(&rewrite_stats, kPsContentImage);
  RecordDaemonServedClass(&rewrite_stats, kPsContentOther);
  // html has no split counter; the caller's total covers it.
  RecordDaemonServedClass(&rewrite_stats, kPsContentHtml);
  // A null stats pointer (a seam without shared statistics) is a no-op.
  RecordDaemonServedClass(nullptr, kPsContentCss);

  EXPECT_EQ(1, stats.GetVariable("ipro_daemon_served_css")->Get());
  EXPECT_EQ(0, stats.GetVariable("ipro_daemon_served_js")->Get());
  EXPECT_EQ(1, stats.GetVariable("ipro_daemon_served_image")->Get());
  EXPECT_EQ(1, stats.GetVariable("ipro_daemon_served_other")->Get());
  EXPECT_EQ(0, stats.GetVariable("ipro_daemon_fallthrough_css")->Get());
}

// ---------------------------------------------------------------------------
// Today's serve for a client that accepts compression, pinned BEFORE the
// stored compressed copies became servable.  A reader that is not told
// otherwise must keep producing exactly this -- the selection, the reads it
// takes, the bytes, the validator, the freshness and the `Vary` of both
// legs -- whatever the client's Accept-Encoding says.  This is the property
// the directive's default rests on, and its rollback.
// ---------------------------------------------------------------------------

TEST_F(DaemonServeArmTest, ACompressionCapableClientGetsTodaysIdentityServe) {
  UseStylesheetFamily();
  for (const char* accept_encoding :
       {"gzip, deflate, br", "br", "gzip", "identity", ""}) {
    SCOPED_TRACE(accept_encoding);
    remove(log_path_.c_str());
    const DaemonServeDecision decision = Serve(CssRequest(accept_encoding));
    EXPECT_EQ(DaemonServeVerdict::kServeOptimized, decision.verdict);
    EXPECT_STREQ(daemon_serve_reason::kOptimizedVariant, decision.reason);
    EXPECT_EQ(kPsServeClassOptimized, decision.serve_class);
    // The read is taken at identity whatever was advertised...
    EXPECT_EQ(0x09u, decision.selection_mask);
    // ...so the identity copy is what is served, never a sibling.
    EXPECT_EQ(0x08, decision.alternate_id);
    EXPECT_EQ(0x08u, decision.stored_mask);
    EXPECT_EQ(24u, decision.body.size());
    EXPECT_EQ("text/css", decision.content_type);
    EXPECT_EQ("W/\"ps-0000000800-0000000000000018\"", decision.etag);
    EXPECT_EQ("must-revalidate, max-age=600", decision.cache_control);
    EXPECT_EQ(100u, decision.age_seconds);
    EXPECT_FALSE(decision.not_modified);
    EXPECT_FALSE(decision.fallback_hit);
    EXPECT_FALSE(decision.emit_vary_accept);
    // Apache's per-leg composition says nothing on the 200 (the compressor
    // states the axis there); nginx's both-legs one states it.
    EXPECT_EQ("", ServeVaryFieldValue(decision, true));
    EXPECT_EQ("Accept-Encoding", ServeVaryFieldValueOnBothLegs(decision, true));
    // The exact reads: the rule-6 probe of the original, then one selection
    // at the identity mask.  No by-id read of a coded sibling.
    EXPECT_EQ(
        "read_alternate url=/a/site.css id=12\n"
        "read_best url=/a/site.css mask=9 selected=8 score=300\n",
        CallLog());
  }

  // And the revalidation of that copy, by the same client.
  DaemonServeRequest conditional = CssRequest("gzip, deflate, br");
  conditional.if_none_match = "W/\"ps-0000000800-0000000000000018\"";
  const DaemonServeDecision revalidated = Serve(conditional);
  EXPECT_TRUE(revalidated.not_modified);
  EXPECT_EQ(0x08, revalidated.alternate_id);
  // 24 bytes is under the compressor's pass-through floor, so the 304 leg
  // states no encoding axis on Apache, and the both-legs one still does.
  EXPECT_EQ("", ServeVaryFieldValue(revalidated, true));
  EXPECT_EQ("Accept-Encoding",
            ServeVaryFieldValueOnBothLegs(revalidated, true));
}

// ---------------------------------------------------------------------------
// The stored compressed copies.  The optimizer writes a gzip and a brotli
// sibling next to each optimized stylesheet, script and SVG image; a reader
// whose switch is on may hand one out, to a client that listed that coding,
// labelled with the coding read off the entry.
// ---------------------------------------------------------------------------

TEST_F(DaemonServeArmTest, TheCodingIsReadOffTheStoredMask) {
  EXPECT_EQ(1u, kPsTransferEncodingGzip);
  EXPECT_EQ(2u, kPsTransferEncodingBrotli);
  EXPECT_EQ("", ServeContentEncodingToken(0x08));
  EXPECT_EQ("gzip", ServeContentEncodingToken(0x48));
  EXPECT_EQ("br", ServeContentEncodingToken(0x88));
  // Only bits 6-7 count; every other axis is irrelevant to the label.
  EXPECT_EQ("br", ServeContentEncodingToken(0xBB));
  EXPECT_EQ("gzip", ServeContentEncodingToken(0x7F));
  // The reserved value names no coding.
  EXPECT_EQ("", ServeContentEncodingToken(0xC8));
}

TEST_F(DaemonServeArmTest, OnlyACodingListedByNameWithAWeightCounts) {
  struct {
    const char* header;
    const char* coding;
    bool advertised;
  } cases[] = {
      {"gzip, deflate, br", "br", true},
      {"gzip, deflate, br", "gzip", true},
      {"gzip, deflate, br, zstd", "br", true},
      {"BR", "br", true},
      {"Gzip", "gzip", true},
      {" br ; q=0.5 ", "br", true},
      {"br;q=1", "br", true},
      {"br;Q=1.000", "br", true},
      {"br;q=0.001", "br", true},
      {"gzip;q=0.8, br;q=0.9", "gzip", true},
      {"br;level=5", "br", true},
      {"", "br", false},
      {"identity", "br", false},
      {"gzip", "br", false},
      {"br;q=0", "br", false},
      {"br;q=0.0", "br", false},
      {"br;q=0.000", "br", false},
      {"br;q=0;level=5", "br", false},
      {"br;q=00", "br", false},
      {"br;q=1.5", "br", false},
      {"br;q=0.0001", "br", false},
      {"br;q=", "br", false},
      {"br;q=-1", "br", false},
      {"br;q= 1", "br", false},
      {"br;q =0", "br", false},
      {"br;q =1", "br", false},
      {"br; Q =0", "br", false},
      {"br; Q =1", "br", false},
      {"br;q\t=0", "br", false},
      {"br;q", "br", false},
      {"br; q ", "br", false},
      {"br;q=abc", "br", false},
      {"br;q=1x", "br", false},
      {"br, br;q=0", "br", false},
      {"*", "br", false},
      {"*;q=1", "gzip", false},
      {"x-brotli", "br", false},
      {"brotli", "br", false},
      {"x-gzip", "gzip", false},
      {"gzip", "", false},
  };
  for (const auto& c : cases) {
    EXPECT_EQ(c.advertised,
              ServeAcceptEncodingAdvertises(c.header, c.coding))
        << "Accept-Encoding: \"" << c.header << "\", coding \"" << c.coding
        << "\"";
  }
}

TEST_F(DaemonServeArmTest,
       TheSelectionKeepsTheClientsCodingOnlyWhenOnAndListed) {
  // Off: identity whatever the client said -- today's selection.
  EXPECT_EQ(0x09u, ServeSelectionMask(0x89, "gzip, deflate, br", false));
  EXPECT_EQ(0x09u, ServeSelectionMask(0x49, "gzip", false));
  // On: the classifier's coding, when the header lists it plainly.
  EXPECT_EQ(0x89u, ServeSelectionMask(0x89, "gzip, deflate, br", true));
  EXPECT_EQ(0x49u, ServeSelectionMask(0x49, "gzip", true));
  EXPECT_EQ(0x09u, ServeSelectionMask(0x09, "identity", true));
  // On, and the classifier answered a coding the header does not plainly
  // list: identity.  A classifier that reads `br;q=0;level=5` or `*` as
  // brotli does not get to decide what this module labels.
  EXPECT_EQ(0x09u, ServeSelectionMask(0x89, "br;q=0;level=5", true));
  EXPECT_EQ(0x09u, ServeSelectionMask(0x89, "*", true));
  EXPECT_EQ(0x09u, ServeSelectionMask(0x89, "gzip", true));
  EXPECT_EQ(0x09u, ServeSelectionMask(0x49, "br", true));
  // The reserved coding is never kept.
  EXPECT_EQ(0x09u, ServeSelectionMask(0xC9, "gzip, br", true));
  // Every other axis is the classifier's, untouched, either way.
  EXPECT_EQ(0xBAu, ServeSelectionMask(0xBA, "br", true));
  EXPECT_EQ(0x3Au, ServeSelectionMask(0xBA, "br", false));
}

TEST_F(DaemonServeArmTest,
       ACodedCopyIsServableOnlyWhenOnListedAndForItsClasses) {
  // Identity is servable to everyone, switch or not -- the floor.
  EXPECT_TRUE(ServeEncodingIsServable(0x08, 0x09, kPsContentCss, false));
  EXPECT_TRUE(ServeEncodingIsServable(0x08, 0x89, kPsContentHtml, true));
  // Off: no coded copy, for anyone.
  EXPECT_FALSE(ServeEncodingIsServable(0x88, 0x89, kPsContentCss, false));
  // On: exactly the coding the selection kept, for a stylesheet, a script
  // or an image -- an image only from the original-format slot, where an
  // SVG URL's coded copies sit.
  EXPECT_TRUE(ServeEncodingIsServable(0x88, 0x89, kPsContentCss, true));
  EXPECT_TRUE(ServeEncodingIsServable(0x48, 0x49, kPsContentJs, true));
  EXPECT_TRUE(ServeEncodingIsServable(0x88, 0x89, kPsContentImage, true));
  // A coded image in a converted-format slot (WebP, AVIF, kSvg): never.
  // The served media type is corrected by sniffing the body, which cannot
  // see through a content coding.
  EXPECT_FALSE(ServeEncodingIsServable(0x89, 0x89, kPsContentImage, true));
  EXPECT_FALSE(ServeEncodingIsServable(0x8A, 0x8A, kPsContentImage, true));
  EXPECT_FALSE(ServeEncodingIsServable(0x8B, 0x89, kPsContentImage, true));
  // The slot rule is the image class's alone.
  EXPECT_TRUE(ServeEncodingIsServable(0x89, 0x89, kPsContentCss, true));
  // Any other coding: never.
  EXPECT_FALSE(ServeEncodingIsServable(0x48, 0x89, kPsContentCss, true));
  EXPECT_FALSE(ServeEncodingIsServable(0x88, 0x49, kPsContentCss, true));
  EXPECT_FALSE(ServeEncodingIsServable(0x88, 0x09, kPsContentCss, true));
  // HTML and unclassified content stay uncoded.
  EXPECT_FALSE(ServeEncodingIsServable(0x88, 0x89, kPsContentHtml, true));
  EXPECT_FALSE(ServeEncodingIsServable(0x88, 0x89, kPsContentOther, true));
  // The reserved coding is never servable.
  EXPECT_FALSE(ServeEncodingIsServable(0xC8, 0xC9, kPsContentCss, true));
  EXPECT_TRUE(ServeEncodedContentClass(kPsContentCss));
  EXPECT_TRUE(ServeEncodedContentClass(kPsContentJs));
  EXPECT_TRUE(ServeEncodedContentClass(kPsContentImage));
  EXPECT_FALSE(ServeEncodedContentClass(kPsContentHtml));
  EXPECT_FALSE(ServeEncodedContentClass(kPsContentOther));
  EXPECT_FALSE(ServeEncodedContentClass(99));
}

TEST_F(DaemonServeArmTest, OnABrotliClientGetsTheStoredBrotliCopy) {
  UseStylesheetFamily();
  const DaemonServeRequest request = CssRequest("gzip, deflate, br");
  const DaemonServeDecision decision = ServeEncoded(request);
  ASSERT_EQ(DaemonServeVerdict::kServeOptimized, decision.verdict);
  EXPECT_EQ(kPsServeClassOptimized, decision.serve_class);
  EXPECT_EQ(0x89u, decision.selection_mask);
  EXPECT_EQ(0x88, decision.alternate_id);
  EXPECT_EQ(0x88u, decision.stored_mask);
  EXPECT_EQ("br", decision.content_encoding);
  EXPECT_EQ(152u, decision.body.size());
  // The client's own mask is the classifier's answer, unmoved by the switch.
  const DaemonServeDecision off = Serve(request);
  EXPECT_EQ(off.client_mask, decision.client_mask);
  // A served coded copy is exact on every axis that matters: no re-notify.
  EXPECT_FALSE(decision.fallback_hit);
  // The validator is per coding: the brotli copy's own capability field,
  // and never the suffix the server's compressor appends to tags of the
  // responses it compresses.
  EXPECT_EQ("W/\"ps-0000008800-0000000000000098\"", decision.etag);
  EXPECT_NE(off.etag, decision.etag);
  EXPECT_FALSE(strings::EndsWith(decision.etag, "-gzip\""));
}

TEST_F(DaemonServeArmTest, OnAGzipOnlyClientGetsTheStoredGzipCopy) {
  UseStylesheetFamily();
  const DaemonServeDecision decision = ServeEncoded(CssRequest("gzip"));
  ASSERT_EQ(DaemonServeVerdict::kServeOptimized, decision.verdict);
  EXPECT_EQ(0x48, decision.alternate_id);
  EXPECT_EQ("gzip", decision.content_encoding);
  EXPECT_EQ(88u, decision.body.size());
  EXPECT_EQ("W/\"ps-0000004800-0000000000000058\"", decision.etag);
}

TEST_F(DaemonServeArmTest, OnWithoutACodedCopyTheIdentityCopyIsServedAsToday) {
  setenv("PS_STUB_READ_CONTENT_TYPE", "1", 1);  // kPsContentCss
  setenv("PS_STUB_ORIGIN_CT", "text/css", 1);
  SetAlternates("08:00:1,0c:00:0");
  const DaemonServeRequest request = CssRequest("gzip, deflate, br");
  const DaemonServeDecision decision = ServeEncoded(request);
  ASSERT_EQ(DaemonServeVerdict::kServeOptimized, decision.verdict);
  EXPECT_EQ(0x08, decision.alternate_id);
  EXPECT_TRUE(decision.content_encoding.empty());
  EXPECT_EQ("W/\"ps-0000000800-0000000000000018\"", decision.etag);
  // NOT a fallback.  The selection kept brotli and the stored copy is
  // identity, but the worker writes the coded siblings of every variant
  // itself: asking it for one on every request would be permanent traffic
  // for nothing.
  EXPECT_NE(decision.stored_mask, decision.selection_mask);
  EXPECT_FALSE(decision.fallback_hit);
  const int before = NotifyCount();
  EXPECT_FALSE(Renotify(request, decision));
  EXPECT_EQ(before, NotifyCount());
}

TEST_F(DaemonServeArmTest, AClientThatDidNotListTheCodingIsNeverServedIt) {
  UseStylesheetFamily();
  // The stand-in's classifier finds `br` by substring, so it answers
  // brotli for every one of these -- and none of them lists `br` by name
  // with a weight above zero.  The coding on the wire is never the
  // classifier's call alone.
  for (const char* accept_encoding :
       {"br;q=0", "br;q=0;level=5", "br;q=00", "br;q=0.0001", "x-brotli",
        "brotli, gzip;q=0"}) {
    SCOPED_TRACE(accept_encoding);
    const DaemonServeDecision decision =
        ServeEncoded(CssRequest(accept_encoding));
    ASSERT_EQ(DaemonServeVerdict::kServeOptimized, decision.verdict);
    ASSERT_EQ(kPsTransferEncodingBrotli,
              ServeTransferEncoding(decision.client_mask));
    EXPECT_EQ(kPsTransferEncodingIdentity,
              ServeTransferEncoding(decision.selection_mask));
    EXPECT_EQ(0x08, decision.alternate_id);
    EXPECT_TRUE(decision.content_encoding.empty());
  }
}

TEST_F(DaemonServeArmTest, AReaderServesUncodedCopiesUnlessTold) {
  UseStylesheetFamily();
  const DaemonServeDecision off = Serve(CssRequest("gzip, deflate, br"));
  EXPECT_EQ(0x08, off.alternate_id);
  EXPECT_TRUE(off.content_encoding.empty());
  // The durable original is origin bytes: never labelled, switch or not.
  SetAlternates("0c:00:0");
  const DaemonServeDecision original = ServeEncoded(CssRequest("br"));
  ASSERT_EQ(DaemonServeVerdict::kServeOriginal, original.verdict);
  EXPECT_TRUE(original.content_encoding.empty());
}

TEST_F(DaemonServeArmTest, AnHtmlEntryIsServedUncodedEvenWhenOn) {
  setenv("PS_STUB_READ_CONTENT_TYPE", "0", 1);  // kPsContentHtml
  setenv("PS_STUB_ORIGIN_CT", "text/html", 1);
  SetAlternates("08:00:1,88:00:1,0c:00:0");
  DaemonServeRequest request = CssRequest("gzip, deflate, br");
  request.url = "/a/page.html";
  request.accept = "text/html";
  const DaemonServeDecision decision = ServeEncoded(request);
  ASSERT_EQ(DaemonServeVerdict::kServeOptimized, decision.verdict);
  EXPECT_EQ(0x08, decision.alternate_id);
  EXPECT_TRUE(decision.content_encoding.empty());
  // The peer did offer the brotli copy; the refusal is this arm's.
  const GoogleString log = CallLog();
  EXPECT_NE(GoogleString::npos, log.find("selected=136"));
  // And the refusal costs ONE by-id read, the uncoded sibling's: no coded
  // id is asked for again once the class refused it, and no id twice.
  EXPECT_EQ(GoogleString::npos,
            log.find("read_alternate url=/a/page.html id=136"));
  const size_t uncoded = log.find("read_alternate url=/a/page.html id=8\n");
  ASSERT_NE(GoogleString::npos, uncoded);
  EXPECT_EQ(GoogleString::npos,
            log.find("read_alternate url=/a/page.html id=8", uncoded + 1));
}

TEST_F(DaemonServeArmTest, TheByIdRetryStillReachesTheUncodedSiblingWhenOn) {
  // The vectorized-URL regime with the switch on: a brotli SVG variant
  // outranks everything and is refused on its format; the client's format
  // exists only as the uncoded copy.  The retry asks for the client's
  // coding first and then for identity, so the variant is still served
  // rather than lost to the durable original.
  SetAlternates("8b:00:1,09:00:1,0c:00:0");
  DaemonServeRequest request = Request("image/webp");
  request.accept_encoding = "gzip, deflate, br";
  const DaemonServeDecision decision = ServeEncoded(request);
  ASSERT_EQ(DaemonServeVerdict::kServeOptimized, decision.verdict);
  EXPECT_EQ(0x09, decision.alternate_id);
  EXPECT_TRUE(decision.content_encoding.empty());
  const GoogleString log = CallLog();
  EXPECT_NE(GoogleString::npos, log.find("selected=139"));
  EXPECT_NE(GoogleString::npos,
            log.find("read_alternate url=/a/photo.png id=137 miss"));
  EXPECT_NE(GoogleString::npos,
            log.find("read_alternate url=/a/photo.png id=136 miss"));
  EXPECT_NE(GoogleString::npos,
            log.find("read_alternate url=/a/photo.png id=9\n"));
}

TEST_F(DaemonServeArmTest, ACodedCopyInAConvertedFormatSlotIsNeverServed) {
  // A brotli copy in the WebP slot outranks its uncoded sibling for a
  // brotli client.  It is refused: the served media type is corrected by
  // sniffing the body, which cannot see through a content coding, so WebP
  // bytes would go out under the origin's image/png.  The uncoded WebP copy
  // is served instead, unlabelled.
  SetAlternates("89:00:1,09:00:1,0c:00:0");
  DaemonServeRequest request = Request("image/webp");
  request.accept_encoding = "gzip, deflate, br";
  const DaemonServeDecision decision = ServeEncoded(request);
  ASSERT_EQ(DaemonServeVerdict::kServeOptimized, decision.verdict);
  EXPECT_EQ(0x09, decision.alternate_id);
  EXPECT_EQ(0x09u, decision.stored_mask);
  EXPECT_TRUE(decision.content_encoding.empty());
  EXPECT_FALSE(decision.fallback_hit);
  const GoogleString log = CallLog();
  // The peer did offer the coded copy; the refusal is this arm's.
  EXPECT_NE(GoogleString::npos, log.find("selected=137"));
  EXPECT_NE(GoogleString::npos,
            log.find("read_alternate url=/a/photo.png id=9\n"));
}

TEST_F(DaemonServeArmTest, ACodedServeVariesOnAcceptEncodingOnBothLegs) {
  const GoogleString small(10, 'a');
  const GoogleString big(kServeCompressorPassThroughBytes + 1, 'a');
  // No compressor ever sees a coded body, so the arm is the only emitter of
  // the encoding axis -- on the 200 and the 304 alike, whatever the length
  // and whether or not the media type is one the server compresses.
  DaemonServeDecision decision = VaryDecision(false, false, small);
  decision.content_encoding = kServeContentEncodingBrotli;
  EXPECT_EQ("Accept-Encoding", ServeVaryFieldValue(decision, false));
  EXPECT_EQ("Accept-Encoding", ServeVaryFieldValue(decision, true));
  decision.not_modified = true;
  EXPECT_EQ("Accept-Encoding", ServeVaryFieldValue(decision, false));
  decision.emit_vary_accept = true;
  EXPECT_EQ("Accept, Accept-Encoding", ServeVaryFieldValue(decision, false));
  EXPECT_EQ("Accept, Accept-Encoding",
            ServeVaryFieldValueOnBothLegs(decision, false));
  decision.not_modified = false;
  EXPECT_EQ("Accept, Accept-Encoding",
            ServeVaryFieldValueOnBothLegs(decision, false));
  // Stated once, even where the compressor rule would state it as well.
  DaemonServeDecision both = VaryDecision(false, true, big);
  both.content_encoding = kServeContentEncodingGzip;
  EXPECT_EQ("Accept-Encoding", ServeVaryFieldValue(both, true));
  EXPECT_EQ("Accept-Encoding", ServeVaryFieldValueOnBothLegs(both, true));
}

TEST_F(DaemonServeArmTest, TheBrotliCopysTagRevalidatesOnlyTheBrotliCopy) {
  UseStylesheetFamily();
  const DaemonServeDecision first =
      ServeEncoded(CssRequest("gzip, deflate, br"));
  ASSERT_EQ("br", first.content_encoding);

  DaemonServeRequest again = CssRequest("gzip, deflate, br");
  again.if_none_match = first.etag;
  const DaemonServeDecision revalidated = ServeEncoded(again);
  EXPECT_TRUE(revalidated.not_modified);
  EXPECT_EQ(kPsServeClassOptimized, revalidated.serve_class);
  // The 304 knows which copy it revalidates, so its `Vary` is its 200's.
  EXPECT_EQ("br", revalidated.content_encoding);
  EXPECT_EQ("Accept-Encoding", ServeVaryFieldValue(revalidated, true));

  // The same tag from a client that is now served the gzip copy names a
  // different representation...
  DaemonServeRequest gzip_only = CssRequest("gzip");
  gzip_only.if_none_match = first.etag;
  EXPECT_FALSE(ServeEncoded(gzip_only).not_modified);
  // ...and so it does once the switch is off and the uncoded copy goes out.
  DaemonServeRequest off = CssRequest("gzip, deflate, br");
  off.if_none_match = first.etag;
  EXPECT_FALSE(Serve(off).not_modified);
}

TEST_F(DaemonServeArmTest, ACodedServeIsRecordedAtTheBytesSent) {
  // The accounting every port performs, end to end: the hit is recorded
  // with the bytes that go on the wire -- the coded copy's -- against the
  // uncompressed original, under the coded copy's own mask, which is what
  // the optimizer splits its per-coding rows on.
  setenv("PS_STUB_ORIGIN_CONTENT_LENGTH", "4096", 1);
  UseStylesheetFamily();
  const DaemonServeDecision decision = ServeEncoded(CssRequest("br"));
  ASSERT_EQ(DaemonServeVerdict::kServeOptimized, decision.verdict);
  ASSERT_TRUE(decision.worker_processed);
  DaemonServeStats stats(abi_.get(), "/run/parity/cache");
  stats.RecordHit(decision.ps_content_type, decision.origin_content_length,
                  decision.body.size(), decision.stored_mask);
  EXPECT_NE(GoogleString::npos,
            CallLog().find("serve_hit type=1 original=4096 optimized=152 "
                           "mask=88"));
}

}  // namespace

// ---------------------------------------------------------------------------
// A lost optimized copy: recognised, limited, and asked for.
// ---------------------------------------------------------------------------

TEST_F(DaemonServeArmTest, AStoredOriginalBesideCopiesOfASmallerCopyIsALoss) {
  // The original is 4307 bytes; the gzip copy compresses 3899: it is the
  // gzip of an optimized copy, and that copy is not in the entry.
  UseStylesheetWithoutAnIdentityCopy("4307", "3899");
  DaemonHealNotifyLimiter limiter(1);
  const DaemonServeDecision decision =
      ServeWithLimiter(CssRequest("identity"), &limiter);
  ASSERT_EQ(DaemonServeVerdict::kServeOriginal, decision.verdict);
  EXPECT_STREQ(daemon_serve_reason::kDurableOriginal, decision.reason);
  EXPECT_EQ(kPsServeClassOriginalPending, decision.serve_class);
  EXPECT_TRUE(decision.lost_optimized_copy);
  EXPECT_EQ(1, GzipCopyReads());
}

TEST_F(DaemonServeArmTest, CompressedCopiesOfTheOriginalItselfAreNotALoss) {
  // Already minimal: the gzip copy compresses the original, all 4307 bytes
  // of it.  Nothing was ever due, and nothing is asked for.
  UseStylesheetWithoutAnIdentityCopy("4307", "4307");
  DaemonHealNotifyLimiter limiter(1);
  const DaemonServeDecision decision =
      ServeWithLimiter(CssRequest("identity"), &limiter);
  ASSERT_EQ(DaemonServeVerdict::kServeOriginal, decision.verdict);
  EXPECT_FALSE(decision.lost_optimized_copy);
}

TEST_F(DaemonServeArmTest, AnOriginalWithNoOptimizerCopiesIsNotALoss) {
  // Not optimized yet, or not optimizable: the entry is the original alone.
  setenv("PS_STUB_READ_CONTENT_TYPE", "1", 1);
  setenv("PS_STUB_ORIGIN_CT", "text/css", 1);
  setenv("PS_STUB_ORIGIN_CONTENT_LENGTH", "4307", 1);
  setenv("PS_STUB_GZIP_ISIZE", "3899", 1);
  SetAlternates("0c:00:0");
  DaemonHealNotifyLimiter limiter(1);
  const DaemonServeDecision decision =
      ServeWithLimiter(CssRequest("identity"), &limiter);
  ASSERT_EQ(DaemonServeVerdict::kServeOriginal, decision.verdict);
  EXPECT_FALSE(decision.lost_optimized_copy);
}

TEST_F(DaemonServeArmTest, WithoutAGzipCopyTheArmDoesNotGuess) {
  // Only the brotli copy is there (an optimizer run without gzip).  A brotli
  // stream does not say what it compresses, so the arm cannot tell a loss
  // from a verdict and reports none.
  setenv("PS_STUB_READ_CONTENT_TYPE", "1", 1);
  setenv("PS_STUB_ORIGIN_CT", "text/css", 1);
  setenv("PS_STUB_ORIGIN_CONTENT_LENGTH", "4307", 1);
  setenv("PS_STUB_GZIP_ISIZE", "3899", 1);
  SetAlternates("88:00:1,0c:00:0");
  DaemonHealNotifyLimiter limiter(1);
  const DaemonServeDecision decision =
      ServeWithLimiter(CssRequest("identity"), &limiter);
  ASSERT_EQ(DaemonServeVerdict::kServeOriginal, decision.verdict);
  EXPECT_FALSE(decision.lost_optimized_copy);
}

TEST_F(DaemonServeArmTest, ACompressedCopyTheOptimizerDidNotWriteIsNotEvidence) {
  // A gzip-coded entry that is not marked as the optimizer's own output
  // says nothing about what the optimizer did with this URL.
  setenv("PS_STUB_READ_CONTENT_TYPE", "1", 1);
  setenv("PS_STUB_ORIGIN_CT", "text/css", 1);
  setenv("PS_STUB_ORIGIN_CONTENT_LENGTH", "4307", 1);
  setenv("PS_STUB_GZIP_ISIZE", "3899", 1);
  SetAlternates("48:00:0,0c:00:0");
  DaemonHealNotifyLimiter limiter(1);
  const DaemonServeDecision decision =
      ServeWithLimiter(CssRequest("identity"), &limiter);
  ASSERT_EQ(DaemonServeVerdict::kServeOriginal, decision.verdict);
  EXPECT_FALSE(decision.lost_optimized_copy);
}

TEST_F(DaemonServeArmTest, ACopyWithNoRecordedOriginalLengthIsNotEvidence) {
  // Without the original's length there is nothing to compare with.
  UseStylesheetWithoutAnIdentityCopy("0", "3899");
  DaemonHealNotifyLimiter limiter(1);
  const DaemonServeDecision decision =
      ServeWithLimiter(CssRequest("identity"), &limiter);
  ASSERT_EQ(DaemonServeVerdict::kServeOriginal, decision.verdict);
  EXPECT_FALSE(decision.lost_optimized_copy);
}

TEST_F(DaemonServeArmTest, AnImageIsNeverAskedFor) {
  // An image's variants can be absent for many reasons the entry does not
  // show; the arm asks for stylesheets and scripts only, and does not even
  // look for an image.
  UseStylesheetWithoutAnIdentityCopy("4307", "3899");
  setenv("PS_STUB_READ_CONTENT_TYPE", "3", 1);  // kPsContentImage
  DaemonHealNotifyLimiter limiter(1);
  const DaemonServeDecision decision =
      ServeWithLimiter(CssRequest("identity"), &limiter);
  ASSERT_EQ(DaemonServeVerdict::kServeOriginal, decision.verdict);
  EXPECT_FALSE(decision.lost_optimized_copy);
  EXPECT_EQ(0, GzipCopyReads());
}

TEST_F(DaemonServeArmTest, AScriptIsAskedForLikeAStylesheet) {
  UseStylesheetWithoutAnIdentityCopy("45000", "21000");
  setenv("PS_STUB_READ_CONTENT_TYPE", "2", 1);  // kPsContentJs
  DaemonHealNotifyLimiter limiter(1);
  const DaemonServeDecision decision =
      ServeWithLimiter(CssRequest("identity"), &limiter);
  ASSERT_EQ(DaemonServeVerdict::kServeOriginal, decision.verdict);
  EXPECT_TRUE(decision.lost_optimized_copy);
}

TEST_F(DaemonServeArmTest, AnAcceptNegotiatingOriginIsNeverAskedFor) {
  // The durable original carries the origin-negotiates-on-Accept mark: the
  // optimizer never derives copies for such a URL, so there is nothing to
  // ask for, whatever else the entry holds.
  setenv("PS_STUB_READ_CONTENT_TYPE", "1", 1);
  setenv("PS_STUB_ORIGIN_CT", "text/css", 1);
  setenv("PS_STUB_ORIGIN_CONTENT_LENGTH", "4307", 1);
  setenv("PS_STUB_GZIP_ISIZE", "3899", 1);
  SetAlternates("48:00:1,88:00:1,0c:04:0");
  DaemonHealNotifyLimiter limiter(1);
  const DaemonServeDecision decision =
      ServeWithLimiter(CssRequest("identity"), &limiter);
  ASSERT_EQ(DaemonServeVerdict::kServeOriginal, decision.verdict);
  ASSERT_TRUE(decision.vary_accept_origin_declared);
  EXPECT_FALSE(decision.lost_optimized_copy);
  EXPECT_EQ(0, GzipCopyReads());
}

TEST_F(DaemonServeArmTest, AReaderWithoutALimiterNeverProbes) {
  // The default, and every port that has not opted in: no extra read, no
  // flag, exactly the serve it always was.
  UseStylesheetWithoutAnIdentityCopy("4307", "3899");
  const DaemonServeDecision decision = Serve(CssRequest("identity"));
  ASSERT_EQ(DaemonServeVerdict::kServeOriginal, decision.verdict);
  EXPECT_FALSE(decision.lost_optimized_copy);
  EXPECT_EQ(0, GzipCopyReads());
}

TEST_F(DaemonServeArmTest, AnOptimizedServeIsNeverALoss) {
  // The whole family is there: the identity copy is served and nothing is
  // looked for.
  UseStylesheetFamily();
  setenv("PS_STUB_ORIGIN_CONTENT_LENGTH", "4307", 1);
  setenv("PS_STUB_GZIP_ISIZE", "3899", 1);
  DaemonHealNotifyLimiter limiter(1);
  const DaemonServeDecision decision =
      ServeWithLimiter(CssRequest("identity"), &limiter);
  ASSERT_EQ(DaemonServeVerdict::kServeOptimized, decision.verdict);
  EXPECT_FALSE(decision.lost_optimized_copy);
  EXPECT_EQ(0, GzipCopyReads());
}

TEST_F(DaemonServeArmTest, TheProbeLeavesTheServedBytesAlone) {
  // The served body borrows from the read the reader holds.  The probe is a
  // second read of another entry; it must not release or replace the first.
  UseStylesheetWithoutAnIdentityCopy("4307", "3899");
  DaemonHealNotifyLimiter limiter(1);
  DaemonServeReader reader(abi_.get(), cache_);
  reader.set_heal_notify_limiter(&limiter);
  const DaemonServeDecision decision =
      reader.Serve(CssRequest("identity"), 1100 /* now_seconds */);
  ASSERT_EQ(DaemonServeVerdict::kServeOriginal, decision.verdict);
  ASSERT_TRUE(decision.lost_optimized_copy);
  // The durable original's body in the stand-in: 16 + 0x0c bytes of 'a'.
  ASSERT_EQ(28u, decision.body.size());
  EXPECT_EQ(GoogleString(28, 'a'), decision.body.as_string());
  EXPECT_EQ(kPsSentinelOriginal, decision.alternate_id);
}

TEST_F(DaemonServeArmTest, TheSecondServeInsideTheWindowIsNotProbed) {
  // One look per URL per window, however many requests arrive.
  UseStylesheetWithoutAnIdentityCopy("4307", "3899");
  DaemonHealNotifyLimiter limiter(1);
  const DaemonServeRequest request = CssRequest("identity");
  EXPECT_TRUE(ServeWithLimiter(request, &limiter, 1100).lost_optimized_copy);
  EXPECT_EQ(1, GzipCopyReads());
  EXPECT_FALSE(ServeWithLimiter(request, &limiter, 1105).lost_optimized_copy);
  EXPECT_FALSE(ServeWithLimiter(request, &limiter, 1109).lost_optimized_copy);
  EXPECT_EQ(1, GzipCopyReads()) << "a serve inside the window read again";
  EXPECT_TRUE(ServeWithLimiter(request, &limiter, 1110).lost_optimized_copy);
  EXPECT_EQ(2, GzipCopyReads());
}

TEST_F(DaemonServeArmTest, AnAlreadyMinimalStylesheetCostsOneLookPerWindow) {
  // The legitimate case is looked at once per window too, and never asked
  // for: the window is taken by the look, not by the answer.
  UseStylesheetWithoutAnIdentityCopy("4307", "4307");
  DaemonHealNotifyLimiter limiter(1);
  const DaemonServeRequest request = CssRequest("identity");
  for (int64 now = 1100; now < 1110; ++now) {
    EXPECT_FALSE(ServeWithLimiter(request, &limiter, now).lost_optimized_copy);
  }
  EXPECT_EQ(1, GzipCopyReads());
}

TEST_F(DaemonServeArmTest, AFloodOfOtherUrlsDoesNotHideALostOne) {
  // Thousands of other stylesheets are served as their stored originals in
  // the same window -- here already minimal ones, which are looked at and
  // found whole.  The one URL that did lose its copy is still recognised.
  DaemonHealNotifyLimiter limiter(1);
  UseStylesheetWithoutAnIdentityCopy("4307", "4307");
  GoogleString url;
  for (int i = 0; i < 3000; ++i) {
    url = StrCat("/flood/s", IntegerToString(i), ".css");
    DaemonServeRequest request = CssRequest("identity");
    request.url = url;
    ASSERT_FALSE(ServeWithLimiter(request, &limiter, 1100).lost_optimized_copy)
        << url;
  }
  UseStylesheetWithoutAnIdentityCopy("4307", "3899");
  EXPECT_TRUE(ServeWithLimiter(CssRequest("identity"), &limiter, 1100)
                  .lost_optimized_copy)
      << "a busy server kept a lost stylesheet from being asked for";
}

TEST_F(DaemonServeArmTest, ALostCopyIsAnsweredWithOneOrdinaryNotification) {
  UseStylesheetWithoutAnIdentityCopy("4307", "3899");
  DaemonHealNotifyLimiter limiter(1);
  const DaemonServeRequest request = CssRequest("identity");
  const DaemonServeDecision decision = ServeWithLimiter(request, &limiter);
  ASSERT_TRUE(decision.lost_optimized_copy);

  const int before = NotifyCount();
  SimpleStats stats(thread_system_.get());
  Variable* notified = stats.AddVariable("ipro_daemon_heal_notified");
  Variable* failed = stats.AddVariable("ipro_daemon_heal_notify_failed");
  EXPECT_TRUE(DaemonServeLostCopyNotify(
      *abi_, "/tmp/ps-worker.sock", request, decision, default_context_,
      default_signature_, notified, failed));
  EXPECT_EQ(before + 1, NotifyCount());
  // The stylesheet's own class and the client's raw mask -- a capability
  // vector the classifier produced, never a reserved value.
  EXPECT_NE(GoogleString::npos,
            CallLog().find(StrCat("url=", kCssUrl, " host=", kHost,
                                  " scheme=", kScheme, " content_type=1 mask=",
                                  static_cast<uint64>(decision.client_mask),
                                  " ")));
  EXPECT_EQ(1, notified->Get());
  EXPECT_EQ(0, failed->Get());
}

TEST_F(DaemonServeArmTest, AFailedHealSendMovesTheFailureCounterOnly) {
  setenv("PS_STUB_NOTIFY_FAIL", "1", 1);
  UseStylesheetWithoutAnIdentityCopy("4307", "3899");
  DaemonHealNotifyLimiter limiter(1);
  const DaemonServeRequest request = CssRequest("identity");
  const DaemonServeDecision decision = ServeWithLimiter(request, &limiter);
  ASSERT_TRUE(decision.lost_optimized_copy);

  SimpleStats stats(thread_system_.get());
  Variable* notified = stats.AddVariable("ipro_daemon_heal_notified");
  Variable* failed = stats.AddVariable("ipro_daemon_heal_notify_failed");
  const int before = NotifyCount();
  EXPECT_FALSE(DaemonServeLostCopyNotify(
      *abi_, "/tmp/ps-worker.sock", request, decision, default_context_,
      default_signature_, notified, failed));
  EXPECT_EQ(before + 1, NotifyCount());
  EXPECT_EQ(0, notified->Get());
  EXPECT_EQ(1, failed->Get());
}

TEST_F(DaemonServeArmTest, NothingIsAskedForWithoutTheFlagOrTheContext) {
  UseStylesheetWithoutAnIdentityCopy("4307", "3899");
  DaemonHealNotifyLimiter limiter(1);
  const DaemonServeRequest request = CssRequest("identity");
  const DaemonServeDecision lost = ServeWithLimiter(request, &limiter);
  ASSERT_TRUE(lost.lost_optimized_copy);
  const int before = NotifyCount();

  // A decision that does not carry the flag asks for nothing.
  DaemonServeDecision plain = lost;
  plain.lost_optimized_copy = false;
  EXPECT_FALSE(DaemonServeLostCopyNotify(
      *abi_, "/tmp/ps-worker.sock", request, plain, default_context_,
      default_signature_, nullptr, nullptr));
  // Nor does a flag on anything but a durable-original serve.
  DaemonServeDecision wrong_verdict = lost;
  wrong_verdict.verdict = DaemonServeVerdict::kServeOptimized;
  EXPECT_FALSE(DaemonServeLostCopyNotify(
      *abi_, "/tmp/ps-worker.sock", request, wrong_verdict, default_context_,
      default_signature_, nullptr, nullptr));
  // Both halves of the option context, or nothing.
  EXPECT_FALSE(DaemonServeLostCopyNotify(*abi_, "/tmp/ps-worker.sock", request,
                                         lost, "", default_signature_, nullptr,
                                         nullptr));
  EXPECT_FALSE(DaemonServeLostCopyNotify(*abi_, "/tmp/ps-worker.sock", request,
                                         lost, default_context_, "", nullptr,
                                         nullptr));
  EXPECT_EQ(before, NotifyCount());
}

// ---------------------------------------------------------------------------
// The limiter on its own.
// ---------------------------------------------------------------------------

TEST(DaemonHealNotifyLimiterTest, OneLookAndOneAskPerUrlPerWindow) {
  DaemonHealNotifyLimiter limiter(1);
  EXPECT_TRUE(limiter.ShouldLook("http://a.test/x.css", 100));
  EXPECT_FALSE(limiter.ShouldLook("http://a.test/x.css", 100));
  EXPECT_FALSE(limiter.ShouldLook("http://a.test/x.css", 109));
  EXPECT_TRUE(limiter.ShouldLook("http://a.test/x.css", 110));
  EXPECT_FALSE(limiter.ShouldLook("http://a.test/x.css", 119));
  // The two questions are independent: a look does not use up the ask.
  EXPECT_TRUE(limiter.AdmitAsk("http://a.test/x.css", 110));
  EXPECT_FALSE(limiter.AdmitAsk("http://a.test/x.css", 119));
  EXPECT_TRUE(limiter.AdmitAsk("http://a.test/x.css", 120));
}

TEST(DaemonHealNotifyLimiterTest, AFloodOfOtherUrlsNeverRefusesAUrl) {
  // The whole server's stylesheets and scripts pass through ShouldLook --
  // not yet optimized, already minimal, or a visitor cycling query strings.
  // However many there are, a URL that is asked about is never refused
  // because of them: at worst its slot was taken over and it is looked at
  // again.
  DaemonHealNotifyLimiter limiter(1);
  for (int i = 0; i < 100000; ++i) {
    limiter.ShouldLook(StrCat("http://flood.test/s.css?v=", IntegerToString(i)),
                       100);
  }
  EXPECT_TRUE(limiter.ShouldLook("http://a.test/lost.css", 100));
  // Nothing but lost URLs ever enters the ask table, so the flood above
  // cannot have touched it.
  EXPECT_TRUE(limiter.AdmitAsk("http://a.test/lost.css", 100));
  EXPECT_FALSE(limiter.AdmitAsk("http://a.test/lost.css", 105));
  // A second flood while the window is live: the URL may be looked at again
  // (its look slot was reused), and is still asked for at most once.
  for (int i = 0; i < 100000; ++i) {
    limiter.ShouldLook(StrCat("http://flood.test/t.css?v=", IntegerToString(i)),
                       105);
  }
  EXPECT_FALSE(limiter.AdmitAsk("http://a.test/lost.css", 106));
  EXPECT_TRUE(limiter.AdmitAsk("http://a.test/lost.css", 110));
}

TEST(DaemonHealNotifyLimiterTest, ItsMemoryIsFixed) {
  // Two tables of kSlots small entries and nothing that grows: no URL text
  // is stored, however long the URL.
  EXPECT_LE(sizeof(DaemonHealNotifyLimiter), static_cast<size_t>(64 * 1024));
  DaemonHealNotifyLimiter limiter(1);
  const GoogleString long_url =
      StrCat("http://a.test/", GoogleString(8000, 'x'), ".css");
  EXPECT_TRUE(limiter.ShouldLook(long_url, 100));
  EXPECT_FALSE(limiter.ShouldLook(long_url, 101));
}

TEST(DaemonHealNotifyLimiterTest, ManyLostUrlsEvictButNeverBlock) {
  // More lost URLs than slots: every one of them is admitted when it comes
  // (it takes its slot over), and none is refused for the others' sake.
  DaemonHealNotifyLimiter limiter(1);
  int admitted = 0;
  const int kUrls = 4 * static_cast<int>(DaemonHealNotifyLimiter::kSlots);
  for (int i = 0; i < kUrls; ++i) {
    if (limiter.AdmitAsk(StrCat("http://a.test/", IntegerToString(i), ".css"),
                         100)) {
      ++admitted;
    }
  }
  EXPECT_EQ(kUrls, admitted);
}

TEST(DaemonHealNotifyLimiterTest, AClockThatStepsBackAdmits) {
  DaemonHealNotifyLimiter limiter(1);
  EXPECT_TRUE(limiter.AdmitAsk("http://a.test/x.css", 5000));
  // The clock was set back by an hour: the URL must not be silenced for it.
  EXPECT_TRUE(limiter.AdmitAsk("http://a.test/x.css", 1400));
  EXPECT_FALSE(limiter.AdmitAsk("http://a.test/x.css", 1405));
}

TEST(DaemonHealNotifyLimiterTest, ConcurrentCallersGetExactlyOneAdmission) {
  DaemonHealNotifyLimiter limiter(1);
  std::atomic<int> admitted{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&limiter, &admitted] {
      for (int i = 0; i < 200; ++i) {
        if (limiter.AdmitAsk("http://a.test/x.css", 100)) {
          admitted.fetch_add(1);
        }
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(1, admitted.load());
}

TEST(DaemonHealNotifyLimiterTest, DifferentKeysPlaceUrlsDifferently) {
  // Where a URL sits in the tables depends on the limiter's key.  Fill two
  // limiters with different keys beyond what they hold: which URLs each one
  // has dropped by then differs.  (That the placement cannot be worked out
  // without the key is the keyed hash's property, not something a count
  // can show; see the two cases below.)
  DaemonHealNotifyLimiter one(1);
  DaemonHealNotifyLimiter two(2);
  std::vector<GoogleString> urls;
  for (int i = 0; i < 3000; ++i) {
    urls.push_back(StrCat("http://a.test/", IntegerToString(i), ".css"));
    one.AdmitAsk(urls.back(), 100);
    two.AdmitAsk(urls.back(), 100);
  }
  int differences = 0;
  for (const GoogleString& url : urls) {
    if (one.AdmitAsk(url, 101) != two.AdmitAsk(url, 101)) {
      ++differences;
    }
  }
  EXPECT_GT(differences, 0);
}

TEST(DaemonHealNotifyLimiterTest, TheKeyedHashMatchesItsPublishedVectors) {
  // SipHash-2-4, 64-bit result: the reference test vectors.  Key bytes
  // 00..0f, input the bytes 00, 01, 02, ... of each length from 0 to 63.
  static const uint64_t kVectors[64] = {
      0x726fdb47dd0e0e31ULL, 0x74f839c593dc67fdULL, 0x0d6c8009d9a94f5aULL,
      0x85676696d7fb7e2dULL, 0xcf2794e0277187b7ULL, 0x18765564cd99a68dULL,
      0xcbc9466e58fee3ceULL, 0xab0200f58b01d137ULL, 0x93f5f5799a932462ULL,
      0x9e0082df0ba9e4b0ULL, 0x7a5dbbc594ddb9f3ULL, 0xf4b32f46226bada7ULL,
      0x751e8fbc860ee5fbULL, 0x14ea5627c0843d90ULL, 0xf723ca908e7af2eeULL,
      0xa129ca6149be45e5ULL, 0x3f2acc7f57c29bdbULL, 0x699ae9f52cbe4794ULL,
      0x4bc1b3f0968dd39cULL, 0xbb6dc91da77961bdULL, 0xbed65cf21aa2ee98ULL,
      0xd0f2cbb02e3b67c7ULL, 0x93536795e3a33e88ULL, 0xa80c038ccd5ccec8ULL,
      0xb8ad50c6f649af94ULL, 0xbce192de8a85b8eaULL, 0x17d835b85bbb15f3ULL,
      0x2f2e6163076bcfadULL, 0xde4daaaca71dc9a5ULL, 0xa6a2506687956571ULL,
      0xad87a3535c49ef28ULL, 0x32d892fad841c342ULL, 0x7127512f72f27cceULL,
      0xa7f32346f95978e3ULL, 0x12e0b01abb051238ULL, 0x15e034d40fa197aeULL,
      0x314dffbe0815a3b4ULL, 0x027990f029623981ULL, 0xcadcd4e59ef40c4dULL,
      0x9abfd8766a33735cULL, 0x0e3ea96b5304a7d0ULL, 0xad0c42d6fc585992ULL,
      0x187306c89bc215a9ULL, 0xd4a60abcf3792b95ULL, 0xf935451de4f21df2ULL,
      0xa9538f0419755787ULL, 0xdb9acddff56ca510ULL, 0xd06c98cd5c0975ebULL,
      0xe612a3cb9ecba951ULL, 0xc766e62cfcadaf96ULL, 0xee64435a9752fe72ULL,
      0xa192d576b245165aULL, 0x0a8787bf8ecb74b2ULL, 0x81b3e73d20b49b6fULL,
      0x7fa8220ba3b2eceaULL, 0x245731c13ca42499ULL, 0xb78dbfaf3a8d83bdULL,
      0xea1ad565322a1a0bULL, 0x60e61c23a3795013ULL, 0x6606d7e446282b93ULL,
      0x6ca4ecb15c5f91e1ULL, 0x9f626da15c9625f3ULL, 0xe51b38608ef25f57ULL,
      0x958a324ceb064572ULL};
  const uint64_t kKey0 = 0x0706050403020100ULL;
  const uint64_t kKey1 = 0x0f0e0d0c0b0a0908ULL;
  char input[64];
  for (int i = 0; i < 64; ++i) {
    input[i] = static_cast<char>(i);
  }
  for (size_t length = 0; length < 64; ++length) {
    SipHash24 whole(kKey0, kKey1);
    whole.Update(input, length);
    EXPECT_EQ(kVectors[length], whole.Finish()) << "length " << length;
    // The same bytes in two pieces, split at every place.
    for (size_t split = 0; split <= length; split += 3) {
      SipHash24 pieces(kKey0, kKey1);
      pieces.Update(input, split);
      pieces.Update(input + split, length - split);
      EXPECT_EQ(kVectors[length], pieces.Finish())
          << "length " << length << " split at " << split;
    }
  }
}

// A plain hash with a seed mixed in, as a limiter might naively use: 64-bit
// FNV-1a started from (offset basis XOR seed).  Kept here only to show what
// the keyed hash is for.
uint64_t SeededFnv1aForComparison(uint64_t seed, StringPiece text) {
  uint64_t hash = 14695981039346656037ULL ^ seed;
  for (size_t i = 0; i < text.size(); ++i) {
    hash ^= static_cast<unsigned char>(text[i]);
    hash *= 1099511628211ULL;
  }
  return hash;
}

// Builds two different 12-letter path segments that a seeded FNV-1a maps to
// the same value whenever its running state has the low byte `low_byte` on
// entry -- whatever the other 56 bits are.  Each step of FNV-1a changes the
// state by (a small number decided by the low byte) times a power of the
// multiplier; kSteps is a short solution of "those differences sum to zero
// modulo 2^64", and the search below finds letters that realise it.
bool BuildSeedIndependentCollision(unsigned char low_byte, size_t step,
                                   unsigned char low_a, unsigned char low_b,
                                   GoogleString* a, GoogleString* b) {
  static const int kSteps[12] = {-1, -8, 2, 4, 10, 6, -14, -2, -18, -13, 15, -17};
  static const char kLetters[] = "abcdefghijklmnopqrstuvwxyz0123456789";
  if (step == 0) {
    low_a = low_byte;
    low_b = low_byte;
  }
  if (step == 12) {
    return true;
  }
  for (const char* x = kLetters; *x != '\0'; ++x) {
    const int change_a = static_cast<int>(low_a ^ *x) - static_cast<int>(low_a);
    for (const char* y = kLetters; *y != '\0'; ++y) {
      const int change_b =
          static_cast<int>(low_b ^ *y) - static_cast<int>(low_b);
      if (change_a - change_b != kSteps[step]) {
        continue;
      }
      a->push_back(*x);
      b->push_back(*y);
      // The multiplier's low byte is 0xb3.
      if (BuildSeedIndependentCollision(
              low_byte, step + 1,
              static_cast<unsigned char>((low_a ^ *x) * 0xb3),
              static_cast<unsigned char>((low_b ^ *y) * 0xb3), a, b)) {
        return true;
      }
      a->pop_back();
      b->pop_back();
    }
  }
  return false;
}

TEST(DaemonHealNotifyLimiterTest, UrlsBuiltToCollideWithoutTheKeyDoNot) {
  // URLs are chosen by whoever sends the request.  With a plain seeded hash,
  // pairs of URLs can be built that share a hash for EVERY seed with a given
  // low byte -- 256 pairs cover every seed -- and a visitor asking for one
  // of a pair would keep the limiter from ever looking at the other.  The
  // limiter's hash is keyed: the same pairs are two URLs to it.
  const GoogleString prefix = "http://a.test/";
  for (int seed_low = 0; seed_low < 256; ++seed_low) {
    // The low byte of the plain hash's state after the common prefix.
    const unsigned char low_byte = static_cast<unsigned char>(
        SeededFnv1aForComparison(static_cast<uint64_t>(seed_low), prefix));
    GoogleString a, b;
    ASSERT_TRUE(BuildSeedIndependentCollision(low_byte, 0, 0, 0, &a, &b))
        << seed_low;
    ASSERT_NE(a, b);
    const GoogleString url_a = StrCat(prefix, a, ".css");
    const GoogleString url_b = StrCat(prefix, b, ".css");
    // The construction is real: the plain hash collides, whatever the rest
    // of the seed.
    for (uint64_t seed_high : {0ULL, 0x0123456789abcdULL, 0xffffffffffffffULL}) {
      const uint64_t seed = (seed_high << 8) | static_cast<uint64_t>(seed_low);
      ASSERT_EQ(SeededFnv1aForComparison(seed, url_a),
                SeededFnv1aForComparison(seed, url_b))
          << url_a << " " << url_b;
    }
    // The limiter keeps them apart, under a key whose low byte is the one
    // the pair was built for.
    DaemonHealNotifyLimiter limiter(static_cast<uint64>(seed_low));
    EXPECT_NE(limiter.UrlKey("http", "a.test", StrCat("/", a, ".css")),
              limiter.UrlKey("http", "a.test", StrCat("/", b, ".css")))
        << url_a << " " << url_b;
    EXPECT_TRUE(limiter.ShouldLook(url_a, 100));
    EXPECT_TRUE(limiter.ShouldLook(url_b, 100))
        << url_b << " was taken for " << url_a;
    EXPECT_TRUE(limiter.AdmitAsk(url_a, 100));
    EXPECT_TRUE(limiter.AdmitAsk(url_b, 100))
        << url_b << " was taken for " << url_a;
  }
}

TEST(DaemonHealNotifyLimiterTest, TheUrlKeyIsTheCacheEntrysNotTheSpellings) {
  // One cache entry, however the request spelled its host: the cache key
  // lowercases the host, drops one trailing dot and drops a default port.
  // The limiter must count those spellings as one URL, or each of them buys
  // another look and another ask.
  DaemonHealNotifyLimiter limiter(1);
  const uint64 key = limiter.UrlKey("http", "a.test", "/x.css");
  EXPECT_EQ(key, limiter.UrlKey("http", "a.test.", "/x.css"));
  EXPECT_EQ(key, limiter.UrlKey("http", "A.Test", "/x.css"));
  EXPECT_EQ(key, limiter.UrlKey("http", "a.test:80", "/x.css"));
  EXPECT_EQ(key, limiter.UrlKey("http", "A.TEST.:443", "/x.css"));
  // What IS another entry stays another URL.
  EXPECT_NE(key, limiter.UrlKey("https", "a.test", "/x.css"));
  EXPECT_NE(key, limiter.UrlKey("http", "a.test:8080", "/x.css"));
  EXPECT_NE(key, limiter.UrlKey("http", "a.test", "/X.css"));
  EXPECT_NE(limiter.UrlKey("http", "[::1]:8080", "/x.css"),
            limiter.UrlKey("http", "[::1]", "/x.css"));
  EXPECT_EQ(limiter.UrlKey("http", "[::1]:80", "/x.css"),
            limiter.UrlKey("http", "[::1]", "/x.css"));
  // The text form and the pieces name the same URL.
  EXPECT_TRUE(limiter.ShouldLook("http://a.test/x.css", 100));
  EXPECT_FALSE(limiter.ShouldLookKey(key, 101));
  EXPECT_TRUE(limiter.AdmitAskKey(key, 100));
  EXPECT_FALSE(limiter.AdmitAsk("http://a.test/x.css", 101));
}

TEST(DaemonHealNotifyLimiterTest, TwoUrlsThatMeetDoNotEvictEachOther) {
  // Each URL has two places it can sit in.  Two hundred lost URLs at once
  // -- far more than a server is expected to have -- and all but a handful
  // are still remembered a second later, so they are not asked for again
  // inside their window.
  DaemonHealNotifyLimiter limiter(1);
  std::vector<GoogleString> urls;
  for (int i = 0; i < 200; ++i) {
    urls.push_back(StrCat("http://a.test/lost", IntegerToString(i), ".css"));
    ASSERT_TRUE(limiter.AdmitAsk(urls.back(), 100));
  }
  int asked_again = 0;
  for (const GoogleString& url : urls) {
    if (limiter.AdmitAsk(url, 101)) {
      ++asked_again;
    }
  }
  EXPECT_LE(asked_again, 8);
}

TEST_F(DaemonServeArmTest, ALookRepeatedInsideTheWindowDoesNotAskAgain) {
  // The look memo sees every stylesheet and script the server has, so a
  // lost URL's entry in it can be pushed out long before its window ends.
  // The URL is then looked at again -- and must NOT be asked for again:
  // the ask has its own table, which only lost URLs enter.
  UseStylesheetWithoutAnIdentityCopy("4307", "3899");
  DaemonHealNotifyLimiter limiter(1);
  const DaemonServeRequest request = CssRequest("identity");
  EXPECT_TRUE(ServeWithLimiter(request, &limiter, 1100).lost_optimized_copy);
  EXPECT_EQ(1, GzipCopyReads());

  // A second later the rest of the site passes through the look memo.
  for (int i = 0; i < 20000; ++i) {
    limiter.ShouldLook(StrCat("http://example.com/other/", IntegerToString(i),
                              ".css"),
                       1101);
  }
  EXPECT_FALSE(ServeWithLimiter(request, &limiter, 1102).lost_optimized_copy)
      << "a second ask inside the window";
  EXPECT_EQ(2, GzipCopyReads())
      << "the look memo still held the URL; this case proved nothing";

  // Both windows over (the second look was at 1102): asked for again.
  EXPECT_TRUE(ServeWithLimiter(request, &limiter, 1112).lost_optimized_copy);
  EXPECT_EQ(3, GzipCopyReads());
}

TEST_F(DaemonServeArmTest, AHostSpelledAnotherWayIsTheSameUrlToTheLimiter) {
  UseStylesheetWithoutAnIdentityCopy("4307", "3899");
  DaemonHealNotifyLimiter limiter(1);
  DaemonServeRequest request = CssRequest("identity");
  EXPECT_TRUE(ServeWithLimiter(request, &limiter, 1100).lost_optimized_copy);
  EXPECT_EQ(1, GzipCopyReads());
  for (const char* host : {"example.com.", "EXAMPLE.com", "example.com:80"}) {
    request.hostname = host;
    EXPECT_FALSE(ServeWithLimiter(request, &limiter, 1101).lost_optimized_copy)
        << host;
  }
  EXPECT_EQ(1, GzipCopyReads()) << "another spelling bought another look";
}

TEST_F(DaemonServeArmTest, AGzipCopyOfSomethingLargerIsNotALoss) {
  // Only "compresses LESS than the original" says an optimized copy was
  // made.  A copy that says it compresses more is not that, and not asked
  // for.
  UseStylesheetWithoutAnIdentityCopy("4307", "5000");
  DaemonHealNotifyLimiter limiter(1);
  const DaemonServeDecision decision =
      ServeWithLimiter(CssRequest("identity"), &limiter);
  ASSERT_EQ(DaemonServeVerdict::kServeOriginal, decision.verdict);
  EXPECT_FALSE(decision.lost_optimized_copy);
  EXPECT_EQ(1, GzipCopyReads());
}

TEST_F(DaemonServeArmTest, AnOriginalLengthTooLargeToRecordIsNotEvidence) {
  // The largest recordable value stands for "4 GiB or more": the length is
  // not known, so nothing can be compared with it.
  UseStylesheetWithoutAnIdentityCopy("4294967295", "3899");
  DaemonHealNotifyLimiter limiter(1);
  const DaemonServeDecision decision =
      ServeWithLimiter(CssRequest("identity"), &limiter);
  ASSERT_EQ(DaemonServeVerdict::kServeOriginal, decision.verdict);
  EXPECT_FALSE(decision.lost_optimized_copy);
}

TEST_F(DaemonServeArmTest, TheNotifyItselfRefusesAnAcceptNegotiatingOrigin) {
  // The detection never flags such a URL; the send checks again, so a
  // decision that reached it some other way still asks for nothing.
  UseStylesheetWithoutAnIdentityCopy("4307", "3899");
  DaemonHealNotifyLimiter limiter(1);
  const DaemonServeRequest request = CssRequest("identity");
  DaemonServeDecision decision = ServeWithLimiter(request, &limiter);
  ASSERT_TRUE(decision.lost_optimized_copy);
  decision.vary_accept_origin_declared = true;
  const int before = NotifyCount();
  EXPECT_FALSE(DaemonServeLostCopyNotify(
      *abi_, "/tmp/ps-worker.sock", request, decision, default_context_,
      default_signature_, nullptr, nullptr));
  EXPECT_EQ(before, NotifyCount());
}

TEST(DaemonHealNotifyLimiterTest, TheProcessLimiterIsOneObject) {
  EXPECT_TRUE(ProcessDaemonHealNotifyLimiter() != nullptr);
  EXPECT_EQ(ProcessDaemonHealNotifyLimiter(), ProcessDaemonHealNotifyLimiter());
}

}  // namespace net_instaweb
