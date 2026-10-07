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

// Where the daemon adapter meets Apache.
//
// The adapter's own behaviour is unit-tested next to the adapter; what cannot
// be reached from there is the WIRING -- that post-config actually runs the
// startup check and actually refuses the start, and that the in-place handler
// actually asks the gate instead of building a recorder itself. Both are two
// lines in files that need a live httpd to execute, so both are pinned by
// reading the source, the same idiom the post-config thread-count call-site
// test already uses.
//
// A source pin is weaker than an execution pin and is written knowing that:
// it catches the deletion or bypass of a call site, which is the realistic
// regression, and it does not catch a call site that runs and does nothing.
// The behavioural half is covered where the behaviour lives.

#include <cctype>

#include "pagespeed/apache/instaweb_handler.h"
#include "pagespeed/apache/streaming_pagespeed_resource_fetch.h"
#include "pagespeed/kernel/base/file_system.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/stdio_file_system.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/system/daemon_serve_arm.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {

namespace {

GoogleString ReadSource(const GoogleString& relative_path) {
  StdioFileSystem file_system;
  NullMessageHandler handler;
  GoogleString source;
  const GoogleString path = StrCat(GTestSrcDir(), "/", relative_path);
  EXPECT_TRUE(file_system.ReadFile(path.c_str(), &source, &handler)) << path;
  return source;
}

class DaemonSeamTest : public testing::Test {};

TEST_F(DaemonSeamTest, PostConfigRunsTheStartupCheckAndCanRefuseTheStart) {
  const GoogleString source = ReadSource("pagespeed/apache/mod_instaweb.cc");

  const size_t post_config = source.find("int pagespeed_post_config(");
  ASSERT_NE(GoogleString::npos, post_config)
      << "the post-config hook has been renamed; this pin needs updating";

  const size_t call = source.find("RunDaemonStartupCheck()", post_config);
  ASSERT_NE(GoogleString::npos, call)
      << "post-config no longer resolves daemon health, so the serving path "
         "would have to probe per request";

  // The refusal has to be wired to the hook's failure return, or a sizing
  // divergence is detected and then ignored.
  const GoogleString tail = source.substr(call, 200);
  EXPECT_NE(GoogleString::npos, tail.find("HTTP_INTERNAL_SERVER_ERROR"))
      << "the startup check's verdict is not connected to the start";
}

TEST_F(DaemonSeamTest, TheInPlaceHandlerAsksTheGate) {
  const GoogleString source =
      ReadSource("pagespeed/apache/instaweb_handler.cc");

  const size_t handler = source.find("InstawebHandler::HandleAsInPlace()");
  ASSERT_NE(GoogleString::npos, handler)
      << "the in-place seam has been renamed; this pin needs updating";

  EXPECT_NE(GoogleString::npos, source.find("IproDispositionFor(", handler))
      << "the in-place seam no longer consults the substrate disposition";
  EXPECT_NE(GoogleString::npos,
            source.find("MakeIproRecorderIfClassic(", handler))
      << "the in-place seam no longer builds its recorder through the gate";
  EXPECT_NE(GoogleString::npos,
            source.find("MakeDaemonIproRecorderIfReady(", handler))
      << "the in-place seam no longer records into the daemon's cache on the "
         "daemon substrate, so nothing would ever be optimized there";
  EXPECT_NE(GoogleString::npos,
            source.find("ServeFromDaemonSubstrate()", handler))
      << "the in-place seam no longer tries to SERVE from the daemon's cache, "
         "so the substrate would record for ever and hand nothing back";
}

TEST_F(DaemonSeamTest, BothArmsReadOneClientsHeadersThroughOneReader) {
  // The mask a request SELECTS with has to be the mask a recording of that
  // same request would NOTIFY at.  Two independent readings of one client's
  // headers would let the arms disagree about what the client can decode, and
  // the disagreement would be invisible -- each arm would look correct on its
  // own, and no behavioural assertion could reach it.  So the sharing is the
  // pin: exactly one function reads those four headers, and both builders
  // call it.
  const GoogleString source =
      ReadSource("pagespeed/apache/instaweb_handler.cc");
  EXPECT_NE(GoogleString::npos, source.find("void InstawebHandler::"
                                            "CapabilityHeaders("))
      << "the shared capability-header reader has been renamed or removed";

  const size_t record =
      source.find("InstawebHandler::BuildDaemonRecordRequest");
  const size_t serve = source.find("InstawebHandler::BuildDaemonServeRequest");
  ASSERT_NE(GoogleString::npos, record);
  ASSERT_NE(GoogleString::npos, serve);
  EXPECT_NE(GoogleString::npos, source.find("CapabilityHeaders(", record))
      << "the record side no longer reads the capability headers through the "
         "shared reader";
  EXPECT_NE(GoogleString::npos, source.find("CapabilityHeaders(", serve))
      << "the serve side no longer reads the capability headers through the "
         "shared reader";

  // And neither builder may go back to reading them itself.  Scoped to the
  // two builders rather than the file, because CapabilityHeaders itself is
  // where those accessors legitimately live.
  const size_t after_serve = source.find("\n}", serve);
  ASSERT_NE(GoogleString::npos, after_serve);
  const GoogleString builders = source.substr(record, after_serve - record);
  EXPECT_EQ(GoogleString::npos,
            builders.find("RequestHeader(HttpAttributes::kAcceptEncoding)"))
      << "a builder reads a capability header directly again; the two arms "
         "can now disagree about one client's capabilities";
}

TEST_F(DaemonSeamTest, TheDaemonSubstrateServesBeforeItRecords) {
  // Order, not merely presence. A request the shared cache can answer has
  // nothing to record, and recording it anyway re-stores an original the
  // daemon already holds -- once per request, per process, for every hot URL
  // on the server. The two calls are one argument apart in the source and no
  // unit test can reach the ordering, so it is pinned here.
  const GoogleString source =
      ReadSource("pagespeed/apache/instaweb_handler.cc");
  const size_t branch =
      source.find("if (disposition == IproDisposition::kDaemonSubstrate)");
  ASSERT_NE(GoogleString::npos, branch);
  const size_t serve = source.find("ServeFromDaemonSubstrate()", branch);
  const size_t record = source.find("MakeDaemonIproRecorderIfReady(", branch);
  ASSERT_NE(GoogleString::npos, serve);
  ASSERT_NE(GoogleString::npos, record);
  EXPECT_LT(serve, record)
      << "the daemon substrate records before it tries to serve";
}

TEST_F(DaemonSeamTest, AFallbackHitIsAnsweredWithOneWorkerRenotify) {
  // A fallback hit is SERVED, and a serve records nothing -- so without a
  // re-notify nothing would ever ask the worker for the variant the client's
  // mask names, and the family would never converge on the viewport, density
  // or Save-Data axes.  The behavioural half -- exactly one NotifyWorker,
  // carrying the raw client mask, suppressed by the durable original's
  // kPsFlagOriginVariesAccept bit and by an unnameable option context -- is
  // covered where the behaviour lives (daemon_serve_arm_test.cc).  What is
  // pinned here is the WIRING: the seam consults the arm's detection, calls
  // the arm's helper after the successful serve, names the request's
  // configuration, and counts the send.
  const GoogleString source =
      ReadSource("pagespeed/apache/instaweb_handler.cc");
  const size_t serve = source.find(
      "bool InstawebHandler::"
      "ServeFromDaemonSubstrate()");
  ASSERT_NE(GoogleString::npos, serve)
      << "the daemon serve seam has been renamed; this pin needs updating";
  const size_t fn_end = source.find("\n}\n", serve);
  ASSERT_NE(GoogleString::npos, fn_end);
  const GoogleString body = source.substr(serve, fn_end - serve);

  const size_t served = body.find("ipro_daemon_served");
  ASSERT_NE(GoogleString::npos, served);
  EXPECT_NE(GoogleString::npos, body.find("decision.fallback_hit"))
      << "the re-notify is no longer conditioned on the arm's fallback "
         "detection";
  const size_t renotify = body.find("DaemonServeFallbackRenotify(");
  EXPECT_NE(GoogleString::npos, renotify)
      << "a fallback hit no longer asks the worker for the client's variant; "
         "the family cannot converge on the viewport, density or Save-Data "
         "axes";
  EXPECT_LT(served, renotify)
      << "the re-notify must follow the successful serve, not precede it";
  EXPECT_NE(GoogleString::npos, body.find("OptionContext::Compute"))
      << "the re-notify would go out without naming the request's "
         "configuration, so the worker would process it under the default "
         "context";
  EXPECT_NE(GoogleString::npos, body.find("ipro_daemon_fallback_notified"))
      << "the re-notify is no longer counted; the rollout gate has nothing "
         "to observe";
  EXPECT_NE(GoogleString::npos, body.find("ipro_daemon_fallback_notify_failed"))
      << "a failed re-notify send is silent; a dead notify socket is "
         "indistinguishable from a quiet converged family at rollout";

  // EXACTLY ONE send site in the whole port: a second call added next door
  // would double the asks per fallback hit, and no behavioural test could
  // see it from here.
  EXPECT_EQ(1, CountSubstring(source, "DaemonServeFallbackRenotify("))
      << "the fallback re-notify has more than one call site";
}

TEST_F(DaemonSeamTest, ALostOptimizedCopyIsAskedForOnceAfterTheServe) {
  // A stylesheet or script whose optimized copy has gone missing is served
  // as its stored original, and that serve records nothing and notifies
  // nothing -- so without this ask the URL stays unoptimized until its
  // stored original expires.  The behaviour (what counts as a loss, the
  // per-URL window, what the notification carries) is covered where it
  // lives, in daemon_serve_arm_test.cc.  What is pinned here is the WIRING:
  // the seam opts the reader in BEFORE the serve, asks AFTER the response
  // has been served and counted, names the request's configuration, and
  // counts the send.
  const GoogleString source =
      ReadSource("pagespeed/apache/instaweb_handler.cc");
  const size_t serve = source.find(
      "bool InstawebHandler::"
      "ServeFromDaemonSubstrate()");
  ASSERT_NE(GoogleString::npos, serve)
      << "the daemon serve seam has been renamed; this pin needs updating";
  const size_t fn_end = source.find("\n}\n", serve);
  ASSERT_NE(GoogleString::npos, fn_end);
  const GoogleString body = source.substr(serve, fn_end - serve);

  const size_t opt_in = body.find(
      "reader->set_heal_notify_limiter(ProcessDaemonHealNotifyLimiter());");
  ASSERT_NE(GoogleString::npos, opt_in)
      << "the reader is never given the limiter, so a lost copy is never "
         "recognised on this port";
  const size_t decided = body.find("reader->Serve(");
  ASSERT_NE(GoogleString::npos, decided);
  EXPECT_LT(opt_in, decided)
      << "the limiter must be set before the serve it applies to";

  const size_t served = body.find("ipro_daemon_served");
  ASSERT_NE(GoogleString::npos, served);
  const size_t gate = body.find("if (decision.lost_optimized_copy) {");
  ASSERT_NE(GoogleString::npos, gate)
      << "the ask is no longer conditioned on the arm's detection";
  const size_t ask = body.find("DaemonServeLostCopyNotify(");
  ASSERT_NE(GoogleString::npos, ask)
      << "a lost optimized copy is no longer asked for";
  EXPECT_LT(served, gate) << "the ask must follow the serve, not precede it";
  EXPECT_LT(gate, ask) << "the ask is not inside its gate";
  EXPECT_NE(GoogleString::npos, body.find("ipro_daemon_heal_notified"))
      << "the ask is no longer counted";
  EXPECT_NE(GoogleString::npos, body.find("ipro_daemon_heal_notify_failed"))
      << "a failed ask is silent";
  // The notification is computed under the request's own configuration:
  // one context computation per notification site in this function.
  EXPECT_EQ(3, CountSubstring(body, "OptionContext::Compute("))
      << "the refresh, fallback and lost-copy notifications each name the "
         "request's configuration exactly once";

  // EXACTLY ONE send site in the whole port.
  EXPECT_EQ(1, CountSubstring(source, "DaemonServeLostCopyNotify("))
      << "the lost-copy notification has more than one call site";
}

TEST_F(DaemonSeamTest, AnAgeExpiredFallThroughSendsTheOriginRefreshSentinel) {
  // An age-expired VARIANT fall-through is the one decline the worker cannot
  // heal from the plain path: the re-record's notification is dedup-skipped
  // as already processed, so the URL re-fetches from the origin on every
  // request, for ever.  The heal is the origin-refreshed sentinel.
  // The behavioural half -- exactly one NotifyWorker carrying the sentinel
  // mask, fire and forget, suppressed on an unflagged fall-through or an
  // unnameable option context, send outcomes moving the counter pair -- is
  // covered where the behaviour lives (daemon_serve_arm_test.cc).  What is
  // pinned here is the WIRING: the fall-through branch consults the arm's
  // marker, calls the arm's helper before returning to the plain path,
  // names the request's configuration, and counts the send.
  const GoogleString source =
      ReadSource("pagespeed/apache/instaweb_handler.cc");
  const size_t serve = source.find(
      "bool InstawebHandler::"
      "ServeFromDaemonSubstrate()");
  ASSERT_NE(GoogleString::npos, serve)
      << "the daemon serve seam has been renamed; this pin needs updating";
  const size_t fn_end = source.find("\n}\n", serve);
  ASSERT_NE(GoogleString::npos, fn_end);
  const GoogleString body = source.substr(serve, fn_end - serve);

  // The fall-through branch, and nothing past it: the sentinel is the
  // fall-through's business, not the serve's.
  const size_t fallthrough =
      body.find("decision.verdict == DaemonServeVerdict::kFallThrough");
  ASSERT_NE(GoogleString::npos, fallthrough);
  const size_t branch_end = body.find("return false;", fallthrough);
  ASSERT_NE(GoogleString::npos, branch_end);
  const GoogleString branch =
      body.substr(fallthrough, branch_end - fallthrough);

  EXPECT_NE(GoogleString::npos,
            branch.find("decision.stale_variant_expired_by_age"))
      << "the sentinel is no longer conditioned on the arm's expiry marker; "
         "without it every age-expired variant fall-through is a permanent "
         "origin-serving regression the counters call routine";
  const size_t notify = branch.find("DaemonServeOriginRefreshedNotify(");
  EXPECT_NE(GoogleString::npos, notify)
      << "an age-expired variant fall-through no longer sends the "
         "origin-refreshed sentinel; the worker's dedup swallows the "
         "re-record's plain notification and the URL never reconverges";
  EXPECT_NE(GoogleString::npos, branch.find("OptionContext::Compute"))
      << "the sentinel would go out without naming the request's "
         "configuration, so the worker would process it under the default "
         "context";
  EXPECT_NE(GoogleString::npos, branch.find("ipro_daemon_refresh_notified"))
      << "the sentinel is no longer counted; the rollout gate has nothing "
         "to observe";
  EXPECT_NE(GoogleString::npos,
            branch.find("ipro_daemon_refresh_notify_failed"))
      << "a failed sentinel send is silent; a dead notify socket is "
         "indistinguishable from a substrate with nothing to heal";

  // EXACTLY ONE send site in the whole port, for the same reason as the
  // re-notify: a second call would double the asks per flagged fall-through,
  // and no behavioural test could see it from here.
  EXPECT_EQ(1, CountSubstring(source, "DaemonServeOriginRefreshedNotify("))
      << "the origin-refreshed sentinel has more than one call site";
}

TEST_F(DaemonSeamTest, AnOptimizedSubstrateServeRecordsItsHit) {
  // In the module-serves topology the daemon only WRITES alternates and never
  // answers a request, so its serve_savings counters can move only if the
  // module records the serve.  The behavioural half -- the values
  // the peer's recorder is handed, and the absent-file treatment -- is covered
  // where the behaviour lives (daemon_serve_arm_test.cc).  What is pinned here
  // is the WIRING: the record call sits on the 200 leg of the substrate serve
  // (a 304 serves no body and counts no hit), after the response has gone out,
  // gated on the same three facts the peer's own front ends gate on.
  const GoogleString source =
      ReadSource("pagespeed/apache/instaweb_handler.cc");
  const size_t serve = source.find(
      "bool InstawebHandler::"
      "ServeFromDaemonSubstrate()");
  ASSERT_NE(GoogleString::npos, serve)
      << "the daemon serve seam has been renamed; this pin needs updating";
  const size_t fn_end = source.find("\n}\n", serve);
  ASSERT_NE(GoogleString::npos, fn_end);
  const GoogleString body = source.substr(serve, fn_end - serve);

  // The 200 leg, and nothing past it: the 304 leg emits no body and must
  // record no hit.
  const size_t sent = body.find(
      "send_out_headers_and_body(request_, response_headers, body, "
      "encoded_body);");
  ASSERT_NE(GoogleString::npos, sent);
  const GoogleString leg = body.substr(sent);

  EXPECT_NE(GoogleString::npos,
            leg.find("decision.serve_class == kPsServeClassOptimized"))
      << "the hit is no longer gated on the arm's optimized classification; "
         "origin-byte serves would be counted as optimized ones";
  EXPECT_NE(GoogleString::npos, leg.find("decision.worker_processed"))
      << "the hit is no longer gated on the worker-processed bit; serves of "
         "entries the optimizer never produced would be counted as savings";
  EXPECT_NE(GoogleString::npos, leg.find("decision.origin_content_length > 0"))
      << "the hit is no longer gated on a recorded origin length; the saving "
         "would be measured against an origin size the entry does not have";
  const size_t record = leg.find("RecordDaemonServeHit(decision)");
  EXPECT_NE(GoogleString::npos, record)
      << "the substrate's 200 leg no longer records the serve hit, so in the "
         "module-serves topology the daemon's serve_savings counters stay "
         "permanently zero";

  // EXACTLY ONE record site in the whole port, for the same reason as the
  // re-notify and the sentinel: a second call would double-count every serve,
  // and no behavioural test could see it from here.  Counted on the call's own
  // spelling, which the helper's definition does not share.
  EXPECT_EQ(1, CountSubstring(source, "RecordDaemonServeHit(decision)"))
      << "the serve-hit recorder has more than one call site";
}

TEST_F(DaemonSeamTest, TheServeHitNamesAHostTheVirtualHostVouchesFor) {
  // The optimizer attributes each recorded serve to the host the module
  // names.  That name is one this virtual host's configuration lists --
  // VouchedServeHost over the request's own server record -- never the
  // request's Host value on its own; the lookup keeps using the request's
  // host.  The rule is unit-tested in serve_host_names_test.cc and
  // apache_host_names_test.cc; which recorder is called is pinned in
  // daemon_serve_arm_test.cc.
  const GoogleString source =
      ReadSource("pagespeed/apache/instaweb_handler.cc");
  const size_t fn = source.find("void InstawebHandler::RecordDaemonServeHit(");
  ASSERT_NE(GoogleString::npos, fn)
      << "the serve-hit recorder has been renamed; this pin needs updating";
  const size_t fn_end = source.find("\n}\n", fn);
  ASSERT_NE(GoogleString::npos, fn_end);
  GoogleString body;
  for (const char c : source.substr(fn, fn_end - fn)) {
    if (!isspace(static_cast<unsigned char>(c))) body.push_back(c);
  }
  EXPECT_NE(GoogleString::npos,
            body.find("decision.stored_mask,VouchedServeHost(stripped_gurl_."
                      "Host(),ApacheConfiguredHostNames(request_->server,"
                      "server_context_->apache_factory()->main_server(),"
                      "server_context_->apache_factory()->"
                      "stated_server_names())));"))
      << "the recorded serve hit no longer names a host the virtual host's "
         "configuration vouches for";
  EXPECT_EQ(GoogleString::npos,
            body.find("decision.stored_mask,daemon_serve_host_)"))
      << "the recorded serve hit names the request's own host again";
}

TEST_F(DaemonSeamTest, ThePerHostAdminConsoleIsGivenItsOwnServeHost) {
  // A per-host console sees only the row of the optimizer's serve savings
  // that its own site records serves under -- the same derivation as the
  // serve hit above.  The whole-server call passes none.
  GoogleString flat;
  for (const char c : ReadSource("pagespeed/apache/instaweb_handler.cc")) {
    if (!isspace(static_cast<unsigned char>(c))) flat.push_back(c);
  }
  const size_t call =
      flat.find("server_context->AdminPage(false/*notglobal*/,");
  ASSERT_NE(GoogleString::npos, call)
      << "the per-host admin call has been reshaped; this pin needs updating";
  const size_t end = flat.find(";", call);
  ASSERT_NE(GoogleString::npos, end);
  EXPECT_NE(GoogleString::npos,
            flat.substr(call, end - call)
                .find("admin_fetch,request_body,VouchedServeHost(instaweb_"
                      "handler.stripped_gurl().Host(),ApacheConfiguredHostNames("
                      "request->server,server_context->apache_factory()->"
                      "main_server(),server_context->apache_factory()->"
                      "stated_server_names())))"))
      << "the per-host admin console is not given its own serve host";
}

TEST_F(DaemonSeamTest, StatedServerNamesAreReadOnceFromTheConfiguration) {
  // Whether a record's configuration states its ServerName is read from
  // httpd's parsed configuration once, after the configuration is read and
  // before anything is served, and kept on the factory; the serving and
  // admin paths only look records up.  The global tree is read in exactly
  // one place.
  GoogleString flat;
  for (const char c : ReadSource("pagespeed/apache/mod_instaweb.cc")) {
    if (!isspace(static_cast<unsigned char>(c))) flat.push_back(c);
  }
  const size_t post_config = flat.find("intpagespeed_post_config(");
  ASSERT_NE(GoogleString::npos, post_config)
      << "the post-config hook has been renamed; this pin needs updating";
  const size_t set = flat.find(
      "factory->set_stated_server_names(ApacheStatedServerNamesFromTree("
      "ap_conftree,server_list));");
  EXPECT_NE(GoogleString::npos, set)
      << "the stated ServerNames are not computed from the configuration";
  EXPECT_LT(post_config, set) << "they are not computed in the post-config "
                                 "hook";
  EXPECT_EQ(1, CountSubstring(flat, "ap_conftree"))
      << "the global configuration tree is read in more than one place";
  for (const char* other :
       {"pagespeed/apache/instaweb_handler.cc",
        "pagespeed/apache/apache_server_context.cc",
        "pagespeed/apache/apache_rewrite_driver_factory.cc"}) {
    EXPECT_EQ(0, CountSubstring(ReadSource(other), "ap_conftree")) << other;
  }
}

TEST_F(DaemonSeamTest, TheServeHitRecordsTheServedEntrysOwnClass) {
  // Which content classes serve_savings can move for on this port is
  // decided by what gets served, not by this seam: the recorder is handed
  // the decision's own class, and the gate in front of it names no class.
  const GoogleString source =
      ReadSource("pagespeed/apache/instaweb_handler.cc");

  const size_t helper = source.find(
      "void InstawebHandler::RecordDaemonServeHit(");
  ASSERT_NE(GoogleString::npos, helper)
      << "the serve-hit helper has been renamed; this pin needs updating";
  const size_t helper_end = source.find("\n}\n", helper);
  ASSERT_NE(GoogleString::npos, helper_end);
  const GoogleString helper_body = source.substr(helper, helper_end - helper);
  EXPECT_NE(GoogleString::npos, helper_body.find("decision.ps_content_type"))
      << "the recorder no longer receives the served entry's own class";

  const size_t serve = source.find(
      "bool InstawebHandler::ServeFromDaemonSubstrate()");
  ASSERT_NE(GoogleString::npos, serve);
  const size_t sent = source.find(
      "send_out_headers_and_body(request_, response_headers, body, "
      "encoded_body);",
      serve);
  ASSERT_NE(GoogleString::npos, sent);
  const size_t record = source.find("RecordDaemonServeHit(decision)", sent);
  ASSERT_NE(GoogleString::npos, record);
  const GoogleString gate = source.substr(sent, record - sent);
  EXPECT_EQ(GoogleString::npos, gate.find("ps_content_type"))
      << "the serve-hit gate now filters by content class; a served entry's "
         "class could then go unrecorded";
}

TEST_F(DaemonSeamTest, TheDaemonSubstrateDoesNotRewriteCachingHeaders) {
  // The substrate's whole safety claim is that it records and changes nothing
  // about the response. The filter that shortens a response's downstream
  // lifetime would falsify that -- it exists to cover a classic in-place
  // serve, which is not happening here -- and whether it is attached is one
  // argument at one call site that no unit test can reach.
  const GoogleString source =
      ReadSource("pagespeed/apache/instaweb_handler.cc");

  const size_t daemon_branch = source.find("MakeDaemonIproRecorderIfReady(");
  ASSERT_NE(GoogleString::npos, daemon_branch);
  const size_t attach = source.find("AttachInPlaceRecorder(", daemon_branch);
  ASSERT_NE(GoogleString::npos, attach)
      << "the daemon branch no longer attaches a recorder at all";
  EXPECT_NE(GoogleString::npos,
            source.substr(attach, 120).find("false /* rewrite_caching_headers"))
      << "the daemon substrate attaches the header-rewriting filter, so an "
         "`s-maxage` is bolted on downstream of a `Cache-Control` the peer "
         "already built from the origin state it stored -- a second, "
         "disagreeing answer to the freshness question, applied to every "
         "response the substrate serves correctly";

  // And the classic path still does, because there it is load-bearing.
  const size_t classic = source.find("MakeIproRecorderIfClassic(");
  ASSERT_NE(GoogleString::npos, classic);
  const size_t classic_attach = source.find("AttachInPlaceRecorder(", classic);
  ASSERT_NE(GoogleString::npos, classic_attach);
  EXPECT_NE(GoogleString::npos, source.substr(classic_attach, 120)
                                    .find("true /* rewrite_caching_headers"))
      << "the classic path stopped rewriting caching headers; that is a "
         "behaviour change to the shipped path, not to the new one";
}

TEST_F(DaemonSeamTest, TheSubstrateStatesVaryOnceForBothLegsOfTheResponse) {
  // The two legs of a substrate response -- the 200 and the 304 that
  // revalidates it -- used to compose `Vary` independently and disagreed on
  // the wire: the 200 carried the encoding token the compressor stamps on it
  // and the 304 carried none, so a cache updating its stored header fields
  // from the 304 (RFC 9111 4.3.4) was told the response varies on a NARROWER
  // set than the one it had keyed on. The fix is structural -- ONE
  // composition, ABOVE the branch that splits the legs -- and its whole
  // strength is the position of two lines that no unit test can execute.
  const GoogleString source =
      ReadSource("pagespeed/apache/instaweb_handler.cc");

  const size_t serve =
      source.find("bool InstawebHandler::ServeFromDaemonSubstrate(");
  ASSERT_NE(GoogleString::npos, serve);

  // EXACTLY ONE composition and EXACTLY ONE emission: a second of either is
  // how the two legs came to disagree in the first place.
  EXPECT_EQ(1, CountSubstring(source, "ServeVaryFieldValue("))
      << "the substrate composes `Vary` in more than one place";
  EXPECT_EQ(1, CountSubstring(source.substr(serve),
                              "response_headers.Add(HttpAttributes::kVary"))
      << "the substrate emits `Vary` from more than one place";

  // And the composition is ABOVE the leg split. Below it, one leg gets a
  // field the other does not, which is the defect restated.
  const size_t compose = source.find("ServeVaryFieldValue(", serve);
  const size_t split = source.find("if (decision.not_modified) {", serve);
  ASSERT_NE(GoogleString::npos, compose);
  ASSERT_NE(GoogleString::npos, split);
  EXPECT_LT(compose, split)
      << "`Vary` is composed inside one leg of the response, so the other "
         "leg cannot carry the same field";

  // A call site can also run and do nothing, and a scan for a leg token is
  // a BLACKLIST: it catches one spelling of the condition, in one span.
  // Five separate reconstructions of the defect passed the scanned-window
  // form of this case -- the leg hoisted into a renamed local above the
  // window, spelled as a status-code comparison, spelled off the request's
  // own status, folded into the predicate argument, and folded into the
  // media-type line one line above the window's start. So the wiring is
  // pinned as a WHITELIST instead: the exact text of the whole path, from
  // the media-type selection through the emission. Anything inserted
  // anywhere along it changes the text. The pin is brittle against
  // reformatting, and that failure is LOUD, which is the safe direction.
  // What it still cannot see: the whole span wrapped, unreindented, in a
  // leg condition -- that shape preserves the text and is left to the
  // format gate, which does not tolerate an unindented block.
  const GoogleString kWiring =
      "  const char* compressor_media_type =\n"
      "      ServeCompressorMediaType(decision, request_->content_type);\n"
      "  const GoogleString vary = ServeVaryFieldValue(\n"
      "      decision, StreamingPagespeedResourceFetch::"
      "IsCompressibleContentType(\n"
      "                    compressor_media_type));\n"
      "  if (!vary.empty()) {\n"
      "    response_headers.Add(HttpAttributes::kVary, vary);\n"
      "  }\n";
  EXPECT_EQ(1, CountSubstring(source, kWiring))
      << "the wiring from the media-type selection to the `Vary` emission is "
         "not the pinned text -- a condition, a substitution, or a reformat "
         "has been introduced along the one path both legs share";
  EXPECT_NE(GoogleString::npos, source.find(kWiring, serve))
      << "the pinned wiring is not inside ServeFromDaemonSubstrate";

  // And the decision cannot be edited on its way there either.
  EXPECT_NE(GoogleString::npos,
            source.find("const DaemonServeDecision decision =", serve))
      << "the substrate's decision is mutable, so a leg field can be "
         "rewritten before the `Vary` composition reads it";
}

TEST_F(DaemonSeamTest, TheCompressorSelectionChangesWhetherItIsAttached) {
  // The arm's own case pins WHICH string the selection returns; this one
  // pins that the difference MATTERS -- that the two sides fall on opposite
  // sides of the predicate the seam feeds the answer to, which is the whole
  // reason the selection exists. It lives here because the predicate is the
  // Apache port's.
  DaemonServeDecision decision;
  decision.content_type = "image/avif";

  // A converted image is not handed to the compressor; the stylesheet the
  // URL maps to would have been. Reading the inherited type here would state
  // the encoding axis for a response the compressor never sees.
  EXPECT_FALSE(StreamingPagespeedResourceFetch::IsCompressibleContentType(
      ServeCompressorMediaType(decision, "text/css")));
  EXPECT_TRUE(
      StreamingPagespeedResourceFetch::IsCompressibleContentType("text/css"));

  // And the other direction: the decision's type IS handed to the
  // compressor where the URL's is not.
  decision.content_type = "text/css";
  EXPECT_TRUE(StreamingPagespeedResourceFetch::IsCompressibleContentType(
      ServeCompressorMediaType(decision, "image/png")));
  EXPECT_FALSE(
      StreamingPagespeedResourceFetch::IsCompressibleContentType("image/png"));
}

TEST_F(DaemonSeamTest, TheDaemonSubstrateDeclinesHeadRequests) {
  // A HEAD has no body to record. The classic path guards on the same thing;
  // without it the daemon branch buffers nothing and then stores an empty
  // body as the resource's durable original.
  const GoogleString source =
      ReadSource("pagespeed/apache/instaweb_handler.cc");
  const size_t daemon_branch =
      source.find("if (disposition == IproDisposition::kDaemonSubstrate)");
  ASSERT_NE(GoogleString::npos, daemon_branch);
  const size_t build = source.find("BuildDaemonRecordRequest(", daemon_branch);
  ASSERT_NE(GoogleString::npos, build);
  const GoogleString between =
      source.substr(daemon_branch, build - daemon_branch);
  EXPECT_NE(GoogleString::npos, between.find("request_->header_only"))
      << "the daemon branch records HEAD requests";
}

TEST_F(DaemonSeamTest, EverySubstrateOutcomeIsCounted) {
  // The two daemon counters are documented as PARTITIONING this substrate's
  // eligible requests, and a partition with a hole is worse than no claim: a
  // process that reached the substrate branch and could not open the volume
  // used to return before either counter moved, so a dead volume was
  // indistinguishable from no traffic at all, and the peer's skew serve-class
  // was unreachable from production while a unit test covered it.
  const GoogleString source =
      ReadSource("pagespeed/apache/instaweb_handler.cc");
  const size_t serve = source.find(
      "bool InstawebHandler::"
      "ServeFromDaemonSubstrate()");
  ASSERT_NE(GoogleString::npos, serve);
  const size_t null_reader = source.find("if (reader == nullptr)", serve);
  ASSERT_NE(GoogleString::npos, null_reader)
      << "the null-reader early exit has moved; this pin needs updating";
  // Bounded by the branch's own closing brace rather than by a byte count: a
  // fixed window silently shrinks as the comment above the code grows, and a
  // pin that stops covering the line it is about fails for the wrong reason
  // (this one did, at 700 bytes, against a 728-byte offset).
  const size_t branch_end = source.find("\n  }", null_reader);
  ASSERT_NE(GoogleString::npos, branch_end);
  const GoogleString branch =
      source.substr(null_reader, branch_end - null_reader);
  EXPECT_NE(GoogleString::npos, branch.find("kPsServeClassOriginalSkew"))
      << "a substrate that could not be opened records no serve class, so the "
         "peer's partition silently loses those responses";
  EXPECT_NE(GoogleString::npos, branch.find("ipro_daemon_fallthrough"))
      << "a substrate that could not be opened moves neither counter, so the "
         "documented sum of served + fell-through is false";
}

TEST_F(DaemonSeamTest, TheSharedStatisticsGetAStartTimeOnce) {
  const GoogleString source = ReadSource(
      "pagespeed/system/system_rewrite_driver_factory.cc");
  const size_t set_call =
      source.find("GetUpDownCounter(RewriteStats::kProcessStartMs)");
  ASSERT_NE(GoogleString::npos, set_call)
      << "the statistics start-time gauge is no longer set where the shared "
         "segment is created";
  const size_t init_true = source.find("stats->Init(true, message_handler())");
  ASSERT_NE(GoogleString::npos, init_true);
  EXPECT_LT(init_true, set_call)
      << "the gauge must be set after the segment exists, still in the root "
         "process, before any child attaches";
  const size_t second_set = source.find(
      "GetUpDownCounter(RewriteStats::kProcessStartMs)", set_call + 1);
  EXPECT_EQ(GoogleString::npos, second_set)
      << "the gauge has more than one writer site";
}

TEST_F(DaemonSeamTest, TheServeLegMovesThePerClassSplit) {
  const GoogleString source =
      ReadSource("pagespeed/apache/instaweb_handler.cc");
  const size_t serve =
      source.find("InstawebHandler::ServeFromDaemonSubstrate");
  ASSERT_NE(GoogleString::npos, serve);
  const size_t fn_end = source.find("\n}\n", serve);
  ASSERT_NE(GoogleString::npos, fn_end);
  const GoogleString body = source.substr(serve, fn_end - serve);

  const size_t served = body.find("ipro_daemon_served()->Add(1)");
  ASSERT_NE(GoogleString::npos, served);
  EXPECT_NE(GoogleString::npos, body.find("RecordDaemonServedClass("))
      << "the per-class split of the serve counter no longer moves on the "
         "serve leg";

  // The record side hands the recorder the counters it moves per class.
  const size_t record = source.find("MakeDaemonIproRecorderIfReady(");
  ASSERT_NE(GoogleString::npos, record);
  const GoogleString call = source.substr(record, 400);
  EXPECT_NE(GoogleString::npos, call.find("rewrite_stats()"))
      << "the recorder no longer receives the shared counters, so the "
         "fall-through split has stopped moving";
}

TEST_F(DaemonSeamTest, TheApachePortConstructsNoDaemonRecorderOutsideTheGate) {
  // The daemon-side recorder gets the same single-owner treatment as the
  // classic one, and for the same reason: "not constructed" is the contract,
  // and a construction site added next door would satisfy every behavioural
  // test while quietly buffering a response body per request on a server
  // whose daemon is not usable.
  StdioFileSystem file_system;
  NullMessageHandler handler;
  const GoogleString dir = StrCat(GTestSrcDir(), "/pagespeed/apache");

  StringVector files;
  ASSERT_TRUE(file_system.ListContents(dir, &files, &handler)) << dir;

  int scanned = 0;
  for (const GoogleString& path : files) {
    if (!StringCaseEndsWith(path, ".cc")) {
      continue;
    }
    GoogleString source;
    ASSERT_TRUE(file_system.ReadFile(path.c_str(), &source, &handler)) << path;
    ++scanned;
    EXPECT_EQ(GoogleString::npos, source.find("new DaemonIproRecorder"))
        << path
        << " constructs a daemon recorder directly, bypassing the gate that "
           "is supposed to be the only construction site";
    // The serve arm gets the same single-owner treatment, and needs it for a
    // sharper reason than the recorder: a reader built without the health
    // check would classify every request as a cold cache and quietly count
    // the whole substrate as a miss.
    EXPECT_EQ(GoogleString::npos, source.find("new DaemonServeReader"))
        << path
        << " constructs a daemon serve reader directly, bypassing the gate "
           "that is supposed to be the only construction site";
  }
  EXPECT_GT(scanned, 10) << "only " << scanned
                         << " Apache source files were scanned; the listing "
                            "looks wrong, so this pin proved nothing";
}

TEST_F(DaemonSeamTest, TheApachePortConstructsNoRecorderOutsideTheGate) {
  // The gate is only the single owner of recorder construction if nothing in
  // this port builds one directly, so this reads EVERY .cc in the port rather
  // than the one file the seam happens to live in today -- a construction site
  // added next door is exactly the regression worth catching, and a one-file
  // check would have called that green.
  //
  // Scoped to the Apache port on purpose: the other ports keep their own
  // construction sites until their own substrate change lands, and this pin
  // must not silently claim otherwise.
  StdioFileSystem file_system;
  NullMessageHandler handler;
  const GoogleString dir = StrCat(GTestSrcDir(), "/pagespeed/apache");

  StringVector files;
  ASSERT_TRUE(file_system.ListContents(dir, &files, &handler)) << dir;

  int scanned = 0;
  for (const GoogleString& path : files) {
    if (!StringCaseEndsWith(path, ".cc")) {
      continue;
    }
    GoogleString source;
    ASSERT_TRUE(file_system.ReadFile(path.c_str(), &source, &handler)) << path;
    ++scanned;
    EXPECT_EQ(GoogleString::npos, source.find("new InPlaceResourceRecorder"))
        << path
        << " constructs a recorder directly, bypassing the gate that is "
           "supposed to be the only construction site";
  }
  // Fail closed: a listing that silently returned nothing would make every
  // assertion above vacuous.
  EXPECT_GT(scanned, 10) << "only " << scanned
                         << " Apache source files were scanned; the listing "
                            "looks wrong, so this pin proved nothing";
}

TEST_F(DaemonSeamTest, AnAlreadyCodedBodyIsNotHandedToTheCompressor) {
  // The compressor is attached to a 200 of a compressible type, exactly as
  // before -- unless the body already carries a content coding: a stored
  // compressed copy compressed again is bytes no client decodes.
  EXPECT_TRUE(InstawebHandler::AttachesCompressor(HttpStatus::kOK, "text/css",
                                                  false));
  EXPECT_TRUE(InstawebHandler::AttachesCompressor(
      HttpStatus::kOK, "application/javascript", false));
  EXPECT_FALSE(InstawebHandler::AttachesCompressor(HttpStatus::kOK,
                                                   "text/css", true));
  EXPECT_FALSE(InstawebHandler::AttachesCompressor(
      HttpStatus::kOK, "application/javascript", true));
  EXPECT_FALSE(InstawebHandler::AttachesCompressor(HttpStatus::kOK,
                                                   "image/svg+xml", true));
  // And the two old exclusions stand.
  EXPECT_FALSE(InstawebHandler::AttachesCompressor(HttpStatus::kNotModified,
                                                   "text/css", false));
  EXPECT_FALSE(InstawebHandler::AttachesCompressor(HttpStatus::kOK,
                                                   "image/png", false));
  EXPECT_FALSE(
      InstawebHandler::AttachesCompressor(HttpStatus::kOK, nullptr, false));
}

TEST_F(DaemonSeamTest, AStoredCodedCopyIsLabelledWithItsOwnCoding) {
  // The behavioural half -- which coding a decision names, read off the
  // stored entry -- is pinned where it is decided
  // (daemon_serve_arm_test.cc).  What is pinned here is that the seam emits
  // exactly that and nothing else: the label comes from the decision, it is
  // on the 200 leg only, no request header names it, and the same fact
  // keeps the compressor off the body.
  const GoogleString source =
      ReadSource("pagespeed/apache/instaweb_handler.cc");
  const size_t serve =
      source.find("bool InstawebHandler::ServeFromDaemonSubstrate()");
  ASSERT_NE(GoogleString::npos, serve);
  const size_t fn_end = source.find("\n}\n", serve);
  ASSERT_NE(GoogleString::npos, fn_end);
  const GoogleString body = source.substr(serve, fn_end - serve);

  EXPECT_NE(GoogleString::npos,
            body.find("const bool encoded_body = "
                      "!decision.content_encoding.empty();"))
      << "the coded-body flag is no longer the decision's own";

  // EXACTLY ONE label, from the decision, inside the 200-only block.
  EXPECT_EQ(1, CountSubstring(body, "HttpAttributes::kContentEncoding"))
      << "the substrate states Content-Encoding in more than one place";
  const size_t label =
      body.find("response_headers.Add(HttpAttributes::kContentEncoding,");
  ASSERT_NE(GoogleString::npos, label);
  EXPECT_NE(GoogleString::npos, body.find("decision.content_encoding);", label))
      << "the Content-Encoding value is not the decision's";
  const size_t two_hundred = body.find("if (!decision.not_modified) {");
  const size_t caching = body.find("response_headers.ComputeCaching();");
  ASSERT_NE(GoogleString::npos, two_hundred);
  ASSERT_NE(GoogleString::npos, caching);
  EXPECT_LT(two_hundred, label)
      << "Content-Encoding is stated outside the 200-only block";
  EXPECT_LT(label, caching)
      << "Content-Encoding is stated outside the 200-only block";
  // The coding is never read off the request in the serving function.
  EXPECT_EQ(GoogleString::npos, body.find("kAcceptEncoding"))
      << "the serving function reads the request's Accept-Encoding itself";

  // The send tells the compressor gate the body is coded, from one call.
  EXPECT_EQ(1, CountSubstring(body,
                              "send_out_headers_and_body(request_, "
                              "response_headers, body, encoded_body);"));

  // And the gate is the predicate, the only place DEFLATE is attached.
  const size_t sender =
      source.find("void InstawebHandler::send_out_headers_and_body(");
  ASSERT_NE(GoogleString::npos, sender);
  const size_t sender_end = source.find("\n}\n", sender);
  ASSERT_NE(GoogleString::npos, sender_end);
  const GoogleString sender_body = source.substr(sender, sender_end - sender);
  EXPECT_NE(GoogleString::npos, sender_body.find("AttachesCompressor("))
      << "the compressor is attached without asking the predicate";
  EXPECT_EQ(1, CountSubstring(sender_body, "ap_add_output_filter(\"DEFLATE\""));
  EXPECT_EQ(1, CountSubstring(source, "ap_add_output_filter(\"DEFLATE\""))
      << "a second DEFLATE attach site bypasses the predicate";
}

TEST_F(DaemonSeamTest, TheReaderTakesTheHostsStoredEncodingsSwitch) {
  // The operator's directive reaches the arm through exactly one call,
  // made before the selection, from this request's configuration.
  const GoogleString source =
      ReadSource("pagespeed/apache/instaweb_handler.cc");
  const size_t serve =
      source.find("bool InstawebHandler::ServeFromDaemonSubstrate()");
  ASSERT_NE(GoogleString::npos, serve);
  const size_t fn_end = source.find("\n}\n", serve);
  ASSERT_NE(GoogleString::npos, fn_end);
  const GoogleString body = source.substr(serve, fn_end - serve);
  const size_t set = body.find("reader->set_serve_stored_encodings(");
  ASSERT_NE(GoogleString::npos, set)
      << "the directive never reaches the serve arm";
  EXPECT_NE(GoogleString::npos,
            body.find("options_->daemon_serve_stored_encodings()", set))
      << "the switch is not taken from this request's configuration";
  const size_t selection = body.find("reader->Serve(");
  ASSERT_NE(GoogleString::npos, selection);
  EXPECT_LT(set, selection) << "the switch is set after the selection";
  EXPECT_EQ(1, CountSubstring(source, "set_serve_stored_encodings("));
}

}  // namespace

}  // namespace net_instaweb
