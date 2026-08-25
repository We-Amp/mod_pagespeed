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

// The record arm: what may go into the optimizer daemon's shared cache for
// one origin response, and whether the daemon may be asked to optimize it.
//
// THE PEER IS NOT MOCKED IN PROCESS.  Every case here binds the REAL client
// library through the REAL loader, against a shared object that reproduces
// the peer's published validation -- its alternate-id gate, its parameter
// size ladder, its Vary predicate, its Cache-Control parser, its content cap
// and its notification rules, each with the peer's own fail direction.  An
// in-process fake would have been a second implementation of the contract
// under test, and one that accepted whatever the caller happened to send
// would let the pair ship non-functional with every assertion green.
//
// What the stand-in is asked is observed from OUTSIDE the module: it appends
// one line per call to a log the test reads back.  So the assertions are
// about what the peer was told, not about what this module believes it told
// it.

#include "pagespeed/system/daemon_record_arm.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <vector>

#include "net/instaweb/rewriter/public/option_context.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/util/platform.h"
#include "pagespeed/system/daemon_abi.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {

namespace {

const char kUrl[] = "/a/style.css";
const char kHost[] = "example.com";
const char kScheme[] = "http";

class DaemonRecordArmTest : public testing::Test {
 public:
  DaemonRecordArmTest() : thread_system_(Platform::CreateThreadSystem()) {
    RewriteOptions::Initialize();
  }
  ~DaemonRecordArmTest() override { RewriteOptions::Terminate(); }

 protected:
  void SetUp() override {
    log_path_ = StrCat(GTestTempDir(), "/daemon_stub_calls.log");
    remove(log_path_.c_str());
    setenv("PS_STUB_LOG", log_path_.c_str(), 1);
    unsetenv("PS_STUB_NOTIFY_FAIL");

    GoogleString error;
    abi_.reset(LoadDaemonAbi(
        StrCat(GTestSrcDir(), "/test/pagespeed/system/libdaemon_stub.so"),
        &error));
    ASSERT_TRUE(abi_ != nullptr) << error;

    // The context an unmodified configuration renders, computed once by this
    // module's own producer so that every case below carries a context that
    // is genuinely valid rather than a literal that could drift from one.
    RewriteOptions options(thread_system_.get());
    ASSERT_EQ(OptionContextStatus::kOk,
              OptionContext::Compute(options, &default_payload_,
                                     &default_signature_));
  }

  void TearDown() override {
    unsetenv("PS_STUB_LOG");
    unsetenv("PS_STUB_NOTIFY_FAIL");
  }

  // Everything the stand-in was asked, in order, one line per call.
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

  // A response every rule accepts, so each case can vary exactly one thing.
  // The StringPieces borrow from this fixture, which outlives every use.
  DaemonRecordInput EligibleInput() const {
    DaemonRecordInput input;
    input.option_context = default_payload_;
    input.option_signature = default_signature_;
    input.url = kUrl;
    input.hostname = kHost;
    input.scheme = kScheme;
    input.content_type = "text/css";
    input.cache_control_lines.push_back("max-age=600, public");
    input.etag = "\"abc\"";
    input.content_length = 11;
    input.accept = "text/css,*/*";
    input.user_agent = "Mozilla/5.0";
    input.accept_encoding = "gzip";
    return input;
  }

  std::unique_ptr<ThreadSystem> thread_system_;
  std::unique_ptr<DaemonAbi> abi_;
  GoogleString log_path_;
  GoogleString default_payload_;
  GoogleString default_signature_;
};

// --- rule: the durable original is the ONLY class this module writes -------

TEST_F(DaemonRecordArmTest, TheGateAlwaysNamesTheDurableOriginalsClass) {
  const DaemonRecordDecision decision =
      EvaluateDaemonRecordGate(*abi_, EligibleInput(), 1000);
  ASSERT_NE(DaemonRecordVerdict::kRefuse, decision.verdict);
  EXPECT_EQ(kPsSentinelOriginal, decision.params.alternate_id);
  EXPECT_EQ(kPsSentinelOriginal, decision.params.full_mask & 0xFFu);
  // The flag that marks an entry as the optimizer's work stays clear: setting
  // it would make a recording indistinguishable from an optimized variant.
  // This response carries no Vary, so the one bit this module DOES set is
  // clear too -- see the Vary-Accept cases below.
  EXPECT_EQ(0, decision.params.flags);
}

TEST_F(DaemonRecordArmTest, TheWriteReachesThePeerAsTheOriginalsClass) {
  const DaemonRecordInput input = EligibleInput();
  const DaemonRecordDecision decision =
      EvaluateDaemonRecordGate(*abi_, input, 1000);
  int cache_marker = 0;
  DaemonRecordWriter writer(abi_.get(), &cache_marker);
  ASSERT_EQ(kPsOk, writer.Begin(input, decision));
  ASSERT_EQ(kPsOk, writer.Append("hello world", 11));
  ASSERT_EQ(kPsOk, writer.Commit());

  const GoogleString log = CallLog();
  EXPECT_NE(GoogleString::npos, log.find("write_original")) << log;
  EXPECT_NE(GoogleString::npos, log.find("alternate_id=12")) << log;
  EXPECT_NE(GoogleString::npos, log.find("full_mask=12")) << log;
  EXPECT_NE(GoogleString::npos, log.find("write_close")) << log;
  EXPECT_NE(GoogleString::npos, log.find("bytes=11")) << log;
}

TEST_F(DaemonRecordArmTest, ThePeerRefusesEveryOtherClassThroughThisWriter) {
  // This is what makes "originals only" a property of what the module CAN do
  // rather than of what it happens to do: the one write entry point it can
  // reach refuses any other id.  Asserted against the peer, not against a
  // branch in this module -- a branch could be deleted, the refusal cannot.
  const DaemonRecordInput input = EligibleInput();
  DaemonRecordDecision decision = EvaluateDaemonRecordGate(*abi_, input, 1000);
  ASSERT_NE(DaemonRecordVerdict::kRefuse, decision.verdict);

  // 0x08 is the identity alternate -- the closest thing to a plausible
  // mistake, and the one the optimizer owns.
  decision.params.alternate_id = 0x08;
  int cache_marker = 0;
  DaemonRecordWriter writer(abi_.get(), &cache_marker);
  EXPECT_EQ(kPsErrInvalidArg, writer.Begin(input, decision));
  EXPECT_FALSE(writer.open());
  EXPECT_EQ(GoogleString::npos, CallLog().find("write_original"));

  decision.params.alternate_id = kPsSentinelOriginal;
  decision.params.full_mask = 0x08;
  DaemonRecordWriter second(abi_.get(), &cache_marker);
  EXPECT_EQ(kPsErrInvalidArg, second.Begin(input, decision));
}

// --- rule: the peer's Vary predicate governs the shared plane --------------

TEST_F(DaemonRecordArmTest, VaryOnAnAllowedAxisIsStorable) {
  DaemonRecordInput input = EligibleInput();
  input.vary = "Accept-Encoding";
  EXPECT_NE(DaemonRecordVerdict::kRefuse,
            EvaluateDaemonRecordGate(*abi_, input, 1000).verdict);
}

TEST_F(DaemonRecordArmTest, TheVaryPredicateAppliedHereIsThePeers) {
  // Scope, stated because the earlier version of this test overstated it:
  // everything below is about the PEER's predicate. That it differs from this
  // module's own is a two-predicate claim and cannot be made from this file,
  // which has no ResponseHeaders and no configuration -- it is made in the
  // recorder's suite, over the path that actually runs both.
  //
  // What IS established here: the record gate consults the peer, and the
  // peer's answer is what the gate returns.
  DaemonRecordInput input = EligibleInput();
  input.vary = "X-Tenant";
  const DaemonRecordDecision decision =
      EvaluateDaemonRecordGate(*abi_, input, 1000);
  EXPECT_EQ(DaemonRecordVerdict::kRefuse, decision.verdict);
  EXPECT_STREQ(daemon_record_reason::kVaryNotStorable, decision.reason);
  EXPECT_EQ(1, abi_->VaryUncacheable("X-Tenant"));
}

TEST_F(DaemonRecordArmTest, ThePeerAdmitsExactlyItsFourAxes) {
  // The allowlist as an executable table, so a change to it on the peer's
  // side surfaces here rather than as a silent widening of what this module
  // is willing to put in a shared cache.
  for (const char* allowed : {"Accept-Encoding", "User-Agent", "Accept",
                              "Save-Data", "accept-encoding", " Accept "}) {
    EXPECT_EQ(0, abi_->VaryUncacheable(allowed)) << allowed;
  }
  for (const char* refused :
       {"*", "Cookie", "Cookie2", "X-Tenant", "Origin", "Accept-Language"}) {
    EXPECT_EQ(1, abi_->VaryUncacheable(refused)) << refused;
  }
  // No Vary at all is storable, and is spelled as absence rather than as "".
  EXPECT_EQ(0, abi_->VaryUncacheable(nullptr));
}

TEST_F(DaemonRecordArmTest, VaryStarIsRefused) {
  DaemonRecordInput input = EligibleInput();
  input.vary = "*";
  EXPECT_EQ(DaemonRecordVerdict::kRefuse,
            EvaluateDaemonRecordGate(*abi_, input, 1000).verdict);
}

TEST_F(DaemonRecordArmTest, RepeatedVaryLinesAreOneList) {
  // Offering the list one line at a time would accept a combination the list
  // as a whole is refused for, so the caller joins them and this pins that
  // the joined form is what the predicate sees.
  EXPECT_EQ(1, abi_->VaryUncacheable("Accept-Encoding,Cookie"));
  EXPECT_EQ(0, abi_->VaryUncacheable("Accept-Encoding,Accept"));
}

// --- the `Vary: Accept` stamping obligation --------------------------------
//
// The peer's write contract puts ONE flag on this module: an origin whose
// `Vary` names `Accept` picked the representation itself, so the single copy
// that gets stored is whichever one the FIRST requester's `Accept` elicited.
// Unmarked, the optimizer derives a whole variant family from it and every
// later client is served bytes transcoded from a stranger's negotiation --
// with no error and nothing downstream able to notice.  The bit cannot be
// added later: on a hit the origin's `Vary` is gone.
//
// The assertions below are on the flag AS THE PEER RECEIVED IT wherever one
// call can show that, because "the decision holds the right value" and "the
// value reached the write" are different claims and only the second one is
// the obligation.

TEST_F(DaemonRecordArmTest, AVaryNamingAcceptIsStampedOnWhatIsStored) {
  DaemonRecordInput input = EligibleInput();
  input.vary = "Accept";
  const DaemonRecordDecision decision =
      EvaluateDaemonRecordGate(*abi_, input, 1000);
  ASSERT_EQ(DaemonRecordVerdict::kStoreAndNotify, decision.verdict);
  EXPECT_EQ(kPsFlagOriginVariesAccept, decision.params.flags);

  int cache_marker = 0;
  DaemonRecordWriter writer(abi_.get(), &cache_marker);
  ASSERT_EQ(kPsOk, writer.Begin(input, decision));
  ASSERT_EQ(kPsOk, writer.Commit());
  // What the PEER was told, read back out of its own log: the decision could
  // carry the bit and the writer could still drop it on the way.
  EXPECT_NE(GoogleString::npos, CallLog().find(" flags=4")) << CallLog();
}

TEST_F(DaemonRecordArmTest, TheStampIsNotAnAdmissionDecision) {
  // An `Accept`-negotiating origin is stored AND notified like any other
  // storable response.  The flag is what the optimizer consults; refusing the
  // response instead would throw away a perfectly cacheable original, and
  // storing it without asking for optimization would be a rule this gate does
  // not have.  Pinned because "stamp it" is easy to over-implement as "refuse
  // it", and the difference is invisible in the flags byte.
  DaemonRecordInput input = EligibleInput();
  input.vary = "Accept";
  const DaemonRecordDecision decision =
      EvaluateDaemonRecordGate(*abi_, input, 1000);
  EXPECT_EQ(DaemonRecordVerdict::kStoreAndNotify, decision.verdict);
  EXPECT_STREQ(daemon_record_reason::kEligible, decision.reason);
  EXPECT_EQ(kPsSentinelOriginal, decision.params.alternate_id);
  EXPECT_EQ(kPsOk, NotifyDaemon(*abi_, "/tmp/socket", input, decision));
}

TEST_F(DaemonRecordArmTest, TheStampIsAbsentWhenTheOriginDoesNotVaryOnAccept) {
  // Three ways of not varying on `Accept`, each of which a wrong
  // implementation gets wrong differently: a different allowed axis (a
  // substring-matching implementation stamps `Accept-Encoding`), no Vary at
  // all (a null-blind implementation stamps everything), and an axis that
  // merely starts the same way.
  for (const char* vary :
       {"Accept-Encoding", "", "Accept-Encoding, User-Agent", "Save-Data"}) {
    DaemonRecordInput input = EligibleInput();
    input.vary = vary;
    const DaemonRecordDecision decision =
        EvaluateDaemonRecordGate(*abi_, input, 1000);
    ASSERT_NE(DaemonRecordVerdict::kRefuse, decision.verdict) << vary;
    EXPECT_EQ(0, decision.params.flags) << "vary=[" << vary << "]";
  }
}

TEST_F(DaemonRecordArmTest,
       ARefusedResponseIsNeverStampedBecauseItIsNeverStored) {
  // `Accept` appears in the list, and the list as a whole is refused.  The
  // peer's two predicates are halves of ONE classification and it guarantees
  // the pairing in this direction -- a refused response never varies on
  // `Accept`, because the class is "store it but never optimize it" and there
  // is nothing to store.  The gate relies on that ordering: it returns on the
  // refusal before it asks the second question.  Both halves are asserted, so
  // a peer that stopped pairing them would be visible here rather than in a
  // stamped entry that was never written.
  EXPECT_EQ(1, abi_->VaryUncacheable("Accept,Cookie"));
  EXPECT_EQ(0, abi_->VaryVariesAccept("Accept,Cookie"));

  DaemonRecordInput input = EligibleInput();
  input.vary = "Accept,Cookie";
  const DaemonRecordDecision decision =
      EvaluateDaemonRecordGate(*abi_, input, 1000);
  EXPECT_EQ(DaemonRecordVerdict::kRefuse, decision.verdict);
  EXPECT_STREQ(daemon_record_reason::kVaryNotStorable, decision.reason);
  int cache_marker = 0;
  DaemonRecordWriter writer(abi_.get(), &cache_marker);
  EXPECT_NE(kPsOk, writer.Begin(input, decision));
  EXPECT_EQ(GoogleString::npos, CallLog().find("write_original")) << CallLog();
}

TEST_F(DaemonRecordArmTest, TheAcceptPredicateAppliedHereIsThePeers) {
  // The allowlist's `Accept` axis as an executable table against the PEER, in
  // the same shape the refusal predicate already has one.  This module holds
  // no `Vary` parser of its own for this question, deliberately: the peer
  // publishes both halves precisely so an embedder does not re-derive either.
  for (const char* varies : {"Accept", "accept", "  Accept  ",
                             "Accept-Encoding,Accept", "Accept, User-Agent"}) {
    EXPECT_EQ(1, abi_->VaryVariesAccept(varies)) << varies;
  }
  for (const char* does_not :
       {"Accept-Encoding", "User-Agent", "Save-Data", "Accept-Language", "*",
        "Accept-Encoding, Save-Data"}) {
    EXPECT_EQ(0, abi_->VaryVariesAccept(does_not)) << does_not;
  }
  // Absence is spelled as absence, not as "", on this predicate too.
  EXPECT_EQ(0, abi_->VaryVariesAccept(nullptr));
}

TEST_F(DaemonRecordArmTest, TheStampedBitDoesNotCollideWithTheOptimizersOwn) {
  // WHAT THIS DOES AND DOES NOT CHECK, stated exactly, because the obvious
  // reading of a test over one constant is stronger than the truth.  It does
  // NOT establish that the mirror matches what the peer publishes: the module
  // cannot include the peer's header, so nothing here can compare against it,
  // and that comparison is made at COMPILE time inside the peer's own tree
  // (tools/parity/daemon-probe/volume_inspect.cc, built at the pin).  What is
  // checkable here is the collision: the bit this module stamps must not be
  // the optimizer's worker-processed bit, because a module that set THAT
  // would make its own recording indistinguishable from optimized output.
  // The literal is restated so that renumbering the mirror is a visible edit
  // here as well as a build failure over there.
  EXPECT_EQ(0x04, kPsFlagOriginVariesAccept);
  EXPECT_EQ(0x02 & kPsFlagOriginVariesAccept, 0)
      << "the module would be claiming the optimizer's worker-processed flag";
}

// --- rule 6: the origin sent something no serve from here can put back -----
//
// The predicate is the PRE-SIDECAR one, and the cases below are the checked-in
// table's rows (tools/parity/adapter_asserts/reconstructible.py) evaluated
// with an EMPTY sidecar set -- which is what this module's serving stack is.
// The rows where the two answers DIFFER are asserted explicitly rather than
// left implicit, because reusing the peer's verdict here is the one wrong
// answer that looks right.

// Builds a header list from alternating name/value literals.  The pieces
// borrow from the literals, which outlive every use.
std::vector<DaemonOriginHeader> Headers(
    std::initializer_list<const char*> pairs) {
  std::vector<DaemonOriginHeader> out;
  const char* name = nullptr;
  for (const char* item : pairs) {
    if (name == nullptr) {
      name = item;
    } else {
      out.push_back(DaemonOriginHeader{name, item});
      name = nullptr;
    }
  }
  return out;
}

TEST_F(DaemonRecordArmTest, TheReproducibleSetIsWhatAServeCanPutBack) {
  // Reproducible: the whole of D6.4 rule 6's allowlist, the serve-generated
  // names, and the one handled `Vary` axis.  Case is the origin's choice on
  // both the field name and the `Vary` token, so both are exercised.
  EXPECT_TRUE(OriginHeadersAreReproducible(
      Headers({"Cache-Control", "max-age=600", "Content-Type", "text/css",
               "Content-Length", "42", "ETag", "\"abc\"", "Last-Modified",
               "Mon, 10 Aug 2026 00:00:00 GMT", "Expires",
               "Tue, 11 Aug 2026 00:00:00 GMT"})));
  EXPECT_TRUE(OriginHeadersAreReproducible(
      Headers({"date", "Mon, 10 Aug 2026 00:00:00 GMT", "Server", "nginx",
               "Connection", "keep-alive", "content-type", "text/css"})));
  EXPECT_TRUE(OriginHeadersAreReproducible(
      Headers({"Content-Type", "text/css", "vary", "accept-encoding"})));
  // BOTH handled `Vary` axes, and `Accept` is here because the serve arm
  // reconstructs it off the stored varies-on-Accept flag rather than because
  // the axis is ignored.
  EXPECT_TRUE(OriginHeadersAreReproducible(
      Headers({"Content-Type", "text/css", "Vary", "Accept"})));
  EXPECT_TRUE(OriginHeadersAreReproducible(Headers(
      {"Content-Type", "text/css", "Vary", "Accept, Accept-Encoding"})));
  // `Age` and `Accept-Ranges` are the two serve-generated names that decide
  // whether this rule is usable at all: the seam hands the predicate the
  // SERVER'S finalized headers, so an `Accept-Ranges` rides on essentially
  // every static resource and an `Age` on everything behind a proxy or CDN.
  // Treating either as origin content marks every such response for ever.
  EXPECT_TRUE(OriginHeadersAreReproducible(
      Headers({"Content-Type", "text/css", "Age", "42"})));
  EXPECT_TRUE(OriginHeadersAreReproducible(
      Headers({"Content-Type", "text/css", "accept-ranges", "bytes"})));

  // Not reproducible.  An arbitrary header, the CORS grant, a security policy,
  // a personalised response, and a representation-level header the entry does
  // not store.
  EXPECT_FALSE(OriginHeadersAreReproducible(
      Headers({"Content-Type", "text/css", "X-Custom", "1"})));
  EXPECT_FALSE(OriginHeadersAreReproducible(Headers(
      {"Content-Type", "text/css", "Access-Control-Allow-Origin", "*"})));
  EXPECT_FALSE(OriginHeadersAreReproducible(
      Headers({"Content-Type", "text/css", "Content-Security-Policy",
               "default-src 'self'"})));
  EXPECT_FALSE(OriginHeadersAreReproducible(
      Headers({"Content-Type", "text/css", "Set-Cookie", "a=b"})));
  EXPECT_FALSE(OriginHeadersAreReproducible(
      Headers({"Content-Type", "text/css", "Content-Encoding", "gzip"})));
  // A near neighbour of a member is not a member: the set is a closed list of
  // exact names.
  EXPECT_FALSE(OriginHeadersAreReproducible(
      Headers({"Content-Type", "text/css",
               "Content-Security-Policy-Report-Only", "default-src 'self'"})));
}

TEST_F(DaemonRecordArmTest, EveryVaryLineIsJudgedAsOneList) {
  // RFC 9110 s5.3: repeated fields are ONE list.  A per-line check passes the
  // first line and never looks at the second, which is exactly the shape that
  // admits an unhandled axis.
  EXPECT_FALSE(OriginHeadersAreReproducible(
      Headers({"Content-Type", "text/css", "Vary", "Accept-Encoding", "Vary",
               "User-Agent"})));
  EXPECT_FALSE(OriginHeadersAreReproducible(Headers(
      {"Content-Type", "text/css", "Vary", "Accept-Encoding, User-Agent"})));
  // THE REVERSE ORDER, and it is the case that actually discriminates.  With
  // the unhandled axis on the FIRST line, an implementation that let each
  // `Vary` line overwrite the last -- rather than appending to one list --
  // still answers FALSE above and TRUE here.  Only this ordering fails it.
  EXPECT_FALSE(OriginHeadersAreReproducible(
      Headers({"Content-Type", "text/css", "Vary", "User-Agent", "Vary",
               "Accept-Encoding"})));
  EXPECT_FALSE(OriginHeadersAreReproducible(Headers(
      {"Content-Type", "text/css", "Vary", "Cookie", "Vary", "Accept"})));
  EXPECT_FALSE(OriginHeadersAreReproducible(
      Headers({"Content-Type", "text/css", "Vary", "*"})));
  // A handled axis beside an unhandled one on ONE line, both orders.
  EXPECT_FALSE(OriginHeadersAreReproducible(Headers(
      {"Content-Type", "text/css", "Vary", "User-Agent, Accept-Encoding"})));
}

TEST_F(DaemonRecordArmTest, ADefaultServerDeploymentIsNotMarked) {
  // THE CASE THE WHOLE IGNORE CLASS EXISTS FOR, and the one a three-header
  // synthetic fixture cannot stand in for: what this predicate is actually
  // handed is the response as the SERVER finalized it, and on a default
  // deployment that is a good deal more than the origin wrote.  If a stock
  // static-resource response marks, the substrate is off everywhere and the
  // rule has taken in-place optimization with it.
  EXPECT_TRUE(OriginHeadersAreReproducible(Headers({
      "Date",
      "Mon, 10 Aug 2026 00:00:00 GMT",
      "Server",
      "Apache/2.4.58 (Ubuntu)",
      "Last-Modified",
      "Sun, 09 Aug 2026 12:00:00 GMT",
      "ETag",
      "\"1a2b-63f0c1\"",
      "Accept-Ranges",
      "bytes",
      "Content-Length",
      "1179",
      "Cache-Control",
      "max-age=600, public",
      "Expires",
      "Mon, 10 Aug 2026 00:10:00 GMT",
      "Content-Type",
      "text/css",
  })));
  // The same response as it arrives through a caching proxy or CDN, which adds
  // exactly one thing: how old the copy is.
  EXPECT_TRUE(OriginHeadersAreReproducible(Headers({
      "Date",
      "Mon, 10 Aug 2026 00:00:00 GMT",
      "Server",
      "Apache/2.4.58 (Ubuntu)",
      "Age",
      "312",
      "Accept-Ranges",
      "bytes",
      "Cache-Control",
      "max-age=600, public",
      "Content-Type",
      "text/css",
  })));
}

TEST_F(DaemonRecordArmTest, ASiteWideConfiguredHeaderIsMarkedAndThatIsCoarse) {
  // THE FALSE POSITIVE, PINNED AS ONE.  A site-wide server directive that adds
  // a header to every response adds it to THIS substrate's responses too, so
  // the header is in fact reproduced and marking on it costs the resource its
  // in-place optimization for nothing.  The predicate marks anyway, because at
  // this seam a configured site-wide header and an origin header for this URL
  // alone are the same bytes in the same list.
  //
  // Pinned rather than left implicit so that the cost is visible in the suite:
  // a deployment that sets a security header site-wide gets no in-place
  // optimization from this substrate until something can tell the two apart.
  EXPECT_FALSE(OriginHeadersAreReproducible(Headers({
      "Date",
      "Mon, 10 Aug 2026 00:00:00 GMT",
      "Server",
      "Apache/2.4.58 (Ubuntu)",
      "Accept-Ranges",
      "bytes",
      "Cache-Control",
      "max-age=600, public",
      "Content-Type",
      "text/css",
      "Strict-Transport-Security",
      "max-age=31536000",
  })));
}

TEST_F(DaemonRecordArmTest, TheSetIsNarrowerThanThePeersSidecarVerdict) {
  // THE ROWS WHERE THE TWO PREDICATES DISAGREE, pinned as the divergence they
  // are.  The peer keeps a per-URL block of the request-independent response
  // headers and can replay every name below verbatim, so its own admission
  // gate calls them reproducible.  Nothing writes that block for a URL THIS
  // module records -- the record arm writes the durable original and nothing
  // else, and no later writer can reconstruct it once the origin's headers are
  // gone -- so here they are exactly what a serve would drop.  A change that
  // made this module reuse the peer's verdict would flip every one of these to
  // true and mark nothing, which is indistinguishable from not having the flag
  // at all.
  for (const char* name :
       {"Access-Control-Allow-Origin", "Content-Security-Policy",
        "X-Content-Type-Options", "Referrer-Policy",
        "Cross-Origin-Opener-Policy", "Cross-Origin-Embedder-Policy",
        "Cross-Origin-Resource-Policy", "Permissions-Policy"}) {
    EXPECT_FALSE(OriginHeadersAreReproducible(
        Headers({"Content-Type", "text/css", name, "x"})))
        << name;
  }
}

TEST_F(DaemonRecordArmTest, AHeaderListNobodyGatheredStampsNothing) {
  // An EMPTY list means the caller did not gather the headers, not that the
  // origin sent none -- every real response carries at least a Content-Type.
  // A port that has not been taught to fill it in keeps its pre-1.8 behaviour
  // instead of marking every entry it records, which would turn in-place
  // optimization off for that port entirely.  Pinned because the fail-closed
  // reading is the tempting one and it is wrong HERE.
  EXPECT_TRUE(OriginHeadersAreReproducible({}));
  const DaemonRecordDecision decision =
      EvaluateDaemonRecordGate(*abi_, EligibleInput(), 1000);
  EXPECT_EQ(0, decision.params.flags);
}

TEST_F(DaemonRecordArmTest, AnUnreproducibleHeaderIsStampedOnWhatIsStored) {
  DaemonRecordInput input = EligibleInput();
  input.response_headers = Headers({"Content-Type", "text/css", "Cache-Control",
                                    "max-age=600", "X-Custom", "1"});
  const DaemonRecordDecision decision =
      EvaluateDaemonRecordGate(*abi_, input, 1000);
  ASSERT_EQ(DaemonRecordVerdict::kStoreAndNotify, decision.verdict);
  EXPECT_EQ(kPsFlagOriginHeadersNotReproducible, decision.params.flags);

  int cache_marker = 0;
  DaemonRecordWriter writer(abi_.get(), &cache_marker);
  ASSERT_EQ(kPsOk, writer.Begin(input, decision));
  ASSERT_EQ(kPsOk, writer.Commit());
  // What the PEER was told, off its own log: the decision could carry the bit
  // and the writer could still drop it on the way.
  EXPECT_NE(GoogleString::npos, CallLog().find(" flags=8")) << CallLog();
}

TEST_F(DaemonRecordArmTest, TheUnreproducibleStampIsNotAnAdmissionDecision) {
  // Stored AND notified, exactly like any other storable response.  This is
  // the option the ruling took over refusing the response at the gate, and the
  // difference is invisible in the flags byte: what the optimizer may do with
  // a URL is not this module's to decide from one front end's inability to
  // serve the result -- the peer's own front stage keeps a header sidecar and
  // can serve it perfectly.  What changes is what THIS module does at serve
  // time.
  DaemonRecordInput input = EligibleInput();
  input.response_headers =
      Headers({"Content-Type", "text/css", "X-Custom", "1"});
  const DaemonRecordDecision decision =
      EvaluateDaemonRecordGate(*abi_, input, 1000);
  EXPECT_EQ(DaemonRecordVerdict::kStoreAndNotify, decision.verdict);
  EXPECT_STREQ(daemon_record_reason::kEligible, decision.reason);
  EXPECT_EQ(kPsSentinelOriginal, decision.params.alternate_id);
  EXPECT_EQ(kPsOk, NotifyDaemon(*abi_, "/tmp/socket", input, decision));
}

TEST_F(DaemonRecordArmTest, TheTwoStampsAreIndependentAndDoNotCollide) {
  // A response can earn both, and the two are computed from different things
  // by different owners: the `Accept` answer is the PEER'S over the joined
  // `Vary`, this one is THIS MODULE'S over the whole response.  A single
  // expression computing both would invite the next reader to source them from
  // the same place.
  DaemonRecordInput input = EligibleInput();
  input.vary = "Accept";
  input.response_headers =
      Headers({"Content-Type", "text/css", "Vary", "Accept", "X-Custom", "1"});
  const DaemonRecordDecision decision =
      EvaluateDaemonRecordGate(*abi_, input, 1000);
  ASSERT_EQ(DaemonRecordVerdict::kStoreAndNotify, decision.verdict);
  EXPECT_EQ(kPsFlagOriginVariesAccept | kPsFlagOriginHeadersNotReproducible,
            decision.params.flags);

  // The same collision check the Vary-Accept bit gets, and for the same
  // reason: the literal is restated so renumbering the mirror is a visible
  // edit here as well as a build failure inside the peer's tree.
  EXPECT_EQ(0x08, kPsFlagOriginHeadersNotReproducible);
  EXPECT_EQ(0, kPsFlagOriginHeadersNotReproducible & kPsFlagOriginVariesAccept);
  EXPECT_EQ(0, kPsFlagOriginHeadersNotReproducible & 0x02)
      << "the module would be claiming the optimizer's worker-processed flag";
}

// --- rule: no-transform stores and never notifies --------------------------

TEST_F(DaemonRecordArmTest, NoTransformStoresButNeverNotifies) {
  DaemonRecordInput input = EligibleInput();
  input.cache_control_lines.clear();
  input.cache_control_lines.push_back("max-age=600, no-transform");

  const DaemonRecordDecision decision =
      EvaluateDaemonRecordGate(*abi_, input, 1000);
  EXPECT_EQ(DaemonRecordVerdict::kStoreOnly, decision.verdict);
  EXPECT_STREQ(daemon_record_reason::kOriginNoTransform, decision.reason);

  // Not merely "the verdict says so": the notify entry point refuses a
  // decision that did not earn one, and nothing reaches the wire.
  EXPECT_EQ(kPsErrInvalidArg,
            NotifyDaemon(*abi_, "/tmp/socket", input, decision));
  EXPECT_EQ(GoogleString::npos, CallLog().find("notify "));
}

TEST_F(DaemonRecordArmTest, NoStoreIsRefusedOutright) {
  DaemonRecordInput input = EligibleInput();
  input.cache_control_lines.clear();
  input.cache_control_lines.push_back("no-store");
  const DaemonRecordDecision decision =
      EvaluateDaemonRecordGate(*abi_, input, 1000);
  EXPECT_EQ(DaemonRecordVerdict::kRefuse, decision.verdict);
  EXPECT_STREQ(daemon_record_reason::kOriginNoStore, decision.reason);
}

// --- rule: the transition guard --------------------------------------------

TEST_F(DaemonRecordArmTest, ARewrittenUrlIsNeitherStoredNorNotified) {
  DaemonRecordInput input = EligibleInput();
  input.url = "/a/style.css.pagespeed.ce.0123456789.css";

  const DaemonRecordDecision decision =
      EvaluateDaemonRecordGate(*abi_, input, 1000);
  EXPECT_EQ(DaemonRecordVerdict::kRefuse, decision.verdict);
  EXPECT_STREQ(daemon_record_reason::kRewrittenUrlShape, decision.reason);

  int cache_marker = 0;
  DaemonRecordWriter writer(abi_.get(), &cache_marker);
  EXPECT_NE(kPsOk, writer.Begin(input, decision));
  EXPECT_EQ(kPsErrInvalidArg,
            NotifyDaemon(*abi_, "/tmp/socket", input, decision));
  const GoogleString log = CallLog();
  EXPECT_EQ(GoogleString::npos, log.find("write_original")) << log;
  EXPECT_EQ(GoogleString::npos, log.find("notify ")) << log;
}

TEST_F(DaemonRecordArmTest, TheMarkerIsMatchedMidNameNotAsASuffix) {
  EXPECT_TRUE(IsRewrittenUrlShape("/a/x.css.pagespeed.ce.abc.css"));
  EXPECT_TRUE(IsRewrittenUrlShape("/a/x.pagespeed."));
  EXPECT_FALSE(IsRewrittenUrlShape("/a/pagespeed/x.css"));
  EXPECT_FALSE(IsRewrittenUrlShape("/a/style.css"));
}

// --- rule: only URLs whose key this module can be sure of ------------------

TEST_F(DaemonRecordArmTest, AUrlThisModuleCannotKeyIsRefused) {
  // The canonical form the shared key is composed from is the peer's, and
  // producing it is not part of the peer's published interface.  So the arm
  // records only the subset that is already canonical under any
  // configuration, rather than writing entries at keys that may never be
  // read.
  for (const char* url : {"/a/style.css?v=2", "/a/sty%6ce.css", "/a/x#frag",
                          "http://example.com/a/style.css", "a/style.css"}) {
    DaemonRecordInput input = EligibleInput();
    input.url = url;
    const DaemonRecordDecision decision =
        EvaluateDaemonRecordGate(*abi_, input, 1000);
    EXPECT_EQ(DaemonRecordVerdict::kRefuse, decision.verdict) << url;
    EXPECT_STREQ(daemon_record_reason::kUrlNotKeySafe, decision.reason) << url;
  }
  EXPECT_TRUE(UrlIsKeySafe("/a/style.css"));
}

// --- rule: the notify mask is derived from the request ---------------------

TEST_F(DaemonRecordArmTest, TheNotifyMaskTracksTheRequest) {
  struct Row {
    const char* accept;
    const char* user_agent;
    const char* accept_encoding;
  };
  const Row rows[] = {
      {"text/css", "Mozilla/5.0", ""},
      {"image/avif,image/webp,*/*", "Mozilla/5.0", "gzip"},
      {"image/webp,*/*", "Mozilla/5.0 (iPhone; Mobile)", "br"},
      {"*/*", "Mozilla/5.0 (iPad; Tablet)", "gzip, br"},
  };
  std::vector<uint32_t> masks;
  for (const Row& row : rows) {
    DaemonRecordInput input = EligibleInput();
    input.accept = row.accept;
    input.user_agent = row.user_agent;
    input.accept_encoding = row.accept_encoding;
    const DaemonRecordDecision decision =
        EvaluateDaemonRecordGate(*abi_, input, 1000);
    ASSERT_EQ(DaemonRecordVerdict::kStoreAndNotify, decision.verdict);
    // The expected value is the peer's OWN classifier over the same headers,
    // asked separately.  That pins that the mask is derived from THIS
    // request; it is deliberately not an independent re-derivation of the
    // classification rules, which would be a second implementation of them.
    EXPECT_EQ(abi_->Classify(row.accept, row.user_agent, nullptr,
                             row.accept_encoding),
              decision.notify_mask);
    masks.push_back(decision.notify_mask);
  }
  // A constant mask would satisfy every equality above only if the oracle
  // were also constant, so the rows must actually differ for this to mean
  // anything.
  std::sort(masks.begin(), masks.end());
  masks.erase(std::unique(masks.begin(), masks.end()), masks.end());
  EXPECT_GT(masks.size(), 1u)
      << "the request matrix does not classify distinctly, so mask equality "
         "proves nothing";
}

// --- rule: the option context ----------------------------------------------

TEST_F(DaemonRecordArmTest, TheEmptyContextSignsIdenticallyOnBothSides) {
  // The value a peer uses to mean "no options context was supplied" is a
  // constant of the FORMAT, not of this build, and the two sides compute it
  // with different toolchains. If they ever disagreed, every entry this
  // module recorded would be filed under a namespace the peer reads as a
  // different one.
  const GoogleString empty_payload = StrCat(kOptionContextFormatVersion, "\n");
  char peer_signature[kPsOptionContextSignatureChars + 1];
  ASSERT_EQ(kPsOk, abi_->OptionContextSignature(
                       empty_payload.data(), empty_payload.size(),
                       peer_signature, sizeof(peer_signature)));
  EXPECT_EQ(OptionContext::DefaultSignature(), GoogleString(peer_signature));
}

TEST_F(DaemonRecordArmTest, ARealConfigurationSignsIdenticallyOnBothSides) {
  // And the same for a payload that is not the format constant.  NOTE that a
  // default-constructed configuration is NOT the empty context: the
  // structural HTML writer is on at every rewrite level, so even an untouched
  // configuration renders a real record. That is the producing side's
  // property, pinned there; what matters here is that whatever it renders,
  // both sides sign the same.
  ASSERT_NE(OptionContext::DefaultSignature(), default_signature_);
  EXPECT_TRUE(
      default_payload_.starts_with(StrCat(kOptionContextFormatVersion, "\n")))
      << default_payload_;

  char peer_signature[kPsOptionContextSignatureChars + 1];
  ASSERT_EQ(kPsOk, abi_->OptionContextSignature(
                       default_payload_.data(), default_payload_.size(),
                       peer_signature, sizeof(peer_signature)));
  EXPECT_EQ(default_signature_, GoogleString(peer_signature));

  // The peer does not take a signature on trust -- it recomputes it -- so a
  // notification carrying this pair is accepted only because the two agree.
  const DaemonRecordInput input = EligibleInput();
  const DaemonRecordDecision decision =
      EvaluateDaemonRecordGate(*abi_, input, 1000);
  ASSERT_EQ(DaemonRecordVerdict::kStoreAndNotify, decision.verdict);
  EXPECT_EQ(kPsOk, NotifyDaemon(*abi_, "/tmp/socket", input, decision));
}

TEST_F(DaemonRecordArmTest, ANonDefaultContextIsStampedOnTheNotification) {
  RewriteOptions options(thread_system_.get());
  options.set_image_jpeg_recompress_quality(71);
  GoogleString payload, signature;
  ASSERT_EQ(OptionContextStatus::kOk,
            OptionContext::Compute(options, &payload, &signature));
  EXPECT_NE(OptionContext::DefaultSignature(), signature);

  DaemonRecordInput input = EligibleInput();
  input.option_context = payload;
  input.option_signature = signature;
  const DaemonRecordDecision decision =
      EvaluateDaemonRecordGate(*abi_, input, 1000);
  ASSERT_EQ(DaemonRecordVerdict::kStoreAndNotify, decision.verdict);
  ASSERT_EQ(kPsOk, NotifyDaemon(*abi_, "/tmp/socket", input, decision));

  const GoogleString log = CallLog();
  EXPECT_NE(GoogleString::npos, log.find(StrCat("signature=", signature)))
      << log;
  EXPECT_NE(GoogleString::npos,
            log.find(StrCat("context_length=",
                            IntegerToString(static_cast<int>(payload.size())))))
      << log;
}

TEST_F(DaemonRecordArmTest, AContextThatCannotBeRenderedSkipsTheNotification) {
  // The failure direction the producing side documents: a caller with no
  // usable context must proceed WITHOUT one, never substitute the default.
  // A default context is a DIFFERENT context, and handing this request's
  // work to it is exactly the cross-configuration reuse the signature exists
  // to prevent.  So: keep the original, ask for nothing.
  DaemonRecordInput input = EligibleInput();
  input.option_context = StringPiece();
  input.option_signature = StringPiece();
  const DaemonRecordDecision decision =
      EvaluateDaemonRecordGate(*abi_, input, 1000);
  EXPECT_EQ(DaemonRecordVerdict::kStoreOnly, decision.verdict);
  EXPECT_STREQ(daemon_record_reason::kNoOptionContext, decision.reason);
  EXPECT_EQ(kPsErrInvalidArg,
            NotifyDaemon(*abi_, "/tmp/socket", input, decision));
  EXPECT_EQ(GoogleString::npos, CallLog().find("notify "));
}

TEST_F(DaemonRecordArmTest, HalfAContextIsNeverSent) {
  DaemonRecordInput input = EligibleInput();
  input.option_signature = StringPiece();
  EXPECT_EQ(DaemonRecordVerdict::kStoreOnly,
            EvaluateDaemonRecordGate(*abi_, input, 1000).verdict);
}

TEST_F(DaemonRecordArmTest, ThePeerRefusesASignatureThatIsNotItsPayloads) {
  // Belt and braces on the pairing rule: even if a wrong pair reached the
  // notification, the peer recomputes and refuses it rather than filing the
  // work under a name nobody used.
  const GoogleString wrong(kPsOptionContextSignatureChars, 'a');
  DaemonRecordInput input = EligibleInput();
  input.option_signature = wrong;
  DaemonRecordDecision decision = EvaluateDaemonRecordGate(*abi_, input, 1000);
  ASSERT_EQ(DaemonRecordVerdict::kStoreAndNotify, decision.verdict);
  EXPECT_EQ(kPsErrInvalidArg,
            NotifyDaemon(*abi_, "/tmp/socket", input, decision));
}

// --- rule: the writer adjusts Age ------------------------------------------

TEST_F(DaemonRecordArmTest, InboundAgeIsFoldedIntoTheInsertionTime) {
  DaemonRecordInput input = EligibleInput();
  input.age_seconds = 120;
  const DaemonRecordDecision decision =
      EvaluateDaemonRecordGate(*abi_, input, 1000);
  // The entry becomes fresh at the time the ORIGIN generated it, not at the
  // time this process stored it.  Without this an entry that arrived through
  // an upstream cache carrying Age serves stale bytes as fresh for the whole
  // of Age -- systematically, and invisibly, behind a CDN.
  EXPECT_EQ(880u, decision.params.cache_inserted_at);

  DaemonRecordInput fresh = EligibleInput();
  EXPECT_EQ(
      1000u,
      EvaluateDaemonRecordGate(*abi_, fresh, 1000).params.cache_inserted_at);

  // An Age larger than the clock is an anomaly, and failing toward the local
  // clock keeps the entry merely over-fresh rather than in another epoch.
  DaemonRecordInput absurd = EligibleInput();
  absurd.age_seconds = 5000;
  EXPECT_EQ(
      1000u,
      EvaluateDaemonRecordGate(*abi_, absurd, 1000).params.cache_inserted_at);
}

TEST_F(DaemonRecordArmTest, TheAdjustedInsertionTimeReachesThePeer) {
  DaemonRecordInput input = EligibleInput();
  input.age_seconds = 120;
  const DaemonRecordDecision decision =
      EvaluateDaemonRecordGate(*abi_, input, 1000);
  int cache_marker = 0;
  DaemonRecordWriter writer(abi_.get(), &cache_marker);
  ASSERT_EQ(kPsOk, writer.Begin(input, decision));
  ASSERT_EQ(kPsOk, writer.Commit());
  const GoogleString log = CallLog();
  EXPECT_NE(GoogleString::npos, log.find("cache_inserted_at=880")) << log;
  // And the parameter block reached the peer at its full published size --
  // one byte short and the peer reads the earlier, shorter revision and
  // silently drops every field from the insertion time onwards.
  EXPECT_NE(
      GoogleString::npos,
      log.find(StrCat(
          "usable=", IntegerToString(static_cast<int>(sizeof(PsWriteParams))))))
      << log;
}

// --- rule: the origin's state is stamped, not ours -------------------------

TEST_F(DaemonRecordArmTest, TheOriginsCacheControlAndValidatorsAreStamped) {
  DaemonRecordInput input = EligibleInput();
  input.cache_control_lines.clear();
  input.cache_control_lines.push_back("max-age=600");
  input.cache_control_lines.push_back("s-maxage=60, public");
  input.last_modified_ms = 1000LL * 1000;
  const DaemonRecordDecision decision =
      EvaluateDaemonRecordGate(*abi_, input, 5000);
  EXPECT_EQ(600u, decision.params.origin_max_age);
  EXPECT_EQ(60u, decision.params.origin_s_maxage);
  EXPECT_EQ(1000u, decision.params.origin_last_modified);

  int cache_marker = 0;
  DaemonRecordWriter writer(abi_.get(), &cache_marker);
  ASSERT_EQ(kPsOk, writer.Begin(input, decision));
  ASSERT_EQ(kPsOk, writer.Commit());
  const GoogleString log = CallLog();
  EXPECT_NE(GoogleString::npos, log.find("max_age=600")) << log;
  EXPECT_NE(GoogleString::npos, log.find("s_maxage=60")) << log;
  EXPECT_NE(GoogleString::npos, log.find("etag=\"abc\"")) << log;
  EXPECT_NE(GoogleString::npos, log.find("origin_ct=text/css")) << log;
}

// --- rule: the content cap --------------------------------------------------

TEST_F(DaemonRecordArmTest, ADeclaredLengthOverTheCapIsRefusedBeforeTheWrite) {
  DaemonRecordInput input = EligibleInput();
  input.content_length = kDaemonOriginalContentCapBytes + 1;
  const DaemonRecordDecision decision =
      EvaluateDaemonRecordGate(*abi_, input, 1000);
  EXPECT_EQ(DaemonRecordVerdict::kRefuse, decision.verdict);
  EXPECT_STREQ(daemon_record_reason::kOverContentCap, decision.reason);
  EXPECT_EQ(GoogleString::npos, CallLog().find("write_original"));
}

TEST_F(DaemonRecordArmTest, AStreamThatRunsPastTheCapStoresNothing) {
  DaemonRecordInput input = EligibleInput();
  input.content_length = 0;  // Length unknown until the response ends.
  const DaemonRecordDecision decision =
      EvaluateDaemonRecordGate(*abi_, input, 1000);
  ASSERT_NE(DaemonRecordVerdict::kRefuse, decision.verdict);

  int cache_marker = 0;
  DaemonRecordWriter writer(abi_.get(), &cache_marker);
  ASSERT_EQ(kPsOk, writer.Begin(input, decision));
  const GoogleString chunk(1024 * 1024, 'x');
  int status = kPsOk;
  for (int i = 0; i < 20 && status == kPsOk; ++i) {
    status = writer.Append(chunk.data(), chunk.size());
  }
  EXPECT_EQ(kPsErrNoSpace, status);
  // Sticky: the entry is abandoned at the crossing write, so pushing more at
  // it turns one honest error into many rather than storing anything.
  EXPECT_EQ(kPsErrNoSpace, writer.Append(chunk.data(), chunk.size()));
}

// --- degraded states --------------------------------------------------------

TEST_F(DaemonRecordArmTest, WithoutACacheNothingIsWritten) {
  const DaemonRecordInput input = EligibleInput();
  const DaemonRecordDecision decision =
      EvaluateDaemonRecordGate(*abi_, input, 1000);
  DaemonRecordWriter writer(abi_.get(), nullptr);
  EXPECT_EQ(kPsErrInvalidArg, writer.Begin(input, decision));
  EXPECT_EQ(GoogleString::npos, CallLog().find("write_original"));
}

TEST_F(DaemonRecordArmTest, AFailedNotificationIsReportedAndNotRetried) {
  setenv("PS_STUB_NOTIFY_FAIL", "1", 1);
  const DaemonRecordInput input = EligibleInput();
  const DaemonRecordDecision decision =
      EvaluateDaemonRecordGate(*abi_, input, 1000);
  ASSERT_EQ(DaemonRecordVerdict::kStoreAndNotify, decision.verdict);
  EXPECT_NE(kPsOk, NotifyDaemon(*abi_, "/tmp/socket", input, decision));
}

}  // namespace

}  // namespace net_instaweb
