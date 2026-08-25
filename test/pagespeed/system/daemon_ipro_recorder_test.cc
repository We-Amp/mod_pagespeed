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

// The daemon-side in-place recorder, driven the way a server drives it.
//
// The gate's rules are asserted next door, over a struct. This suite asserts
// the SEAM: that a real ResponseHeaders is turned into that struct correctly,
// that the checks which happen before the gate happen at all, and that the
// object survives its own lifecycle. Every one of those is a place where a
// rule can be perfectly implemented and still never run.
//
// Nothing is mocked below the recorder. The peer is the real client library
// bound through the real loader against the real stand-in, the headers are
// real ResponseHeaders with ComputeCaching() called on them, and the
// lifecycle is the one the server's output filters drive -- including the
// self-delete, which is why the outcome is read from a sink the test owns
// rather than from the object.

#include "pagespeed/system/daemon_ipro_recorder.h"

#include <cstdio>
#include <cstdlib>
#include <memory>

#include "net/instaweb/rewriter/public/option_context.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "pagespeed/kernel/base/null_mutex.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/http_options.h"
#include "pagespeed/kernel/http/request_headers.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "pagespeed/kernel/util/platform.h"
#include "pagespeed/system/daemon_abi.h"
#include "pagespeed/system/daemon_record_arm.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/base/mock_timer.h"

namespace net_instaweb {

namespace {

const char kBody[] = "body{color:red}";

class DaemonIproRecorderTest : public testing::Test {
 public:
  DaemonIproRecorderTest()
      : thread_system_(Platform::CreateThreadSystem()),
        timer_(new NullMutex, 1000 * Timer::kSecondMs) {
    RewriteOptions::Initialize();
  }
  ~DaemonIproRecorderTest() override { RewriteOptions::Terminate(); }

 protected:
  void SetUp() override {
    log_path_ = StrCat(GTestTempDir(), "/daemon_recorder_calls.log");
    remove(log_path_.c_str());
    setenv("PS_STUB_LOG", log_path_.c_str(), 1);

    GoogleString error;
    abi_.reset(LoadDaemonAbi(
        StrCat(GTestSrcDir(), "/test/pagespeed/system/libdaemon_stub.so"),
        &error));
    ASSERT_TRUE(abi_ != nullptr) << error;

    RewriteOptions options(thread_system_.get());
    ASSERT_EQ(OptionContextStatus::kOk,
              OptionContext::Compute(options, &request_.option_context,
                                     &request_.option_signature));
    request_.url = "/a/style.css";
    request_.hostname = "example.com";
    request_.scheme = "http";
    request_.accept = "text/css,*/*";
    request_.user_agent = "Mozilla/5.0";
    request_.accept_encoding = "gzip";
    // The ordinary case: no cookie, no authorization.
    request_.request_properties =
        RequestHeaders::Properties(false, false, false);
  }

  void TearDown() override { unsetenv("PS_STUB_LOG"); }

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

  // A response every check accepts, so each case can vary exactly one thing.
  void MakeOkHeaders(ResponseHeaders* headers) {
    headers->set_major_version(1);
    headers->set_minor_version(1);
    headers->SetStatusAndReason(HttpStatus::kOK);
    headers->Add(HttpAttributes::kContentType, "text/css");
    headers->Add(HttpAttributes::kCacheControl, "max-age=600, public");
    headers->Add(HttpAttributes::kEtag, "\"abc\"");
    headers->SetDate(timer_.NowMs());
    headers->ComputeCaching();
  }

  // Builds a recorder, streams `body` through it, and hands it the final
  // headers -- which destroys it. Returns what it did.
  //
  // `cache` is a non-null token: the recorder passes it straight to the peer
  // and the stand-in does not dereference it, so this exercises the real
  // call path without a real volume.
  DaemonRecordOutcome Run(ResponseHeaders* headers, StringPiece body,
                          bool entire_response_received = true,
                          const HttpOptions* http_options = nullptr,
                          ResponseHeaders* preliminary = nullptr,
                          const char* saved_cache_control = nullptr) {
    HttpOptions defaults = kDefaultHttpOptionsForTests;
    DaemonRecordOutcome outcome;
    DaemonIproRecorder* recorder = new DaemonIproRecorder(
        abi_.get(), &cache_token_, "/tmp/socket", request_,
        http_options == nullptr ? defaults : *http_options, &timer_, nullptr);
    recorder->set_outcome_sink_for_testing(&outcome);
    if (preliminary != nullptr) {
      recorder->ConsiderResponseHeaders(IproRecorder::kPreliminaryHeaders,
                                        preliminary);
    }
    if (saved_cache_control != nullptr) {
      recorder->SaveCacheControl(saved_cache_control);
    }
    recorder->Write(body, nullptr);
    recorder->DoneAndSetHeaders(headers, entire_response_received);
    return outcome;
  }

  std::unique_ptr<ThreadSystem> thread_system_;
  MockTimer timer_;
  std::unique_ptr<DaemonAbi> abi_;
  DaemonRecordRequest request_;
  GoogleString log_path_;
  int cache_token_ = 0;
};

// --- the ordinary path ------------------------------------------------------

TEST_F(DaemonIproRecorderTest, RecordsAndNotifiesAnEligibleResponse) {
  ResponseHeaders headers;
  MakeOkHeaders(&headers);
  const DaemonRecordOutcome outcome = Run(&headers, kBody);

  EXPECT_TRUE(outcome.done);
  EXPECT_TRUE(outcome.gate_ran);
  EXPECT_EQ(DaemonRecordVerdict::kStoreAndNotify, outcome.verdict);
  EXPECT_TRUE(outcome.stored);
  EXPECT_TRUE(outcome.notified);

  const GoogleString log = CallLog();
  EXPECT_NE(GoogleString::npos, log.find("alternate_id=12")) << log;
  EXPECT_NE(GoogleString::npos,
            log.find(StrCat("bytes=",
                            IntegerToString(static_cast<int>(strlen(kBody))))))
      << log;
  EXPECT_NE(GoogleString::npos, log.find("etag=\"abc\"")) << log;
  EXPECT_NE(GoogleString::npos, log.find("notify ")) << log;
}

TEST_F(DaemonIproRecorderTest, TheRecorderDeletesItself) {
  const int64 before = DaemonIproRecorder::num_constructed();
  ResponseHeaders headers;
  MakeOkHeaders(&headers);
  Run(&headers, kBody);
  // Construction is counted, which is what the seam pins are written against.
  // That the object is GONE is what the leak checker asserts on every run of
  // this suite; this is the half that can be stated here.
  EXPECT_EQ(before + 1, DaemonIproRecorder::num_constructed());
}

// --- BLOCKER 1's regression: the joined Vary must outlive the join ---------

TEST_F(DaemonIproRecorderTest, RepeatedVaryLinesAreJoinedAndSurviveTheCall) {
  // Two `Vary: Accept-Encoding` lines are an ordinary result of two modules
  // each adding one. Joined they are 30 bytes -- past any small-string buffer
  // -- so a joined value held only for the duration of the assignment is read
  // after it is freed. Both axes are allowed, so a CORRECT implementation
  // stores; an implementation that reads freed storage produces whatever was
  // in it, which is why this asserts the outcome rather than the bytes.
  ResponseHeaders headers;
  MakeOkHeaders(&headers);
  headers.Add(HttpAttributes::kVary, HttpAttributes::kAcceptEncoding);
  headers.Add(HttpAttributes::kVary, HttpAttributes::kAcceptEncoding);
  headers.ComputeCaching();

  const DaemonRecordOutcome outcome = Run(&headers, kBody);
  EXPECT_TRUE(outcome.gate_ran);
  EXPECT_EQ(DaemonRecordVerdict::kStoreAndNotify, outcome.verdict);
  EXPECT_TRUE(outcome.stored);
}

TEST_F(DaemonIproRecorderTest, AVaryTheSharedKeyCannotSeparateIsRefused) {
  // The joined list is what the peer's predicate sees, so a second line
  // carrying an axis outside its four refuses the whole response even though
  // the first line is fine. Asking per line would have admitted it.
  ResponseHeaders headers;
  MakeOkHeaders(&headers);
  headers.Add(HttpAttributes::kVary, HttpAttributes::kAcceptEncoding);
  headers.Add(HttpAttributes::kVary, "X-Tenant");
  headers.ComputeCaching();

  const DaemonRecordOutcome outcome = Run(&headers, kBody);
  EXPECT_TRUE(outcome.gate_ran);
  EXPECT_EQ(DaemonRecordVerdict::kRefuse, outcome.verdict);
  EXPECT_STREQ(daemon_record_reason::kVaryNotStorable, outcome.reason);
  EXPECT_FALSE(outcome.stored);
}

// --- BLOCKER 2's regression: the status gate -------------------------------

TEST_F(DaemonIproRecorderTest, ARedirectIsNeverRecordedAsTheOriginal) {
  // A 301 is a CACHEABLE resource status in this module, so without an
  // explicit 200 check it reaches the gate, passes every rule there -- none
  // of which is about status -- and its body is stored as the durable
  // ORIGINAL for the resource's URL, with no Location anywhere in the entry.
  // The serving side would then hand that out as the resource.
  ResponseHeaders headers;
  MakeOkHeaders(&headers);
  headers.SetStatusAndReason(HttpStatus::kMovedPermanently);
  headers.Add(HttpAttributes::kLocation, "http://example.com/b/style.css");
  headers.ComputeCaching();

  const DaemonRecordOutcome outcome = Run(&headers, "");
  EXPECT_FALSE(outcome.gate_ran)
      << "a redirect reached the record gate; the status check is missing";
  EXPECT_FALSE(outcome.stored);
  EXPECT_EQ(GoogleString::npos, CallLog().find("write_original"));
}

TEST_F(DaemonIproRecorderTest, AnErrorStatusIsNeverRecorded) {
  for (int status : {HttpStatus::kNotFound, HttpStatus::kInternalServerError}) {
    ResponseHeaders headers;
    MakeOkHeaders(&headers);
    headers.SetStatusAndReason(static_cast<HttpStatus::Code>(status));
    headers.ComputeCaching();
    const DaemonRecordOutcome outcome = Run(&headers, kBody);
    EXPECT_FALSE(outcome.gate_ran) << status;
    EXPECT_FALSE(outcome.stored) << status;
  }
}

TEST_F(DaemonIproRecorderTest, ATransientNonTwoHundredIsNeverRecorded) {
  // 304 and 206 are not errors and are not the resource either.
  for (int status : {304, 206}) {
    ResponseHeaders headers;
    MakeOkHeaders(&headers);
    headers.SetStatusAndReason(static_cast<HttpStatus::Code>(status));
    headers.ComputeCaching();
    const DaemonRecordOutcome outcome = Run(&headers, kBody);
    EXPECT_FALSE(outcome.gate_ran) << status;
    EXPECT_FALSE(outcome.stored) << status;
  }
}

// --- BLOCKER 3's regression: authorization ---------------------------------

TEST_F(DaemonIproRecorderTest, AnAuthorizedRequestIsNotPutInTheSharedCache) {
  // The cache is shared across every virtual host on the server. A response
  // to a request that carried Authorization may only go in it if the origin
  // said `public`; anything less and the next user's request can reach it.
  //
  // This is exactly the case a default-constructed properties value gets
  // wrong: it assumes no authorization, which is the permissive answer.
  request_.request_properties =
      RequestHeaders::Properties(false, false, true /* auth */);
  ResponseHeaders headers;
  MakeOkHeaders(&headers);  // max-age=600, public
  headers.Replace(HttpAttributes::kCacheControl, "max-age=300");
  headers.ComputeCaching();

  const DaemonRecordOutcome outcome = Run(&headers, kBody);
  EXPECT_FALSE(outcome.gate_ran)
      << "an authenticated request's response reached the record gate";
  EXPECT_FALSE(outcome.stored);
  EXPECT_EQ(GoogleString::npos, CallLog().find("write_original"));
}

TEST_F(DaemonIproRecorderTest, AnAuthorizedRequestMarkedPublicIsRecorded) {
  // The other half, so the case above is a rule and not a blanket refusal.
  request_.request_properties = RequestHeaders::Properties(false, false, true);
  ResponseHeaders headers;
  MakeOkHeaders(&headers);
  const DaemonRecordOutcome outcome = Run(&headers, kBody);
  EXPECT_TRUE(outcome.gate_ran);
  EXPECT_TRUE(outcome.stored);
}

// --- MAJOR 5's regression: respect_vary is configured, not constant --------

TEST_F(DaemonIproRecorderTest, StrictestOfBothIsReal) {
  // THE DIVERGENCE, re-derived on the path that actually runs -- and it is
  // not the one this PR originally claimed. Two corrections, both found by
  // running it rather than reasoning about it:
  //
  //   `Vary: Cookie` is NOT a divergence here. The module-side rule that
  //   tolerates it needs an HTML-like content type, and the in-place path is
  //   resources. Both sides refuse it.
  //
  //   `Vary: *` is NOT a divergence either. It never reaches the Vary loop:
  //   the caching computation has already marked the response uncacheable.
  //   The module is stricter there.
  //
  // What DOES diverge is an ordinary named axis outside the four the shared
  // key separates. `Accept-Language` is one an origin really sends. With the
  // shipped default (respect-Vary-on-resources OFF) and no validator, this
  // module admits it on a resource -- the branch that would refuse needs
  // either that option or an HTML-like type. The peer refuses it, because a
  // single stored representation behind a key that cannot tell languages
  // apart hands the next reader the wrong one.
  ResponseHeaders headers;
  MakeOkHeaders(&headers);
  headers.Add(HttpAttributes::kVary, "Accept-Language");
  headers.ComputeCaching();

  const HttpOptions shipped_default = kDefaultHttpOptionsForTests;
  ASSERT_FALSE(shipped_default.respect_vary)
      << "this test is about the shipped default; it no longer is one";

  // Half one: THIS MODULE's predicate, called exactly as the recorder calls
  // it, admits the response.
  EXPECT_TRUE(headers.IsProxyCacheable(
      request_.request_properties,
      ResponseHeaders::GetVaryOption(shipped_default.respect_vary),
      ResponseHeaders::kNoValidator))
      << "the module-side predicate no longer admits this, so the case below "
         "is no longer a divergence";

  // Half two: THE PEER's predicate refuses it.
  EXPECT_EQ(1, abi_->VaryUncacheable("Accept-Language"));

  // And the recorder reaches the gate -- proving the module-side predicate
  // really did admit it -- and then lands on the peer's answer.
  const DaemonRecordOutcome outcome =
      Run(&headers, kBody, true, &shipped_default);
  EXPECT_TRUE(outcome.gate_ran) << "the module-side gate refused it first, so "
                                   "the peer's predicate never ran";
  EXPECT_EQ(DaemonRecordVerdict::kRefuse, outcome.verdict);
  EXPECT_STREQ(daemon_record_reason::kVaryNotStorable, outcome.reason);
}

TEST_F(DaemonIproRecorderTest, TheModuleIsStricterOnSomeAxesAndThatIsFine) {
  // The other direction of strictest-of-both, stated so the rule does not
  // read as "the peer always decides". On these the module refuses first and
  // the peer is never consulted.
  for (const char* vary : {"*", "Cookie"}) {
    ResponseHeaders headers;
    MakeOkHeaders(&headers);
    headers.Add(HttpAttributes::kVary, vary);
    headers.ComputeCaching();
    const DaemonRecordOutcome outcome = Run(&headers, kBody);
    EXPECT_FALSE(outcome.gate_ran) << vary;
    EXPECT_FALSE(outcome.stored) << vary;
  }
}

TEST_F(DaemonIproRecorderTest, RespectVaryIsHonouredWhenTheOperatorSetsIt) {
  // With the option ON the module-side predicate refuses first, so the
  // response never reaches the gate. That is the difference a hard-coded
  // constant erased -- and hard-coding the STRICT end, as this originally
  // did, also made the peer's own allowlist unreachable.
  ResponseHeaders headers;
  MakeOkHeaders(&headers);
  headers.Add(HttpAttributes::kVary, HttpAttributes::kAccept);
  headers.ComputeCaching();

  HttpOptions respect = kDefaultHttpOptionsForTests;
  respect.respect_vary = true;
  const DaemonRecordOutcome strict = Run(&headers, kBody, true, &respect);
  EXPECT_FALSE(strict.gate_ran);
  EXPECT_FALSE(strict.stored);

  // And with the default it is admitted by both sides: `Accept` is one of the
  // axes the shared key does separate.
  const DaemonRecordOutcome loose = Run(&headers, kBody);
  EXPECT_TRUE(loose.gate_ran);
  EXPECT_TRUE(loose.stored);
}

TEST_F(DaemonIproRecorderTest, AnAcceptNegotiatingOriginIsStoredMarked) {
  // The end of the seam for the stamping obligation: a REAL response, through
  // the real header parsing and the real join, reaching the peer with the
  // marker set.  The gate's own suite pins the flag on the decision; what this
  // adds is that nothing between a `Vary: Accept` header line and the write
  // loses it -- the join, the module-side cacheability check, the gate and the
  // writer are all in the path here and none of them is in that one.
  ResponseHeaders headers;
  MakeOkHeaders(&headers);
  headers.Add(HttpAttributes::kVary, HttpAttributes::kAccept);
  headers.ComputeCaching();

  const DaemonRecordOutcome outcome = Run(&headers, kBody);
  ASSERT_TRUE(outcome.gate_ran);
  EXPECT_TRUE(outcome.stored);
  const GoogleString log = CallLog();
  // 4 ALONE: the Vary-Accept marker and NOT the
  // origin-headers-not-reproducible one.  `Accept` is a handled `Vary` axis
  // precisely BECAUSE of the bit this response does earn -- the fact is
  // recorded here and the serve arm emits `Vary: Accept` back off it -- so the
  // header is reproduced and there is nothing for rule 6 to mark.  Asserted on
  // the whole byte rather than on one bit, because that is what the peer was
  // told and because 12 here would mean the substrate declines every
  // Accept-negotiating origin it records.
  EXPECT_NE(GoogleString::npos, log.find(" flags=4")) << log;
}

TEST_F(DaemonIproRecorderTest, ARepeatedVaryIsClassifiedAsOneListHereToo) {
  // The join is load-bearing for the STAMP as well as for the refusal, and in
  // the opposite direction: `Accept` arriving on its own line, beside another
  // allowed axis, is still an Accept-negotiating origin.  A per-line
  // classification that stopped at the first line would store this unmarked --
  // storable, notified, and wrong for every later client.
  ResponseHeaders headers;
  MakeOkHeaders(&headers);
  headers.Add(HttpAttributes::kVary, HttpAttributes::kAcceptEncoding);
  headers.Add(HttpAttributes::kVary, HttpAttributes::kAccept);
  headers.ComputeCaching();

  const DaemonRecordOutcome outcome = Run(&headers, kBody);
  ASSERT_TRUE(outcome.gate_ran);
  EXPECT_TRUE(outcome.stored);
  const GoogleString log = CallLog();
  // 4 alone, for the reason given on the case above -- and both axes on the
  // two lines are handled ones, so the joined list is reproducible too.
  EXPECT_NE(GoogleString::npos, log.find(" flags=4")) << log;
}

TEST_F(DaemonIproRecorderTest, AnOrdinaryResponseReachesThePeerUnmarked) {
  // The other direction, on the same path: the marker is not something this
  // module puts on everything it records.  Without this the two cases above
  // would pass equally well against an implementation that stamps
  // unconditionally, which is the same wrong answer with the opposite sign --
  // it would tell the optimizer to leave every recorded original alone.
  ResponseHeaders headers;
  MakeOkHeaders(&headers);
  headers.Add(HttpAttributes::kVary, HttpAttributes::kAcceptEncoding);
  headers.ComputeCaching();

  const DaemonRecordOutcome outcome = Run(&headers, kBody);
  ASSERT_TRUE(outcome.stored);
  const GoogleString log = CallLog();
  EXPECT_NE(GoogleString::npos, log.find(" flags=0")) << log;
}

// --- rule 6: the header set the seam actually hands the gate ---------------

TEST_F(DaemonIproRecorderTest, AnUnreproducibleHeaderReachesThePeerMarked) {
  // The end of the seam for the rule-6 stamp, and the half the gate's own
  // suite cannot cover: the gate is handed a header LIST, and nothing else in
  // the tree checks that the list a real ResponseHeaders produces is the whole
  // response.  A seam that gathered only the headers it already had names for
  // would pass every gate test and mark nothing.
  ResponseHeaders headers;
  MakeOkHeaders(&headers);
  headers.Add("X-Custom", "1");
  headers.ComputeCaching();

  const DaemonRecordOutcome outcome = Run(&headers, kBody);
  ASSERT_TRUE(outcome.gate_ran);
  EXPECT_TRUE(outcome.stored);
  // STORED AND NOTIFIED, marked: the bit is a statement, not a refusal.
  EXPECT_TRUE(outcome.notified);
  const GoogleString log = CallLog();
  EXPECT_NE(GoogleString::npos, log.find(" flags=8")) << log;
}

TEST_F(DaemonIproRecorderTest, TheOrdinaryResponseTheSeamBuildsIsNotMarked) {
  // The non-vacuity half, and it is not decoration: `MakeOkHeaders` produces a
  // `Date` line, and `Date` is stamped per response by whatever serves the
  // request rather than reproduced from an entry.  Without the ignore set the
  // rule marks EVERY response -- in-place optimization off, everywhere, for a
  // rule about custom headers.  This case is what stands between that and a
  // silent, total regression.
  ResponseHeaders headers;
  MakeOkHeaders(&headers);
  const DaemonRecordOutcome outcome = Run(&headers, kBody);
  ASSERT_TRUE(outcome.stored);
  const GoogleString log = CallLog();
  EXPECT_NE(GoogleString::npos, log.find(" flags=0")) << log;
}

TEST_F(DaemonIproRecorderTest, ARewrittenCacheControlDoesNotMoveTheRuleSix) {
  // The one header the seam can hand the gate that the ORIGIN did not send is
  // a `Cache-Control` the serving path rewrote -- the case SaveCacheControl
  // exists for.  It must not move this verdict, and it cannot: the field is in
  // the reproducible set whatever its value.  Pinned because "the list is the
  // final headers, not the origin's" is the kind of detail that turns into a
  // marked-everything regression the first time someone attaches a header
  // filter to this substrate.
  ResponseHeaders headers;
  MakeOkHeaders(&headers);
  const DaemonRecordOutcome outcome =
      Run(&headers, kBody, true, nullptr, nullptr, "max-age=300");
  ASSERT_TRUE(outcome.stored);
  EXPECT_NE(GoogleString::npos, CallLog().find(" flags=0")) << CallLog();
}

TEST_F(DaemonIproRecorderTest, AFinalizedDefaultResponseIsNotMarked) {
  // THE REALISTIC CASE, at the seam rather than at the predicate.  What
  // reaches the gate here is what the SERVER finalized, and on a default
  // deployment a static resource carries rather more than the three fields a
  // synthetic fixture uses.  `Accept-Ranges` in particular rides on
  // essentially every one of them: if it marked, the substrate would be off
  // for every resource on every server, and every other test in this file
  // would still pass.
  ResponseHeaders headers;
  MakeOkHeaders(&headers);
  headers.Add(HttpAttributes::kServer, "Apache/2.4.58 (Ubuntu)");
  headers.Add("Accept-Ranges", "bytes");
  headers.Add(HttpAttributes::kLastModified, "Sun, 09 Aug 2026 12:00:00 GMT");
  headers.Add(HttpAttributes::kExpires, "Mon, 10 Aug 2026 00:10:00 GMT");
  headers.ComputeCaching();

  const DaemonRecordOutcome outcome = Run(&headers, kBody);
  ASSERT_TRUE(outcome.gate_ran);
  ASSERT_TRUE(outcome.stored);
  EXPECT_NE(GoogleString::npos, CallLog().find(" flags=0")) << CallLog();
}

TEST_F(DaemonIproRecorderTest, AnOriginBehindAProxyIsNotMarked) {
  // `Age` is the other name that would take a whole class of deployment to
  // zero: every response through a caching proxy or CDN carries one, and it is
  // superseded on the way out rather than lost -- this substrate computes its
  // own `Age` from the entry on every serve.
  ResponseHeaders headers;
  MakeOkHeaders(&headers);
  headers.Add(HttpAttributes::kAge, "312");
  headers.ComputeCaching();

  const DaemonRecordOutcome outcome = Run(&headers, kBody);
  ASSERT_TRUE(outcome.gate_ran);
  ASSERT_TRUE(outcome.stored);
  EXPECT_NE(GoogleString::npos, CallLog().find(" flags=0")) << CallLog();
}

TEST_F(DaemonIproRecorderTest, ASiteWideConfiguredHeaderIsMarkedAtTheSeamToo) {
  // The false positive, at the seam: a site-wide directive's header is
  // indistinguishable here from an origin header for this URL alone, so it
  // marks. Pinned so the cost of the coarse reading is visible where the
  // finalized-headers fact is established rather than only where the predicate
  // is declared.
  ResponseHeaders headers;
  MakeOkHeaders(&headers);
  headers.Add("Strict-Transport-Security", "max-age=31536000");
  headers.ComputeCaching();

  const DaemonRecordOutcome outcome = Run(&headers, kBody);
  ASSERT_TRUE(outcome.gate_ran);
  EXPECT_TRUE(outcome.stored);
  EXPECT_NE(GoogleString::npos, CallLog().find(" flags=8")) << CallLog();
}

// --- the rest of the seam ---------------------------------------------------

TEST_F(DaemonIproRecorderTest, TheOriginsCacheControlIsRecordedNotOurs) {
  // The serving path may rewrite Cache-Control before the final headers are
  // seen. SaveCacheControl is how the ORIGIN's value reaches the recorder,
  // and recording the rewritten one would file our own answer as the
  // origin's.
  ResponseHeaders headers;
  MakeOkHeaders(&headers);
  headers.Replace(HttpAttributes::kCacheControl,
                  "max-age=600, public, s-maxage=10");
  headers.ComputeCaching();

  const DaemonRecordOutcome outcome =
      Run(&headers, kBody, true, nullptr, nullptr, "max-age=600, public");
  ASSERT_TRUE(outcome.stored);
  const GoogleString log = CallLog();
  EXPECT_NE(GoogleString::npos, log.find("max_age=600")) << log;
  EXPECT_NE(GoogleString::npos, log.find("s_maxage=0")) << log;
}

TEST_F(DaemonIproRecorderTest, AnEncodedBodyIsNotTheOriginal) {
  // Decided from the PRELIMINARY headers, before the body is buffered: what
  // arrives after an upstream has encoded it is the encoded form, and storing
  // that as "the bytes the origin sent" is false in the one field the class
  // is about.
  ResponseHeaders preliminary;
  MakeOkHeaders(&preliminary);
  preliminary.Add(HttpAttributes::kContentEncoding, "gzip");
  preliminary.ComputeCaching();

  ResponseHeaders headers;
  MakeOkHeaders(&headers);
  const DaemonRecordOutcome outcome =
      Run(&headers, kBody, true, nullptr, &preliminary);
  EXPECT_FALSE(outcome.gate_ran);
  EXPECT_FALSE(outcome.stored);
}

TEST_F(DaemonIproRecorderTest, ATruncatedResponseIsNotRecorded) {
  ResponseHeaders headers;
  MakeOkHeaders(&headers);
  const DaemonRecordOutcome outcome =
      Run(&headers, kBody, false /* entire_response_received */);
  EXPECT_FALSE(outcome.gate_ran);
  EXPECT_FALSE(outcome.stored);
}

TEST_F(DaemonIproRecorderTest, TheCapIsAppliedWhileTheBodyAccumulates) {
  // Not to a finished buffer: the point of a cap is not to hold 16 MB in
  // order to discover it was 16 MB. Written as many chunks so that the
  // decision has to be taken at the byte that crosses, and the buffer is
  // released rather than grown afterwards.
  ResponseHeaders headers;
  MakeOkHeaders(&headers);
  DaemonRecordOutcome outcome;
  HttpOptions defaults = kDefaultHttpOptionsForTests;
  DaemonIproRecorder* recorder =
      new DaemonIproRecorder(abi_.get(), &cache_token_, "/tmp/socket", request_,
                             defaults, &timer_, nullptr);
  recorder->set_outcome_sink_for_testing(&outcome);
  const GoogleString chunk(1024 * 1024, 'x');
  for (int i = 0; i < 17; ++i) {
    EXPECT_TRUE(recorder->Write(chunk, nullptr));
  }
  recorder->DoneAndSetHeaders(&headers, true);

  EXPECT_FALSE(outcome.gate_ran);
  EXPECT_FALSE(outcome.stored);
  EXPECT_EQ(GoogleString::npos, CallLog().find("write_original"));
}

TEST_F(DaemonIproRecorderTest, ARequestWithNoUsableContextStoresButIsSilent) {
  request_.option_context.clear();
  request_.option_signature.clear();
  ResponseHeaders headers;
  MakeOkHeaders(&headers);
  const DaemonRecordOutcome outcome = Run(&headers, kBody);
  EXPECT_EQ(DaemonRecordVerdict::kStoreOnly, outcome.verdict);
  EXPECT_STREQ(daemon_record_reason::kNoOptionContext, outcome.reason);
  EXPECT_TRUE(outcome.stored);
  EXPECT_FALSE(outcome.notified);
  EXPECT_EQ(GoogleString::npos, CallLog().find("notify "));
}

TEST_F(DaemonIproRecorderTest, WithoutACacheTheGateIsNeverEvenConsulted) {
  ResponseHeaders headers;
  MakeOkHeaders(&headers);
  DaemonRecordOutcome outcome;
  HttpOptions defaults = kDefaultHttpOptionsForTests;
  DaemonIproRecorder* recorder = new DaemonIproRecorder(
      abi_.get(), nullptr, "/tmp/socket", request_, defaults, &timer_, nullptr);
  recorder->set_outcome_sink_for_testing(&outcome);
  recorder->Write(kBody, nullptr);
  recorder->DoneAndSetHeaders(&headers, true);
  EXPECT_TRUE(outcome.done);
  EXPECT_FALSE(outcome.gate_ran);
  EXPECT_EQ(GoogleString::npos, CallLog().find("write_original"));
}

}  // namespace

}  // namespace net_instaweb
