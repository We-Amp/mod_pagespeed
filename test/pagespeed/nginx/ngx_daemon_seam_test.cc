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

// The nginx port's in-place recorder seam.
//
// nginx builds the classic in-place recorder through the shared record gate
// and holds it by the IproRecorder interface, the same shape the Apache port
// already had. The behaviour is meant to be identical -- the same object,
// constructed at the same point with the same arguments, driven through the
// same calls -- so there is no behaviour to execute, and there can be no
// linkable unit test for the wiring either (nginx is a headers-only library
// in this build graph; see the comment at the top of
// test/pagespeed/nginx/BUILD). What can and must be pinned from here is that
// the seam exists and that the concrete construction is gone, the same idiom
// as test/pagespeed/apache/daemon_seam_test.cc.
//
// A source pin is weaker than an execution pin and is written knowing that:
// it catches the deletion or bypass of the seam, which is the realistic
// regression, and it does not catch a gate call made with wrong arguments.
// And StripWhitespace strips whitespace, not comments: every pin in this
// file matches comment text too, so a pin's token may not be named in a
// comment either.

#include <cctype>

#include "base/logging.h"
#include "pagespeed/kernel/base/file_system.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/stdio_file_system.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

const char kNgxPagespeedCc[] = "pagespeed/nginx/ngx_pagespeed.cc";
const char kNgxPagespeedH[] = "pagespeed/nginx/ngx_pagespeed.h";
const char kNgxServerContextCc[] = "pagespeed/nginx/ngx_server_context.cc";
const char kNgxFactoryCc[] = "pagespeed/nginx/ngx_rewrite_driver_factory.cc";
const char kNgxDaemonServeNotifyCc[] =
    "pagespeed/nginx/ngx_daemon_serve_notify.cc";

// A data file that will not read -- or reads empty -- is a broken pin, not a
// passing one.
GoogleString ReadSource(const GoogleString& relative_path) {
  StdioFileSystem file_system;
  NullMessageHandler handler;
  GoogleString source;
  const GoogleString path = StrCat(GTestSrcDir(), "/", relative_path);
  CHECK(file_system.ReadFile(path.c_str(), &source, &handler)) << path;
  CHECK(!source.empty()) << path;
  return source;
}

// Removes every whitespace byte -- including line breaks and indentation --
// so the result is identical no matter where clang-format puts a line break:
// the same call wrapped across several lines or written on one normalizes to
// the same string. An expected fragment here must itself contain no internal
// whitespace.
GoogleString StripWhitespace(const GoogleString& text) {
  GoogleString out;
  for (const char c : text) {
    if (!isspace(static_cast<unsigned char>(c))) {
      out.push_back(c);
    }
  }
  return out;
}

// Returns the position of the brace that closes the block opened at
// `open_pos` (which must index a '{'), or npos.  Counts braces only: good
// enough for blocks whose string literals and comments contain none, which
// is what it is used on.
size_t MatchingBrace(const GoogleString& text, size_t open_pos) {
  int depth = 0;
  for (size_t i = open_pos; i < text.size(); ++i) {
    if (text[i] == '{') {
      ++depth;
    } else if (text[i] == '}') {
      if (--depth == 0) {
        return i;
      }
    }
  }
  return GoogleString::npos;
}

class NgxDaemonSeamTest : public testing::Test {};

TEST_F(NgxDaemonSeamTest, TheRecorderIsBuiltThroughTheSharedGate) {
  const GoogleString cc = StripWhitespace(ReadSource(kNgxPagespeedCc));
  const size_t seam = cc.find("ps_in_place_check_header_filter(");
  ASSERT_NE(GoogleString::npos, seam)
      << "the in-place header filter has been renamed; this pin needs "
         "updating";
  // The assignment spelling, anchored to the seam: the bare call name could
  // match a comment or a moved call, and the seam is the function the pin is
  // about.
  EXPECT_NE(GoogleString::npos,
            cc.find("ctx->recorder=MakeIproRecorderIfClassic(", seam))
      << "the classic in-place recorder is no longer built through the "
         "record gate inside the in-place header filter";
}

TEST_F(NgxDaemonSeamTest, TheBodyFilterIsOnlyEngagedWithARecorder) {
  const GoogleString cc = StripWhitespace(ReadSource(kNgxPagespeedCc));
  const size_t seam = cc.find("ps_in_place_check_header_filter(");
  ASSERT_NE(GoogleString::npos, seam)
      << "the in-place header filter has been renamed; this pin needs "
         "updating";
  // A disposition that returns no recorder must not force the response body
  // into memory for a recorder that does not exist.
  const size_t guard = cc.find("if(ctx->recorder!=nullptr){", seam);
  EXPECT_NE(GoogleString::npos, guard)
      << "the in-place body filter is engaged without a null test on the "
         "recorder";
  EXPECT_NE(GoogleString::npos, cc.find("r->filter_need_in_memory=1;}", guard))
      << "the in-memory engagement is not inside the recorder null test";
}

TEST_F(NgxDaemonSeamTest, TheContextHoldsTheRecorderByItsInterface) {
  const GoogleString h = StripWhitespace(ReadSource(kNgxPagespeedH));
  EXPECT_NE(GoogleString::npos, h.find("IproRecorder*recorder;"))
      << "ps_request_ctx_t::recorder is no longer held as IproRecorder*";
  EXPECT_EQ(GoogleString::npos, h.find("InPlaceResourceRecorder*recorder;"))
      << "the context member is still the concrete type";
}

TEST_F(NgxDaemonSeamTest, TheNginxPortConstructsNoRecorderOutsideTheGate) {
  // The gate is only the single owner of recorder construction if nothing in
  // this port builds one directly, so this reads EVERY .cc in the port rather
  // than the one file the seam happens to live in today -- a construction
  // site added next door is exactly the regression worth catching, and a
  // one-file check would have called that green.
  //
  // Scoped to the nginx port on purpose: the other ports keep their own
  // construction sites until their own substrate change lands, and this pin
  // must not silently claim otherwise.
  StdioFileSystem file_system;
  NullMessageHandler handler;
  const GoogleString dir = StrCat(GTestSrcDir(), "/pagespeed/nginx");

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
        << " constructs an in-place recorder directly, bypassing the gate "
           "that is supposed to be the only construction site";
  }
  // Fail closed: a listing that silently returned nothing would make every
  // assertion above vacuous.
  EXPECT_GT(scanned, 10) << "only " << scanned
                         << " nginx source files were scanned; the listing "
                            "looks wrong, so this pin proved nothing";
}

TEST_F(NgxDaemonSeamTest, TheStartupCheckRunsInTheMasterBeforeRootInit) {
  const GoogleString cc = StripWhitespace(ReadSource(kNgxPagespeedCc));
  const size_t fn = cc.find("ngx_int_tps_init_module(");
  ASSERT_NE(GoogleString::npos, fn)
      << "ps_init_module was renamed; this pin needs updating";
  const size_t post_config = cc.find("PostConfig(", fn);
  ASSERT_NE(GoogleString::npos, post_config)
      << "ps_init_module no longer PostConfig()s the driver factory";
  const size_t check = cc.find("RunDaemonStartupCheck()", post_config);
  EXPECT_NE(GoogleString::npos, check)
      << "the master no longer resolves every server's daemon relationship "
         "before fork";
  const size_t root_init = cc.find("RootInit(", post_config);
  ASSERT_NE(GoogleString::npos, root_init)
      << "ps_init_module no longer RootInit()s the driver factory";
  EXPECT_LT(check, root_init)
      << "the daemon startup check must run before RootInit, not after";
  // A refusal has to fail the start -- on the initial cycle only (a reload
  // must never exit out of a live master).
  EXPECT_NE(GoogleString::npos,
            cc.find("if(!server_context->RunDaemonStartupCheck()){if(initial_"
                    "cycle){returnNGX_ERROR;}",
                    fn))
      << "a refused daemon startup check no longer fails the initial start";
}

TEST_F(NgxDaemonSeamTest, TheServeHitNamesAHostTheServerBlockVouchesFor) {
  // The optimizer attributes each recorded serve to the host the module
  // names: one the request's own server{} block lists (VouchedServeHost over
  // NgxConfiguredHostNames), never the request's Host value on its own.  The
  // rule is unit-tested in serve_host_names_test.cc, with nginx's "_",
  // "*.example.com", ".example.com" and "~regex" shapes.
  const GoogleString cc = StripWhitespace(ReadSource(kNgxPagespeedCc));
  const size_t fn = cc.find("ngx_int_tPsServeFromDaemonSubstrate(");
  ASSERT_NE(GoogleString::npos, fn)
      << "PsServeFromDaemonSubstrate was renamed; this pin needs updating";
  const size_t open = cc.find("{", fn);
  ASSERT_NE(GoogleString::npos, open);
  const size_t block_end = MatchingBrace(cc, open);
  ASSERT_NE(GoogleString::npos, block_end);
  const GoogleString body = cc.substr(fn, block_end - fn);
  EXPECT_NE(GoogleString::npos,
            body.find("decision.stored_mask,VouchedServeHost(serve_request."
                      "hostname,NgxConfiguredHostNames(r)));"))
      << "the recorded serve hit no longer names a host the server block's "
         "configuration vouches for";
  EXPECT_EQ(GoogleString::npos,
            body.find("decision.stored_mask,serve_request.hostname);"))
      << "the recorded serve hit names the request's own host again";
}

TEST_F(NgxDaemonSeamTest, TheConfiguredNamesAreTheServerBlocksServerNames) {
  // The names come from the server block's own configuration: server_name
  // (nginx's primary name) and every server_names entry.
  const GoogleString cc = StripWhitespace(ReadSource(kNgxPagespeedCc));
  const size_t fn = cc.find(
      "ConfiguredHostNamesNgxCollectConfiguredHostNames(constngx_http_core_"
      "srv_conf_t*cscf){");
  ASSERT_NE(GoogleString::npos, fn)
      << "NgxCollectConfiguredHostNames was renamed; this pin needs updating";
  const size_t open = cc.find("{", fn);
  ASSERT_NE(GoogleString::npos, open);
  const size_t block_end = MatchingBrace(cc, open);
  ASSERT_NE(GoogleString::npos, block_end);
  const GoogleString body = cc.substr(fn, block_end - fn);
  EXPECT_NE(GoogleString::npos,
            body.find("names.primary=str_to_string_piece(cscf->server_name)"
                      ".as_string();"))
      << "the primary name is not the server block's server_name";
  EXPECT_NE(GoogleString::npos, body.find("i<cscf->server_names.nelts"))
      << "not every server_names entry is passed";
  EXPECT_NE(GoogleString::npos,
            body.find("names.aliases.push_back(str_to_string_piece(server_"
                      "names[i].name).as_string());"))
      << "the server_names entries are not passed as configured";
}

TEST_F(NgxDaemonSeamTest, ARequestNeverReadsNginxsOwnServerNamesArray) {
  // nginx keeps a server block's server_names array in the configuration's
  // temporary pool and destroys that pool once the configuration is loaded.
  // A request that walks the array reads freed memory: the worker process
  // crashed on the admin pages that way.  So the names are copied while the
  // configuration is merged, per server block, and a request reads only the
  // copy held by its own server block's pagespeed configuration.
  const GoogleString cc = StripWhitespace(ReadSource(kNgxPagespeedCc));

  const size_t fn = cc.find(
      "ConfiguredHostNamesNgxConfiguredHostNames(ngx_http_request_t*r){");
  ASSERT_NE(GoogleString::npos, fn)
      << "NgxConfiguredHostNames was renamed; this pin needs updating";
  const size_t open = cc.find("{", fn);
  ASSERT_NE(GoogleString::npos, open);
  const size_t block_end = MatchingBrace(cc, open);
  ASSERT_NE(GoogleString::npos, block_end);
  const GoogleString body = cc.substr(fn, block_end - fn);
  EXPECT_EQ(GoogleString::npos, body.find("server_names"))
      << "a request reads nginx's server_names array, which is freed once "
         "the configuration is loaded";
  EXPECT_EQ(GoogleString::npos, body.find("ngx_http_core_module"))
      << "a request reads the core module's server configuration for its "
         "names again";
  EXPECT_NE(GoogleString::npos,
            body.find("constps_srv_conf_t*cfg_s=ps_get_srv_config(r);"))
      << "the names are not read from the request's own server block";
  EXPECT_NE(GoogleString::npos, body.find("return*cfg_s->host_names;"))
      << "a request is not given the copy made at configuration time";

  // The copy is made in the per-server-block merge, from that block's own
  // core configuration, and nowhere else is the array walked.
  const size_t merge = cc.find(
      "char*ps_merge_srv_conf(ngx_conf_t*cf,void*parent,void*child){");
  ASSERT_NE(GoogleString::npos, merge)
      << "ps_merge_srv_conf was reshaped; this pin needs updating";
  const size_t merge_open = cc.find("{", merge);
  ASSERT_NE(GoogleString::npos, merge_open);
  const size_t merge_end = MatchingBrace(cc, merge_open);
  ASSERT_NE(GoogleString::npos, merge_end);
  EXPECT_NE(GoogleString::npos,
            cc.substr(merge, merge_end - merge)
                .find("cfg_s->host_names=newConfiguredHostNames("
                      "NgxCollectConfiguredHostNames(static_cast<ngx_http_"
                      "core_srv_conf_t*>(ngx_http_conf_get_module_srv_conf("
                      "cf,ngx_http_core_module))));"))
      << "the names are not copied while the configuration is merged";
  size_t walks = 0;
  for (size_t at = cc.find("->server_names."); at != GoogleString::npos;
       at = cc.find("->server_names.", at + 1)) {
    ++walks;
  }
  EXPECT_EQ(2u, walks)
      << "nginx's server_names array is used outside the configuration-time "
         "collector (its elts and its nelts, once each)";
}

TEST_F(NgxDaemonSeamTest, TheAdminConsoleIsGivenItsOwnServeHost) {
  // A per-host console sees only its own site's row of the optimizer's
  // serve savings: the admin call passes the host this server block records
  // its serves under, derived exactly as for the serve hit.
  const GoogleString cc = StripWhitespace(ReadSource(kNgxPagespeedCc));
  const size_t call = cc.find(
      "cfg_s->server_context->AdminPage(response_category==RequestRouting::"
      "kGlobalAdmin,");
  ASSERT_NE(GoogleString::npos, call)
      << "the admin call has been reshaped; this pin needs updating";
  const size_t end = cc.find(";", call);
  ASSERT_NE(GoogleString::npos, end);
  EXPECT_NE(GoogleString::npos,
            cc.substr(call, end - call)
                .find("ctx->base_fetch,request_body,VouchedServeHost(url."
                      "Host(),NgxConfiguredHostNames(r)))"))
      << "the admin console is not given its own serve host";
}

TEST_F(NgxDaemonSeamTest, ARegularExpressionEntryIsNeverPassed) {
  // nginx stores "~pattern" entries without the "~"; a pattern made only of
  // host characters would otherwise read as an exact name.  The skip sits
  // under the same build condition as nginx's own regex field.
  const GoogleString cc = StripWhitespace(ReadSource(kNgxPagespeedCc));
  const size_t fn = cc.find(
      "ConfiguredHostNamesNgxCollectConfiguredHostNames(constngx_http_core_"
      "srv_conf_t*cscf){");
  ASSERT_NE(GoogleString::npos, fn)
      << "NgxCollectConfiguredHostNames was renamed; this pin needs updating";
  const size_t open = cc.find("{", fn);
  ASSERT_NE(GoogleString::npos, open);
  const size_t block_end = MatchingBrace(cc, open);
  ASSERT_NE(GoogleString::npos, block_end);
  const GoogleString body = cc.substr(fn, block_end - fn);
  const size_t skip = body.find(
      "#if(NGX_PCRE)if(server_names[i].regex!=nullptr){continue;}#endif");
  ASSERT_NE(GoogleString::npos, skip)
      << "regular-expression server_names entries are not skipped";
  EXPECT_LT(skip, body.find("names.aliases.push_back("))
      << "the skip does not come before the entry is passed";
}

TEST_F(NgxDaemonSeamTest, TheStartupCheckIsSkippedUnderAConfigTest) {
  // A config test must not touch the daemon: the probe open can create a
  // volume file, and Apache's post_config does not run under -t either.
  const GoogleString cc = StripWhitespace(ReadSource(kNgxPagespeedCc));
  const size_t fn = cc.find("ngx_int_tps_init_module(");
  ASSERT_NE(GoogleString::npos, fn)
      << "ps_init_module was renamed; this pin needs updating";
  const size_t guard = cc.find("if(!ngx_test_config){", fn);
  ASSERT_NE(GoogleString::npos, guard)
      << "the daemon startup check runs under a config test";
  // Containment, not just ordering: the check has to sit inside the block
  // the guard opens, so moving it out while leaving the guard's text in
  // place fails here.
  const GoogleString kGuard = "if(!ngx_test_config){";
  const size_t block_end = MatchingBrace(cc, guard + kGuard.size() - 1);
  ASSERT_NE(GoogleString::npos, block_end)
      << "the config-test guard's block does not close";
  const size_t check = cc.find("RunDaemonStartupCheck()", guard);
  ASSERT_NE(GoogleString::npos, check)
      << "the daemon startup check is gone from ps_init_module";
  EXPECT_LT(check, block_end)
      << "the check is not inside the config-test guard";
  // And nowhere else: a second call site outside the guard would run under
  // a config test again.
  EXPECT_EQ(1, CountSubstring(cc, "RunDaemonStartupCheck()"))
      << "the daemon startup check has more than one call site";
}

TEST_F(NgxDaemonSeamTest, ARefusalOnlyFailsTheInitialStart) {
  // A reload must never exit out of a live master and orphan its workers:
  // on a later cycle the refusal is still logged and the verdict stays
  // not-ready, but the configuration keeps running.
  const GoogleString cc = StripWhitespace(ReadSource(kNgxPagespeedCc));
  const size_t fn = cc.find("ngx_int_tps_init_module(");
  ASSERT_NE(GoogleString::npos, fn)
      << "ps_init_module was renamed; this pin needs updating";
  EXPECT_NE(GoogleString::npos,
            cc.find("constboolinitial_cycle=(cycle->old_cycle==nullptr||"
                    "cycle->old_cycle->conf_ctx==nullptr);",
                    fn))
      << "the initial-cycle test is gone";
  EXPECT_NE(GoogleString::npos,
            cc.find("if(!server_context->RunDaemonStartupCheck()){if(initial_"
                    "cycle){returnNGX_ERROR;}",
                    fn))
      << "a refusal can still exit out of a live master on a reload";
}

TEST_F(NgxDaemonSeamTest, TheWorkerOpensItsOwnVolumeHandleAfterFork) {
  const GoogleString cc = StripWhitespace(ReadSource(kNgxPagespeedCc));
  const size_t fn = cc.find("ngx_int_tps_init_child_process(");
  ASSERT_NE(GoogleString::npos, fn)
      << "ps_init_child_process was renamed; this pin needs updating";
  const size_t child_init = cc.find("ChildInit(", fn);
  ASSERT_NE(GoogleString::npos, child_init)
      << "ps_init_child_process no longer ChildInit()s the driver factory";
  const size_t record = cc.find("RecordCache()", child_init);
  EXPECT_NE(GoogleString::npos, record)
      << "the worker no longer opens its own daemon volume handle after "
         "fork; the first open takes a blocking file lock and starts "
         "background threads, so it cannot happen lazily inside a request";
  const size_t fn_end = cc.find("}//namespacenet_instaweb", fn);
  ASSERT_NE(GoogleString::npos, fn_end);
  EXPECT_LT(record, fn_end)
      << "the RecordCache() call is not inside ps_init_child_process";
}

TEST_F(NgxDaemonSeamTest, TheVolumeHandleOpensOnlyInAServingProcess) {
  // init_process also runs in nginx's cache manager and cache loader, which
  // serve no request; the volume handle belongs to a serving worker only.
  const GoogleString cc = StripWhitespace(ReadSource(kNgxPagespeedCc));
  const size_t fn = cc.find("ngx_int_tps_init_child_process(");
  ASSERT_NE(GoogleString::npos, fn)
      << "ps_init_child_process was renamed; this pin needs updating";
  EXPECT_NE(GoogleString::npos,
            cc.find("if((ngx_process==NGX_PROCESS_WORKER||ngx_process=="
                    "NGX_PROCESS_SINGLE)&&cfg_s->server_context->daemon_"
                    "health()==DaemonHealth::kReady){cfg_s->server_context->"
                    "daemon_adapter()->RecordCache();",
                    fn))
      << "RecordCache() is not gated to a serving process";
}

TEST_F(NgxDaemonSeamTest, TheMasterNeverOpensTheVolumeHandle) {
  const GoogleString cc = StripWhitespace(ReadSource(kNgxPagespeedCc));
  const size_t fn = cc.find("ngx_int_tps_init_module(");
  ASSERT_NE(GoogleString::npos, fn)
      << "ps_init_module was renamed; this pin needs updating";
  const size_t fn_end = cc.find(
      "std::vector<std::unique_ptr<webbotauth::KeyDirectoryWarmer>>"
      "g_key_directory_warmers;",
      fn);
  ASSERT_NE(GoogleString::npos, fn_end)
      << "the marker after ps_init_module moved; this pin needs updating";
  const GoogleString body = cc.substr(fn, fn_end - fn);
  EXPECT_EQ(GoogleString::npos, body.find("RecordCache("))
      << "the master opens the daemon volume handle; the first open must "
         "happen in a worker, after fork";
}

TEST_F(NgxDaemonSeamTest, TheRecordArmAsksForADispositionInTheResourceHandler) {
  const GoogleString cc = StripWhitespace(ReadSource(kNgxPagespeedCc));
  const size_t fn = cc.find("ps_resource_handler(ngx_http_request_t*r");
  ASSERT_NE(GoogleString::npos, fn)
      << "ps_resource_handler was renamed; this pin needs updating";
  const size_t fn_end = cc.find("voidps_send_to_pagespeed(", fn);
  ASSERT_NE(GoogleString::npos, fn_end)
      << "the function after ps_resource_handler moved; this pin needs "
         "updating";
  const GoogleString body = cc.substr(fn, fn_end - fn);
  EXPECT_NE(GoogleString::npos,
            body.find("IproDispositionFor(cfg_s->server_context->daemon_"
                      "health())"))
      << "the resource handler no longer asks the gate for this server's "
         "in-place disposition";
  EXPECT_NE(GoogleString::npos, body.find("ctx->daemon_record_pending=true;"))
      << "the daemon substrate no longer marks the request for the header "
         "filter to record";
}

TEST_F(NgxDaemonSeamTest, TheInPlaceBlockIsEnteredForGetAndHeadOnly) {
  // Both branches of the block (the classic recorder and the daemon mark)
  // sit behind this one condition, so pinning it pins both at once.  The
  // comment above the condition spells GET and HEAD in prose and carries no
  // NGX_HTTP_ token, so the spelling here is unambiguous.  The same
  // condition leaves internally redirected requests to the server.
  const GoogleString cc = StripWhitespace(ReadSource(kNgxPagespeedCc));
  const size_t fn = cc.find("ps_resource_handler(ngx_http_request_t*r");
  ASSERT_NE(GoogleString::npos, fn)
      << "ps_resource_handler was renamed; this pin needs updating";
  const size_t fn_end = cc.find("voidps_send_to_pagespeed(", fn);
  ASSERT_NE(GoogleString::npos, fn_end)
      << "the function after ps_resource_handler moved; this pin needs "
         "updating";
  const GoogleString body = cc.substr(fn, fn_end - fn);
  const GoogleString condition =
      "if(options->in_place_rewriting_enabled()&&options->"
      "enabled()&&options->IsAllowed(url.Spec())&&(r->method&"
      "(NGX_HTTP_GET|NGX_HTTP_HEAD))&&!r->internal){";
  const size_t block = body.find(condition);
  ASSERT_NE(GoogleString::npos, block)
      << "the in-place block admits more than GET and HEAD, or an "
         "internally redirected request";
  // Both branches really are inside it: the daemon record mark and the
  // classic lookup sit between the block's braces.
  const size_t open = block + condition.size() - 1;
  const size_t close = MatchingBrace(body, open);
  ASSERT_NE(GoogleString::npos, close);
  const GoogleString in_block = body.substr(open, close - open);
  EXPECT_NE(GoogleString::npos,
            in_block.find("ctx->daemon_record_pending=true;"))
      << "the daemon record mark moved out of the in-place block";
  EXPECT_NE(GoogleString::npos, in_block.find("FetchInPlaceResource("))
      << "the classic in-place lookup moved out of the in-place block";
  EXPECT_EQ(GoogleString::npos,
            body.find("ctx->daemon_record_pending=true;", close))
      << "a second daemon record mark exists outside the in-place block";
}

TEST_F(NgxDaemonSeamTest, AnInPlaceLookupMissKeepsTheRequestsHeaders) {
  // The pass that reports an in-place lookup miss only decides: the server
  // answers the request itself, so response headers already set on the
  // request must survive it.  Pinned as the shape of the branch: the miss
  // case comes first and is empty, and the clean sits in a later arm.
  const GoogleString cc = StripWhitespace(ReadSource(kNgxPagespeedCc));
  const size_t fn = cc.find("ngx_int_tps_base_fetch_handler(");
  ASSERT_NE(GoogleString::npos, fn)
      << "ps_base_fetch_handler was renamed; this pin needs updating";
  const size_t miss = cc.find(
      "if(ctx->base_fetch->base_fetch_type()==kIproLookup&&!status_ok){", fn);
  ASSERT_NE(GoogleString::npos, miss)
      << "the in-place lookup-miss pass is no longer singled out";
  const size_t next_arm = cc.find(
      "}elseif(ctx->preserve_caching_headers!=kDontPreserveHeaders){", miss);
  ASSERT_NE(GoogleString::npos, next_arm)
      << "the arm after the lookup-miss case moved; this pin needs updating";
  const GoogleString miss_arm = cc.substr(miss, next_arm - miss);
  EXPECT_EQ(GoogleString::npos, miss_arm.find("ngx_http_clean_header"))
      << "the in-place lookup-miss pass cleans the request's response "
         "headers";
  EXPECT_EQ(GoogleString::npos, miss_arm.find("hash=0"))
      << "the in-place lookup-miss pass drops response headers";
}

TEST_F(NgxDaemonSeamTest, TheDaemonMarkSkipsHeadRequests) {
  // The method is the HEAD guard, tested where the mark is set: nginx only
  // sets r->header_only in its core header filter, downstream of this
  // module's, so a flag test at the module's own filter position can never
  // see a HEAD.  A HEAD has no body to record.
  const GoogleString cc = StripWhitespace(ReadSource(kNgxPagespeedCc));
  const size_t fn = cc.find("ps_resource_handler(ngx_http_request_t*r");
  ASSERT_NE(GoogleString::npos, fn)
      << "ps_resource_handler was renamed; this pin needs updating";
  const size_t fn_end = cc.find("voidps_send_to_pagespeed(", fn);
  ASSERT_NE(GoogleString::npos, fn_end);
  const GoogleString body = cc.substr(fn, fn_end - fn);
  EXPECT_NE(GoogleString::npos, body.find("!(r->method&NGX_HTTP_HEAD)){"
                                          "ctx->daemon_record_pending=true;"))
      << "the daemon record mark is set for a HEAD; the guard must be the "
         "method, not r->header_only (set downstream of this module's "
         "filter)";
}

TEST_F(NgxDaemonSeamTest, TheRecordKeyIsTheModulesOwnUrl) {
  // The record key must be the URL the rest of the module agreed on:
  // PageSpeed control parameters stripped, X-Forwarded-Proto honoured.
  // That is ctx->url_string, which the classic recorder already keys on --
  // and which the serve arm will key on.  Pinned inside the daemon branch
  // of the header filter, bounded by its own braces, because the classic
  // part of the same function legitimately calls ps_determine_url.
  const GoogleString cc = StripWhitespace(ReadSource(kNgxPagespeedCc));
  const size_t branch = cc.find("if(ctx->daemon_record_pending){");
  ASSERT_NE(GoogleString::npos, branch)
      << "the daemon branch of the in-place header filter moved; this pin "
         "needs updating";
  const size_t open = cc.find("{", branch + 1);
  ASSERT_NE(GoogleString::npos, open);
  const size_t block_end = MatchingBrace(cc, open);
  ASSERT_NE(GoogleString::npos, block_end)
      << "the daemon branch's block does not close";
  const GoogleString body = cc.substr(branch, block_end - branch);
  EXPECT_NE(GoogleString::npos,
            body.find("GoogleUrlfull_url(ctx->url_string);"))
      << "the record key is not built from ctx->url_string";
  EXPECT_EQ(GoogleString::npos, body.find("ps_determine_url("))
      << "the record key is built from the raw URL again: PageSpeed "
         "parameters unstripped, X-Forwarded-Proto ignored";
}

TEST_F(NgxDaemonSeamTest, TheDaemonRecorderIsBuiltThroughTheOpenGate) {
  // MakeDaemonIproRecorderIfOpen is the only construction spelling this
  // thread may use: it never opens the volume handle and never waits.  The
  // ...IfReady variant performs the blocking open on the calling thread and
  // must not appear anywhere in this port.
  const GoogleString cc = StripWhitespace(ReadSource(kNgxPagespeedCc));
  const size_t fn = cc.find(
      "ps_in_place_check_header_filter(ngx_http_"
      "request_t*r");
  ASSERT_NE(GoogleString::npos, fn)
      << "the in-place header filter was renamed; this pin needs updating";
  const size_t fn_end =
      cc.find("ps_in_place_body_filter(ngx_http_request_t*", fn);
  ASSERT_NE(GoogleString::npos, fn_end)
      << "the in-place body filter moved; this pin needs updating";
  const GoogleString body = cc.substr(fn, fn_end - fn);
  EXPECT_NE(GoogleString::npos,
            body.find("MakeDaemonIproRecorderIfOpen(server_context->daemon_"
                      "adapter(),"))
      << "the daemon recorder is no longer built through the non-blocking "
         "gate inside the in-place header filter";
  EXPECT_EQ(GoogleString::npos, cc.find("MakeDaemonIproRecorderIfReady("))
      << "the blocking recorder gate appeared in ngx_pagespeed.cc; the event "
         "thread must never open the volume handle";
}

TEST_F(NgxDaemonSeamTest, TheRequestPropertiesCountNginxBasicAuth) {
  // The daemon shares its cache across virtual hosts, so an authenticated
  // response must not be recorded as an anonymous one.  NOTE, kept honest:
  // on nginx today this pin guards a line with NO behavioural effect --
  // headers_in.user is only ever populated from a decodable Authorization:
  // Basic header, which the header test already covers -- so this pin can
  // never go RED for a real authorization regression.  The line stays as
  // the counterpart of Apache's request_->user in case a module ever
  // populates the field otherwise.
  const GoogleString cc = StripWhitespace(ReadSource(kNgxPagespeedCc));
  const size_t fn = cc.find(
      "ps_in_place_check_header_filter(ngx_http_"
      "request_t*r");
  ASSERT_NE(GoogleString::npos, fn)
      << "the in-place header filter was renamed; this pin needs updating";
  const size_t fn_end =
      cc.find("ps_in_place_body_filter(ngx_http_request_t*", fn);
  ASSERT_NE(GoogleString::npos, fn_end);
  const GoogleString body = cc.substr(fn, fn_end - fn);
  EXPECT_NE(GoogleString::npos, body.find("r->headers_in.user.len>0"))
      << "a basic-authenticated request without an Authorization header is "
         "no longer counted as authorized";
}

TEST_F(NgxDaemonSeamTest, TheFiltersNeverOpenTheVolumeHandle) {
  // RecordCache() performs the blocking open.  It belongs to worker init and
  // to the off-thread open attempt, never to a request filter; the latch and
  // the poster are the only open-adjacent calls the header filter may make.
  const GoogleString cc = StripWhitespace(ReadSource(kNgxPagespeedCc));
  const size_t header_fn = cc.find(
      "ps_in_place_check_header_filter(ngx_http_"
      "request_t*r");
  ASSERT_NE(GoogleString::npos, header_fn);
  const size_t body_fn =
      cc.find("ps_in_place_body_filter(ngx_http_request_t*", header_fn);
  ASSERT_NE(GoogleString::npos, body_fn);
  const size_t filters_end = cc.find("voidps_in_place_filter_init(", body_fn);
  ASSERT_NE(GoogleString::npos, filters_end);
  const GoogleString both = cc.substr(header_fn, filters_end - header_fn);
  EXPECT_EQ(GoogleString::npos, both.find("RecordCache("))
      << "a request filter opens the daemon volume handle inline";
  const GoogleString header_body = cc.substr(header_fn, body_fn - header_fn);
  EXPECT_NE(GoogleString::npos,
            header_body.find("ShouldPostDaemonOpenAttempt()"))
      << "the open attempt is posted without the time latch";
  EXPECT_NE(GoogleString::npos,
            header_body.find("PostDaemonOpenAttempt(g_daemon_record_sequence,"))
      << "the header filter no longer posts the off-thread open attempt";
}

TEST_F(NgxDaemonSeamTest, TheDaemonCompletionRunsOffTheEventThread) {
  const GoogleString cc = StripWhitespace(ReadSource(kNgxPagespeedCc));
  const size_t fn = cc.find("ps_in_place_body_filter(ngx_http_request_t*r");
  ASSERT_NE(GoogleString::npos, fn)
      << "the in-place body filter was renamed; this pin needs updating";
  const size_t fn_end = cc.find("voidps_in_place_filter_init(", fn);
  ASSERT_NE(GoogleString::npos, fn_end)
      << "the filter init moved; this pin needs updating";
  const GoogleString body = cc.substr(fn, fn_end - fn);
  EXPECT_NE(GoogleString::npos,
            body.find("TryQueueDaemonCompletion(body_bytes,dropped)"))
      << "daemon completions are posted without the queue caps";
  EXPECT_NE(GoogleString::npos,
            body.find("->FindVariable(kIproDaemonRecordDropped)"))
      << "the drop counter is not looked up null-tolerantly from the "
         "request's own statistics object; GetVariable's CHECK would abort "
         "the worker if the counter were ever absent";
  EXPECT_NE(GoogleString::npos, body.find("std::min(ctx->daemon_body_bytes,"))
      << "the queue reservation is not clamped at the recorder's own "
         "content cap, so a response past it over-counts against the byte "
         "cap and inflates the drop counter";
  EXPECT_NE(GoogleString::npos,
            body.find("g_daemon_record_sequence->Add(newDaemonRecordCompletion("
                      "recorder,headers,"))
      << "the daemon completion is no longer handed to the pool sequence";
  // The request disengages the recorder at once: the closure owns it from
  // the Add on, and a later buffer pass must not find it on the context.
  EXPECT_NE(GoogleString::npos,
            body.find("ctx->recorder=nullptr;ctx->daemon_recorder=false;"))
      << "the context still points at a recorder the closure owns";
  // The classic completion stays inline.
  EXPECT_NE(GoogleString::npos,
            body.find("ctx->recorder->DoneAndSetHeaders(&response_headers,"))
      << "the classic recorder's inline completion is gone";
}

TEST_F(NgxDaemonSeamTest, TheSmaxageRewriteSkipsTheDaemonSubstrate) {
  const GoogleString cc = StripWhitespace(ReadSource(kNgxPagespeedCc));
  const size_t fn = cc.find("ps_etag_header_filter(ngx_http_request_t*r");
  ASSERT_NE(GoogleString::npos, fn)
      << "ps_etag_header_filter was renamed; this pin needs updating";
  const size_t fn_end = cc.find("staticngx_int_tps_read_file_buffer(", fn);
  ASSERT_NE(GoogleString::npos, fn_end)
      << "the function after ps_etag_header_filter moved; this pin needs "
         "updating";
  const GoogleString body = cc.substr(fn, fn_end - fn);
  EXPECT_NE(GoogleString::npos,
            body.find("if(ctx&&ctx->recorder&&!ctx->daemon_recorder){"))
      << "the s-maxage rewrite applies to a daemon-side recorder; on that "
         "substrate the response served is the origin's own";
}

TEST_F(NgxDaemonSeamTest, TheCompletionPoolIsOwnedByTheRecordArm) {
  // The record arm drains its completions on its OWN one-worker pool, not
  // the factory's low-priority rewrite pool: a commit stalled in the daemon
  // must never starve an image rewrite.  The pool exists only where a
  // serving process has at least one recordable server.
  const GoogleString cc = StripWhitespace(ReadSource(kNgxPagespeedCc));
  const size_t fn = cc.find("ngx_int_tps_init_child_process(");
  ASSERT_NE(GoogleString::npos, fn)
      << "ps_init_child_process was renamed; this pin needs updating";
  const size_t fn_end = cc.find("}//namespacenet_instaweb", fn);
  ASSERT_NE(GoogleString::npos, fn_end);
  const GoogleString body = cc.substr(fn, fn_end - fn);
  EXPECT_NE(GoogleString::npos, body.find("newQueuedWorkerPool(1,"))
      << "the record arm's own one-worker pool is gone";
  EXPECT_NE(GoogleString::npos,
            body.find("g_daemon_record_sequence=g_daemon_record_pool->"
                      "NewSequence();"))
      << "the completion sequence is not taken from the record arm's own "
         "pool";
  EXPECT_NE(GoogleString::npos,
            body.find("if(any_daemon_ready){g_daemon_record_pool="))
      << "the pool is created unconditionally; it must exist only for a "
         "serving process with at least one recordable server";
  // The gate itself, not only its spelling: the flag starts false and is set
  // only under the serving-process-and-ready condition.
  EXPECT_NE(GoogleString::npos, body.find("boolany_daemon_ready=false;"))
      << "the pool gate no longer starts closed";
  EXPECT_NE(GoogleString::npos,
            body.find("DaemonHealth::kReady){cfg_s->server_context->"
                      "daemon_adapter()->RecordCache();any_daemon_ready="
                      "true;}"))
      << "the pool gate is no longer set under the serving-process, "
         "ready-verdict condition";
  EXPECT_EQ(1, CountSubstring(body, "any_daemon_ready=true;"))
      << "the pool gate is set in more than one place";
  EXPECT_EQ(GoogleString::npos, body.find("kLowPriorityRewriteWorkers"))
      << "the completion sequence still comes from the factory's "
         "low-priority rewrite pool, where a stalled commit starves image "
         "rewrites";
}

TEST_F(NgxDaemonSeamTest, TheRecordPoolShutsDownBeforeTheVolumeClose) {
  // The pool is port-owned, so the factory never shuts it down: the exit
  // hook does, after the factory's ShutDown (no PSOL thread may still touch
  // an adapter) and before the volume-close loop (no completion may still
  // hold a handle).  All three positions are found from the same anchor.
  const GoogleString cc = StripWhitespace(ReadSource(kNgxPagespeedCc));
  const size_t fn = cc.find("voidps_exit_child_process(");
  ASSERT_NE(GoogleString::npos, fn)
      << "ps_exit_child_process was renamed; this pin needs updating";
  const size_t open = cc.find("{", fn);
  ASSERT_NE(GoogleString::npos, open);
  const size_t block_end = MatchingBrace(cc, open);
  ASSERT_NE(GoogleString::npos, block_end);
  // Everything below is searched inside the function's own block, so a
  // second occurrence later in the file cannot re-anchor the ordering.
  const GoogleString body = cc.substr(open, block_end - open);
  const size_t factory_down = body.find("cfg_m->driver_factory->ShutDown();");
  ASSERT_NE(GoogleString::npos, factory_down)
      << "ps_exit_child_process no longer shuts the driver factory down";
  const size_t pool_down = body.find("g_daemon_record_pool->ShutDown();");
  ASSERT_NE(GoogleString::npos, pool_down)
      << "the record arm's pool is not shut down in the exit hook";
  const size_t pool_gone = body.find("deleteg_daemon_record_pool;");
  ASSERT_NE(GoogleString::npos, pool_gone)
      << "the record arm's pool is not deleted in the exit hook";
  const size_t close = body.find("daemon_adapter()->CloseRecordCache();");
  ASSERT_NE(GoogleString::npos, close);
  EXPECT_LT(factory_down, pool_down)
      << "the record pool shuts down before the factory's pools are joined";
  EXPECT_LT(pool_down, close)
      << "the record pool shuts down after the volume handles close";
  EXPECT_LT(pool_down, pool_gone);
  EXPECT_LT(pool_gone, close)
      << "the record pool is deleted after the volume handles close";
}

TEST_F(NgxDaemonSeamTest, TheDropCounterIsRegisteredThroughInitStats) {
  // The counter must be registered inside the factory's InitStats, which
  // runs for EVERY statistics object before that object is initialised.
  // Any later registration -- ps_init_module included -- adds a variable to
  // an already-frozen shared-memory segment, which is fatal at worker init.
  const GoogleString factory = StripWhitespace(ReadSource(kNgxFactoryCc));
  const size_t fn = factory.find("voidNgxRewriteDriverFactory::InitStats(");
  ASSERT_NE(GoogleString::npos, fn)
      << "NgxRewriteDriverFactory::InitStats was renamed; this pin needs "
         "updating";
  const size_t open = factory.find("{", fn);
  ASSERT_NE(GoogleString::npos, open);
  const size_t block_end = MatchingBrace(factory, open);
  ASSERT_NE(GoogleString::npos, block_end)
      << "InitStats's block does not close";
  const size_t reg =
      factory.find("ps_daemon_record_completion_init_stats(statistics);", fn);
  ASSERT_NE(GoogleString::npos, reg)
      << "the daemon record drop counter is not registered in the factory's "
         "InitStats";
  EXPECT_LT(reg, block_end) << "the registration is not inside InitStats";
  EXPECT_EQ(1,
            CountSubstring(factory, "ps_daemon_record_completion_init_stats("))
      << "the drop counter has more than one registration call site";
  // And nowhere downstream: a registration in ps_init_module runs after the
  // shared segment is frozen.
  const GoogleString cc = StripWhitespace(ReadSource(kNgxPagespeedCc));
  EXPECT_EQ(GoogleString::npos,
            cc.find("ps_daemon_record_completion_init_stats"))
      << "the drop counter is registered from the module, after the "
         "statistics segment is frozen";
}

TEST_F(NgxDaemonSeamTest, TheNginxPortNeverUsesTheBlockingRecorderGate) {
  // Scanned across every .cc in the port, not just the file the seam lives
  // in today: a call added next door blocks the event thread exactly the
  // same.  See TheNginxPortConstructsNoRecorderOutsideTheGate for the idiom.
  StdioFileSystem file_system;
  NullMessageHandler handler;
  const GoogleString dir = StrCat(GTestSrcDir(), "/pagespeed/nginx");

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
    EXPECT_EQ(GoogleString::npos, source.find("MakeDaemonIproRecorderIfReady"))
        << path
        << " uses the blocking recorder gate; the event thread must build "
           "through MakeDaemonIproRecorderIfOpen only";
  }
  EXPECT_GT(scanned, 10) << "only " << scanned
                         << " nginx source files were scanned; the listing "
                            "looks wrong, so this pin proved nothing";
}

TEST_F(NgxDaemonSeamTest, TheNginxPortNeverUsesTheBlockingServeFactory) {
  // The serve half of the same rule: MakeDaemonServeReaderIfReady obtains
  // the handle through RecordCache(), which performs the blocking open on
  // the calling thread.  It must not be spelled anywhere under
  // pagespeed/nginx/ -- the event thread builds through
  // MakeDaemonServeReaderIfOpen only.  Directory scan of every .cc AND .h
  // in the port (a header declares inline code too), same idiom as the
  // recorder gate pin.
  StdioFileSystem file_system;
  NullMessageHandler handler;
  const GoogleString dir = StrCat(GTestSrcDir(), "/pagespeed/nginx");

  StringVector files;
  ASSERT_TRUE(file_system.ListContents(dir, &files, &handler)) << dir;

  int scanned = 0;
  for (const GoogleString& path : files) {
    if (!StringCaseEndsWith(path, ".cc") && !StringCaseEndsWith(path, ".h")) {
      continue;
    }
    GoogleString source;
    ASSERT_TRUE(file_system.ReadFile(path.c_str(), &source, &handler)) << path;
    ++scanned;
    EXPECT_EQ(GoogleString::npos, source.find("MakeDaemonServeReaderIfReady"))
        << path
        << " uses the blocking serve-reader factory; the event thread must "
           "build through MakeDaemonServeReaderIfOpen only";
  }
  EXPECT_GT(scanned, 10) << "only " << scanned
                         << " nginx source files were scanned; the listing "
                            "looks wrong, so this pin proved nothing";
}

TEST_F(NgxDaemonSeamTest, TheDaemonSubstrateIsAskedFirstAndNeverBlocks) {
  const GoogleString cc = StripWhitespace(ReadSource(kNgxPagespeedCc));
  const size_t fn = cc.find("ps_resource_handler(ngx_http_request_t*r");
  ASSERT_NE(GoogleString::npos, fn)
      << "ps_resource_handler was renamed; this pin needs updating";
  const size_t fn_end = cc.find("voidps_send_to_pagespeed(", fn);
  ASSERT_NE(GoogleString::npos, fn_end)
      << "the function after ps_resource_handler moved; this pin needs "
         "updating";
  const GoogleString body = cc.substr(fn, fn_end - fn);
  EXPECT_NE(GoogleString::npos,
            body.find("constngx_int_tserve_rc=PsServeFromDaemonSubstrate(r,"
                      "cfg_s,*request_headers,url);"))
      << "the daemon branch no longer asks the substrate first, with the "
         "handler's parsed headers and URL handed over";
  EXPECT_NE(GoogleString::npos,
            body.find("if(serve_rc!=NGX_DECLINED){returnserve_rc;}"))
      << "a served request no longer returns from inside the branch; it "
         "would reach the html-rewrite tail";
  EXPECT_EQ(GoogleString::npos, body.find("RecordCache("))
      << "the request path opens the daemon volume handle inline; the open "
         "belongs to worker init and the record pool's posted attempt";
  EXPECT_EQ(1, CountSubstring(cc, "MakeDaemonServeReaderIfOpen("))
      << "the non-opening serve factory is not called exactly once";
}

TEST_F(NgxDaemonSeamTest, TheServeKeyIsTheRecordKey) {
  // URL, host and scheme come from the SAME source the record arm uses:
  // the resource handler's already-parsed GoogleUrl -- PageSpeed parameters
  // stripped, X-Forwarded-Proto honoured, the very object ctx->url_string
  // was synced from.  A serve key that differs from the record key never
  // hits, and a second parse (of the URL or of the headers) is both a
  // skew risk and the dominant per-request cost this function must not pay.
  const GoogleString cc = StripWhitespace(ReadSource(kNgxPagespeedCc));
  const size_t fn = cc.find("voidBuildDaemonServeRequest(");
  ASSERT_NE(GoogleString::npos, fn)
      << "BuildDaemonServeRequest was renamed; this pin needs updating";
  const size_t open = cc.find("{", fn);
  ASSERT_NE(GoogleString::npos, open);
  const size_t block_end = MatchingBrace(cc, open);
  ASSERT_NE(GoogleString::npos, block_end)
      << "BuildDaemonServeRequest's block does not close";
  const GoogleString body = cc.substr(fn, block_end - fn);
  EXPECT_NE(GoogleString::npos,
            body.find("full_url.PathAndLeaf().CopyToString(&strings->url)"))
      << "the serve key's URL is not taken from the parsed GoogleUrl";
  EXPECT_NE(GoogleString::npos,
            body.find("full_url.Host().CopyToString(&strings->host)"))
      << "the serve key's host is not taken from the parsed GoogleUrl";
  EXPECT_NE(GoogleString::npos,
            body.find("full_url.Scheme().CopyToString(&strings->scheme)"))
      << "the serve key's scheme is not taken from the parsed GoogleUrl";
  EXPECT_EQ(GoogleString::npos, body.find("ps_determine_url("))
      << "the serve key is built from the raw URL: PageSpeed parameters "
         "unstripped, X-Forwarded-Proto ignored";
  EXPECT_EQ(GoogleString::npos, body.find("GoogleUrlfull_url("))
      << "the serve key re-parses the URL; the handler's parsed GoogleUrl "
         "is the single source, passed in";
  EXPECT_EQ(GoogleString::npos, body.find("copy_request_headers_from_ngx("))
      << "the serve builder re-parses the request headers; the handler's "
         "parsed RequestHeaders is passed in";
  // The resource handler hands its own parsed objects over (the call site
  // pins the wiring; comment-blind like the needles above).
  EXPECT_NE(GoogleString::npos,
            cc.find("PsServeFromDaemonSubstrate(r,cfg_s,*request_headers,"
                    "url)"))
      << "the serve call no longer receives the handler's parsed headers "
         "and URL";
}

TEST_F(NgxDaemonSeamTest, TheVaryStampIsWithdrawnOnceAndOnlyForAPresentVary) {
  // The compressor's own Vary stamp is withdrawn only while a Vary that
  // lists the encoding token is really on the response, and the mark is
  // consumed by the first response that sees it.
  const GoogleString cc = StripWhitespace(ReadSource(kNgxPagespeedCc));
  const size_t fn = cc.find("ngx_int_tps_etag_header_filter(");
  ASSERT_NE(GoogleString::npos, fn)
      << "ps_etag_header_filter was renamed; this pin needs updating";
  const size_t open = cc.find("{", fn);
  ASSERT_NE(GoogleString::npos, open);
  const size_t close = MatchingBrace(cc, open);
  ASSERT_NE(GoogleString::npos, close);
  const GoogleString body = cc.substr(open, close - open);
  const size_t consumed = body.find("ctx->psol_vary_accept_only=false;");
  const size_t checked =
      body.find("ps_vary_lists_accept_encoding(header->value)");
  const size_t withdrawn = body.find("r->gzip_vary=0;");
  ASSERT_NE(GoogleString::npos, consumed) << "the mark is not one-shot";
  ASSERT_NE(GoogleString::npos, checked)
      << "the stamp is withdrawn without looking at the response's Vary";
  ASSERT_NE(GoogleString::npos, withdrawn);
  EXPECT_LT(consumed, withdrawn);
  EXPECT_LT(checked, withdrawn);
  EXPECT_EQ(GoogleString::npos, body.find("r->gzip_vary=0;", withdrawn + 1))
      << "a second, unconditional withdrawal exists";
}

TEST_F(NgxDaemonSeamTest, TheServedRequestIsNeverMarkedForRecording) {
  // A served request must not also be marked for recording, and the
  // substrate function itself never sets the mark: the mark exists only in
  // the resource handler's daemon branch, under the serve's false return.
  //
  // The absence needles below are COMMENT-BLIND ON PURPOSE: the whitespace
  // strip does not remove comments, so an explanatory comment that names
  // one of these symbols inside the function turns the pin red too.  That
  // is intended -- the function must not even discuss the mechanism.
  const GoogleString cc = StripWhitespace(ReadSource(kNgxPagespeedCc));
  const size_t fn = cc.find("ngx_int_tPsServeFromDaemonSubstrate(");
  ASSERT_NE(GoogleString::npos, fn)
      << "PsServeFromDaemonSubstrate was renamed; this pin needs updating";
  const size_t open = cc.find("{", fn);
  ASSERT_NE(GoogleString::npos, open);
  const size_t block_end = MatchingBrace(cc, open);
  ASSERT_NE(GoogleString::npos, block_end)
      << "PsServeFromDaemonSubstrate's block does not close";
  const GoogleString body = cc.substr(fn, block_end - fn);
  EXPECT_EQ(GoogleString::npos, body.find("daemon_record_pending=true;"))
      << "the serve path marks a request for recording";
  EXPECT_EQ(GoogleString::npos, body.find("psol_vary_accept_only"))
      << "the serve block sets the vary flag itself; the one place that "
         "sets it is the header copy, from the Vary the arm composed";
  EXPECT_NE(GoogleString::npos,
            body.find("serve_stats->Record(kPsServeClassOriginalSkew)"))
      << "a null reader no longer records the skew class";
  EXPECT_NE(GoogleString::npos, body.find("ipro_daemon_fallthrough()->Add(1)"))
      << "the fall-through counter is not recorded";
  EXPECT_NE(GoogleString::npos, body.find("serve_stats->RecordHit("))
      << "the serve-hit recorder is not called under Apache's condition";
  // The media-type fallback runs on BOTH legs: its result feeds the Vary
  // compressibility predicate, and the predicate's input must not depend on
  // the leg.  The Content-Type header stays leg-gated by clearing what the
  // type-map call put on r->headers_out for a 304 (value, length, lowcase
  // pointer).
  EXPECT_NE(GoogleString::npos, body.find("if(decision.content_type.empty()&&"))
      << "the media-type fallback no longer runs on both legs; the Vary "
         "predicate's input would depend on the leg";
  EXPECT_EQ(GoogleString::npos,
            body.find("!decision.not_modified&&decision.content_type.empty()"))
      << "the media-type fallback is gated on the leg again; a typeless "
         "entry would state the encoding token on the 200 and not on the 304";
  EXPECT_NE(GoogleString::npos,
            body.find("ngx_str_null(&r->headers_out.content_type);"))
      << "the 304's invented Content-Type is not cleared";
  EXPECT_NE(GoogleString::npos,
            body.find("r->headers_out.content_type_lowcase=nullptr;"))
      << "the 304's invented Content-Type lowcase pointer is not cleared";
  // The 200 leg disables the server's own not-modified filter, so it cannot
  // mint a 304 the arm refused to mint (If-None-Match: *, If-Match,
  // If-Unmodified-Since).
  EXPECT_NE(GoogleString::npos, body.find("r->disable_not_modified=1;"))
      << "the 200 leg no longer disables the server's not-modified filter";
  // The compressibility predicate consults the module's own gzip type list
  // (the pdf/postscript/csv row); replacing this call with a literal would
  // pass every other test in the tree, which is why it is pinned here.
  EXPECT_NE(GoogleString::npos, body.find("NgxGZipSetterCompressesType("))
      << "the serve block no longer asks the gzip setter's type list";
  EXPECT_EQ(GoogleString::npos, body.find("MakeDaemonServeReaderIfReady("))
      << "the blocking serve factory appeared in the serve block";

  // The served counter: present exactly once, and WHERE it sits is pinned,
  // not only that it exists -- before ngx_http_send_header, so the
  // header-only early return (304, HEAD) cannot skip it.  Each needle is
  // searched independently from the start of the function slice, so the
  // ordering assertion can actually fail.
  EXPECT_EQ(1, CountSubstring(body, "ipro_daemon_served()->Add(1)"))
      << "the served counter must move exactly once per answered request";
  const size_t served_pos = body.find("ipro_daemon_served()->Add(1)");
  const size_t send_pos = body.find("ngx_http_send_header(r)");
  ASSERT_NE(GoogleString::npos, send_pos);
  EXPECT_LT(served_pos, send_pos)
      << "the served counter moved back behind the send, where the 304 and "
         "HEAD early returns skip it";

  // The body is copied before the reader dies, and an empty buffer is a
  // sync buffer (string_piece_to_buffer_chain's idiom -- a non-special
  // zero-size buffer trips ngx_http_write_filter's "zero size buf" alert).
  const size_t copy_pos =
      body.find("ngx_memcpy(body_copy,decision.body.data(),body_size);");
  ASSERT_NE(GoogleString::npos, copy_pos)
      << "the single pool copy of the body is gone";
  const size_t reset_pos = body.find("reader.reset();");
  ASSERT_NE(GoogleString::npos, reset_pos);
  EXPECT_LT(copy_pos, reset_pos)
      << "the body copy no longer precedes the reader's destruction";
  EXPECT_NE(GoogleString::npos, body.find("body_buf->sync=1;"))
      << "an empty serve buffer is not a sync buffer";
  EXPECT_NE(GoogleString::npos, body.find("body_buf->temporary=1;"))
      << "the in-memory body buffer lost its temporary marking";
}

TEST_F(NgxDaemonSeamTest, TheCapabilityHeadersAreReadJoinedNotSingle) {
  // Lookup1 reports a comma-split header as ABSENT: any multi-token Accept
  // or Accept-Encoding ("image/avif,image/webp,*/*", "gzip, deflate, br")
  // is several values, and a single-value lookup answers nullptr.  Read
  // joined, both arms would classify such a request as a no-capability
  // client -- the record key would carry the wrong mask and the serve key
  // would select the wrong variant.  The same defect on Cache-Control
  // ("no-cache, no-store") reads a forced reload as not asked.  THE CLASS
  // PIN: no single-value read of ANY request header inside either request
  // builder, not just the headers named here.
  const GoogleString cc = StripWhitespace(ReadSource(kNgxPagespeedCc));
  EXPECT_EQ(GoogleString::npos,
            cc.find("request_headers.Lookup1(HttpAttributes::kAccept)"))
      << "the Accept header is read single-valued again";
  EXPECT_EQ(GoogleString::npos,
            cc.find("request_headers.Lookup1(HttpAttributes::kAcceptEncoding)"))
      << "Accept-Encoding is read single-valued again";
  EXPECT_EQ(GoogleString::npos,
            cc.find("request_headers.Lookup1(HttpAttributes::kUserAgent)"))
      << "User-Agent is read single-valued again";
  EXPECT_EQ(2, CountSubstring(cc, "LookupJoined(HttpAttributes::kAccept)"))
      << "the Accept header is not read joined in both arms";
  EXPECT_EQ(2,
            CountSubstring(cc, "LookupJoined(HttpAttributes::kAcceptEncoding)"))
      << "Accept-Encoding is not read joined in both arms";

  // The serve request builder: every request-header read joined, with
  // Cache-Control and Pragma among them (the forced-reload pair).
  const size_t serve_fn = cc.find("voidBuildDaemonServeRequest(");
  ASSERT_NE(GoogleString::npos, serve_fn)
      << "BuildDaemonServeRequest was renamed; this pin needs updating";
  const size_t serve_open = cc.find("{", serve_fn);
  ASSERT_NE(GoogleString::npos, serve_open);
  const size_t serve_end = MatchingBrace(cc, serve_open);
  ASSERT_NE(GoogleString::npos, serve_end)
      << "BuildDaemonServeRequest's block does not close";
  const GoogleString serve_body = cc.substr(serve_fn, serve_end - serve_fn);
  EXPECT_EQ(GoogleString::npos, serve_body.find("request_headers.Lookup1("))
      << "a request header is read single-valued inside "
         "BuildDaemonServeRequest; the class rule is LookupJoined for EVERY "
         "header, and this needle is comment-blind on purpose";
  EXPECT_NE(GoogleString::npos,
            serve_body.find("request_headers.LookupJoined(HttpAttributes::"
                            "kCacheControl)"))
      << "Cache-Control is not read joined; \"no-cache, no-store\" would "
         "read as absent and the forced reload would be missed";
  EXPECT_NE(GoogleString::npos,
            serve_body.find("request_headers.LookupJoined(HttpAttributes::"
                            "kPragma)"))
      << "Pragma is not read joined; a repeated Pragma line would read as "
         "absent";

  // The record request builder (inline in the in-place check header
  // filter): the same class rule over the same headers.
  const size_t rec_fn = cc.find("DaemonRecordRequestrecord_request;");
  ASSERT_NE(GoogleString::npos, rec_fn)
      << "the record request builder moved; this pin needs updating";
  const size_t rec_end = cc.find("MakeDaemonIproRecorderIfOpen(", rec_fn);
  ASSERT_NE(GoogleString::npos, rec_end)
      << "the record request builder's end moved; this pin needs updating";
  const GoogleString rec_body = cc.substr(rec_fn, rec_end - rec_fn);
  EXPECT_EQ(GoogleString::npos, rec_body.find("request_headers.Lookup1("))
      << "a request header is read single-valued inside the record request "
         "builder; the class rule is LookupJoined for EVERY header, and "
         "this needle is comment-blind on purpose";
}

TEST_F(NgxDaemonSeamTest, TheServeNotifiesArePostedNeverInline) {
  // The three shared notify functions are a socket call to another process:
  // they may be spelled ONLY inside the posted closure
  // (ngx_daemon_serve_notify.cc), never in a file that runs on the event
  // thread.  Directory scan of every .cc and .h in the port, the same idiom
  // as the blocking-factory pin; the needles are comment-blind on purpose.
  StdioFileSystem file_system;
  NullMessageHandler handler;
  const GoogleString dir = StrCat(GTestSrcDir(), "/pagespeed/nginx");
  StringVector files;
  ASSERT_TRUE(file_system.ListContents(dir, &files, &handler)) << dir;

  int scanned = 0;
  for (const GoogleString& path : files) {
    if (!StringCaseEndsWith(path, ".cc") && !StringCaseEndsWith(path, ".h")) {
      continue;
    }
    GoogleString source;
    ASSERT_TRUE(file_system.ReadFile(path.c_str(), &source, &handler)) << path;
    ++scanned;
    const GoogleString stripped = StripWhitespace(source);
    if (StringCaseEndsWith(path, "ngx_daemon_serve_notify.cc")) {
      // The one place each spelling must exist, exactly once each.
      EXPECT_EQ(1, CountSubstring(stripped, "DaemonServeFallbackRenotify("))
          << path << " no longer calls the fallback re-notify";
      EXPECT_EQ(1,
                CountSubstring(stripped, "DaemonServeOriginRefreshedNotify("))
          << path << " no longer calls the origin-refreshed notify";
      EXPECT_EQ(1, CountSubstring(stripped, "DaemonServeLostCopyNotify("))
          << path << " no longer calls the lost-copy notify";
      continue;
    }
    EXPECT_EQ(GoogleString::npos, stripped.find("DaemonServeFallbackRenotify("))
        << path
        << " calls the fallback re-notify inline; a notification is a "
           "socket call and belongs on the record arm's sequence";
    EXPECT_EQ(GoogleString::npos,
              stripped.find("DaemonServeOriginRefreshedNotify("))
        << path
        << " calls the origin-refreshed notify inline; a notification is a "
           "socket call and belongs on the record arm's sequence";
    EXPECT_EQ(GoogleString::npos, stripped.find("DaemonServeLostCopyNotify("))
        << path
        << " calls the lost-copy notify inline; a notification is a socket "
           "call and belongs on the record arm's sequence";
  }
  EXPECT_GT(scanned, 10) << "only " << scanned
                         << " nginx source files were scanned; the listing "
                            "looks wrong, so this pin proved nothing";
}

TEST_F(NgxDaemonSeamTest, TheOriginRefreshedPostPrecedesTheRecordMark) {
  // The purge must sit in the sequence's FIFO ahead of the same request's
  // re-record, or the URL regresses to origin serving.  Pinned as two
  // program orders: inside the serve block the post is on the fall-through
  // leg, and in the resource handler the serve call (which carries the
  // post) precedes the record mark.  Each needle is searched independently
  // from the start of its slice.
  const GoogleString cc = StripWhitespace(ReadSource(kNgxPagespeedCc));
  const size_t fn = cc.find("ngx_int_tPsServeFromDaemonSubstrate(");
  ASSERT_NE(GoogleString::npos, fn);
  const size_t open = cc.find("{", fn);
  ASSERT_NE(GoogleString::npos, open);
  const size_t block_end = MatchingBrace(cc, open);
  ASSERT_NE(GoogleString::npos, block_end);
  const GoogleString body = cc.substr(fn, block_end - fn);
  const size_t fallthrough =
      body.find("if(decision.verdict==DaemonServeVerdict::kFallThrough){");
  ASSERT_NE(GoogleString::npos, fallthrough);
  const size_t aged = body.find("if(decision.stale_variant_expired_by_age){");
  ASSERT_NE(GoogleString::npos, aged)
      << "the age-expired gate is gone; the sentinel can never be posted";
  const size_t post = body.find("DaemonServeNotifyKind::kOriginRefreshed");
  ASSERT_NE(GoogleString::npos, post)
      << "the origin-refreshed post is gone from the serve block";
  const size_t decline = body.find("returnNGX_DECLINED;", fallthrough);
  ASSERT_NE(GoogleString::npos, decline);
  EXPECT_LT(fallthrough, aged);
  EXPECT_LT(aged, post)
      << "the origin-refreshed post is not inside the age-expired gate";
  EXPECT_LT(post, decline)
      << "the origin-refreshed post is not on the fall-through leg";

  const size_t handler = cc.find("ps_resource_handler(ngx_http_request_t*r");
  ASSERT_NE(GoogleString::npos, handler);
  const size_t handler_end = cc.find("voidps_send_to_pagespeed(", handler);
  ASSERT_NE(GoogleString::npos, handler_end);
  const GoogleString hbody = cc.substr(handler, handler_end - handler);
  const size_t serve_call = hbody.find("PsServeFromDaemonSubstrate(r,cfg_s,");
  ASSERT_NE(GoogleString::npos, serve_call);
  const size_t mark = hbody.find("ctx->daemon_record_pending=true;");
  ASSERT_NE(GoogleString::npos, mark);
  EXPECT_LT(serve_call, mark)
      << "the record mark no longer follows the serve call, so the posted "
         "purge would not precede the same request's record completion";
}

TEST_F(NgxDaemonSeamTest, TheFallbackPostFollowsTheServeDecision) {
  // The fallback re-notify is posted only after the serve is decided (and
  // counted): a serve first, then the ask for the variant the client's mask
  // really names.  And it sits BEFORE the send, where the served counter
  // sits: behind the send's early return (header-only, not-modified) a
  // fallback hit answered as a 304 or to a HEAD would never notify, where
  // Apache's block is unconditional.  Each needle searched independently
  // from the slice start.
  const GoogleString cc = StripWhitespace(ReadSource(kNgxPagespeedCc));
  const size_t fn = cc.find("ngx_int_tPsServeFromDaemonSubstrate(");
  ASSERT_NE(GoogleString::npos, fn);
  const size_t open = cc.find("{", fn);
  ASSERT_NE(GoogleString::npos, open);
  const size_t block_end = MatchingBrace(cc, open);
  ASSERT_NE(GoogleString::npos, block_end);
  const GoogleString body = cc.substr(fn, block_end - fn);
  const size_t serve = body.find("reader->Serve(");
  ASSERT_NE(GoogleString::npos, serve);
  const size_t counted = body.find("ipro_daemon_served()->Add(1)");
  ASSERT_NE(GoogleString::npos, counted);
  const size_t gate = body.find("if(decision.fallback_hit){");
  ASSERT_NE(GoogleString::npos, gate)
      << "the fallback gate is gone; the re-notify can never be posted";
  const size_t post = body.find("DaemonServeNotifyKind::kFallbackRenotify");
  ASSERT_NE(GoogleString::npos, post)
      << "the fallback re-notify post is gone from the serve block";
  const size_t send = body.find("ngx_http_send_header(r)");
  ASSERT_NE(GoogleString::npos, send);
  EXPECT_LT(serve, post) << "the re-notify would post before the decision";
  EXPECT_LT(counted, post)
      << "the re-notify would post before the serve is counted";
  EXPECT_LT(gate, post) << "the re-notify post is not inside its gate";
  EXPECT_LT(post, send)
      << "the re-notify post moved behind the send; a fallback hit answered "
         "as a 304 or to a HEAD leaves through the early return and never "
         "notifies, where Apache's block is unconditional";
}

TEST_F(NgxDaemonSeamTest, TheLostCopyPostFollowsTheServeAndPrecedesTheSend) {
  // A stored original served for a URL whose optimized copy went missing is
  // answered with one posted notification.  Pinned: the reader is given the
  // limiter before the serve; the post sits inside the decision's own gate,
  // after the serve is counted and BEFORE the send (behind the send's early
  // return a 304 or a HEAD would never ask); and nothing is sent inline --
  // the only way out of this function for a notification is the poster.
  const GoogleString cc = StripWhitespace(ReadSource(kNgxPagespeedCc));
  const size_t fn = cc.find("ngx_int_tPsServeFromDaemonSubstrate(");
  ASSERT_NE(GoogleString::npos, fn);
  const size_t open = cc.find("{", fn);
  ASSERT_NE(GoogleString::npos, open);
  const size_t block_end = MatchingBrace(cc, open);
  ASSERT_NE(GoogleString::npos, block_end);
  const GoogleString body = cc.substr(fn, block_end - fn);

  const size_t opt_in = body.find(
      "reader->set_heal_notify_limiter(ProcessDaemonHealNotifyLimiter());");
  ASSERT_NE(GoogleString::npos, opt_in)
      << "the reader is never given the limiter, so a lost copy is never "
         "recognised on this port";
  const size_t serve = body.find("reader->Serve(");
  ASSERT_NE(GoogleString::npos, serve);
  EXPECT_LT(opt_in, serve);
  const size_t counted = body.find("ipro_daemon_served()->Add(1)");
  ASSERT_NE(GoogleString::npos, counted);
  const size_t gate = body.find("if(decision.lost_optimized_copy){");
  ASSERT_NE(GoogleString::npos, gate)
      << "the lost-copy gate is gone; the notification can never be posted";
  const size_t post = body.find("DaemonServeNotifyKind::kLostCopy");
  ASSERT_NE(GoogleString::npos, post)
      << "the lost-copy post is gone from the serve block";
  const size_t send = body.find("ngx_http_send_header(r)");
  ASSERT_NE(GoogleString::npos, send);
  EXPECT_LT(counted, gate);
  EXPECT_LT(gate, post) << "the post is not inside its gate";
  EXPECT_LT(post, send)
      << "the post moved behind the send; a stored original answered as a "
         "304 or to a HEAD would never ask";
  EXPECT_NE(GoogleString::npos, body.find("ipro_daemon_heal_notified()"));
  EXPECT_NE(GoogleString::npos, body.find("ipro_daemon_heal_notify_failed()"));
  EXPECT_EQ(GoogleString::npos, body.find("DaemonServeLostCopyNotify("))
      << "the notification is sent inline on the event thread";
}

TEST_F(NgxDaemonSeamTest, ThePostedNotifyNeverCarriesABorrowedDecision) {
  // Across the thread boundary the closure owns copies only: the request's
  // three strings, the option-context halves, and the decision scalars.
  // NEVER a DaemonServeDecision with its body pointing into the dead
  // reader.  Pinned on the helper's own source: it never reads
  // decision.body, and it reconstitutes a scalar-only decision in Run().
  const GoogleString helper =
      StripWhitespace(ReadSource(kNgxDaemonServeNotifyCc));
  EXPECT_EQ(GoogleString::npos, helper.find("decision.body"))
      << "the posted notification touches the decision's borrowed body; "
         "the reader that owns it is dead by the time Run() executes";
  EXPECT_EQ(GoogleString::npos, helper.find("decision->body"))
      << "the posted notification touches the decision's borrowed body";
  EXPECT_NE(GoogleString::npos, helper.find("DaemonServeDecisiondecision;"))
      << "Run() no longer reconstitutes a scalar-only decision";
}

TEST_F(NgxDaemonSeamTest, TheVolumeHandleClosesAfterTheFactoryShutsDown) {
  // The close joins the storage layer's background threads, so it belongs
  // where nginx expects shutdown work -- after the factory shutdown, when no
  // PSOL thread can still be inside an adapter call, and inside the exit
  // hook, not the later pool destruction.
  const GoogleString cc = StripWhitespace(ReadSource(kNgxPagespeedCc));
  const size_t fn = cc.find("voidps_exit_child_process(");
  ASSERT_NE(GoogleString::npos, fn)
      << "ps_exit_child_process was renamed; this pin needs updating";
  const size_t open = cc.find("{", fn);
  ASSERT_NE(GoogleString::npos, open);
  const size_t block_end = MatchingBrace(cc, open);
  ASSERT_NE(GoogleString::npos, block_end)
      << "ps_exit_child_process's block does not close";
  // Anchored on the call's own spelling, which prose cannot reproduce, and
  // the close is searched from the start of the block so the ordering
  // assertion below can actually fail.
  const size_t shutdown = cc.find("cfg_m->driver_factory->ShutDown();", open);
  ASSERT_NE(GoogleString::npos, shutdown)
      << "ps_exit_child_process no longer shuts the driver factory down";
  const size_t close = cc.find("daemon_adapter()->CloseRecordCache();", open);
  EXPECT_NE(GoogleString::npos, close)
      << "ps_exit_child_process does not close the worker's daemon volume "
         "handle";
  ASSERT_NE(GoogleString::npos, close);
  EXPECT_LT(close, block_end)
      << "the volume handle close is not inside ps_exit_child_process";
  EXPECT_LT(shutdown, close)
      << "the volume handle close must come after the factory shutdown";
}

TEST_F(NgxDaemonSeamTest, TheServerContextInitialisesItsFactoryPointer) {
  const GoogleString cc = StripWhitespace(ReadSource(kNgxServerContextCc));
  const size_t ctor = cc.find("NgxServerContext::NgxServerContext(");
  ASSERT_NE(GoogleString::npos, ctor)
      << "the constructor was renamed; this pin needs updating";
  const size_t open = cc.find("{", ctor);
  ASSERT_NE(GoogleString::npos, open);
  // With the separator before it: an initialiser-list entry, not a mention.
  const size_t init = cc.find("),ngx_factory_(factory)", ctor);
  EXPECT_NE(GoogleString::npos, init)
      << "ngx_factory_ is not initialised; the accessor would return an "
         "indeterminate pointer";
  ASSERT_NE(GoogleString::npos, init);
  EXPECT_LT(init, open) << "ngx_factory_(factory) is not in the initialiser "
                           "list";
}

TEST_F(NgxDaemonSeamTest, TheServeLegMovesThePerClassSplit) {
  const GoogleString cc = StripWhitespace(ReadSource(kNgxPagespeedCc));
  const size_t served = cc.find("ipro_daemon_served()->Add(1)");
  ASSERT_NE(GoogleString::npos, served);
  EXPECT_NE(GoogleString::npos, cc.find("RecordDaemonServedClass("))
      << "the per-class split of the serve counter no longer moves on the "
         "serve leg";
  // The record side hands the recorder the counters it moves per class.
  const size_t record = cc.find("MakeDaemonIproRecorderIfOpen(");
  ASSERT_NE(GoogleString::npos, record);
  const GoogleString call = cc.substr(record, 400);
  EXPECT_NE(GoogleString::npos, call.find("rewrite_stats()"))
      << "the recorder no longer receives the shared counters, so the "
         "fall-through split has stopped moving";
}

TEST_F(NgxDaemonSeamTest, TheHeaderCopyHandsTheCodingToNginxsCompressors) {
  // nginx's gzip header filter -- and a brotli one -- leave a response alone
  // when r->headers_out.content_encoding is set.  The serve composes a
  // Content-Encoding header for a stored compressed copy and relies on the
  // header copy to put it in that field; this pins the mapping it relies on.
  // Green on first run by construction: the mapping predates the stored
  // copies, and this keeps it from being dropped as dead weight.
  const GoogleString cc = StripWhitespace(ReadSource(kNgxPagespeedCc));
  const size_t copy = cc.find("ngx_int_tcopy_response_headers_to_ngx(");
  ASSERT_NE(GoogleString::npos, copy)
      << "copy_response_headers_to_ngx was renamed; this pin needs updating";
  const size_t open = cc.find("{", copy);
  ASSERT_NE(GoogleString::npos, open);
  const size_t block_end = MatchingBrace(cc, open);
  ASSERT_NE(GoogleString::npos, block_end);
  const GoogleString body = cc.substr(copy, block_end - copy);
  EXPECT_NE(GoogleString::npos,
            body.find("elseif(STR_EQ_LITERAL(name,\"Content-Encoding\")){"
                      "headers_out->content_encoding=header;"))
      << "a Content-Encoding header no longer reaches "
         "headers_out.content_encoding, so nginx's compressors would "
         "compress a stored compressed copy again";
  // And the serve block hands its composed headers to that copy.
  EXPECT_NE(GoogleString::npos,
            cc.find("copy_response_headers_to_ngx(r,response_headers,"
                    "kDontPreserveHeaders)"));
}

TEST_F(NgxDaemonSeamTest, TheReaderTakesTheServerBlocksStoredEncodingsSwitch) {
  const GoogleString cc = StripWhitespace(ReadSource(kNgxPagespeedCc));
  const size_t fn = cc.find("ngx_int_tPsServeFromDaemonSubstrate(");
  ASSERT_NE(GoogleString::npos, fn)
      << "PsServeFromDaemonSubstrate was renamed; this pin needs updating";
  const size_t open = cc.find("{", fn);
  ASSERT_NE(GoogleString::npos, open);
  const size_t block_end = MatchingBrace(cc, open);
  ASSERT_NE(GoogleString::npos, block_end);
  const GoogleString body = cc.substr(fn, block_end - fn);
  EXPECT_NE(
      GoogleString::npos,
      body.find("constNgxRewriteOptions*config=server_context->config();"))
      << "the switch is not read from this server block's configuration";
  const size_t set = body.find(
      "reader->set_serve_stored_encodings(config!=nullptr&&config->daemon_"
      "serve_stored_encodings());");
  ASSERT_NE(GoogleString::npos, set)
      << "the directive never reaches the serve arm";
  const size_t selection = body.find("reader->Serve(");
  ASSERT_NE(GoogleString::npos, selection);
  EXPECT_LT(set, selection) << "the switch is set after the selection";
  EXPECT_EQ(1, CountSubstring(cc, "set_serve_stored_encodings("));
}

}  // namespace
}  // namespace net_instaweb
