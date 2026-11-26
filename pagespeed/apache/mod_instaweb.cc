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

// Register handlers, define configuration options and set up other things
// that mod_pagespeed needs to do to be an Apache module.

#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <utility>  // for pair
#include <vector>

#include "base/logging.h"
#include "net/instaweb/public/global_constants.h"
#include "net/instaweb/public/version.h"
#include "net/instaweb/rewriter/public/process_context.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/rewrite_query.h"
#include "pagespeed/apache/apache_config.h"
#include "pagespeed/apache/apache_directive_table.h"
#include "pagespeed/apache/apache_rewrite_driver_factory.h"
#include "pagespeed/apache/apache_self_fetch.h"
#include "pagespeed/apache/apache_server_context.h"
#include "pagespeed/apache/apr_timer.h"
#include "pagespeed/apache/header_util.h"
#include "pagespeed/apache/html_flush_window.h"
#include "pagespeed/apache/in_place_record_brigade.h"
#include "pagespeed/apache/instaweb_context.h"
#include "pagespeed/apache/instaweb_handler.h"
// clang-format off
#include "pagespeed/apache/mod_instaweb.h"
#include "pagespeed/system/daemon_abi.h"
#include "pagespeed/system/daemon_adapter.h"
#include "pagespeed/system/daemon_adapter_pool.h"
#include "pagespeed/system/daemon_html_engine.h"
// clang-format on
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/http/content_type.h"
#include "pagespeed/kernel/http/google_url.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "pagespeed/system/file_owner.h"
#include "pagespeed/system/ipro_recorder.h"
#include "pagespeed/system/system_caches.h"
#include "pagespeed/system/system_server_context.h"
#include "util_filter.h"  // NOLINT
// Note: a very useful reference is this file, which demos many Apache module
// options:
//    http://svn.apache.org/repos/asf/httpd/httpd/trunk/modules/examples/mod_example_hooks.c

// The httpd header must be after the pagepseed_server_context.h. Otherwise,
// the compiler will complain
// "strtoul_is_not_a_portable_function_use_strtol_instead".
#include "ap_release.h"                              // NOLINT
#include "apr_pools.h"                               // NOLINT
#include "apr_strings.h"                             // NOLINT
#include "http_config.h"                             // NOLINT
#include "http_protocol.h"                           // NOLINT
#include "http_request.h"                            // NOLINT
#include "pagespeed/apache/apache_httpd_includes.h"  // NOLINT

// This include-file is order-dependent; it must come after the above apache
// includes, and not be in abc-order with the net/instaweb/... includes.
#include "pagespeed/apache/apache_logging_includes.h"

//#include "strings/stringpiece_utils.h"
#include "pagespeed/apache/log_message_handler.h"
#include "unixd.h"  // NOLINT

#if (AP_SERVER_MAJORVERSION_NUMBER == 2) && (AP_SERVER_MINORVERSION_NUMBER >= 4)
#define MPS_APACHE_24
#endif

// Apache 2.4 renames unixd_config -> ap_unixd_config
#ifdef MPS_APACHE_24
#define unixd_config ap_unixd_config
#endif

struct apr_pool_t;

namespace net_instaweb {

namespace {

// The directive names, the directive table and the placement rules are in
// apache_directive_table.h, shared with the offline config linter.
// If you add a new option, exercise it in a system test under
// test/system/ to make sure it parses correctly.
using namespace apache_directive;  // NOLINT(build/namespaces)

enum RewriteOperation : std::uint8_t { REWRITE, FLUSH, FINISH };

// TODO(sligocki): Move inside PSOL.
// Check if pagespeed optimization rules applicable.
bool check_pagespeed_applicable(request_rec* request,
                                const ContentType& content_type) {
  // We can't operate on Content-Ranges.
  if (apr_table_get(request->headers_out, "Content-Range") != nullptr) {
    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, APR_SUCCESS, request,
                  "Request not rewritten because: header Content-Range set.");
    return false;
  }

  // Only rewrite HTML-like content.
  if (!content_type.IsHtmlLike()) {
    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, APR_SUCCESS, request,
                  "Request not rewritten because: request->content_type does "
                  "not appear to be HTML (was %s)",
                  request->content_type);
    return false;
  }

  // mod_pagespeed often creates requests while rewriting an HTML.  These
  // requests are only intended to fetch resources (images, css, javascript) but
  // in some circumstances they can end up fetching HTML.  This HTML, if
  // rewritten, could in turn spawn more requests which could cascade into a
  // bad situation.  To mod_pagespeed, any fetched HTML is an error condition,
  // so there's no reason to rewrite it anyway.
  if (InstawebHandler::is_pagespeed_subrequest(request)) {
    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, APR_SUCCESS, request,
                  "Request not rewritten because: User-Agent appears to be "
                  "mod_pagespeed");
    return false;
  }

  return true;
}

DaemonHtmlHeaderLines apache_header_lines(const request_rec* request);

// Create a new bucket from buf using HtmlRewriter.
// TODO(lsong): the content is copied multiple times. The buf is
// copied/processed to string output, then output is copied to new bucket.
apr_bucket* rewrite_html(InstawebContext* context, request_rec* request,
                         RewriteOperation operation, const char* buf, int len) {
  if (context == nullptr) {
    LOG(DFATAL) << "Context is null";
    return nullptr;
  }
  if (buf != nullptr) {
    context->PopulateHeaders(request);
    context->Rewrite(buf, len);
  }
  if (operation == REWRITE) {
    // Once a window's worth of output is held it is sent on, headers first,
    // so the response is not held whole until its end.
    if (!HeldOutputIsSentOn(context->output().size())) {
      return nullptr;
    }
  } else if (operation == FLUSH) {
    context->Flush();
    // If the flush happens before any rewriting, don't fallthrough and
    // replace the headers with those in the context, because they haven't
    // been populated yet so we end up with NO headers. See issue 385. Once
    // they are populated, the flush sends them, so they are written even if
    // there is no output yet, or they would go out without the Vary names.
    if (context->output().empty() && !context->populated_headers()) {
      return nullptr;
    }
  } else if (operation == FINISH) {
    // A response that ends without a body byte (a HEAD request, or a 200 with
    // an empty body) never ran PopulateHeaders on data, so the context's
    // response headers are still empty. Read the origin's headers now, or the
    // clear-and-write-back below would replace them with nothing.
    context->PopulateHeaders(request);
    context->Finish();
  }

  // Check to see if we've added in the headers already.  If not,
  // clear out the existing headers (to avoid duplication), add them,
  // and make a note of it.
  if (!context->sent_headers()) {
    ResponseHeaders* headers = context->response_headers();
    apr_table_clear(request->headers_out);
    ResponseHeadersToApacheRequest(*headers, request);
    headers->Clear();
    context->set_sent_headers(true);
    // The rewriter's own headers, its Vary tokens among them, are now in
    // place; what changes after this is the server's.
    DaemonHtmlRecording* recording = context->daemon_html_recording();
    if (recording != nullptr && recording->needs_final_headers()) {
      recording->SetBaselineHeaders(apache_header_lines(request));
    }
  }

  const GoogleString& output = context->output();
  if (output.empty()) {
    return nullptr;
  }

  // Use the rewritten content. Create in heap since output will
  // be emptied for reuse.
  apr_bucket* bucket = apr_bucket_heap_create(
      output.data(), output.size(), nullptr, request->connection->bucket_alloc);
  context->clear();
  return bucket;
}

// Apache's pool-based cleanup is not effective on process shutdown.  To allow
// valgrind to report clean results, we must take matters into our own hands.
// We employ a statically allocated class object and rely on its destructor to
// get a reliable cleanup hook.  I am, in general, strongly opposed to this
// sort of technique, and it violates the C++ Style Guide:
//   http://google-styleguide.googlecode.com/svn/trunk/cppguide.xml
// However, it is not possible to use valgrind to track memory leaks
// in our Apache module without this approach.
//
// We also need this context hold any data needed for statistics
// collected in advance of the creation of the Statistics object, such
// as directives-parsing time.
class ApacheProcessContext {
 public:
  ApacheProcessContext() : apache_cmds_(nullptr) {
    ApacheRewriteDriverFactory::Initialize();
    InstallCommands();
  }

  ~ApacheProcessContext() {
    // Redirect LOG() to stderr before destroying the factory. Worker threads
    // may still be executing tasks (e.g., InPlaceRewriteContext::Harvest) that
    // call LOG() during the factory shutdown sequence. Without this, those
    // LOG() calls crash in spdlog::logger::sink_it_() because spdlog's global
    // logger (a Meyer's singleton) may already be destroyed by the time this
    // static destructor runs (static destruction order fiasco).
    pagespeed_logging::ShutDownLogging();

    // We must delete the factory before ProcessContext's dtor is called, which
    // terminates the protobuf libraries.  It is unsafe to free our structures
    // after the protobuf library has been shut down.
    //
    // Similarly, the ApacheRewriteDriverFactory destructor may involve the
    // shutdown process, and we want that to happen before we clean up various
    // globals in ApacheRewriteDriverFactory::Terminate, as they may still
    // be needed. For example, the SHM segment table is required for shutting
    // down SHM stats.
    factory_.reset(nullptr);

    ApacheRewriteDriverFactory::Terminate();
    delete[] apache_cmds_;
    log_message_handler::ShutDown();
  }

  void InstallCommands();
  void FillInApacheCommand(StringPiece option_name,
                           RewriteOptions::OptionScope scope,
                           const char* help_text, command_rec* cmd);

  ApacheRewriteDriverFactory* factory(server_rec* server) {
    // We are not mutex-protecting the factory-creation for now as the
    // server_rec initialization loop appears to be single-threaded in
    // Apache.
    if (factory_.get() == nullptr) {
      factory_ = std::make_unique<ApacheRewriteDriverFactory>(
          process_context_, server, kModPagespeedVersion);
      factory_->Init();
    }
    return factory_.get();
  }

  // The factory if the configuration created one, else nullptr.
  ApacheRewriteDriverFactory* existing_factory() { return factory_.get(); }

  // Checks cmd to see if it's process scope, and if so if it's used in an
  // incorrect context, returning an error message if so.
  const char* CheckProcessScope(const cmd_parms* cmd, bool* is_process_scope);

  std::unique_ptr<ApacheRewriteDriverFactory> factory_;
  // Process-scoped static variable cleanups, mainly for valgrind.
  ProcessContext process_context_;
  command_rec* apache_cmds_;

  using VhostCommandHandlingMap = std::map<const command_rec*, VHostHandling>;
  VhostCommandHandlingMap vhost_command_handling_map_;
  StringVector cmd_names_;
};
ApacheProcessContext apache_process_context;

using AddTimeFn = void (ApacheServerContext::*)(int64);

class ScopedTimer {
 public:
  ScopedTimer(ApacheServerContext* server_context, AddTimeFn add_time_fn)
      : server_context_(server_context),
        add_time_fn_(add_time_fn),
        start_time_us_(timer_.NowUs()) {}

  ~ScopedTimer() {
    int64 delta_us = timer_.NowUs() - start_time_us_;
    (server_context_->*add_time_fn_)(delta_us);
  }

 private:
  ApacheServerContext* server_context_;
  AddTimeFn add_time_fn_;
  AprTimer timer_;
  int64 start_time_us_;
};

bool is_html_like(const request_rec* request) {
  const ContentType* type = MimeTypeToContentType(request->content_type);
  return type != nullptr && type->IsHtmlLike();
}

// DaemonHtmlCache.  These functions read the response and never write it:
// the body through the bytes process_bucket has already read for the
// rewriter, the headers through copies of the tables.  The HTML cache engine
// (daemon_html_engine.h) classifies what they record; on this server it only
// reports, in report-only and serve-variants alike.  With the option off the
// only thing that runs is the mode check in instaweb_out_filter.
apr_status_t delete_daemon_html_recording(void* data) {
  delete static_cast<DaemonHtmlRecording*>(data);
  return APR_SUCCESS;
}

// Lines whose changes after the module's own are not the server's: what the
// module itself sets, and the digest compression drops with the body it
// changes.  The engine's final view ignores the transport lines itself.
bool is_module_or_digest_line(StringPiece name) {
  return StringCaseEqual(name, "Content-MD5") ||
         StringCaseEqual(name, kModPagespeedHeader) ||
         StringCaseEqual(name, kPageSpeedHeader);
}

// Both header tables, in order, as copies, without the lines above when
// `skip_module_lines`.
DaemonHtmlHeaderLines apache_header_lines(const request_rec* request,
                                          bool skip_module_lines) {
  DaemonHtmlHeaderLines lines;
  for (const apr_table_t* table :
       {request->headers_out, request->err_headers_out}) {
    const apr_array_header_t* header = apr_table_elts(table);
    const apr_table_entry_t* entries =
        reinterpret_cast<const apr_table_entry_t*>(header->elts);
    for (int i = 0; i < header->nelts; ++i) {
      if (entries[i].key != nullptr && entries[i].val != nullptr &&
          !(skip_module_lines && is_module_or_digest_line(entries[i].key))) {
        lines.emplace_back(entries[i].key, entries[i].val);
      }
    }
  }
  return lines;
}

// The baseline and the final view: compared with each other, so the lines
// whose changes do not count are left out of both.
DaemonHtmlHeaderLines apache_header_lines(const request_rec* request) {
  return apache_header_lines(request, /*skip_module_lines=*/true);
}

// The origin's headers as they arrive at the rewriter's input: before
// build_context_for_request removes Content-Encoding and Content-Length, and
// before the module adds its signature and its Vary tokens.  mod_expires and
// mod_headers have not run yet at this position;
// instaweb_daemon_html_report_filter brings their changes in.  Returns
// nullptr when the engine records nothing (no optimizer library, counted
// there as unavailable).
DaemonHtmlRecording* start_daemon_html_recording(request_rec* request,
                                                 ApacheServerContext* context) {
  DaemonHtmlEngine* engine = context->daemon_html_engine();
  if (engine == nullptr) return nullptr;
  DaemonHtmlRequestFacts facts;
  facts.method = request->method == nullptr ? "" : request->method;
  // Not unparsed_uri: a request in absolute form carries the scheme and host
  // there, and the policy reads a path.
  facts.path =
      request->parsed_uri.path == nullptr ? "" : request->parsed_uri.path;
  if (request->parsed_uri.query != nullptr) {
    StrAppend(&facts.path, "?", request->parsed_uri.query);
  }
  facts.host = request->hostname == nullptr ? "" : request->hostname;
  const apr_table_t* in = request->headers_in;
  facts.has_authorization =
      apr_table_get(in, HttpAttributes::kAuthorization) != nullptr ||
      apr_table_get(in, "Proxy-Authorization") != nullptr;
  facts.authenticated_user = request->user != nullptr;
  facts.has_cookie = apr_table_get(in, HttpAttributes::kCookie) != nullptr ||
                     apr_table_get(in, HttpAttributes::kCookie2) != nullptr;
  const char* request_cc = apr_table_get(in, HttpAttributes::kCacheControl);
  if (request_cc != nullptr) {
    facts.has_cache_control = true;
    facts.cache_control = request_cc;
  }
  // Nothing blocking is ever handed to a port by a recording that only
  // reports.
  std::unique_ptr<DaemonHtmlRecording> recording =
      engine->StartRecording(facts, /*port=*/nullptr);
  if (recording == nullptr) return nullptr;
  recording->set_status(request->status);
  DaemonHtmlHeaderLines origin =
      apache_header_lines(request, /*skip_module_lines=*/false);
  // Apache keeps these two in the request until its header filter.
  if (request->content_type != nullptr &&
      apr_table_get(request->headers_out, HttpAttributes::kContentType) ==
          nullptr) {
    origin.emplace_back(HttpAttributes::kContentType, request->content_type);
  }
  if (request->content_encoding != nullptr &&
      apr_table_get(request->headers_out, HttpAttributes::kContentEncoding) ==
          nullptr) {
    origin.emplace_back(HttpAttributes::kContentEncoding,
                        request->content_encoding);
  }
  recording->SetOriginHeaders(std::move(origin));
  DaemonHtmlRecording* raw = recording.release();
  apr_pool_cleanup_register(request->pool, raw, delete_daemon_html_recording,
                            apr_pool_cleanup_null);
  return raw;
}

// Runs at AP_FTYPE_CONTENT_SET + 1, added only for a response being captured,
// before build_context_for_request adds instaweb_fix_headers_filter at the same
// type, so it runs first of the two: after mod_expires, mod_headers and
// mod_deflate, before the module rewrites the caching headers.  What those
// stages changed since the module's own changes reaches the recording as its
// final view, apart from what compression and the module itself change: Vary
// is compared without Accept-Encoding and an ETag without a -gzip suffix, so
// a Vary or ETag line from mod_headers counts.  It passes every brigade on
// untouched.
//
// Design limit: a header that a filter sets after this one is not seen, so a
// filter added at AP_FTYPE_CONTENT_SET + 1 after this one, or one at
// AP_FTYPE_PROTOCOL or later, does not reach the report.
apr_status_t instaweb_daemon_html_report_filter(ap_filter_t* filter,
                                                apr_bucket_brigade* bb) {
  DaemonHtmlRecording* recording =
      static_cast<DaemonHtmlRecording*>(filter->ctx);
  request_rec* request = filter->r;
  if (recording->needs_final_headers()) {
    recording->NoteFinalHeaders(apache_header_lines(request));
  }
  bool saw_eos = false;
  for (apr_bucket* bucket = APR_BRIGADE_FIRST(bb);
       bucket != APR_BRIGADE_SENTINEL(bb); bucket = APR_BUCKET_NEXT(bucket)) {
    if (APR_BUCKET_IS_EOS(bucket)) saw_eos = true;
  }
  const apr_status_t status = ap_pass_brigade(filter->next, bb);
  if (saw_eos) recording->Finish(request->connection->aborted != 0);
  return status;
}

// Builds a new context for an HTML request, returning NULL if we decide
// that we should not handle the request for various reasons.
// TODO(sligocki): Move most of these checks into non-Apache specific code.
InstawebContext* build_context_for_request(request_rec* request) {
  ApacheServerContext* server_context =
      InstawebContext::ServerContextFromServerRec(request->server);
  // Escape ASAP if we're in unplugged mode or proxy_all_requests_mode.
  if (server_context->global_config()->unplugged() ||
      server_context->global_config()->proxy_all_requests_mode()) {
    return nullptr;
  }

  if (request->unparsed_uri == nullptr) {
    // TODO(jmarantz): consider adding Debug message if unparsed_uri is NULL,
    // possibly of request->the_request which was non-null in the case where
    // I found this in the debugger.
    ap_log_rerror(APLOG_MARK, APLOG_ERR, APR_SUCCESS, request,
                  "Request not rewritten because: "
                  "request->unparsed_uri == NULL");
    return nullptr;
  }

  ap_log_rerror(APLOG_MARK, APLOG_DEBUG, APR_SUCCESS, request,
                "ModPagespeed OutputFilter called for request %s",
                request->unparsed_uri);

  // Requests with a non-NULL main pointer are internal requests created by
  // apache (or other modules in apache).  We don't need to process them.
  // E.g. An included header file will be processed as a separate request.
  // mod_pagespeed needs to process only the "completed" page with the header
  // inlined, not the separate header request.
  // See http://httpd.apache.org/dev/apidoc/apidoc_request_rec.html for
  // request documentation.
  if (request->main != nullptr) {
    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, APR_SUCCESS, request,
                  "Request not rewritten because: request->main != NULL");
    return nullptr;
  }

  // TODO(sligocki): Should we rewrite any other statuses?
  // Maybe 206 Partial Content?
  // TODO(sligocki): Make this decision inside PSOL.
  if (request->status != 200) {
    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, APR_SUCCESS, request,
                  "Request not rewritten because: request->status != 200 "
                  "(was %d)",
                  request->status);
    return nullptr;
  }

  const ContentType* content_type =
      MimeTypeToContentType(request->content_type);
  // TODO(sligocki): Move inside PSOL.
  if (content_type == nullptr) {
    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, APR_SUCCESS, request,
                  "Request not rewritten because: request->content_type was "
                  "not a recognized type (was %s)",
                  request->content_type);
    return nullptr;
  }

  // Check if pagespeed optimization is applicable.
  // TODO(sligocki): Put other checks in this function.
  if (!check_pagespeed_applicable(request, *content_type)) {
    return nullptr;
  }

  // Check if mod_instaweb has already rewritten the HTML.  If the server is
  // setup as both the original and the proxy server, mod_pagespeed filter may
  // be applied twice. To avoid this, skip the content if it is already
  // optimized by mod_pagespeed.
  // Check both tables: an upstream FastCGI or CGI origin can deliver the
  // header in err_headers_out.
  // TODO(sligocki): Move inside PSOL.
  if (apr_table_get(request->headers_out, kModPagespeedHeader) != nullptr ||
      apr_table_get(request->err_headers_out, kModPagespeedHeader) != nullptr) {
    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, APR_SUCCESS, request,
                  "Request not rewritten because: X-Mod-Pagespeed header set.");
    return nullptr;
  }

  InstawebHandler instaweb_handler(request);
  const RewriteOptions* options = instaweb_handler.options();

  // The constructor above (via ComputeCustomOptions -> GetQueryOptions)
  // already scanned and stripped any PageSpeed/ModPagespeed option headers
  // from the in-memory response_headers_ copy, regardless of what happens
  // next. Write that stripped state back to the real Apache request now, up
  // front, so every early return below -- the invalid-URL one included --
  // leaves with the option headers removed, not just the paths that go on
  // to rewrite the response.
  instaweb_handler.RemoveStrippedResponseHeadersFromApacheRequest();

  const GoogleUrl& stripped_gurl = instaweb_handler.stripped_gurl();
  if (!stripped_gurl.IsWebValid()) {
    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, APR_SUCCESS, request,
                  "Request not rewritten because: invalid URL %s.",
                  stripped_gurl.spec_c_str());
    return nullptr;
  }

  // TODO(sligocki): Move inside PSOL.
  // Is PageSpeed turned off? We check after parsing query params so that
  // they can override .conf settings.
  if (!options->enabled()) {
    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, APR_SUCCESS, request,
                  "Request not rewritten because: PageSpeed is off");
    return nullptr;
  }

  GoogleString final_url;
  stripped_gurl.Spec().CopyToString(&final_url);

  // TODO(sligocki): Move inside PSOL.
  // Do Disallow statements restrict us from rewriting this URL?
  if (!options->IsAllowed(final_url)) {
    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, APR_SUCCESS, request,
                  "Request not rewritten because: ModPagespeedDisallow");
    return nullptr;
  }

  InstawebContext* context = new InstawebContext(
      request, instaweb_handler.ReleaseRequestHeaders(), *content_type,
      server_context, final_url, instaweb_handler.request_context(),
      instaweb_handler.pagespeed_query_params(),
      instaweb_handler.pagespeed_option_cookies(),
      instaweb_handler.use_custom_options(), *options);

  // TODO(sligocki): Move inside PSOL.
  InstawebContext::ContentEncoding encoding = context->content_encoding();
  if ((encoding == InstawebContext::kGzip) ||
      (encoding == InstawebContext::kDeflate)) {
    // Unset the content encoding because the InstawebContext will decode the
    // content before parsing.
    apr_table_unset(request->headers_out, HttpAttributes::kContentEncoding);
    apr_table_unset(request->err_headers_out, HttpAttributes::kContentEncoding);
  } else if (encoding == InstawebContext::kOther) {
    // We don't know the encoding, so we cannot rewrite the HTML.
    const char* encoding =
        apr_table_get(request->headers_out, HttpAttributes::kContentEncoding);
    ap_log_rerror(APLOG_MARK, APLOG_INFO, APR_SUCCESS, request,
                  "Request not rewritten because: Content-Encoding is "
                  "unsupported (was %s)",
                  encoding);
    // We need to cleanup the rewrite driver; the pool will clean up the
    // context object.
    context->Finish();
    return nullptr;
  }

  // Set X-Mod-Pagespeed header.
  // TODO(sligocki): Move inside PSOL.
  apr_table_set(request->headers_out, kModPagespeedHeader,
                options->EffectiveXHeaderValue().c_str());

  apr_table_unset(request->headers_out, HttpAttributes::kContentLength);
  apr_table_unset(request->headers_out, "Content-MD5");
  apr_table_unset(request->headers_out, HttpAttributes::kContentEncoding);

  // Make sure compression is enabled for this response.
  ap_add_output_filter("DEFLATE", nullptr, request, request->connection);

  if (options->modify_caching_headers()) {
    ap_add_output_filter(kModPagespeedFixHeadersName, nullptr, request,
                         request->connection);
  }

  ap_log_rerror(APLOG_MARK, APLOG_DEBUG, APR_SUCCESS, request,
                "Request accepted.");
  return context;
}

// This returns 'false' if the output filter should stop its loop over
// the brigade and return an error.
bool process_bucket(ap_filter_t* filter, request_rec* request,
                    InstawebContext* context, apr_bucket* bucket,
                    apr_status_t* return_code) {
  // Remove the bucket from the old brigade. We will create new bucket or
  // reuse the bucket to insert into the new brigade.
  APR_BUCKET_REMOVE(bucket);
  *return_code = APR_SUCCESS;
  apr_bucket_brigade* context_bucket_brigade = context->bucket_brigade();
  apr_bucket* new_bucket = nullptr;
  if (!APR_BUCKET_IS_METADATA(bucket)) {
    const char* buf = nullptr;
    size_t bytes = 0;
    *return_code = apr_bucket_read(bucket, &buf, &bytes, APR_BLOCK_READ);
    if (*return_code == APR_SUCCESS) {
      if (context->daemon_html_recording() != nullptr) {
        context->daemon_html_recording()->AddBody(buf, bytes);
      }
      new_bucket = rewrite_html(context, request, REWRITE, buf, bytes);
    } else {
      ap_log_rerror(APLOG_MARK, APLOG_ERR, *return_code, request,
                    "Reading bucket failed (rcode=%d)", *return_code);
      apr_bucket_delete(bucket);
      return false;
    }
    // Processed the bucket, now delete it.
    apr_bucket_delete(bucket);
    if (new_bucket != nullptr) {
      // Output of a page larger than a window: pass it on now, without a
      // FLUSH bucket, and stop on an error as a flush does.
      APR_BRIGADE_INSERT_TAIL(context_bucket_brigade, new_bucket);
      *return_code = ap_pass_brigade(filter->next, context_bucket_brigade);
      if (*return_code != APR_SUCCESS) {
        return false;
      }
    }
  } else if (APR_BUCKET_IS_EOS(bucket)) {
    if (context->daemon_html_recording() != nullptr) {
      context->daemon_html_recording()->set_body_ended();
    }
    new_bucket = rewrite_html(context, request, FINISH, nullptr, 0);
    if (new_bucket != nullptr) {
      APR_BRIGADE_INSERT_TAIL(context_bucket_brigade, new_bucket);
    }
    // Insert the EOS bucket to the new brigade.
    APR_BRIGADE_INSERT_TAIL(context_bucket_brigade, bucket);
    // OK, we have seen the EOS. Time to pass it along down the chain.
    *return_code = ap_pass_brigade(filter->next, context_bucket_brigade);
    return false;
  } else if (APR_BUCKET_IS_FLUSH(bucket) &&
             context->FlushWouldSendEditableHeaders()) {
    // Nothing has been parsed, so there is nothing of ours to flush, and
    // passing the bucket on would send the headers before the rule_packs
    // filter can edit them. The next flush or the end of the response sends
    // them, edited. The origin's headers are read now: a response that ends
    // without a body byte must still write them back, not an empty set.
    context->PopulateHeaders(request);
    apr_bucket_delete(bucket);
  } else if (APR_BUCKET_IS_FLUSH(bucket)) {
    new_bucket = rewrite_html(context, request, FLUSH, nullptr, 0);
    if (new_bucket != nullptr) {
      APR_BRIGADE_INSERT_TAIL(context_bucket_brigade, new_bucket);
    }
    APR_BRIGADE_INSERT_TAIL(context_bucket_brigade, bucket);
    // OK, Time to flush, pass it along down the chain.
    *return_code = ap_pass_brigade(filter->next, context_bucket_brigade);
    if (*return_code != APR_SUCCESS) {
      return false;
    }
  } else {
    // TODO(lsong): remove this log.
    ap_log_rerror(APLOG_MARK, APLOG_INFO, APR_SUCCESS, request,
                  "Unknown meta data");
    APR_BRIGADE_INSERT_TAIL(context_bucket_brigade, bucket);
  }
  return true;
}

// Entry point from Apache for streaming HTML-like content.
apr_status_t instaweb_out_filter(ap_filter_t* filter, apr_bucket_brigade* bb) {
  // Do nothing if there is nothing, and stop passing to other filters.
  if (APR_BRIGADE_EMPTY(bb)) {
    return APR_SUCCESS;
  }

  request_rec* request = filter->r;
  InstawebContext* context = static_cast<InstawebContext*>(filter->ctx);

  // Initialize per-request context structure.  Note that instaweb_out_filter
  // may get called multiple times per HTTP request, and this occurs only
  // on the first call.
  if (context == nullptr) {
    ApacheServerContext* report_context =
        InstawebContext::ServerContextFromServerRec(request->server);
    // As on nginx, the report covers a 200 of an HTML type that carries a
    // body: a recording of anything else, a HEAD included, is not started.
    // serve-variants records the same responses and, until this server
    // stores and serves, only reports them.
    const bool report =
        report_context->global_config()->daemon_html_cache() !=
            RewriteOptions::kDaemonHtmlCacheOff &&
        request->main == nullptr && request->status == HTTP_OK &&
        !request->header_only && is_html_like(request);
    DaemonHtmlRecording* recording =
        report ? start_daemon_html_recording(request, report_context)
               : nullptr;
    // Added before build_context_for_request adds the fix-headers filter.
    ap_filter_t* report_filter =
        recording == nullptr
            ? nullptr
            : ap_add_output_filter(kModPagespeedDaemonHtmlReportName,
                                   recording, request, request->connection);
    context = build_context_for_request(request);
    if (context == nullptr) {
      // The pool frees a recording that was never finished.
      if (report_filter != nullptr) ap_remove_output_filter(report_filter);
      ap_remove_output_filter(filter);
      return ap_pass_brigade(filter->next, bb);
    }
    filter->ctx = context;
    if (recording != nullptr) {
      context->set_daemon_html_recording(recording);
      recording->SetBaselineHeaders(apache_header_lines(request));
    }
  }

  ApacheServerContext* server_context = context->apache_server_context();
  ScopedTimer timer(server_context, &ApacheServerContext::AddHtmlRewriteTimeUs);

  apr_status_t return_code = APR_SUCCESS;
  while (!APR_BRIGADE_EMPTY(bb)) {
    apr_bucket* bucket = APR_BRIGADE_FIRST(bb);
    if (!process_bucket(filter, request, context, bucket, &return_code)) {
      return return_code;
    }
  }

  apr_brigade_cleanup(bb);
  return return_code;
}

// This is called when mod_pagespeed rewrites HTML, so that headers related to
// caching maybe updated correctly.
//
// We expect this to run after mod_headers and mod_expires, triggered
// by the call to ap_add_output_filter(kModPagespeedFixHeadersName...)
// in build_context_for_request.
// This method is not called if users set "ModifyCachingHeaders off".
//
// This function removes any Expires, Last-Modified or Etag settings added
// by the user's .conf files.
//
// This function also replaces the Cache-Control header with a no-cache value
// if one of the following conditions are met:
// 1) Downstream caching integration is disabled.
// 2) Downstream caching is enabled, downstream cache beaconing key is
//    configured, and the value of the PS-ShouldBeacon header on the request
//    matches the configured beaconing key.
// It retains the original Cache-Control header in all other cases, which
// correspond to downstream caching integration being enabled and the page
// being served not being instrumented for beaconing.
apr_status_t instaweb_fix_headers_filter(ap_filter_t* filter,
                                         apr_bucket_brigade* bb) {
  request_rec* request = filter->r;

  // Escape ASAP if we're in unplugged mode.
  ApacheServerContext* server_context =
      InstawebContext::ServerContextFromServerRec(request->server);
  if (server_context->global_config()->unplugged()) {
    ap_remove_output_filter(filter);
    return ap_pass_brigade(filter->next, bb);
  }

  // TODO(sligocki): Consider moving inside PSOL.  Note that this is a
  // little thornier than it looks because PSOL headers are different
  // from Apache headers and to share code easily we'd have to
  // translate.  We can do that easily but it seems like a waste of
  // CPU time since this will occur on every HTML request.  However,
  // there is hope in pagespeed/kernel/http/caching_headers.h, which
  // provides an abstracted interface to any underlying representation.
  // We could build on that pattern to do platform-independent header
  // manipulations in PSOL rather than direct calls to ResponseHeaders.
  //
  // TODO(jmarantz): merge this logic with that in
  // ResponseHeaders::CacheControlValuesToPreserve and
  // ServerContext::ApplyInputCacheControl
  DisableCachingRelatedHeaders(request);

  InstawebHandler instaweb_handler(request);
  const RewriteOptions* options = instaweb_handler.options();
  if (!options->IsDownstreamCacheIntegrationEnabled()) {
    // Downstream cache integration is not enabled. Disable original
    // Cache-Control headers.
    DisableCacheControlHeader(request);
  } else {
    // Downstream cache integration is enabled. If a rebeaconing key has been
    // configured and there is a ShouldBeacon header with the correct key,
    // disable original Cache-Control headers so that the instrumented page is
    // served out with no-cache.
    const char* should_beacon =
        apr_table_get(request->headers_in, kPsaShouldBeacon);
    if (options->MatchesDownstreamCacheRebeaconingKey(should_beacon)) {
      DisableCacheControlHeader(request);
    }
  }

  // Remove ourselves so that we only run once per request.
  ap_remove_output_filter(filter);
  return ap_pass_brigade(filter->next, bb);
}

// Entry point from Apache for recording resources for IPRO.
// Modeled loosely after ap_content_length_filter() in protocol.c.
// TODO(sligocki): Perhaps we can merge this filter with ApacheToMpsFilter().
apr_status_t instaweb_in_place_filter(ap_filter_t* filter,
                                      apr_bucket_brigade* bb) {
  // Do nothing if there is nothing, and stop passing to other filters.
  if (APR_BRIGADE_EMPTY(bb)) {
    return APR_SUCCESS;
  }

  request_rec* request = filter->r;

  // Escape ASAP if we're in unplugged mode.
  ApacheServerContext* server_context =
      InstawebContext::ServerContextFromServerRec(request->server);
  if (server_context->global_config()->unplugged()) {
    ap_remove_output_filter(filter);
    return ap_pass_brigade(filter->next, bb);
  }

  // This should always be set by handle_as_in_place() in instaweb_handler.cc.
  // The concrete recorder depends on which in-place substrate this server is
  // on; these filters drive the lifecycle and do not care which.
  IproRecorder* recorder = static_cast<IproRecorder*>(filter->ctx);
  CHECK(recorder != nullptr);

  // Feed the body to the recorder.  The walk stops at EOS, or as soon as the
  // recorder no longer wants the body -- a response it will not record, HTML
  // above all, is not read at all, so its FILE buckets stay FILE buckets.
  const apr_status_t return_code =
      FeedBrigadeToInPlaceRecorder(bb, recorder, [request, recorder]() {
        ResponseHeaders response_headers(recorder->http_options());
        ApacheRequestToResponseHeaders(*request, &response_headers, nullptr);

        // The content-type is likely to be missing from the Apache response
        // headers until AP_FTYPE_PROTOCOL, and this filter is run earlier, at
        // AP_FTYPE_CONTENT_SET + 1.  However, the content-type may be in
        // the request object, and if we populate it into the response headers
        // early, we can check for uninteresting content-types in
        // ConsiderResponseHeaders and avoid the overhead of collecting the
        // content into memory.
        if ((request->content_type != nullptr) &&
            (response_headers.DetermineContentType() == nullptr)) {
          response_headers.Replace(HttpAttributes::kContentType,
                                   request->content_type);
        }
        // The content-type is not the only fact that reaches headers_out
        // only at AP_FTYPE_PROTOCOL: the origin's own Content-Encoding can
        // sit in err_headers_out or request->content_encoding at this point,
        // and an encoded origin response must be refused from THESE headers,
        // before the body is buffered, rather than recorded as the identity
        // original its bytes are not.
        AddOriginContentEncoding(*request, &response_headers);

        recorder->ConsiderResponseHeaders(IproRecorder::kPreliminaryHeaders,
                                          &response_headers);
      });
  if (return_code != APR_SUCCESS) {
    ap_log_rerror(APLOG_MARK, APLOG_ERR, return_code, request,
                  "Reading bucket failed (rcode=%d)", return_code);
    return return_code;
  }

  // instaweb_in_place_check_headers_filter cleans up the recorder.
  return ap_pass_brigade(filter->next, bb);
}

int AddOriginCachingHeaderCallback(void* rec, const char* key,
                                   const char* value) {
  static_cast<ResponseHeaders*>(rec)->Add(key, value);
  return 1;
}

// Runs at AP_FTYPE_CONTENT_SET - 3, ahead of mod_expires (CONTENT_SET - 2)
// and mod_headers (CONTENT_SET), and hands the recorder the caching headers
// as the handler set them -- for a proxied response, as the origin sent them.
// Whether the response may be stored, and for how long, is decided from
// these rather than from what those directives write for browsers.  Both
// tables, since a handler may put its headers in either.  Once per response.
apr_status_t instaweb_in_place_origin_caching_filter(ap_filter_t* filter,
                                                     apr_bucket_brigade* bb) {
  if (APR_BRIGADE_EMPTY(bb)) {
    return APR_SUCCESS;
  }
  request_rec* request = filter->r;
  IproRecorder* recorder = static_cast<IproRecorder*>(filter->ctx);
  if (recorder != nullptr) {
    ResponseHeaders origin;
    for (int i = 0; i < IproRecorder::kNumOriginCachingHeaderNames; ++i) {
      const char* name = IproRecorder::kOriginCachingHeaderNames[i];
      apr_table_do(AddOriginCachingHeaderCallback, &origin,
                   request->headers_out, name, nullptr);
      if (request->err_headers_out != nullptr) {
        apr_table_do(AddOriginCachingHeaderCallback, &origin,
                     request->err_headers_out, name, nullptr);
      }
    }
    recorder->SetOriginCachingHeaders(origin);
  }
  ap_remove_output_filter(filter);
  return ap_pass_brigade(filter->next, bb);
}

// An origin may send the RequestOptionOverride token in its response to have
// its own option headers honoured.  The token is a credential.  The option
// scan in the HTML output filter drops it from the responses it reads; this
// filter drops it from every response, including the ones that filter never
// sees (other content types and statuses, proxied and in-place resources).
// It runs after that filter and before in-place recording.
apr_status_t instaweb_strip_option_token_filter(ap_filter_t* filter,
                                                apr_bucket_brigade* bb) {
  request_rec* request = filter->r;
  const GoogleString names[] = {
      StrCat(RewriteQuery::kModPagespeed,
             RewriteOptions::kRequestOptionOverride),
      StrCat(RewriteQuery::kPageSpeed, RewriteOptions::kRequestOptionOverride),
  };
  for (const GoogleString& name : names) {
    apr_table_unset(request->headers_out, name.c_str());
    if (request->err_headers_out != nullptr) {
      apr_table_unset(request->err_headers_out, name.c_str());
    }
  }
  ap_remove_output_filter(filter);
  return ap_pass_brigade(filter->next, bb);
}

void instaweb_insert_strip_option_token_filter(request_rec* request) {
  if (request->main == nullptr) {
    ap_add_output_filter(kModPagespeedStripOptionTokenName, nullptr, request,
                         request->connection);
  }
}

// Runs immediately after mod_headers and other filters which muck with the
// headers, but before headers are finalized.
//
// Sets cache-control to include s-maxage, and tells the recorder what the
// original cache control was so it can properly save it with the rest of the
// headers once they're finalized.
apr_status_t instaweb_in_place_fix_headers_filter(ap_filter_t* filter,
                                                  apr_bucket_brigade* bb) {
  request_rec* request = filter->r;
  ApacheServerContext* server_context =
      InstawebContext::ServerContextFromServerRec(request->server);
  if (!server_context->global_config()->unplugged()) {
    IproRecorder* recorder = static_cast<IproRecorder*>(filter->ctx);
    if (recorder != nullptr) {
      int s_maxage_sec =
          server_context->global_config()->EffectiveInPlaceSMaxAgeSec();
      if (s_maxage_sec != -1) {
        bool had_original = false;
        GoogleString original_cache_control;
        // Reads Cache-Control from headers_out and err_headers_out, and
        // leaves a single merged header in headers_out.
        if (ApplyInPlaceSMaxAge(request, s_maxage_sec, &had_original,
                                &original_cache_control)) {
          // We modified the cache control header; save the original first.
          recorder->SaveCacheControl(
              had_original ? original_cache_control.c_str() : nullptr);
        }
      }
    }
  }

  // Remove ourselves so that we only run once per request.
  ap_remove_output_filter(filter);
  return ap_pass_brigade(filter->next, bb);
}

// Runs after instaweb_in_place_fix_headers_filter and after headers are
// finalized.  We have to run instaweb_in_place_filter earlier because by now
// the response body is gzipped.  TODO(sligocki): Run as a single filter after
// mod_headers, etc. using an inflater to gunzip the file? Or storing the
// gzipped version in cache?
//
// The sole purpose of this filter is to pass the finalized headers to
// recorder.
apr_status_t instaweb_in_place_check_headers_filter(ap_filter_t* filter,
                                                    apr_bucket_brigade* bb) {
  // Do nothing if there is nothing, and stop passing to other filters.
  if (APR_BRIGADE_EMPTY(bb)) {
    return APR_SUCCESS;
  }

  request_rec* request = filter->r;
  ApacheServerContext* server_context =
      InstawebContext::ServerContextFromServerRec(request->server);
  // Escape ASAP if we're in unplugged mode.
  if (server_context->global_config()->unplugged()) {
    ap_remove_output_filter(filter);
    return ap_pass_brigade(filter->next, bb);
  }

  // This should always be set by Instaweb::HandleAsInPlace().
  IproRecorder* recorder = static_cast<IproRecorder*>(filter->ctx);

  // We do not want to call Done until the last bucket comes in, because the
  // instaweb_in_place_filter needs to record the body, so iterate to EOS
  // bucket if present.  If it's not present, we'll get called again until it is
  // present.
  for (apr_bucket* bucket = APR_BRIGADE_FIRST(bb);
       (recorder != nullptr) && (bucket != APR_BRIGADE_SENTINEL(bb));
       bucket = APR_BUCKET_NEXT(bucket)) {
    if (AP_BUCKET_IS_ERROR(bucket)) {
      // Also seen here when a filter between the two in-place filters ended
      // the response abnormally: the body is incomplete, so record nothing.
      recorder->Fail();
    } else if (APR_BUCKET_IS_EOS(bucket)) {
      ResponseHeaders response_headers(recorder->http_options());

      // Note: Since we're post-AP_FTYPE_PROTOCOL the error headers and regular
      // headers have already been merged in Apache, so no need to gather
      // the error headers here.
      ApacheRequestToResponseHeaders(*request, &response_headers, nullptr);

      // Note: For some reason Apache never actually sets the Date header in
      // request->headers_out, but without it set we consider it uncacheable,
      // so we set it here.
      // TODO(sligocki): Perhaps we should stop requiring Date header to
      // consider resources cacheable?
      AprTimer timer;
      response_headers.SetDate(timer.NowMs());
      response_headers.ComputeCaching();

      // We now have the final headers.  If they don't let us cache then we'll
      // abort even though we've already buffered up the whole resource.
      InstawebHandler::AboutToBeDoneWithRecorder(request, recorder);
      // Deletes recorder
      recorder->DoneAndSetHeaders(&response_headers,
                                  !request->connection->aborted);

      // https://github.com/apache/incubator-pagespeed-mod/issues/1191
      // identifies a case where there must have been two EOS markers passed
      // into this function, either because there were two in the brigade or
      // because this filter was called twice.  To defend against this, null the
      // dead recorder pointer and the reference in filter->ctx.
      recorder = nullptr;
      filter->ctx = nullptr;
    }
  }

  return ap_pass_brigade(filter->next, bb);
}

void pagespeed_child_init(apr_pool_t* pool, server_rec* server_list) {
  // Create PageSpeed context used by instaweb rewrite-driver.  This is
  // per-process, so we initialize all the server's context by iterating the
  // server lists in server->next.
  bool need_init = true;
  for (server_rec* server = server_list; server != nullptr;
       server = server->next) {
    ApacheServerContext* server_context =
        InstawebContext::ServerContextFromServerRec(server);
    if (!server_context->global_config()->unplugged()) {
      if (need_init) {
        ApacheRewriteDriverFactory* factory =
            apache_process_context.factory(server_list);
        factory->ChildInit();
        need_init = false;
      }
      DCHECK(server_context != nullptr);
      DCHECK(server_context->initialized());
    }
  }
}

// True when this process runs as root and Apache has parsed a non-root
// User and Group for its children to switch to: then what the root process
// creates is for that user.
bool apache_children_switch_user() {
  // (Apache will not switch from current euid if it's not root --- see
  //  http://httpd.apache.org/docs/2.2/mod/mpm_common.html#user).
  if (geteuid() != 0) {
    return false;
  }

  // .user_id, .group_id default to -1 if they haven't been parsed yet.
  return (unixd_config.user_id != 0) &&
         (unixd_config.user_id != static_cast<uid_t>(-1)) &&
         (unixd_config.group_id != 0) &&
         (unixd_config.group_id != static_cast<gid_t>(-1));
}

bool give_dir_apache_user_permissions(ApacheRewriteDriverFactory* factory,
                                      const GoogleString& path) {
  if (!apache_children_switch_user()) {
    return true;
  }

  // The directory must be a plain directory for the children's user; one
  // configured as a symbolic link is left for the operator.
  GoogleString why;
  switch (SetOwnerThroughDescriptor(path, unixd_config.user_id,
                                    unixd_config.group_id,
                                    OwnerMatch::kUserAndGroup, &why)) {
    case SetOwnerResult::kDone:
      return true;
    case SetOwnerResult::kSymbolicLink:
      factory->message_handler()->Message(
          kWarning,
          "Not setting ownership of %s: it is a symbolic link; make its "
          "target writable by the server's User yourself",
          path.c_str());
      return true;
    case SetOwnerResult::kFailed:
      break;
  }
  factory->message_handler()->Message(
      kError, "Unable to set proper ownership of %s (%s)", path.c_str(),
      why.c_str());
  return false;
}

// If we are running as root, hands over the ownership of data directories
// we made to the eventual Apache uid/gid.
bool give_apache_user_permissions(ApacheRewriteDriverFactory* factory) {
  const StringSet& created_dirs = factory->created_directories();
  bool ret = true;
  for (const auto& created_dir : created_dirs) {
    ret &= give_dir_apache_user_permissions(factory, created_dir);
  }
  return ret;
}

// Create directory and make sure permissions are set correctly so that
// Apache processes can read and write from it.
const char* init_dir(ApacheServerContext* server_context, apr_pool_t* pool,
                     const char* directive_name, const char* path) {
  const char* ret = nullptr;
  if (*path != '/') {
    ret = apr_pstrcat(pool, directive_name, " ", path,
                      " must start with a slash.", NULL);
  } else {
    if (!server_context->InitPath(path) ||
        !give_apache_user_permissions(server_context->apache_factory())) {
      ret = apr_pstrcat(pool, "Directory ", path,
                        " could not be created "
                        "or permissions could not be set.",
                        NULL);
    }
  }
  return ret;
}

// Hook from Apache for a configuration test (-t), which runs no post_config:
// a directory the configuration created before Apache knew its User and
// Group is given to them here, as post_config would, so a test as root does
// not leave one the children cannot use.  Opens no cache volume.
void pagespeed_test_config(apr_pool_t* pconf, server_rec* server_list) {
  ApacheRewriteDriverFactory* factory =
      apache_process_context.existing_factory();
  if (factory != nullptr) {
    give_apache_user_permissions(factory);
  }
}

// Hook from Apache for initialization after config is read.
// Initialize statistics, set appropriate directory permissions, etc.
int pagespeed_post_config(apr_pool_t* pool, apr_pool_t* plog, apr_pool_t* ptemp,
                          server_rec* server_list) {
  // This routine is complicated by the fact that statistics use inter-process
  // mutexes and have static data, which co-mingles poorly with this otherwise
  // re-entrant module.  The situation that gets interesting is when there are
  // multiple VirtualHosts, some of which have statistics enabled and some of
  // which don't.  We don't want the behavior to be order-dependent so we
  // do multiple passes.
  //
  // TODO(jmarantz): test VirtualHost

  ApacheRewriteDriverFactory* factory =
      apache_process_context.factory(server_list);

  // Which server records state a ServerName in the configuration just read
  // (rather than carry one httpd derived), read once from httpd's parsed
  // configuration while it and the records belong to the same configuration;
  // the serving and admin paths only look records up.  A restart re-reads
  // the configuration and comes through here again with new records.  The
  // walk starts at this cycle's own main record, `server_list`, which is
  // the record the factory was created for.
  DCHECK(server_list == factory->main_server());
  factory->set_stated_server_names(
      ApacheStatedServerNamesFromTree(ap_conftree, server_list));

  // Thread-count resolution is deferred to here: ap_mpm_query() cannot report
  // the MPM's threading model, the configured ThreadsPerChild, or the child
  // count the thread-count policy divides by until the configuration has been
  // processed, and it can't answer at all if the MPM module is loaded after
  // mod_pagespeed.  This must run before anything reads the thread counts,
  // and after directives have been parsed so an explicit NumRewriteThreads /
  // NumExpensiveRewriteThreads still wins.
  factory->FinalizeThreadCounts();
  factory->caches()->set_thread_limit(factory->LookupThreadLimit() +
                                      factory->num_rewrite_threads() +
                                      factory->num_expensive_rewrite_threads());

  // A server record whose configuration sets no CacheFragment gets a keyspace
  // of its own, derived from its own names and addresses.  Records can share
  // a server context (a virtual host with no ModPagespeed directive uses the
  // main server's), so each context takes the value of the record it was
  // made for, and a request served for another record applies that record's
  // value to its own options (InstawebHandler::ComputeCustomOptions).  The
  // value is withheld from the options payload, so the log is where it can
  // be read.
  {
    const GoogleString main_fragment =
        InstawebContext::ServerContextFromServerRec(server_list)
            ->global_config()
            ->cache_fragment();
    std::map<const server_rec*, GoogleString> site_fragments;
    for (server_rec* server = server_list; server != nullptr;
         server = server->next) {
      ApacheServerContext* context =
          InstawebContext::ServerContextFromServerRec(server);
      const GoogleString& fragment = context->global_config()->cache_fragment();
      if (!fragment.empty()) {
        if (server != server_list && fragment == main_fragment) {
          factory->message_handler()->Message(
              kWarning,
              "virtual host %s uses the server-wide CacheFragment and shares "
              "cache entries with every host that does; set one in its "
              "<VirtualHost> block to separate them.",
              server->server_hostname != nullptr ? server->server_hostname
                                                 : "?");
        }
        continue;
      }
      SiteIdentity identity;
      AppendApacheSiteIdentity(server, server_list,
                               factory->stated_server_names(), &identity);
      site_fragments[server] = DeriveSiteCacheFragment(identity);
      ap_log_error(
          APLOG_MARK, APLOG_NOTICE, APR_SUCCESS, server,
          "mod_pagespeed: server %s uses cache fragment %s",
          server->server_hostname != nullptr ? server->server_hostname : "?",
          site_fragments[server].c_str());
    }
    for (server_rec* server = server_list; server != nullptr;
         server = server->next) {
      ApacheServerContext* context =
          InstawebContext::ServerContextFromServerRec(server);
      auto found = site_fragments.find(server);
      if (found != site_fragments.end() && context->server() == server) {
        context->global_config()->set_cache_fragment(found->second);
      }
    }
    factory->set_site_cache_fragments(std::move(site_fragments));
  }

  std::vector<SystemServerContext*> server_contexts;
  std::set<ApacheServerContext*> server_contexts_covered;
  ApacheServerContext* base_context =
      InstawebContext::ServerContextFromServerRec(server_list);
  for (server_rec* server = server_list; server != nullptr;
       server = server->next) {
    ApacheServerContext* server_context =
        InstawebContext::ServerContextFromServerRec(server);
    if (server_contexts_covered.insert(server_context).second) {
      CHECK(server_context != nullptr);
      server_contexts.push_back(server_context);
    }

    if (factory->use_per_vhost_statistics() && server != server_list &&
        server_context == base_context) {
      factory->message_handler()->Message(
          kWarning,
          "ModPagespeedUsePerVHostStatistics is on, but virtual host %s "
          "carries no ModPagespeed directive of its own and therefore shares "
          "the main server's statistics; add a ModPagespeed directive (for "
          "example 'ModPagespeed on') inside its <VirtualHost> block to give "
          "it separate statistics.",
          server->server_hostname != nullptr ? server->server_hostname : "?");
    }

    // We also want propagate all the per-process options to each vhost. The
    // normal merge in merge_server_config isn't enough since that merges the
    // non-per process things from a dummy ServerContext corresponding to the
    // top-level config, not ApacheRewriteDriverFactory::default_options where
    // the process scope options go.
    //
    // We do this here rather than merge_server_config since we want to touch
    // the ServerContext corresponding to the top-level/non-<VirtualHost>
    // block, too.
    server_context->global_config()->MergeOnlyProcessScopeOptions(
        *factory->default_options());
  }

  GoogleString error_message;
  int error_index = -1;
  Statistics* global_statistics = nullptr;
  factory->PostConfig(server_contexts, &error_message, &error_index,
                      &global_statistics);
  if (error_index != -1) {
    ApacheServerContext* server_context =
        dynamic_cast<ApacheServerContext*>(server_contexts[error_index]);
    server_context->message_handler()->Message(
        kError,
        "mod_pagespeed is enabled. %s:"
        " defn_name=%s"
        " defn_line_number=%d"
        " server_hostname=%s"
        " port=%d",
        error_message.c_str(), server_context->server()->defn_name,
        server_context->server()->defn_line_number,
        server_context->server()->server_hostname,
        server_context->server()->port);
    return HTTP_INTERNAL_SERVER_ERROR;
  }

  // Resolve every server's relationship with the optimizer daemon before any
  // request can be served, so the serving path only ever reads a verdict.
  //
  // A refusal here fails the whole start, which is the point: the single
  // condition that refuses is a cache-volume sizing divergence between this
  // module and the daemon, and that divergence is SILENT at run time -- the
  // two sides would open different volume files, share nothing, and look
  // healthy while optimizing nothing.  Every other daemon problem degrades to
  // in-place optimization off with one loud line, and the server starts.
  //
  // Virtual hosts that name the same socket and volume share one adapter
  // (#627): one handle on the volume per child, not one per virtual host.
  // The pool is this pass's own and ends with it; the server contexts keep
  // their adapters, and the children inherit them unopened.  Every pass --
  // the second start-up read, a graceful restart, a re-read -- builds a new
  // one, so no verdict carries over from an earlier configuration.
  DaemonAdapterPool daemon_adapter_pool;
  for (SystemServerContext* system_server_context : server_contexts) {
    ApacheServerContext* server_context =
        dynamic_cast<ApacheServerContext*>(system_server_context);
    CHECK(server_context != nullptr);
    if (!server_context->RunDaemonStartupCheck(&daemon_adapter_pool)) {
      return HTTP_INTERNAL_SERVER_ERROR;
    }
  }

  // chown any directories we created. We may have to do it here in
  // post_config since we may not have our user/group yet during parse
  // (example: Fedora 11).
  //
  // We also have to do it during the parse, however, since if we're started
  // to /just/ check the config with -t (as opposed to doing it as a
  // preliminary for a proper startup) we won't get a post_config!
  give_apache_user_permissions(factory);

  // If no shared-mem statistics are enabled, then init using the default
  // NullStatistics.
  if (global_statistics == nullptr) {
    ApacheRewriteDriverFactory::InitStats(factory->statistics());
  }

  // The cache volumes RootInit is about to open (creating them on a fresh
  // directory) are created for the children's user.
  if (apache_children_switch_user()) {
    factory->caches()->set_cache_file_owner(unixd_config.user_id,
                                            unixd_config.group_id);
  }

  factory->RootInit();

  return APACHE_OK;
}

// Here log transaction will wait for all the asynchronous resource fetchers to
// finish.
apr_status_t pagespeed_log_transaction(request_rec* request) {
  return DECLINED;
}

// Make sure that requests from ourselves don't get marked as coming from
// this server, as we don't want them matching 'Require local' or any other
// rule that admits the server's own addresses: the module fetches its own
// resources from the address the original request arrived on, whoever the
// original client was.
int pagespeed_modify_request(request_rec* r) {
  // Escape ASAP if we're in unplugged mode.
  ApacheServerContext* server_context =
      InstawebContext::ServerContextFromServerRec(r->server);
  if (server_context->global_config()->unplugged()) {
    return APACHE_OK;
  }
  ApacheTreatSelfFetchAsRemote(r);
  return APACHE_OK;
}

// This function is a callback and it declares what
// other functions should be called for request
// processing and configuration requests. This
// callback function declares the Handlers for
// other events.
void mod_pagespeed_register_hooks(apr_pool_t* pool) {
  // Enable logging using pagespeed style
  log_message_handler::Install(pool);

  // Use instaweb to handle generated resources.
  ap_hook_handler(InstawebHandler::instaweb_handler, nullptr, nullptr,
                  APR_HOOK_FIRST - 1);

  // Try to provide more accurate IP information for requests we create.
  // After mod_remoteip, which would otherwise replace the address given to
  // a self-fetch with one taken from a forwarding header.
  static const char* const kRunAfterRemoteIp[] = {"mod_remoteip.c", nullptr};
  ap_hook_post_read_request(pagespeed_modify_request, kRunAfterRemoteIp,
                            nullptr, APR_HOOK_FIRST);

  // We register our output filter at (AP_FTYPE_RESOURCE + 1) so that
  // mod_pagespeed runs after mod_include.  See Issue
  // http://github.com/apache/incubator-pagespeed-mod/issues/182
  // and httpd/src/modules/filters/mod_include.c, which initializes
  // server-side-includes with ap_register_output_filter(...AP_FTYPE_RESOURCE).
  ap_register_output_filter(kModPagespeedFilterName, instaweb_out_filter,
                            nullptr,
                            static_cast<ap_filter_type>(AP_FTYPE_RESOURCE + 1));

  // For HTML rewrites, we must apply our caching semantics later
  // in the filter-chain than mod_headers or mod_expires.  See:
  //   APACHE_DIST/src/modules/metadata/mod_headers.c:857
  //         --> mod_headers is installed at AP_FTYPE_CONTENT_SET
  //   APACHE_DIST/src/modules/metadata/mod_expires.c:554
  //         --> mod_expires is installed at AP_FTYPE_CONTENT_SET - 2
  // Thus we can override its settings by installing at +1.
  ap_register_output_filter(
      kModPagespeedFixHeadersName, instaweb_fix_headers_filter, nullptr,
      static_cast<ap_filter_type>(AP_FTYPE_CONTENT_SET + 1));

  // Run after contents are set, but before mod_deflate, which runs at
  // AP_FTYPE_CONTENT_SET.  We use a separate filter rather
  // than just adding logic to instaweb_fix_headers_filter because the
  // recorder gets passed in as the filter->ctx when it is registered
  // in InstawebHandler::HandleAsInPlace.
  ap_register_output_filter(
      kModPagespeedInPlaceFilterName, instaweb_in_place_filter, nullptr,
      static_cast<ap_filter_type>(AP_FTYPE_CONTENT_SET - 1));

  // Ahead of mod_expires (AP_FTYPE_CONTENT_SET - 2) and mod_headers
  // (AP_FTYPE_CONTENT_SET): the caching headers as the handler set them, for
  // the recorder's storage decision.
  ap_register_output_filter(
      kModPagespeedInPlaceOriginCachingName,
      instaweb_in_place_origin_caching_filter, nullptr,
      static_cast<ap_filter_type>(AP_FTYPE_CONTENT_SET - 3));

  // After the HTML output filter (AP_FTYPE_RESOURCE + 1), which reads the
  // token, and ahead of every in-place recording filter.  Added to every
  // response, error responses included.
  ap_register_output_filter(
      kModPagespeedStripOptionTokenName, instaweb_strip_option_token_filter,
      nullptr, static_cast<ap_filter_type>(AP_FTYPE_CONTENT_SET - 4));
  ap_hook_insert_filter(instaweb_insert_strip_option_token_filter, nullptr,
                        nullptr, APR_HOOK_MIDDLE);
  ap_hook_insert_error_filter(instaweb_insert_strip_option_token_filter,
                              nullptr, nullptr, APR_HOOK_MIDDLE);

  // DaemonHtmlCache reads the final view here: mod_expires, mod_headers and
  // mod_deflate have run, instaweb_fix_headers_filter has not.  Added per
  // response, only when the option is on.  Headers set by a filter that runs
  // after it (one added later at this type, or at AP_FTYPE_PROTOCOL) are not
  // seen.
  ap_register_output_filter(
      kModPagespeedDaemonHtmlReportName, instaweb_daemon_html_report_filter,
      nullptr, static_cast<ap_filter_type>(AP_FTYPE_CONTENT_SET + 1));
  // Run after headers are set by mod_headers, mod_expires, etc. and
  // after Content-Type has been set (which appears to be at
  // AP_FTYPE_PROTOCOL).  We cannot simply collect the bytes at
  // AP_FTYPE_PROTOCOL+1 because, it appears, at that time the headers
  // have been serialized into the content, and it's rather embarassing
  // to have to rescan for the end of the headers.
  ap_register_output_filter(kModPagespeedInPlaceCheckHeadersName,
                            instaweb_in_place_check_headers_filter, nullptr,
                            static_cast<ap_filter_type>(AP_FTYPE_PROTOCOL + 1));
  // For IPRO recording, run after headers are set by mod_headers or
  // mod_expires, but early enough that we can still change them to set
  // s-maxage.
  ap_register_output_filter(
      kModPagespeedInPlaceFixHeadersName, instaweb_in_place_fix_headers_filter,
      nullptr, static_cast<ap_filter_type>(AP_FTYPE_CONTENT_SET + 1));

  ap_hook_post_config(pagespeed_post_config, nullptr, nullptr, APR_HOOK_MIDDLE);
  ap_hook_test_config(pagespeed_test_config, nullptr, nullptr, APR_HOOK_MIDDLE);
  ap_hook_child_init(pagespeed_child_init, nullptr, nullptr, APR_HOOK_LAST);
  ap_hook_log_transaction(pagespeed_log_transaction, nullptr, nullptr,
                          APR_HOOK_LAST);

  // mod_rewrite damages the URLs written by mod_pagespeed.  See
  // Issues 63 & 72.  To defend against this, we must either add
  // additional mod_rewrite rules to exclude pagespeed resources or
  // pre-scan for pagespeed resources before mod_rewrite runs and copy
  // the URL somewhere safe (a request->note) before mod_rewrite
  // corrupts it.  The latter is easier to deploy as it does not
  // require users editing their rewrite rules for mod_pagespeed.
  // mod_rewrite registers at APR_HOOK_FIRST.  We'd like to leave
  // space for user modules at APR_HOOK_FIRST-1, so we go to
  // APR_HOOK_FIRST - 2.
  ap_hook_translate_name(InstawebHandler::save_url_hook, nullptr, nullptr,
                         APR_HOOK_FIRST - 2);

  // By default, apache imposes limitations on URL segments of around
  // 256 characters that appear to correspond to filename limitations.
  // To prevent that, we hook map_to_storage for our own purposes.
  ap_hook_map_to_storage(InstawebHandler::instaweb_map_to_storage, nullptr,
                         nullptr, APR_HOOK_FIRST - 2);
}

apr_status_t pagespeed_child_exit(void* data) {
  ApacheServerContext* server_context = static_cast<ApacheServerContext*>(data);
  if (server_context->PoolDestroyed()) {
    // Route logging to stderr before tearing down the factory/worker pools and
    // the Apache log pool. The spdlog path is already crash-safe via the
    // process-immortal held logger; this additionally protects the
    // registered-sink path (SendToSinks -> ApacheGLogSink -> ap_log_perror on
    // the Apache pool) from a worker that LOG()s while that pool is being torn
    // down. Runs during apr pool cleanup, i.e. before C++ static destructors.
    pagespeed_logging::ShutDownLogging();

    // When the last server context is destroyed, it's important that we also
    // clean up the factory, so we don't end up with dangling pointers in case
    // we are not unloaded fully on a config check (e.g. on Ubuntu 11).
    apache_process_context.factory_.reset(nullptr);
  }
  return APR_SUCCESS;
}

void* mod_pagespeed_create_server_config(apr_pool_t* pool, server_rec* server) {
  // Note: when statically loaded server->module_config is NULL when
  // initializing and this is called for the first time.
  ApacheServerContext* server_context =
      server->module_config == nullptr
          ? nullptr
          : InstawebContext::ServerContextFromServerRec(server);

  if (server_context == nullptr) {
    ApacheRewriteDriverFactory* factory =
        apache_process_context.factory(server);
    server_context = factory->MakeApacheServerContext(server);
    apr_pool_cleanup_register(pool, server_context, pagespeed_child_exit,
                              apr_pool_cleanup_null);
  }
  return server_context;
}

const char kEnabledEnumHint[] = " on|off|unplugged";

const char* ParseHint(RewriteOptions::EnabledEnum x) {
  return kEnabledEnumHint;
}

template <typename OptType, typename Options>
const char* ParseOption(Options* options, cmd_parms* cmd,
                        void (Options::*fn)(OptType val), const char* arg) {
  const char* ret = nullptr;
  OptType parsed;
  if (RewriteOptions::ParseFromString(arg, &parsed)) {
    (options->*fn)(parsed);
  } else {
    ret = apr_pstrcat(cmd->pool, cmd->directive->directive, ParseHint(parsed),
                      NULL);
  }
  return ret;
}

template <class Options>
const char* ParseIntBoundedOption(Options* options, cmd_parms* cmd,
                                  void (Options::*fn)(int val), const char* arg,
                                  int lower, int upper) {
  int val;
  const char* ret = nullptr;
  if (StringToInt(arg, &val) && val >= lower && val <= upper) {
    (options->*fn)(val);
  } else {
    GoogleString message = absl::StrFormat(
        " must specify a 32-bit integer between %d and %d", lower, upper);
    ret = apr_pstrcat(cmd->pool, cmd->directive->directive, message.c_str(),
                      NULL);
  }
  return ret;
}

void warn_deprecated(cmd_parms* cmd, const char* remedy) {
  ap_log_error(APLOG_MARK, APLOG_WARNING, APR_SUCCESS, cmd->server,
               "%s is deprecated.  %s", cmd->directive->directive, remedy);
}

// Determines the Option structure into which to write a parsed directive.
// If the directive was parsed from the default pagespeed.conf file then
// we will write the information into the factory's RewriteOptions. In that
// case, it's also possible that an overlay config for SPDY is being parsed
// (for backwards compat), in which case we will store it inside the directive
// object.
//
// However, if this was parsed from a Directory scope or .htaccess file then we
// will be using the RewriteOptions structure from a tree of ApacheConfig
// objects that is built up per-request.
//
// Returns NULL if successful, error string otherwise.
// Writes out the ApacheConfig* into *config_out.
static const char* CmdOptions(const cmd_parms* cmd, void* data,
                              ApacheConfig** config_out) {
  ApacheConfig* config = static_cast<ApacheConfig*>(data);
  if (config == nullptr) {
    // See if there is an overlay config.
    if (cmd->directive->data != nullptr) {
      config = static_cast<ApacheConfig*>(cmd->directive->data);
    } else {
      ApacheServerContext* server_context =
          InstawebContext::ServerContextFromServerRec(cmd->server);
      config = server_context->global_config();
    }
  } else {
    // If we're here, we are inside path-specific configuration, so we should
    // not see SPDY vs. non-SPDY distinction.
    if (cmd->directive->data != nullptr) {
      *config_out = nullptr;
      return kConditionalInDirectoryError;
    }
  }
  // Apache parses .htaccess files with cmd->pool == cmd->temp_pool, which
  // ap_check_cmd_context(NOT_IN_HTACCESS) tests.
  if (ap_check_cmd_context(const_cast<cmd_parms*>(cmd), NOT_IN_HTACCESS) !=
      nullptr) {
    config->mark_set_from_htaccess();
  }
  *config_out = config;
  return nullptr;
}

// This should be called for global options to see if they were used properly.
// In particular, it returns an error string if a global option is inside a
// <ModPagespeedIf. It also either warns or errors out if we're using a global
// option inside a virtual host, depending on "mode".  The rule itself is
// CheckProcessScopeUse, shared with the offline config linter.
//
// Returns NULL if successful, error string otherwise.
static char* CheckGlobalOption(const cmd_parms* cmd, VHostHandling mode,
                               MessageHandler* handler) {
  ProcessScopeUse use = CheckProcessScopeUse(cmd->directive->directive, mode,
                                             cmd->server->is_virtual != 0,
                                             cmd->directive->data != nullptr);
  if (!use.warning.empty()) {
    handler->MessageS(kWarning, use.warning);
  }
  if (!use.error.empty()) {
    return apr_pstrdup(cmd->pool, use.error.c_str());
  }
  return nullptr;
}

const char* ApacheProcessContext::CheckProcessScope(const cmd_parms* cmd,
                                                    bool* is_process_scope) {
  VhostCommandHandlingMap::const_iterator p =
      vhost_command_handling_map_.find(cmd->cmd);
  *is_process_scope = (p != vhost_command_handling_map_.end());
  const char* ret = nullptr;
  if (cmd->server->is_virtual || (cmd->directive->data != nullptr)) {
    if (*is_process_scope) {
      ret = CheckGlobalOption(cmd, p->second, factory_->message_handler());
    }
  }
  return ret;
}

// Returns true if standard parsing handled the option and sets *err_msg to NULL
// if OK, and to the error string managed in cmd->pool otherwise.
bool StandardParsingHandled(cmd_parms* cmd,
                            RewriteOptions::OptionSettingResult result,
                            const GoogleString& msg, const char** err_msg) {
  switch (result) {
    case RewriteOptions::kOptionOk:
      *err_msg = nullptr;  // No error.
      return true;
    case RewriteOptions::kOptionNameUnknown:
      // RewriteOptions didn't recognize the option, but we might do so
      // with our own code.
      return false;
    case RewriteOptions::kOptionValueInvalid:
      // The option is recognized, but the value is not. Return the error
      // message.
      *err_msg = apr_pstrdup(cmd->pool, msg.c_str());
      return true;
  }
  LOG(DFATAL) << "Should be unreachable";
  return true;
}

// Callback function that parses a single-argument directive.  This is called
// by the Apache config parser.
static const char* ParseDirective(cmd_parms* cmd, void* data, const char* arg) {
  ApacheServerContext* server_context =
      InstawebContext::ServerContextFromServerRec(cmd->server);
  ApacheRewriteDriverFactory* factory = server_context->apache_factory();
  MessageHandler* handler = factory->message_handler();
  StringPiece directive(cmd->directive->directive);
  StringPiece prefix(RewriteQuery::kModPagespeed);
  const char* ret = nullptr;
  ApacheConfig* config;
  ret = CmdOptions(cmd, data, &config);
  if (ret != nullptr) {
    return ret;
  }

  // We have "FileCachePath" mapped in gperf, but here we do more than just
  // setting the option. This must precede the call to SetOptionFromName which
  // would catch this directive but miss the call to
  // give_apache_user_permissions.
  if (StringCaseEqual(directive, kModPagespeedFileCachePath)) {
    ret = init_dir(server_context, cmd->pool, kModPagespeedFileCachePath, arg);
    if (ret == nullptr) {
      config->set_file_cache_path(arg);
    }
    return ret;
  }
  if (StringCaseEqual(directive, kModPagespeedLogDir)) {
    ret = init_dir(server_context, cmd->pool, kModPagespeedLogDir, arg);
    if (ret == nullptr) {
      config->set_log_dir(arg);
    }
    return ret;
  }

  // Rename deprecated options so lookup below will succeed.
  directive = CanonicalDirectiveName(directive);

  if (StringCaseStartsWith(directive, prefix)) {
    StringPiece option = directive.substr(prefix.size());
    GoogleString msg;

    bool use_global_config = false;
    // See if it's a global option, and perhaps not in place.
    ret = apache_process_context.CheckProcessScope(cmd, &use_global_config);
    if (ret != nullptr) {
      return ret;
    }
    // Options that are per-process are always parsed into
    // ApacheRewriteDriverFactory::default_options(), and then propagated
    // in the post-config hook (pagespeed_post_config).
    if (use_global_config) {
      config = ApacheConfig::DynamicCast(factory->default_options());
    }

    // See whether generic RewriteOptions name handling can figure this one out.
    // An .htaccess file is a directory-level source: parse an ExperimentSpec
    // there at directory scope, which refuses its fetch-steering components
    // as the fetch directives themselves are refused.
    RewriteOptions::OptionSettingResult result =
        (StringCaseEqual(option, RewriteOptions::kExperimentSpec) &&
         ap_check_cmd_context(cmd, NOT_IN_HTACCESS) != nullptr)
            ? config->ParseAndSetOptionFromNameWithScope(
                  option, arg, RewriteOptions::kDirectoryScope, &msg, handler)
            : config->ParseAndSetOptionFromName1(option, arg, &msg, handler);
    if (result == RewriteOptions::kOptionNameUnknown) {
      // RewriteOptions didn't know; try the driver factory.
      // TODO(morlovich): It may be cleaner to not have process-scope options
      // in RewriteOptions at all, but rather something RewriteDriverFactory
      // specific, as long as we can provide a painless way of integrating it
      // in the server and parsing it (areas where the current manual approach
      // fails).
      result = factory->ParseAndSetOption1(
          option, arg,
          !cmd->server->is_virtual,  // is_process_scope
          &msg, handler);
    }
    if (StandardParsingHandled(cmd, result, msg, &ret)) {
      return ret;
    }

    if (RewriteOptions::IsDeprecatedOptionName(option)) {
      warn_deprecated(cmd, "Please remove it from your configuration.");
      return nullptr;
    }
  }

  // Options which we handle manually.
  if (StringCaseEqual(directive, RewriteQuery::kModPagespeed)) {
    ret = ParseOption<RewriteOptions::EnabledEnum>(
        static_cast<RewriteOptions*>(config), cmd, &RewriteOptions::set_enabled,
        arg);
  } else {
    ret = apr_pstrcat(cmd->pool, "Unknown directive ",
                      directive.as_string().c_str(), NULL);
  }

  return ret;
}

// Recursively walks the configuration we've parsed inside a
// <ModPagespeedIf> block, checking to make sure it's sane, and stashing
// pointers to the overlay ApacheConfig's we will use once Apache actually
// bothers calling our ParseDirective* methods. Returns NULL if OK, error string
// on error.
static const char* ProcessParsedScope(ApacheServerContext* server_context,
                                      ap_directive_t* root, bool for_spdy) {
  for (ap_directive_t* cur = root; cur != nullptr; cur = cur->next) {
    StringPiece directive(cur->directive);
    if (!StringCaseStartsWith(directive, RewriteQuery::kModPagespeed)) {
      return kConditionalOnlyOurDirectivesError;
    }
    if (StringCaseStartsWith(directive, kModPagespeedIf)) {
      return kConditionalNestedError;
    }

    if (cur->first_child != nullptr) {
      const char* kid_result =
          ProcessParsedScope(server_context, cur->first_child, for_spdy);
      if (kid_result != nullptr) {
        return kid_result;
      }
    }

    // Store the appropriate config to use in the ap_directive_t's
    // module data pointer, so we can retrieve it in CmdOptions when executing
    // parsing callback for it.
    cur->data = for_spdy ? server_context->SpdyConfigOverlay()
                         : server_context->NonSpdyConfigOverlay();
  }
  return nullptr;  // All OK.
}

// Callback that parses <ModPagespeedIf>. Unlike with ParseDirective*, we're
// supposed to make a new directive tree, and return it out via *mconfig. It
// will have its directives parsed by Apache at some point later.
static const char* ParseScope(cmd_parms* cmd, ap_directive_t** mconfig,
                              const char* arg) {
  StringPiece mode(arg);
  ApacheServerContext* server_context =
      InstawebContext::ServerContextFromServerRec(cmd->server);

  bool for_spdy = false;
  if (!ParseConditionalArgument(mode, &for_spdy)) {
    return kConditionalArgumentError;
  }

  // We need to manually check nesting since Apache's code doesn't seem to catch
  // violations for sections that parse blocks like <ModPagespeedIf>
  // (technically, commands with EXEC_ON_READ set).
  //
  // Unfortunately, ap_check_cmd_context doesn't work entirely
  // right, either, so we do our own handling inside CmdOptions as well; this is
  // kept mostly to produce a nice complaint in case someone puts
  // a <ModPagespeedIf> inside a <Limit>.
  const char* ret =
      ap_check_cmd_context(cmd, NOT_IN_DIR_LOC_FILE | NOT_IN_LIMIT);
  if (ret != nullptr) {
    return ret;
  }

  // Recursively parse this section. This is basically copy-pasted from
  // mod_version.c in Apache sources.
  ap_directive_t* parent = nullptr;
  ap_directive_t* current = nullptr;

  ret = ap_build_cont_config(cmd->pool, cmd->temp_pool, cmd, &current, &parent,
                             apr_pstrdup(cmd->pool, kModPagespeedIf));
  *mconfig = current;

  // Do our syntax checking and stash some ApacheConfig pointers.
  if (ret == nullptr) {
    ret = ProcessParsedScope(server_context, current, for_spdy);
  }

  return ret;
}

// Callback function that parses a two-argument directive.  This is called
// by the Apache config parser.
static const char* ParseDirective2(cmd_parms* cmd, void* data, const char* arg1,
                                   const char* arg2) {
  ApacheServerContext* server_context =
      InstawebContext::ServerContextFromServerRec(cmd->server);
  ApacheRewriteDriverFactory* factory = server_context->apache_factory();
  MessageHandler* handler = factory->message_handler();

  ApacheConfig* config;
  const char* ret = CmdOptions(cmd, data, &config);
  if (ret != nullptr) {
    return ret;
  }

  StringPiece prefix(RewriteQuery::kModPagespeed);
  StringPiece directive = cmd->directive->directive;
  // Go through generic path first.
  if (StringCaseStartsWith(directive, prefix)) {
    GoogleString msg;
    StringPiece option = directive.substr(prefix.size());
    RewriteOptions::OptionSettingResult result =
        config->ParseAndSetOptionFromName2(option, arg1, arg2, &msg, handler);
    if (result == RewriteOptions::kOptionNameUnknown) {
      // RewriteOptions didn't know; try the driver factory.
      result = factory->ParseAndSetOption2(
          option, arg1, arg2,
          !cmd->server->is_virtual,  // is_process_scope
          &msg, handler);
    }
    if (StandardParsingHandled(cmd, result, msg, &ret)) {
      return ret;
    }
  }

  return apr_pstrcat(cmd->pool, cmd->directive->directive,
                     " unknown directive.", NULL);
}

// Callback function that parses a three-argument directive.  This is called
// by the Apache config parser.
static const char* ParseDirective3(cmd_parms* cmd, void* data, const char* arg1,
                                   const char* arg2, const char* arg3) {
  ApacheServerContext* server_context =
      InstawebContext::ServerContextFromServerRec(cmd->server);
  ApacheRewriteDriverFactory* factory = server_context->apache_factory();
  MessageHandler* handler = factory->message_handler();
  ApacheConfig* config;
  const char* ret = CmdOptions(cmd, data, &config);
  if (ret != nullptr) {
    return ret;
  }

  StringPiece prefix(RewriteQuery::kModPagespeed);
  StringPiece directive = cmd->directive->directive;
  // Go through generic path first.
  if (StringCaseStartsWith(directive, prefix)) {
    GoogleString msg;
    RewriteOptions::OptionSettingResult result =
        config->ParseAndSetOptionFromName3(directive.substr(prefix.size()),
                                           arg1, arg2, arg3, &msg, handler);
    if (StandardParsingHandled(cmd, result, msg, &ret)) {
      return ret;
    }
  }

  return apr_pstrcat(cmd->pool, cmd->directive->directive,
                     " unknown directive.", NULL);
}

// Setting up Apache options is cumbersome for several reasons:
//
// 1. Apache appears to require the option table be entirely constructed
//    using static data.  So we cannot use helper functions to create the
//    helper table, so that we can populate it from another table.
// 2. You have to fill in the table with a function pointer with a K&R
//    C declaration that does not specify its argument types.  There appears
//    to be a type-correct union hidden behind an ifdef for
//    AP_HAVE_DESIGNATED_INITIALIZER, but that doesn't work.  It gives a
//    syntax error; its comments indicate it is there for Doxygen.
// 3. Although you have to pre-declare all the options, you need to again
//    dispatch based on the name of the options.  You could, conceivably,
//    provide a different function pointer for each call.  This might look
//    feasible with the 'mconfig' argument to AP_INIT_TAKE1, but mconfig
//    must be specified in a static initializer.  So it wouldn't be that easy
//    to, say, create a C++ object for each config parameter.
//
// Googling for AP_MODULE_DECLARE_DATA didn't shed any light on how to do this
// using a style suitable for programming after 1980.  So all we can do is make
// this a little less ugly with wrapper macros and helper functions.
//
// TODO(jmarantz): investigate usage of RSRC_CONF -- perhaps many of these
// options should be allowable inside a Directory or Location by ORing in
// ACCESS_CONF to RSRC_CONF.

// One command_rec initializer per directive kind; apache_directive::InfoFor
// gives each kind's req_override, and the static_asserts below tie its
// argument count and callback to the AP_INIT_* macro used here.
#define PAGESPEED_APACHE_CMD_kScope(name, help)                              \
  AP_INIT_TAKE1(name, reinterpret_cast<const char* (*)()>(ParseScope), NULL, \
                InfoFor(Kind::kScope).req_override, help)
#define PAGESPEED_APACHE_CMD_kDir1(name, help)                             \
  AP_INIT_TAKE1(name, reinterpret_cast<const char* (*)()>(ParseDirective), \
                NULL, InfoFor(Kind::kDir1).req_override, help)
#define PAGESPEED_APACHE_CMD_kServer1(name, help)                          \
  AP_INIT_TAKE1(name, reinterpret_cast<const char* (*)()>(ParseDirective), \
                NULL, InfoFor(Kind::kServer1).req_override, help)
#define PAGESPEED_APACHE_CMD_kDir2(name, help)                              \
  AP_INIT_TAKE2(name, reinterpret_cast<const char* (*)()>(ParseDirective2), \
                NULL, InfoFor(Kind::kDir2).req_override, help)
#define PAGESPEED_APACHE_CMD_kFetch2(name, help)                            \
  AP_INIT_TAKE2(name, reinterpret_cast<const char* (*)()>(ParseDirective2), \
                NULL, InfoFor(Kind::kFetch2).req_override, help)
#define PAGESPEED_APACHE_CMD_kFetch23(name, help)                            \
  AP_INIT_TAKE23(name, reinterpret_cast<const char* (*)()>(ParseDirective3), \
                 NULL, InfoFor(Kind::kFetch23).req_override, help)
#define PAGESPEED_APACHE_CMD_kServer2(name, help)                           \
  AP_INIT_TAKE2(name, reinterpret_cast<const char* (*)()>(ParseDirective2), \
                NULL, InfoFor(Kind::kServer2).req_override, help)
#define PAGESPEED_APACHE_CMD_kDir3(name, help)                              \
  AP_INIT_TAKE3(name, reinterpret_cast<const char* (*)()>(ParseDirective3), \
                NULL, InfoFor(Kind::kDir3).req_override, help)
#define PAGESPEED_APACHE_CMD(kind, name, help) \
  PAGESPEED_APACHE_CMD_##kind(name, help),

static_assert(kOrAll == OR_ALL, "httpd OR_ALL");
static_assert(kAccessConf == ACCESS_CONF, "httpd ACCESS_CONF");
static_assert(kRsrcConf == RSRC_CONF, "httpd RSRC_CONF");
static_assert(kExecOnRead == EXEC_ON_READ, "httpd EXEC_ON_READ");

constexpr bool KindIs(Kind kind, Args args, Handler handler) {
  return InfoFor(kind).args == args && InfoFor(kind).handler == handler;
}
static_assert(KindIs(Kind::kScope, Args::kTake1, Handler::kParseScope), "");
static_assert(KindIs(Kind::kDir1, Args::kTake1, Handler::kParseDirective), "");
static_assert(KindIs(Kind::kServer1, Args::kTake1, Handler::kParseDirective),
              "");
static_assert(KindIs(Kind::kDir2, Args::kTake2, Handler::kParseDirective2), "");
static_assert(KindIs(Kind::kFetch2, Args::kTake2, Handler::kParseDirective2),
              "");
static_assert(KindIs(Kind::kFetch23, Args::kTake23, Handler::kParseDirective3),
              "");
static_assert(KindIs(Kind::kServer2, Args::kTake2, Handler::kParseDirective2),
              "");
static_assert(KindIs(Kind::kDir3, Args::kTake3, Handler::kParseDirective3), "");

static const command_rec mod_pagespeed_filter_cmds[] = {
    PAGESPEED_APACHE_DIRECTIVE_TABLE(
        PAGESPEED_APACHE_CMD)};  // Do not null terminate; we use arraysize for initialization.

// We use pool-based cleanup for ApacheConfigs.  This is 99% effective.
// There is at least one base config which is created with create_dir_config,
// but whose pool is never freed.  To allow clean valgrind reports, we
// must delete that config too.  So we keep a backup cleanup-set for
// configs at end-of-process, and keep that set up-to-date when the
// pool deletion does work.
apr_status_t delete_config(void* data) {
  ApacheConfig* config = static_cast<ApacheConfig*>(data);
  delete config;
  return APR_SUCCESS;
}

// Function to allow all modules to create per directory configuration
// structures.
// dir is the directory currently being processed.
// Returns the per-directory structure created.
void* create_dir_config(apr_pool_t* pool, char* dir) {
  if (dir == nullptr) {
    return nullptr;
  }
  ThreadSystem* thread_system =
      apache_process_context.factory_->thread_system();
  ApacheConfig* config = new ApacheConfig(dir, thread_system);
  config->SetDefaultRewriteLevel(RewriteOptions::kCoreFilters);
  apr_pool_cleanup_register(pool, config, delete_config, apr_pool_cleanup_null);
  return config;
}

// Function to allow all modules to merge the per directory configuration
// structures for two directories.
// base_conf is the directory structure created for the parent directory.
// new_conf is the directory structure currently being processed.
// This function returns the new per-directory structure created
void* merge_dir_config(apr_pool_t* pool, void* base_conf, void* new_conf) {
  ApacheConfig* dir1 = static_cast<ApacheConfig*>(base_conf);
  ApacheConfig* dir2 = static_cast<ApacheConfig*>(new_conf);

  // To make it easier to debug the merged configurations, we store
  // the name of both input configurations as the description for
  // the merged configuration.
  ApacheConfig* dir3 = new ApacheConfig(
      StrCat("Combine(", dir1->description(), ", ", dir2->description(), ")"),
      dir1->thread_system());

  // Apache does not notify us when it is done adding directives to a
  // configuration, so we don't have a good opportunity to Freeze it
  // until it use used as a merge source.  We don't want to do this in
  // Merge because, for C++ cleanliness/readability, we want to let
  // Merge take a const RewrieOptions&, so we must Freeze at the call site.
  dir1->Freeze();
  dir3->Merge(*dir1);
  dir2->Freeze();
  dir3->Merge(*dir2);
  apr_pool_cleanup_register(pool, dir3, delete_config, apr_pool_cleanup_null);
  return dir3;
}

void* merge_server_config(apr_pool_t* pool, void* base_conf, void* new_conf) {
  ApacheServerContext* global_context =
      static_cast<ApacheServerContext*>(base_conf);
  ApacheServerContext* vhost_context =
      static_cast<ApacheServerContext*>(new_conf);

  std::unique_ptr<ApacheConfig> merged_config(
      global_context->global_config()->Clone());
  merged_config->Merge(*vhost_context->global_config());
  // Note that we don't need to do any special handling of cache paths here,
  // since it's all related to actually creating the directories + giving
  // permissions, so doing it at top-level is sufficient.
  vhost_context->reset_global_options(merged_config.release());

  // Merge the overlays, if any exist. (SPDY one no longer supported).
  if (global_context->has_non_spdy_config_overlay() ||
      vhost_context->has_non_spdy_config_overlay()) {
    std::unique_ptr<ApacheConfig> new_non_spdy_overlay(
        global_context->NonSpdyConfigOverlay()->Clone());
    new_non_spdy_overlay->Merge(*vhost_context->NonSpdyConfigOverlay());
    vhost_context->set_non_spdy_config_overlay(new_non_spdy_overlay.release());
  }

  return new_conf;
}

}  // namespace

}  // namespace net_instaweb

extern "C" {
// Export our module so Apache is able to load us.
// See http://gcc.gnu.org/wiki/Visibility for more information.
#if defined(__linux)
#pragma GCC visibility push(default)
#endif

// Declare and populate the module's data structure.  The
// name of this structure ('pagespeed_module') is important - it
// must match the name of the module.  This structure is the
// only "glue" between the httpd core and the module.
module AP_MODULE_DECLARE_DATA pagespeed_module = {
    // Only one callback function is provided.  Real
    // modules will need to declare callback functions for
    // server/directory configuration, configuration merging
    // and other tasks.
    STANDARD20_MODULE_STUFF,
    net_instaweb::create_dir_config,
    net_instaweb::merge_dir_config,
    net_instaweb::mod_pagespeed_create_server_config,
    net_instaweb::merge_server_config,
    nullptr,  // directives initialized via static ctor calling
              // InstallCommands().
    net_instaweb::mod_pagespeed_register_hooks,
};

#if defined(__linux)
#pragma GCC visibility pop
#endif
}  // extern "C"

namespace net_instaweb {

void ApacheProcessContext::FillInApacheCommand(
    StringPiece option_name, RewriteOptions::OptionScope scope,
    const char* help_text, command_rec* cmd) {
  // cmd_names_ is used as a backing store for option names w/ModPagespeed
  // tacked on to their front.
  cmd_names_.push_back(GoogleString());
  StrAppend(&cmd_names_.back(), "ModPagespeed", option_name);
  cmd->name = cmd_names_.back().c_str();
  cmd->func = reinterpret_cast<const char* (*)()>(ParseDirective);
  cmd->cmd_data = nullptr;
  OptionCommand command = CommandForOptionScope(scope);
  if (command.vhost != VHostHandling::kNotProcessScope) {
    vhost_command_handling_map_[cmd] = command.vhost;
  }
  cmd->req_override = command.req_override;
  cmd->args_how = TAKE1;
  cmd->errmsg = help_text;
}

// Runs via static construction and module-load time, so that it can
// install the Apache command-table in the module-record before Apache
// initializes the module.
void ApacheProcessContext::InstallCommands() {
  // Similar to the instantiation in ApacheConfig::AddProperties(), we
  // instantiate an ApacheConfig with a null thread system as we
  // are only using it to populate a static table which must be
  // established very early when mod_pagespeed.so is dynamically loaded,
  // to build the Apache directives parse-table before Apache attempts
  // to initialize our module.
  ApacheConfig config_template("install_commands", nullptr);
  const RewriteOptions::OptionBaseVector& v = config_template.all_options();
  const RewriteOptions::Properties* deprecated_properties =
      RewriteOptions::deprecated_properties();
  int num_cmds = std::size(net_instaweb::mod_pagespeed_filter_cmds);

  // Allocate memory for all the rewrite_options, even though we
  // will only initialize the ones with non-null help.  We could
  // also do a 2-pass to count how many we will allocate.  +1 to
  // leave room for a NULL terminator.
  apache_cmds_ =
      new command_rec[num_cmds + v.size() + deprecated_properties->size() + 1];
  memcpy(apache_cmds_, net_instaweb::mod_pagespeed_filter_cmds,
         num_cmds * sizeof(*apache_cmds_));
  command_rec* cmd = apache_cmds_ + num_cmds;

  // The same walk the offline config linter makes.
  ForEachOptionCommand(
      config_template,
      [this, &cmd](StringPiece option_name, RewriteOptions::OptionScope scope,
                   const char* help_text, bool deprecated) {
        FillInApacheCommand(option_name, scope, help_text, cmd);
        ++cmd;
      });

  // Add a null terminator.
  cmd->name = nullptr;
  cmd->func = nullptr;
  cmd->cmd_data = nullptr;
  cmd->req_override = 0;
  cmd->args_how = RAW_ARGS;
  cmd->errmsg = nullptr;
  pagespeed_module.cmds = apache_cmds_;
}

}  // namespace net_instaweb
