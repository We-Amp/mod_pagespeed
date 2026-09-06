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

#include "pagespeed/apache/instaweb_handler.h"

#include <cstddef>
#include <memory>

#include "base/logging.h"
#include "http_config.h"
#include "http_core.h"
#include "http_protocol.h"
#include "http_request.h"
#include "net/instaweb/http/public/async_fetch.h"
#include "net/instaweb/http/public/cache_url_async_fetcher.h"
#include "net/instaweb/http/public/request_context.h"
#include "net/instaweb/public/global_constants.h"
#include "net/instaweb/rewriter/public/domain_lawyer.h"
#include "net/instaweb/rewriter/public/option_context.h"
#include "net/instaweb/rewriter/public/resource_fetch.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/rewrite_query.h"
#include "net/instaweb/rewriter/public/rewrite_stats.h"
#include "net/instaweb/rewriter/public/static_asset_manager.h"
#include "pagespeed/apache/apache_config.h"
#include "pagespeed/apache/apache_fetch.h"
#include "pagespeed/apache/apache_logging_includes.h"
#include "pagespeed/apache/apache_message_handler.h"
#include "pagespeed/apache/apache_request_context.h"
#include "pagespeed/apache/apache_rewrite_driver_factory.h"
#include "pagespeed/apache/apache_server_context.h"
#include "pagespeed/apache/apache_writer.h"
#include "pagespeed/apache/apr_timer.h"
#include "pagespeed/apache/header_util.h"
#include "pagespeed/apache/in_place_not_modified_vary_fetch.h"
#include "pagespeed/apache/instaweb_context.h"
#include "pagespeed/apache/mod_instaweb.h"
#include "pagespeed/apache/simple_buffered_apache_fetch.h"
#include "pagespeed/apache/streaming_pagespeed_resource_fetch.h"
#include "pagespeed/automatic/proxy_fetch.h"
#include "pagespeed/automatic/proxy_interface.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/escaping.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/ref_counted_ptr.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/string_writer.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/http_options.h"
#include "pagespeed/kernel/http/request_headers.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "pagespeed/system/admin_site.h"
#include "pagespeed/system/daemon_ipro_recorder.h"
#include "pagespeed/system/daemon_record_arm.h"
#include "pagespeed/system/daemon_serve_arm.h"
#include "pagespeed/system/in_place_resource_recorder.h"
#include "pagespeed/system/ipro_record_gate.h"
#include "pagespeed/system/ipro_recorder.h"
#include "util_filter.h"

namespace net_instaweb {

namespace {

const char kAdminHandler[] = "pagespeed_admin";
const char kGlobalAdminHandler[] = "pagespeed_global_admin";
const char kStatisticsHandler[] = "mod_pagespeed_statistics";
const char kConsoleHandler[] = "pagespeed_console";
const char kGlobalStatisticsHandler[] = "mod_pagespeed_global_statistics";
const char kMessageHandler[] = "mod_pagespeed_message";

// Returns the textual client IP for `request`. Reads Apache 2.4's
// useragent_ip (mod_remoteip-aware). 1.1 targets Apache 2.4 only; the
// vendored httpd24 tree provides this field. Empty string if unavailable.
const char* ClientIpForAdminWarning(request_rec* request) {
  const char* ip = request->useragent_ip;
  return ip != nullptr ? ip : "";
}
const char kLogRequestHeadersHandler[] = "mod_pagespeed_log_request_headers";
const char kGenerateResponseWithOptionsHandler[] =
    "mod_pagespeed_response_options_handler";
const char kResourceUrlNote[] = "mod_pagespeed_resource";
const char kResourceUrlNo[] = "<NO>";
const char kResourceUrlYes[] = "<YES>";

// Set the maximum size we allow for processing a POST body. The limit of 128k
// is based on a best guess for the maximum size of beacons required for
// critical CSS.
// TODO(jud): Factor this out, potentially into an option, and pass the value to
// any filters using beacons with POST requests (CriticalImagesBeaconFilter for
// instance).
const size_t kMaxPostSizeBytes = 131072;

}  // namespace

InstawebHandler::InstawebHandler(request_rec* request)
    : request_(request),
      server_context_(
          InstawebContext::ServerContextFromServerRec(request->server)),
      rewrite_driver_(nullptr),
      driver_owned_(true),
      num_response_attributes_(0),
      fetch_(nullptr) {
  apache_request_context_ = server_context_->NewApacheRequestContext(request);
  request_context_.reset(apache_request_context_);

  // Global options
  options_ = server_context_->global_config();

  request_headers_ = std::make_unique<RequestHeaders>();
  ApacheRequestToRequestHeaders(*request, request_headers_.get());

  original_url_ = InstawebContext::MakeRequestUrl(*options_, request);
  apache_request_context_->set_url(original_url_);

  // Note: request_context_ must be initialized before ComputeCustomOptions().
  ComputeCustomOptions();
  request_context_->set_options(options_->ComputeHttpOptions());
}

InstawebHandler::~InstawebHandler() {
  // If fetch_ is null we either never tried to fetch anything or it took
  // ownership of itself after timing out.
  if (fetch_ != nullptr) {
    WaitForFetch();
    delete fetch_;
  }
  if (driver_owned_ && rewrite_driver_ != nullptr) {
    rewrite_driver_->Cleanup();
    rewrite_driver_ = nullptr;
  }
}

void InstawebHandler::WaitForFetch() {
  if (fetch_ == nullptr) {
    return;  // Nothing to wait for.
  }
  fetch_->Wait();
}

void InstawebHandler::DisownDriver() {
  DCHECK(rewrite_driver_ != nullptr);
  driver_owned_ = false;
}

// Makes a driver from the request_context and options.  Note that
// this can only be called once, as it potentially mutates the options
// as it transfers ownership of custom_options.
RewriteDriver* InstawebHandler::MakeDriver() {
  CHECK(fetch_ == nullptr) << "Call MakeDriver before MakeFetch";
  DCHECK(rewrite_driver_ == nullptr)
      << "We can only call MakeDriver once per InstawebHandler:"
      << original_url_;

  rewrite_driver_ =
      ResourceFetch::GetDriver(stripped_gurl_, custom_options_.release(),
                               server_context_, request_context_);

  // If there were custom options, the ownership of the memory has
  // now been transferred to the driver, but options_ still points
  // to the same object, so it can still be used as long as the
  // driver is alive.  However, for Karma, and in case some other
  // option-merging is added to the driver someday, let's use the
  // pointer from the driver now.
  options_ = ApacheConfig::DynamicCast(rewrite_driver_->options());
  return rewrite_driver_;
}

ApacheFetch* InstawebHandler::MakeFetch(const GoogleString& url, bool buffered,
                                        StringPiece debug_info) {
  DCHECK(fetch_ == nullptr);
  // ApacheFetch takes ownership of request_headers.
  RequestHeaders* request_headers = new RequestHeaders();
  ApacheRequestToRequestHeaders(*request_, request_headers);
  ApacheWriter* writer =
      new ApacheWriter(request_, server_context_->thread_system());
  if (rewrite_driver_ == nullptr) {
    MakeDriver();
  }
  fetch_ = new ApacheFetch(url, debug_info, rewrite_driver_, writer,
                           request_headers, request_context_, options_,
                           server_context_->message_handler());
  fetch_->set_buffered(buffered || options_->force_buffering());
  return fetch_;
}

/* static */
void InstawebHandler::send_out_headers_and_body(
    request_rec* request, const ResponseHeaders& response_headers,
    const GoogleString& output) {
  // We always disable downstream header filters when sending out
  // pagespeed resources, since we've captured them in the origin fetch.
  ResponseHeadersToApacheRequest(response_headers, request);
  request->status = response_headers.status_code();
  DisableDownstreamHeaderFilters(request);
  if (response_headers.status_code() == HttpStatus::kOK &&
      StreamingPagespeedResourceFetch::IsCompressibleContentType(
          request->content_type)) {
    // Make sure compression is enabled for this response.
    ap_add_output_filter("DEFLATE", nullptr, request, request->connection);
  }

  // Recompute the content-length, because the content may have changed.
  ap_set_content_length(request, output.size());
  // Send the body
  ap_rwrite(output.c_str(), output.size(), request);
}

// Evaluate custom_options based upon global_options, directory-specific
// options and query-param/request-header options. Stores computed options
// in custom_options_ if needed.  Sets options_ to point to the correct
// options to use.
void InstawebHandler::ComputeCustomOptions() {
  // Set directory specific options.  These will be the options for the
  // directory the resource is in, which under some configurations will be
  // different from the options for the directory that the referencing html is
  // in.  This can lead to us using different options here when regenerating
  // the resource than would be used if the resource were generated as part of
  // a rewrite kicked off by a request for the referencing html file.  This is
  // hard to fix, so instead we're documenting that you must make sure the
  // configuration for your resources matches the configuration for your html
  // files.
  {
    // In subscope so directory_options can't be used later by mistake since
    // it should only be used for computing the custom options.
    ApacheConfig* directory_options =
        static_cast<ApacheConfig*> ap_get_module_config(
            request_->per_dir_config, &pagespeed_module);
    if ((directory_options != nullptr) && directory_options->modified()) {
      custom_options_.reset(
          server_context_->apache_factory()->NewRewriteOptions());
      custom_options_->Merge(*options_);
      directory_options->Freeze();
      custom_options_->Merge(*directory_options);
    }
  }

  // TODO(sligocki): Move inside PSOL.
  // Merge in query-param or header-based options.
  // Note: We do not generally get response headers in the resource flow,
  // so NULL is passed in instead.
  stripped_gurl_.Reset(original_url_);

  // Note: options is not actually the final options for this request, but the
  // final options depend upon the ResponseHeaders, so these are the best we
  // have. As long as we don't allow changing implicit cache TTL in
  // ResponseHeaders, this should be fine.
  const RewriteOptions* directory_aware_options =
      (custom_options_.get() != nullptr) ? custom_options_.get() : options_;
  response_headers_ = std::make_unique<ResponseHeaders>(
      directory_aware_options->ComputeHttpOptions());

  // Copy headers_out and err_headers_out into response_headers.
  // Note that err_headers_out will come after the headers_out in the list of
  // headers. Because of this, err_headers_out will effectively override
  // headers_out when we call GetQueryOptions as it applies the header options
  // in order.
  ApacheRequestToResponseHeaders(*request_, response_headers_.get(),
                                 response_headers_.get());
  num_response_attributes_ = response_headers_->NumAttributes();

  // Get the remote configuration options before GetQueryOptions, as the query
  // options should override the remote config.
  if (!directory_aware_options->remote_configuration_url().empty()) {
    std::unique_ptr<RewriteOptions> remote_options(
        directory_aware_options->Clone());

    server_context_->GetRemoteOptions(remote_options.get(), false);
    if (custom_options_.get() == nullptr) {
      custom_options_.reset(
          server_context_->apache_factory()->NewRewriteOptions());
      custom_options_->Merge(*options_);
    }
    custom_options_->Merge(*remote_options);
  }

  if (!server_context_->GetQueryOptions(
          request_context(), directory_aware_options, &stripped_gurl_,
          request_headers_.get(), response_headers_.get(), &rewrite_query_)) {
    server_context_->message_handler()->Message(
        kWarning,
        "Invalid PageSpeed query params or headers for "
        "request %s. Serving with default options.",
        stripped_gurl_.spec_c_str());
  }
  const RewriteOptions* query_options = rewrite_query_.options();
  if (query_options != nullptr) {
    if (custom_options_.get() == nullptr) {
      custom_options_.reset(
          server_context_->apache_factory()->NewRewriteOptions());
      custom_options_->Merge(*options_);
    }
    custom_options_->Merge(*query_options);
    // Don't run any experiments if we're handling a customized request, unless
    // EnrollExperiment is on.
    if (!custom_options_->enroll_experiment()) {
      custom_options_->set_running_experiment(false);
    }
  }
  if (custom_options_.get() != nullptr) {
    options_ = custom_options_.get();
  }
}

void InstawebHandler::RemoveStrippedResponseHeadersFromApacheRequest() {
  // Write back the modified response headers if any have been stripped by
  // GetQueryOptions (which indicates that options were found).
  // Note: GetQueryOptions should not add or mutate headers, only remove
  // them.
  DCHECK(response_headers_->NumAttributes() <= num_response_attributes_);
  if (response_headers_->NumAttributes() < num_response_attributes_) {
    // Something was stripped, but we don't know if it came from
    // headers_out or err_headers_out.  We need to treat them separately.
    if (apr_is_empty_table(request_->err_headers_out)) {
      // We know that response_headers were all from request->headers_out
      apr_table_clear(request_->headers_out);
      ResponseHeadersToApacheRequest(*response_headers_, request_);
    } else if (apr_is_empty_table(request_->headers_out)) {
      // We know that response_headers were all from err_headers_out
      apr_table_clear(request_->err_headers_out);
      ErrorHeadersToApacheRequest(*response_headers_, request_);
    } else {
      // We don't know which table changed, so scan them individually and
      // write them both back. This should be a rare case and could be
      // optimized a bit if we find that we're spending time here.
      ResponseHeaders tmp_err_resp_headers(options_->ComputeHttpOptions());
      ResponseHeaders tmp_resp_headers(options_->ComputeHttpOptions());
      ThreadSystem* thread_system = server_context_->thread_system();
      std::unique_ptr<ApacheConfig> unused_opts1 =
          std::make_unique<ApacheConfig>("unused_options1", thread_system);
      std::unique_ptr<ApacheConfig> unused_opts2 =
          std::make_unique<ApacheConfig>("unused_options2", thread_system);

      ApacheRequestToResponseHeaders(*request_, &tmp_resp_headers,
                                     &tmp_err_resp_headers);

      // Use ScanHeader's parsing logic to find and strip the PageSpeed
      // options from the headers. Use NULL for device_properties as no
      // device property information is needed for the stripping.
      RequestContextPtr null_request_context;
      RewriteQuery::ScanHeader(
          true /* enable options */, "" /* request option override */,
          null_request_context, &tmp_err_resp_headers,
          nullptr /* device_properties */, unused_opts1.get(),
          server_context_->message_handler());
      RewriteQuery::ScanHeader(
          true /* enable options */, "" /* request option override */,
          null_request_context, &tmp_resp_headers,
          nullptr /* device_properties */, unused_opts2.get(),
          server_context_->message_handler());

      // Write the stripped headers back to the Apache record.
      apr_table_clear(request_->err_headers_out);
      apr_table_clear(request_->headers_out);
      ResponseHeadersToApacheRequest(tmp_resp_headers, request_);
      ErrorHeadersToApacheRequest(tmp_err_resp_headers, request_);
      // Note that the ordering here matches the comment above the
      // call to ApacheRequestToResponseHeaders in
      // ComputeCustomOptions.
    }
  }
}

// Handle url as .pagespeed. rewritten resource.  The response is streamed
// to the client as it is produced, through an unbuffered ApacheFetch, the
// same mechanism the in-place (IPRO) path uses: the fetch callbacks all
// run on this request thread while WaitForFetch() pumps the driver's
// scheduler sequence, and each Write() goes straight to ap_rwrite.
void InstawebHandler::HandleAsPagespeedResource() {
  RewriteDriver* driver = MakeDriver();
  MakeFetch(false /* not buffered */, "ps-resource");
  // The old buffered path never sent X-Content-Type-Options on resources;
  // whatever nosniff the response should carry is already present in its
  // headers (e.g. via FixFetchFallbackHeaders).  is_proxy solely controls
  // whether ApacheFetch adds its own nosniff header, so use it to keep the
  // wire behavior unchanged.
  fetch_->set_is_proxy(true);
  // On failure we answer the request ourselves below, exactly like the old
  // buffered path did; ApacheFetch must not emit the failed fetch's
  // headers or body.
  fetch_->set_handle_error(false);
  DisownDriver();

  StreamingPagespeedResourceFetch streaming_fetch(request_, fetch_);
  ResourceFetch::StartWithDriver(stripped_gurl_,
                                 ResourceFetch::kDontAutoCleanupDriver,
                                 server_context_, driver, &streaming_fetch);
  // Unlike the old ResourceFetch::BlockingFetch flow, there is no
  // wall-clock ceiling here: like the IPRO path, WaitForFetch() pumps the
  // driver's scheduler sequence until the fetch reports Done.  Every
  // segment of that wait is individually bounded:
  //  - HTTP cache lookups: Cyclone and the shm/LRU L1 answer synchronously
  //    (CycloneCache::Get); external caches sit behind AsyncCache, which
  //    reports kNotFound immediately when unhealthy and cancels queued
  //    lookups under load-shedding, with memcached/redis I/O timeouts
  //    underneath (SystemCaches::ConstructExternalCacheInterfaces*).
  //  - Reconstruction: origin fetches are bounded by the fetcher timeout
  //    (CurlUrlAsyncFetcher), and the rewrite is bounded by the fetch
  //    deadline alarm (RewriteContext::FetchContext::SetupDeadlineAlarm)
  //    where an input fallback exists, or by low-priority-worker
  //    load-shedding cancellation otherwise.
  //  - Cross-thread deliveries are queued to the scheduler sequence, whose
  //    Add() signals the scheduler so Wait() wakes immediately.
  // Wait() logs "Waiting for completion" if this ever runs long.
  WaitForFetch();

  if (!fetch_->status_ok()) {
    if (!fetch_->response_committed()) {
      // Nothing was sent to the client: with handle_error disabled,
      // ApacheFetch suppresses all output for error responses.  Send out
      // the same 404 the buffered path produced (this also increments
      // resource_404_count).
      server_context_->ReportResourceNotFound(original_url_, request_);
    } else {
      // Headers (and possibly an error body) already reached the client --
      // e.g. ApacheFetch's missing-Content-Type 403 -- so emitting a second
      // response is not possible; just log.
      server_context_->message_handler()->Message(
          kInfo, "Resource fetch for %s failed after response was committed.",
          original_url_.c_str());
    }
  }
  driver->Cleanup();
}

static apr_status_t DeleteInPlaceRecorder(void* object) {
  IproRecorder* recorded = static_cast<IproRecorder*>(object);
  delete recorded;
  return APR_SUCCESS;
}

// Attaches `recorder` to the response so the in-place output filters drive
// its lifecycle.  Takes ownership through the request pool.
//
// `rewrite_caching_headers` decides whether the middle filter is attached at
// all, and it is a SUBSTRATE decision rather than a configuration one.  That
// filter shortens the response's downstream lifetime with an `s-maxage` --
// ten seconds by default -- and the reason it exists is that the classic
// path is about to start serving an unoptimized response from its own cache
// and wants downstream caches to come back soon for the optimized one.  On a
// substrate where this module serves nothing back, that trade has no upside
// and a real cost: it would collapse every eligible response's downstream
// freshness while giving nothing in return, which is not a recording change,
// it is a serving change.
void InstawebHandler::AttachInPlaceRecorder(IproRecorder* recorder,
                                            bool rewrite_caching_headers) {
  // See mod_instaweb.cc:mod_pagespeed_register_hooks for why the classic path
  // needs all three filters.
  ap_add_output_filter(kModPagespeedInPlaceFilterName, recorder, request_,
                       request_->connection);
  if (rewrite_caching_headers) {
    ap_add_output_filter(kModPagespeedInPlaceFixHeadersName, recorder, request_,
                         request_->connection);
  }
  ap_add_output_filter(kModPagespeedInPlaceCheckHeadersName, recorder, request_,
                       request_->connection);
  // Add a contingency cleanup path in case some module munches
  // (or doesn't produce at all) an EOS bucket. If everything
  // goes well, we will just remove it befoe cleaning up ourselves.
  apr_pool_cleanup_register(request_->pool, recorder, DeleteInPlaceRecorder,
                            apr_pool_cleanup_null);
}

// Reads one request header straight off the Apache request.
//
// Deliberately not request_headers_, which has been stripped of headers that
// must not travel on a resource fetch -- and which is therefore the wrong
// place to ask what the CLIENT sent.  apr_table_get is case insensitive.
StringPiece InstawebHandler::RequestHeader(const char* name) const {
  const char* value = apr_table_get(request_->headers_in, name);
  return value == nullptr ? StringPiece() : StringPiece(value);
}

// Gathers everything the daemon-side recorder needs about the REQUEST.
//
// Done here, before the response starts, because the recorder outlives this
// handler: it is driven by output filters that run after handle_as_resource
// has returned, by which point the resolved options this reads may be gone.
// The four request fields the peer's classifier derives a capability mask
// from, read off the Apache request.
//
// ONE EXTRACTION POINT FOR BOTH ARMS, and that is the point of the function
// rather than a tidy-up.  The mask a request SELECTS with has to be the mask
// a recording of that same request would NOTIFY at; two independent readings
// of one client's headers would let the two arms disagree about what the
// client can decode, and the disagreement would be invisible -- each arm
// would look correct on its own. There is no assertion that could pin that
// from outside, so the shared reader is the pin.
void InstawebHandler::CapabilityHeaders(StringPiece* accept,
                                        StringPiece* user_agent,
                                        StringPiece* save_data,
                                        StringPiece* accept_encoding) const {
  *accept = RequestHeader(HttpAttributes::kAccept);
  *user_agent = RequestHeader(HttpAttributes::kUserAgent);
  *save_data = RequestHeader("Save-Data");
  *accept_encoding = RequestHeader(HttpAttributes::kAcceptEncoding);
}

void InstawebHandler::BuildDaemonRecordRequest(
    const RequestHeaders::Properties& request_properties,
    DaemonRecordRequest* request) {
  request->request_properties = request_properties;
  stripped_gurl_.PathAndLeaf().CopyToString(&request->url);
  stripped_gurl_.Host().CopyToString(&request->hostname);
  stripped_gurl_.Scheme().CopyToString(&request->scheme);

  StringPiece accept, user_agent, save_data, accept_encoding;
  CapabilityHeaders(&accept, &user_agent, &save_data, &accept_encoding);
  accept.CopyToString(&request->accept);
  user_agent.CopyToString(&request->user_agent);
  save_data.CopyToString(&request->save_data);
  accept_encoding.CopyToString(&request->accept_encoding);

  // The resolved configuration for THIS request, stated as a value.
  //
  // Both halves or neither: a payload with no signature has no name.  A
  // refusal here is not an error and is not logged per request -- it means
  // this request's configuration could not be named, and the record arm
  // responds by keeping the original and asking for nothing, which is the
  // gate's kNoOptionContext outcome.
  if (options_ != nullptr &&
      OptionContext::Compute(*options_, &request->option_context,
                             &request->option_signature) !=
          OptionContextStatus::kOk) {
    request->option_context.clear();
    request->option_signature.clear();
  }
}

// Gathers everything the daemon-side serve arm needs about the REQUEST.
//
// The four capability fields come from CapabilityHeaders, which is also what
// the record side uses -- the same four values, from one reader, for the
// reason stated there.
void InstawebHandler::BuildDaemonServeRequest(DaemonServeRequest* request) {
  stripped_gurl_.PathAndLeaf().CopyToString(&daemon_serve_url_);
  stripped_gurl_.Host().CopyToString(&daemon_serve_host_);
  stripped_gurl_.Scheme().CopyToString(&daemon_serve_scheme_);
  request->url = daemon_serve_url_;
  request->hostname = daemon_serve_host_;
  request->scheme = daemon_serve_scheme_;

  CapabilityHeaders(&request->accept, &request->user_agent, &request->save_data,
                    &request->accept_encoding);
  request->if_none_match = RequestHeader(HttpAttributes::kIfNoneMatch);
  request->if_modified_since = RequestHeader(HttpAttributes::kIfModifiedSince);

  // A forced reload, in both of the spellings a client still sends.  It is
  // handed to the peer's freshness evaluator rather than acted on here,
  // because the peer distinguishes "the client asked" from "the bytes are
  // old" and only the second means the entry expired.
  const StringPiece cache_control =
      RequestHeader(HttpAttributes::kCacheControl);
  request->force_revalidate =
      cache_control.find("no-cache") != StringPiece::npos ||
      StringCaseEqual(RequestHeader(HttpAttributes::kPragma), "no-cache");
}

// Records ONE serve class against the peer's serve-stats mmap.  A no-op when
// no daemon is configured for this server.
void InstawebHandler::RecordDaemonServeClass(int serve_class) {
  if (server_context_->daemon_serve_stats() != nullptr) {
    server_context_->daemon_serve_stats()->Record(serve_class);
  }
}

// Records ONE serve HIT against the peer's serve-stats mmap: the per-type
// original/optimized byte totals and the hit count.  In this topology the
// MODULE is the serving front end -- the daemon only writes alternates and
// never answers a request -- so without this call the daemon's serve_savings
// counters can never move here.  The GATE is the caller's; this helper
// only translates the decision into the peer's accounting vocabulary: the
// entry's content class, the origin length the saving is measured against,
// the bytes actually served, and the served variant's mask (which answers the
// SVG-served accounting on the peer's side).
void InstawebHandler::RecordDaemonServeHit(
    const DaemonServeDecision& decision) {
  if (server_context_->daemon_serve_stats() != nullptr) {
    server_context_->daemon_serve_stats()->RecordHit(
        decision.ps_content_type, decision.origin_content_length,
        decision.body.size(), decision.stored_mask);
  }
}

// Answers this request from the optimizer daemon's shared cache, if it can.
//
// Returns true when the response has been emitted and the handler is done.
// A false return is the substrate DECLINING -- not an error: the request then
// goes out by the plain, non-in-place path, exactly as it would on a server
// with no daemon at all, and the recording branch above runs.
bool InstawebHandler::ServeFromDaemonSubstrate() {
  // The single construction site, asked the way the record side asks its
  // own: nullptr unless the daemon owns the in-place cache for this server
  // AND this process has a usable handle on its volume.
  std::unique_ptr<DaemonServeReader> reader(
      MakeDaemonServeReaderIfReady(server_context_->daemon_adapter()));
  if (reader == nullptr) {
    // THIS IS A CLASSIFIED SERVE TOO, and counting it is what keeps the
    // partition honest.  Reaching this branch at all means the disposition is
    // kDaemonSubstrate, i.e. the adapter's health said the daemon owns the
    // in-place cache -- so a null reader is not "no daemon configured", it is
    // this PROCESS having no usable handle on the volume.  Returning silently
    // made a dead volume indistinguishable from no traffic: neither counter
    // moved, their documented sum was false, and the peer's skew class was
    // unreachable from production even though a unit test covered it.
    RecordDaemonServeClass(kPsServeClassOriginalSkew);
    server_context_->rewrite_stats()->ipro_daemon_fallthrough()->Add(1);
    return false;
  }

  DaemonServeRequest serve_request;
  BuildDaemonServeRequest(&serve_request);
  const DaemonServeDecision decision =
      reader->Serve(serve_request, server_context_->timer()->NowMs() / 1000);

  // EXACTLY ONE CLASS PER RESPONSE, recorded here because this is the only
  // place that knows the whole outcome -- including a fall-through, which is
  // a classified serve of origin bytes and not an absence of one.
  RecordDaemonServeClass(decision.serve_class);

  if (decision.verdict == DaemonServeVerdict::kFallThrough) {
    server_context_->rewrite_stats()->ipro_daemon_fallthrough()->Add(1);
    // AN AGE-EXPIRED VARIANT IS THE ONE FALL-THROUGH THE WORKER CANNOT HEAL
    // FROM THE PLAIN PATH.  The request now goes to the origin and is
    // re-recorded, and the record arm notifies the worker -- but the
    // worker's processed-set dedup answers that notification "already
    // processed, skipping", because the URL was optimized once and nothing
    // in the expiry lifecycle clears the dedup entry.  The variant is
    // declined on freshness on EVERY request and the notification that
    // would rebuild it is swallowed every time: the URL regresses to
    // origin-serving one freshness lifetime after optimization, permanently
    //.  The origin-refreshed sentinel is the worker's heal for
    // exactly this transition -- it purges the stale variant set, clears
    // dedup and rebuilds -- so the flagged fall-through is answered with it
    // here, before the re-record it precedes.
    //
    // The ask is the arm's helper, on the fallback re-notify's terms: fire
    // and forget, once, no retry; both halves of the option context or
    // neither; and the two counters move on SEND OUTCOMES only, so a
    // suppression (an unflagged fall-through, an unnameable configuration)
    // moves neither and a failed send moves the failure one.  Asked only
    // when the decision SAYS there is something to ask for: the context
    // computation is not free, and an ordinary cold-key fall-through -- the
    // common case -- should not pay it.
    if (decision.stale_variant_expired_by_age) {
      DaemonAdapter* adapter = server_context_->daemon_adapter();
      GoogleString option_context, option_signature;
      if (adapter != nullptr && adapter->abi() != nullptr &&
          options_ != nullptr &&
          OptionContext::Compute(*options_, &option_context,
                                 &option_signature) ==
              OptionContextStatus::kOk) {
        RewriteStats* stats = server_context_->rewrite_stats();
        DaemonServeOriginRefreshedNotify(
            *adapter->abi(), adapter->socket_path(), serve_request, decision,
            option_context, option_signature,
            stats->ipro_daemon_refresh_notified(),
            stats->ipro_daemon_refresh_notify_failed());
      }
    }
    return false;
  }

  ResponseHeaders response_headers;
  response_headers.set_major_version(1);
  response_headers.set_minor_version(1);
  response_headers.SetStatusAndReason(
      decision.not_modified ? HttpStatus::kNotModified : HttpStatus::kOK);
  if (!decision.cache_control.empty()) {
    response_headers.Add(HttpAttributes::kCacheControl, decision.cache_control);
  }
  if (!decision.etag.empty()) {
    response_headers.Add(HttpAttributes::kEtag, decision.etag);
  }
  // `Vary` IS COMPOSED ONCE, ABOVE THE 200/304 BRANCH, and that placement is
  // the fix rather than an arrangement of it.  The two legs used to state the
  // field independently: a 200 went out with `Vary: Accept, Accept-Encoding`
  // -- `Accept` from the arm, `Accept-Encoding` from the compressor attached
  // a few lines down -- and the 304 that revalidated the same entry seconds
  // later went out with `Vary: Accept`.  A cache updates its stored header
  // fields from the 304 (RFC 9111 4.3.4), so it was told the response varies
  // on a NARROWER set than the one it had keyed on, which is the dangerous
  // direction.  One call site cannot disagree with itself.
  //
  // THE MEDIA TYPE HANDED TO THE PREDICATE IS THE ONE THE COMPRESSOR WILL
  // SEE, not this request's inherited type: the 200 leg writes the response
  // headers first, and writing a `Content-Type` is what sets the request's
  // own (ResponseHeadersToApacheRequest calls ap_set_content_type), so the
  // attach a few lines down tests exactly this string whenever the decision
  // has one.  The selection is the arm's, so that it is a rule with a case
  // over it rather than a ternary nothing observes; see
  // ServeCompressorMediaType.
  const char* compressor_media_type =
      ServeCompressorMediaType(decision, request_->content_type);
  const GoogleString vary = ServeVaryFieldValue(
      decision, StreamingPagespeedResourceFetch::IsCompressibleContentType(
                    compressor_media_type));
  if (!vary.empty()) {
    response_headers.Add(HttpAttributes::kVary, vary);
  }
  // `Age` ON EVERY HIT, including the 304.  What goes out carries the entry's
  // full remaining lifetime, so a cache downstream told `max-age=600` and not
  // told the response is already 500 seconds old keeps it for 1100. RFC 9111
  // s5.1 requires a cache that has the information to send it, and this arm
  // has it: the peer's freshness evaluator computed it on the way here.
  response_headers.Add(HttpAttributes::kAge,
                       IntegerToString(decision.age_seconds));
  if (!decision.not_modified) {
    if (!decision.content_type.empty()) {
      response_headers.Add(HttpAttributes::kContentType, decision.content_type);
    }
    // `Last-Modified` IS THE ORIGIN'S, so it goes on origin bytes and on
    // nothing else.  Putting it on an optimized variant would advertise the
    // origin's validator for a representation the origin never produced --
    // two planes' validators on one response, and the one a client is most
    // likely to send back.
    if (decision.verdict == DaemonServeVerdict::kServeOriginal &&
        decision.origin_last_modified != 0) {
      response_headers.SetLastModified(
          static_cast<int64>(decision.origin_last_modified) * Timer::kSecondMs);
    }
  }
  response_headers.ComputeCaching();

  if (decision.not_modified) {
    // A 304 carries no body and must not be given a length.  Emitted by
    // hand rather than through send_out_headers_and_body, which sets a
    // content length unconditionally.
    ResponseHeadersToApacheRequest(response_headers, request_);
    request_->status = HttpStatus::kNotModified;
    DisableDownstreamHeaderFilters(request_);
  } else {
    GoogleString body;
    decision.body.CopyToString(&body);
    send_out_headers_and_body(request_, response_headers, body);

    // A 200 IS A SERVE; a 304 is not, and neither is an origin-byte or
    // unoptimized answer.  Record the hit here, on the 200 leg only, gated on
    // the same three facts the peer's own front ends gate on: the arm
    // classified the serve OPTIMIZED, the entry was produced by the worker,
    // and it carries the origin length the saving is measured against.  In
    // this topology the module is the only serving front end, so this is the
    // only place the daemon's serve_savings counters can move.
    if (decision.serve_class == kPsServeClassOptimized &&
        decision.worker_processed && decision.origin_content_length > 0) {
      RecordDaemonServeHit(decision);
    }
  }

  server_context_->rewrite_stats()->ipro_daemon_served()->Add(1);

  // A FALLBACK HIT IS SERVED -- and now it is also ANSWERED.  The serve
  // recorded nothing, so without this call nothing would ever ask the worker
  // for the variant this client's mask names, and the family would converge
  // on the viewport, density and Save-Data axes only if a miss happened to
  // come first.  One notification, fire and forget, covering every axis the
  // served mask differs on -- viewport, density, Save-Data and format alike.
  // The arm's helper owns the detection re-check, the 0x04 gate and the SVG
  // carve-out (DaemonServeFallbackRenotify).
  //
  // Asked only when the decision SAYS there is something to ask for: the
  // option-context computation below is not free, and an exact serve -- the
  // steady state of a converged family -- should not pay it.  A request whose
  // configuration cannot be named asks for nothing, mirroring the record
  // gate's kNoOptionContext outcome.
  //
  // The two counters are the helper's to move, on SEND OUTCOMES only: a
  // suppression here (the 0x04 gate, an unnameable context) moves neither,
  // and a failed send moves the failure one -- so at rollout a dead socket
  // reads as `notify_failed` climbing while a quiet converged family reads
  // as both flat.
  if (decision.fallback_hit) {
    DaemonAdapter* adapter = server_context_->daemon_adapter();
    GoogleString option_context, option_signature;
    if (adapter != nullptr && adapter->abi() != nullptr &&
        options_ != nullptr &&
        OptionContext::Compute(*options_, &option_context, &option_signature) ==
            OptionContextStatus::kOk) {
      RewriteStats* stats = server_context_->rewrite_stats();
      DaemonServeFallbackRenotify(*adapter->abi(), adapter->socket_path(),
                                  serve_request, decision, option_context,
                                  option_signature,
                                  stats->ipro_daemon_fallback_notified(),
                                  stats->ipro_daemon_fallback_notify_failed());
    }
  }
  return true;
}

// Handle url with In Place Resource Optimization (IPRO) flow.
bool InstawebHandler::HandleAsInPlace() {
  bool handled = false;

  // Which in-place substrate this server is on.  Resolved once at startup;
  // read here, never recomputed and never logged from, so a missing daemon
  // costs one line at startup rather than one per request.
  const IproDisposition disposition =
      IproDispositionFor(server_context_->daemon_health());
  if (disposition == IproDisposition::kOff) {
    // A daemon was configured and is not usable.  In-place optimization is
    // off: nothing is served from the classic in-place cache and nothing is
    // recorded into it.  The request falls through to ordinary serving.
    return handled;
  }

  // We need to see if the origin request has cookies, so examine the
  // Apache request directly, as request_headers_ has been stripped of
  // headers we don't want to transmit for resource fetches.
  //
  // Note that apr_table_get is case insensitive. See
  // http://apr.apache.org/docs/apr/2.0/group__apr__tables.html#ga4db13e3915c6b9a3142b175d4c15d915
  //
  // Computed BEFORE the substrate branch, and used by both.  These are the
  // request-side facts that decide whether a response may be kept in a cache
  // other requests can reach -- authorization above all -- and neither
  // substrate is entitled to a weaker answer than the other.
  RequestHeaders::Properties request_properties(
      apr_table_get(request_->headers_in, HttpAttributes::kCookie) != nullptr,
      apr_table_get(request_->headers_in, HttpAttributes::kCookie2) != nullptr,
      (apr_table_get(request_->headers_in, HttpAttributes::kAuthorization) !=
       nullptr) ||
          (request_->user != nullptr));

  if (disposition == IproDisposition::kDaemonSubstrate) {
    // The optimizer daemon owns the in-place cache for this server.  This
    // handler asks it for an answer FIRST, and records only when there was
    // none: a request that can be served from the shared cache has nothing
    // to record, and recording it anyway would re-store an original the
    // daemon already holds.
    //
    // THE CONVERGENCE CONTRACT THAT ORDERING IMPLIES.  Because a serve
    // records nothing, a served request used to be invisible to the worker --
    // including a FALLBACK hit, where the variant served is not the one the
    // client's mask names on the viewport, density or Save-Data axes.  On
    // this seam those axes now converge via the re-notify in
    // ServeFromDaemonSubstrate: the fallback hit is served, and exactly one
    // fire-and-forget notification asks the worker for the client's variant,
    // gated on the durable original's kPsFlagOriginVariesAccept bit and on a
    // nameable option context (see DaemonServeFallbackRenotify for both).
    // The FORMAT axis keeps its old path for a genuinely MISSING format: a
    // format the client did not advertise is refused and retried by exact id
    // rather than served, so when no servable variant exists at all it still
    // converges by miss -> record -> notify.  An ORIGINAL-format fallback,
    // though, converges via this re-notify with no miss involved: the
    // original-format sibling IS served (any client can decode it), its mask
    // differs from the client's on the format axis alone, and the re-notify
    // asks for the client's format sibling directly.  What the re-notify
    // never covers is a permanently client-unmatchable variant class -- SVG
    // -- because such a variant is never served on this arm; that is the
    // inherited carve-out, documented beside the gate.
    //
    // THE FILTER THAT SHORTENS A RESPONSE'S DOWNSTREAM LIFETIME IS STILL NOT
    // ATTACHED, and the reason has changed rather than gone away.  It exists
    // to cover a classic in-place serve of UNOPTIMIZED bytes, by asking
    // downstream caches to come back soon for the optimized ones.  On this
    // substrate the optimized bytes are what gets served, under a
    // `Cache-Control` the peer builds from the origin state it stored -- so
    // an `s-maxage` bolted on afterwards would not be a safety margin, it
    // would be a second, disagreeing answer to the freshness question,
    // shortening every response the substrate serves correctly.  A
    // fall-through does emit the origin's own response, which is the
    // unoptimized case -- but it emits it by the PLAIN path, exactly as if
    // this module were not installed, and that path is not this filter's to
    // shorten either.
    //
    // A HEAD carries no body, so there is nothing to record; the classic path
    // declines to record one for the same reason.
    //
    // Recorded on every eligible MISS, not only on a cold one.  Re-recording
    // REPLACES for a single writer, and a server is not one: every process
    // holds its own handle on the shared volume, so for a hot URL concurrent
    // writers are the steady state and superseded copies -- each a whole
    // response body -- accumulate under that URL until the storage layer's
    // chain ceiling refuses further writes.  The peer's own contract says to
    // serialise such writers; there is no cross-process ordering available
    // here to do it with.  That costs VOLUME as well as work, it does not
    // come back down by itself, and it is one of the reasons the directives
    // are documented as not yet ready to enable.
    if (ServeFromDaemonSubstrate()) {
      return true;
    }
    if (!request_->header_only) {
      DaemonRecordRequest record_request;
      BuildDaemonRecordRequest(request_properties, &record_request);
      IproRecorder* recorder = MakeDaemonIproRecorderIfReady(
          server_context_->daemon_adapter(), record_request,
          options_->ComputeHttpOptions(), server_context_->timer(),
          server_context_->message_handler());
      if (recorder != nullptr) {
        AttachInPlaceRecorder(recorder, false /* rewrite_caching_headers */);
      }
    }
    return handled;
  }

  RewriteDriver* driver = MakeDriver();
  MakeFetch(false /* not buffered */, "ipro");
  fetch_->set_handle_error(false);

  // THE 304 LEG STATES THE `Vary` ITS 200 STATES.  The classic path
  // composes no encoding axis itself: the 200's `Vary: Accept-Encoding`
  // token is stamped by the serving chain's compressor as the optimized
  // bytes are written out, and that compressor states nothing on a
  // bodyless 304, so the two legs of one response used to disagree about
  // what the response varies on.  The wrapper closes the gap on the 304 leg
  // alone, from the same media-type question the compressor is asked; see
  // InPlaceNotModifiedVaryFetch for what that composition does and does not
  // reproduce of the compressor's own decision.
  InPlaceNotModifiedVaryFetch vary_fetch(request_->content_type,
                                         request_->header_only != 0, fetch_);

  DisownDriver();
  driver->FetchInPlaceResource(stripped_gurl_, false /* proxy_mode */,
                               &vary_fetch);
  WaitForFetch();
  if (fetch_->status_ok()) {
    server_context_->rewrite_stats()->ipro_served()->Add(1);
    handled = true;
  } else if ((fetch_->response_headers()->status_code() ==
              CacheUrlAsyncFetcher::kNotInCacheStatus) &&
             !request_->header_only) {
    server_context_->rewrite_stats()->ipro_not_in_cache()->Add(1);
    // This URL was not found in cache (neither the input resource nor
    // a ResourceNotCacheable entry) so we need to get it into cache
    // (or at least a note that it cannot be cached stored there).
    // We do that using an Apache output filter.
    //
    // We use stripped_gurl_.Spec() rather than 'original_url_' for
    // InPlaceResourceRecorder as we want any ?ModPagespeed query-params to
    // be stripped from the cache key before we store the result in HTTPCache.
    //
    // The recorder is built through the record gate rather than here: that
    // function is the single place in the tree allowed to construct one, and
    // it returns nullptr for every substrate other than the classic one.
    InPlaceResourceRecorder* recorder = MakeIproRecorderIfClassic(
        disposition, request_context_, stripped_gurl_.Spec(),
        driver->CacheFragment(), request_properties,
        options_->ipro_max_response_bytes(),
        options_->ipro_max_concurrent_recordings(),
        server_context_->http_cache(), server_context_->statistics(),
        server_context_->message_handler());
    if (recorder != nullptr) {
      AttachInPlaceRecorder(recorder, true /* rewrite_caching_headers */);
    }
  } else {
    server_context_->rewrite_stats()->ipro_not_rewritable()->Add(1);
  }
  driver->Cleanup();

  return handled;
}

bool InstawebHandler::HandleAsProxy() {
  // Consider Issue 609: proxying an external CSS file via MapProxyDomain, and
  // the CSS file makes reference to a font file, which mod_pagespeed does not
  // know anything about, and does not know how to absolutify.  We need to
  // handle the request for the external font file here, even if IPRO (in place
  // resource optimization) is off.
  bool is_proxy = false;
  GoogleString mapped_url;
  GoogleString host_header;
  if (options_->domain_lawyer()->MapOriginUrl(stripped_gurl_, &mapped_url,
                                              &host_header, &is_proxy) &&
      is_proxy) {
    // TODO(jmarantz): make this unbuffered, verifying that it will
    // only call back to apache on the request thread.
    RewriteDriver* driver = MakeDriver();
    MakeFetch(mapped_url, true /* buffered */, "proxy");
    fetch_->set_is_proxy(true);
    DisownDriver();
    server_context_->proxy_fetch_factory()->StartNewProxyFetch(
        mapped_url, fetch_, driver, nullptr, nullptr);
    WaitForFetch();
    return true;  // handled
  }
  return false;  // declined
}

void InstawebHandler::HandleAsProxyForAll() {
  static const char kLoopValue[] = "MPS";

  // Note: we can't use MakeFetch here as we want ProxyInterface to create the
  // RewriteDriver.
  std::unique_ptr<RequestHeaders> request_headers =
      std::make_unique<RequestHeaders>();
  ApacheRequestToRequestHeaders(*request_, request_headers.get());

  // Do loop detection.
  if (request_headers->HasValue(HttpAttributes::kXPageSpeedLoop, kLoopValue)) {
    write_handler_response(
        "Loop detected on fetch in ProxyAllRequests mode; "
        "you may need to authorize more domains. ",
        request_);
    return;
  }
  request_headers->Add(HttpAttributes::kXPageSpeedLoop, kLoopValue);
  SimpleBufferedApacheFetch fetch(request_context_, request_headers.release(),
                                  server_context_->thread_system(), request_,
                                  server_context_->message_handler());

  ProxyInterface proxy_interface(
      ApacheServerContext::kProxyInterfaceStatsPrefix,
      apache_request_context_->local_ip(),
      apache_request_context_->local_port(), server_context_,
      server_context_->statistics());
  proxy_interface.Fetch(original_url_, server_context_->message_handler(),
                        &fetch);

  fetch.Wait();
}

// Determines whether the url can be handled as a mod_pagespeed or in-place
// optimized resource, and handles it, returning true.  Success status is
// written to the status code in the response headers.
/* static */
bool InstawebHandler::handle_as_resource(ApacheServerContext* server_context,
                                         request_rec* request,
                                         GoogleUrl* gurl) {
  if (!gurl->IsWebValid()) {
    return false;
  }

  InstawebHandler instaweb_handler(request);
  std::unique_ptr<RequestHeaders> request_headers =
      std::make_unique<RequestHeaders>();
  const RewriteOptions* options = instaweb_handler.options();

  // Finally, do the actual handling.
  bool handled = false;
  if (server_context->IsPagespeedResource(*gurl)) {
    handled = true;
    instaweb_handler.HandleAsPagespeedResource();
  } else if (instaweb_handler.HandleAsProxy()) {
    handled = true;
  } else if (options->in_place_rewriting_enabled() && options->enabled() &&
             options->IsAllowed(gurl->Spec())) {
    handled = instaweb_handler.HandleAsInPlace();
  }

  return handled;
}

// Write response headers and send out headers and output, including the option
// for a custom Content-Type.
//
// TODO(jmarantz): consider deleting this helper method putting all responses
// through ApacheFetch.
/* static */
void InstawebHandler::write_handler_response(const StringPiece& output,
                                             request_rec* request,
                                             ContentType content_type,
                                             const StringPiece& cache_control) {
  // We don't need custom options for our produced resources. In fact, options
  // shouldn't matter.
  ResponseHeaders response_headers(kDeprecatedDefaultHttpOptions);
  response_headers.SetStatusAndReason(HttpStatus::kOK);
  response_headers.set_major_version(1);
  response_headers.set_minor_version(1);

  response_headers.Add(HttpAttributes::kContentType, content_type.mime_type());
  // http://msdn.microsoft.com/en-us/library/ie/gg622941(v=vs.85).aspx
  // Script and styleSheet elements will reject responses with
  // incorrect MIME types if the server sends the response header
  // "X-Content-Type-Options: nosniff". This is a security feature
  // that helps prevent attacks based on MIME-type confusion.
  response_headers.Add(HttpAttributes::kXContentTypeOptions,
                       HttpAttributes::kNosniff);
  AprTimer timer;
  int64 now_ms = timer.NowMs();
  response_headers.SetDate(now_ms);
  response_headers.SetLastModified(now_ms);
  response_headers.Add(HttpAttributes::kCacheControl, cache_control);
  send_out_headers_and_body(request, response_headers, output.as_string());
}

/* static */
void InstawebHandler::write_handler_response(const StringPiece& output,
                                             request_rec* request) {
  write_handler_response(output, request, kContentTypeHtml,
                         HttpAttributes::kNoCacheMaxAge0);
}

// Returns request URL if it was a .pagespeed. rewritten resource URL.
// Otherwise returns NULL. Since other Apache modules can change request->uri,
// we stow the original request URL in a note. This method reads that note
// and thus should return the URL that the browser actually requested (rather
// than a mod_rewrite altered URL).
/* static */
const char* InstawebHandler::get_instaweb_resource_url(
    request_rec* request, ApacheServerContext* server_context) {
  const char* resource = apr_table_get(request->notes, kResourceUrlNote);

  // If our translate_name hook, save_url_hook, failed to run because some
  // other module's translate_hook returned OK first, then run it now. The
  // main reason we try to do this early is to save our URL before mod_rewrite
  // mutates it.
  if (resource == nullptr) {
    InstawebHandler::save_url_in_note(request, server_context);
    resource = apr_table_get(request->notes, kResourceUrlNote);
  }

  if (resource != nullptr && strcmp(resource, kResourceUrlNo) == 0) {
    return nullptr;
  }

  const char* url = apr_table_get(request->notes, kPagespeedOriginalUrl);
  return url;
}

namespace {

// Used by log_request_headers for testing only.
struct HeaderLoggingData {
  HeaderLoggingData(StringWriter* writer_in, MessageHandler* handler_in)
      : writer(writer_in), handler(handler_in) {}
  StringWriter* writer;
  MessageHandler* handler;
};

}  // namespace

// Helper function to support the LogRequestHeadersHandler.  Called once for
// each header to write header data in a form suitable for javascript inlining.
// Used only for tests.
/* static */
int InstawebHandler::log_request_headers(void* logging_data, const char* key,
                                         const char* value) {
  HeaderLoggingData* hld = static_cast<HeaderLoggingData*>(logging_data);
  StringWriter* writer = hld->writer;
  MessageHandler* handler = hld->handler;

  GoogleString escaped_key;
  GoogleString escaped_value;

  EscapeToJsStringLiteral(key, false, &escaped_key);
  EscapeToJsStringLiteral(value, false, &escaped_value);

  writer->Write("alert(\"", handler);
  writer->Write(escaped_key, handler);
  writer->Write("=", handler);
  writer->Write(escaped_value, handler);
  writer->Write("\");\n", handler);

  return 1;  // Continue iteration.
}

/* static */
void InstawebHandler::instaweb_static_handler(
    request_rec* request, ApacheServerContext* server_context) {
  StaticAssetManager* static_asset_manager =
      server_context->static_asset_manager();
  StringPiece request_uri_path = request->parsed_uri.path;
  // Strip out the common prefix url before sending to StaticAssetManager.
  StringPiece file_name = request_uri_path.substr(
      server_context->apache_factory()->static_asset_prefix().length());
  StringPiece file_contents;
  StringPiece cache_header;
  ContentType content_type;
  if (static_asset_manager->GetAsset(file_name, &file_contents, &content_type,
                                     &cache_header)) {
    write_handler_response(file_contents, request, content_type, cache_header);
  } else {
    server_context->ReportResourceNotFound(request->parsed_uri.path, request);
  }
}

// Append the query params from a request into data. This just parses the query
// params from a request URL. For parsing the query params from a POST body, use
// parse_body_from_post(). Return true if successful, otherwise, returns false
// and sets ret to the appropriate status.
/* static */
bool InstawebHandler::parse_query_params(const request_rec* request,
                                         GoogleString* data,
                                         apr_status_t* ret) {
  // Add a dummy host (www.example.com) to the request URL to make it absolute
  // so that GoogleUrl can be used for parsing.
  GoogleUrl base("http://www.example.com");
  GoogleUrl url(base, request->unparsed_uri);

  if (!url.IsWebValid() || !url.has_query()) {
    *ret = HTTP_BAD_REQUEST;
    return false;
  }

  url.Query().AppendToString(data);
  return true;
}

// Read the body from a POST request and append to data. Return true if
// successful, otherwise, returns false and sets ret to the appropriate status.
bool InstawebHandler::parse_body_from_post(const request_rec* request,
                                           GoogleString* data,
                                           apr_status_t* ret) {
  if (request->method_number != M_POST) {
    *ret = HTTP_METHOD_NOT_ALLOWED;
    return false;
  }

  // Verify that the request has the correct content type for a form POST
  // submission. Ideally, we could use request->content_type here, but that is
  // coming back as NULL, even when the header was set correctly.
  const char* content_type =
      apr_table_get(request->headers_in, HttpAttributes::kContentType);
  if (content_type == nullptr) {
    *ret = HTTP_BAD_REQUEST;
    return false;
  }
  GoogleString mime_type;
  GoogleString charset;
  if (!ParseContentType(content_type, &mime_type, &charset)) {
    *ret = HTTP_BAD_REQUEST;
    return false;
  }
  // TODO(jud): Set the charset on the beacon to a known value (say, UTF-8), and
  // check here that it's as expected. Intended as a cheap-and-nasty test that
  // the beacon came from our JS and not some black hat. Easily subvertible but
  // better than nothing (?).
  if (!StringCaseEqual(mime_type, "application/x-www-form-urlencoded") &&
      !StringCaseEqual(mime_type, "multipart/form-data")) {
    *ret = HTTP_BAD_REQUEST;
    return false;
  }

  // Setup the number of bytes to try to read from the POST body. If the
  // Content-Length header is set, use it, otherwise try to pull up to
  // kMaxPostSizeBytes.
  int content_len = kMaxPostSizeBytes;
  const char* content_len_str =
      apr_table_get(request->headers_in, HttpAttributes::kContentLength);
  if (content_len_str != nullptr) {
    if (!StringToInt(content_len_str, &content_len)) {
      *ret = HTTP_BAD_REQUEST;
      return false;
    }
    if (static_cast<size_t>(content_len) > kMaxPostSizeBytes) {
      *ret = HTTP_REQUEST_ENTITY_TOO_LARGE;
      return false;
    }
  }

  // Parse the incoming brigade and add the contents to data. In apache 2.4 we
  // could just use ap_parse_form_data. See the example at
  // http://httpd.apache.org/docs/2.4/developer/modguide.html#snippets.
  apr_bucket_brigade* bbin =
      apr_brigade_create(request->pool, request->connection->bucket_alloc);

  bool eos = false;

  while (!eos) {
    apr_status_t rv =
        ap_get_brigade(request->input_filters, bbin, AP_MODE_READBYTES,
                       APR_BLOCK_READ, content_len);
    if (rv != APR_SUCCESS) {
      // Form input read failed.
      *ret = HTTP_INTERNAL_SERVER_ERROR;
      return false;
    }
    for (apr_bucket* bucket = APR_BRIGADE_FIRST(bbin);
         bucket != APR_BRIGADE_SENTINEL(bbin);
         bucket = APR_BUCKET_NEXT(bucket)) {
      if (!APR_BUCKET_IS_METADATA(bucket)) {
        const char* buf = nullptr;
        size_t bytes = 0;
        rv = apr_bucket_read(bucket, &buf, &bytes, APR_BLOCK_READ);
        if (rv != APR_SUCCESS) {
          *ret = HTTP_INTERNAL_SERVER_ERROR;
          return false;
        }
        if (data->length() + bytes > kMaxPostSizeBytes) {
          *ret = HTTP_REQUEST_ENTITY_TOO_LARGE;
          return false;
        }
        data->append(buf, bytes);
      } else if (APR_BUCKET_IS_EOS(bucket)) {
        eos = true;
        break;
      }
    }
    apr_brigade_cleanup(bbin);
  }

  // No need to modify ret as it is only used if reading the POST failed.
  return true;
}

// Read the raw POST body without content-type validation.
bool InstawebHandler::read_post_body(const request_rec* request,
                                     GoogleString* data, apr_status_t* ret) {
  if (request->method_number != M_POST) {
    *ret = HTTP_METHOD_NOT_ALLOWED;
    return false;
  }

  int content_len = kMaxPostSizeBytes;
  const char* content_len_str =
      apr_table_get(request->headers_in, HttpAttributes::kContentLength);
  if (content_len_str != nullptr) {
    if (!StringToInt(content_len_str, &content_len)) {
      *ret = HTTP_BAD_REQUEST;
      return false;
    }
    if (static_cast<size_t>(content_len) > kMaxPostSizeBytes) {
      *ret = HTTP_REQUEST_ENTITY_TOO_LARGE;
      return false;
    }
  }

  apr_bucket_brigade* bbin =
      apr_brigade_create(request->pool, request->connection->bucket_alloc);
  bool eos = false;
  while (!eos) {
    apr_status_t rv =
        ap_get_brigade(request->input_filters, bbin, AP_MODE_READBYTES,
                       APR_BLOCK_READ, content_len);
    if (rv != APR_SUCCESS) {
      *ret = HTTP_INTERNAL_SERVER_ERROR;
      return false;
    }
    for (apr_bucket* bucket = APR_BRIGADE_FIRST(bbin);
         bucket != APR_BRIGADE_SENTINEL(bbin);
         bucket = APR_BUCKET_NEXT(bucket)) {
      if (!APR_BUCKET_IS_METADATA(bucket)) {
        const char* buf = nullptr;
        size_t bytes = 0;
        rv = apr_bucket_read(bucket, &buf, &bytes, APR_BLOCK_READ);
        if (rv != APR_SUCCESS) {
          *ret = HTTP_INTERNAL_SERVER_ERROR;
          return false;
        }
        if (data->length() + bytes > kMaxPostSizeBytes) {
          *ret = HTTP_REQUEST_ENTITY_TOO_LARGE;
          return false;
        }
        data->append(buf, bytes);
      } else if (APR_BUCKET_IS_EOS(bucket)) {
        eos = true;
        break;
      }
    }
    apr_brigade_cleanup(bbin);
  }
  return true;
}

/* static */
apr_status_t InstawebHandler::instaweb_beacon_handler(
    request_rec* request, ApacheServerContext* server_context) {
  GoogleString data;
  apr_status_t ret = DECLINED;
  if (request->method_number == M_GET) {
    if (!parse_query_params(request, &data, &ret)) {
      return ret;
    }
  } else if (request->method_number == M_POST) {
    GoogleString query_param_data, post_data;
    // Even if the beacon is a POST, the originating url should be in the query
    // params, not the POST body.
    if (!parse_query_params(request, &query_param_data, &ret)) {
      return ret;
    }
    if (!parse_body_from_post(request, &post_data, &ret)) {
      return ret;
    }
    StrAppend(&data, query_param_data, "&", post_data);
  } else {
    return HTTP_METHOD_NOT_ALLOWED;
  }
  RequestContextPtr request_context(
      server_context->NewApacheRequestContext(request));
  StringPiece user_agent =
      apr_table_get(request->headers_in, HttpAttributes::kUserAgent);
  server_context->HandleBeacon(data, user_agent, request_context);
  apr_table_set(request->headers_out, HttpAttributes::kCacheControl,
                HttpAttributes::kNoCacheMaxAge0);
  return HTTP_NO_CONTENT;
}

/* static */
bool InstawebHandler::IsBeaconUrl(const RewriteOptions::BeaconUrl& beacons,
                                  const GoogleUrl& gurl) {
  // Check if the full path without query parameters equals the beacon URL,
  // either the http or https version (we're too lazy to check specifically).
  // This handles both GETs, which include query parameters, and POSTs,
  // which will only have the originating url in the query params.
  if (!gurl.IsWebValid()) {
    return false;
  }
  // Ignore query params in the beacon URLs. Normally the beacon URL won't have
  // a query param, but it could have been added using ModPagespeedBeaconUrl.
  return (gurl.PathSansQuery() == beacons.http_in ||
          gurl.PathSansQuery() == beacons.https_in);
}

/* static */
bool InstawebHandler::is_pagespeed_subrequest(request_rec* request) {
  StringPiece user_agent =
      apr_table_get(request->headers_in, HttpAttributes::kUserAgent);
  return (user_agent.find(kModPagespeedSubrequestUserAgent) != user_agent.npos);
}

/* static */
apr_status_t InstawebHandler::instaweb_handler(request_rec* request) {
  apr_status_t ret = DECLINED;
  ApacheServerContext* server_context =
      InstawebContext::ServerContextFromServerRec(request->server);
  ApacheConfig* global_config = server_context->global_config();
  // Escape ASAP if we're in unplugged mode.
  if (global_config->unplugged()) {
    return DECLINED;
  }

  // Flushing the cache mutates global_options, so this has to happen before we
  // construct the options that we use to decide whether IPRO is enabled.  Note
  // that the global_config might be altered by this, but the pointer will not
  // change.
  server_context->FlushCacheIfNecessary();

  ApacheRewriteDriverFactory* factory = server_context->apache_factory();
  ApacheMessageHandler* message_handler = factory->apache_message_handler();
  StringPiece request_handler_str = request->handler;

  const char* url = InstawebContext::MakeRequestUrl(*global_config, request);
  GoogleUrl gurl;
  if (url == nullptr || !gurl.Reset(url)) {
    return DECLINED;  // URL not valid, let someone other module handle.
  }

  // Loopback determination for StrictAdminAccess (opt-in, default off). Uses
  // the validated client connection IP (mod_remoteip-aware useragent_ip), NOT
  // the client-controlled Host header. When StrictAdminAccess is off this value
  // is ignored by the *AccessAllowed overloads below, preserving today's
  // default-open behavior exactly.
  const bool client_is_loopback =
      IsLoopbackClientIp(ClientIpForAdminWarning(request));

  if (global_config->proxy_all_requests_mode() && gurl.IsWebValid()) {
    InstawebHandler instaweb_handler(request);
    // TODO(morlovich): Still export stats and the like?
    instaweb_handler.HandleAsProxyForAll();
    return APACHE_OK;
  }

  if (request_handler_str == kStatisticsHandler &&
      global_config->StatisticsAccessAllowed(gurl, client_is_loopback)) {
    WarnIfNonLoopbackAdminAccess(ClientIpForAdminWarning(request),
                                 AdminHandlerFamily::kStatistics,
                                 kStatisticsHandler, message_handler);
    InstawebHandler instaweb_handler(request);
    server_context->StatisticsPage(
        false /* not global */, instaweb_handler.query_params(),
        instaweb_handler.options(),
        instaweb_handler.MakeFetch(false /* unbuffered */, "local-stats"));
    return APACHE_OK;
  } else if (request_handler_str == kGlobalStatisticsHandler &&
             global_config->GlobalStatisticsAccessAllowed(gurl,
                                                          client_is_loopback)) {
    WarnIfNonLoopbackAdminAccess(ClientIpForAdminWarning(request),
                                 AdminHandlerFamily::kGlobalStatistics,
                                 kGlobalStatisticsHandler, message_handler);
    InstawebHandler instaweb_handler(request);
    server_context->StatisticsPage(
        true /* global */, instaweb_handler.query_params(),
        instaweb_handler.options(),
        instaweb_handler.MakeFetch(false /* unbuffered */, "global-stats"));
    return APACHE_OK;
  } else if (request_handler_str == kAdminHandler &&
             global_config->AdminAccessAllowed(gurl, client_is_loopback)) {
    WarnIfNonLoopbackAdminAccess(ClientIpForAdminWarning(request),
                                 AdminHandlerFamily::kAdmin, kAdminHandler,
                                 message_handler);
    InstawebHandler instaweb_handler(request);
    // Read POST body for JSON API endpoints.
    GoogleString request_body;
    if (request->method_number == M_POST) {
      apr_status_t body_ret;
      if (!InstawebHandler::read_post_body(request, &request_body, &body_ret)) {
        request_body.clear();
        ap_log_rerror(APLOG_MARK, APLOG_WARNING, body_ret, request,
                      "Failed to read admin POST body (status=%d), "
                      "proceeding with empty body",
                      body_ret);
      }
    }
    // The fetch has to be buffered because if it's a cache lookup it could
    // complete asynchrously via the rewrite thread.
    server_context->AdminPage(
        false /* not global */, instaweb_handler.stripped_gurl(),
        instaweb_handler.query_params(), instaweb_handler.options(),
        instaweb_handler.MakeFetch(true /* buffered */, "local-admin"),
        request_body);
    ret = APACHE_OK;
  } else if (request_handler_str == kGlobalAdminHandler &&
             global_config->GlobalAdminAccessAllowed(gurl,
                                                     client_is_loopback)) {
    WarnIfNonLoopbackAdminAccess(ClientIpForAdminWarning(request),
                                 AdminHandlerFamily::kGlobalAdmin,
                                 kGlobalAdminHandler, message_handler);
    InstawebHandler instaweb_handler(request);
    // Read POST body for JSON API endpoints.
    GoogleString request_body;
    if (request->method_number == M_POST) {
      apr_status_t body_ret;
      if (!InstawebHandler::read_post_body(request, &request_body, &body_ret)) {
        request_body.clear();
        ap_log_rerror(APLOG_MARK, APLOG_WARNING, body_ret, request,
                      "Failed to read global admin POST body (status=%d), "
                      "proceeding with empty body",
                      body_ret);
      }
    }
    // The fetch has to be buffered because if it's a cache lookup it could
    // complete asynchrously via the rewrite thread.
    server_context->AdminPage(
        true /* global */, instaweb_handler.stripped_gurl(),
        instaweb_handler.query_params(), instaweb_handler.options(),
        instaweb_handler.MakeFetch(true /* buffered */, "global-admin"),
        request_body);
    ret = APACHE_OK;
  } else if (global_config->enable_cache_purge() &&
             !global_config->purge_method().empty() &&
             (global_config->purge_method() == request->method)) {
    InstawebHandler instaweb_handler(request);
    AdminSite* admin_site = server_context->admin_site();
    // I'm not convinced that the purge handler must complete synchronously.  It
    // schedules work on the rewrite driver factory's scheduler, and while in my
    // testing it processes everything on the calling thread I'm not sure this
    // is part of the contract.  The response is just headers and a few bytes of
    // body, so buffering is basically free.  To be on the safe side let's
    // buffer this one too.
    admin_site->PurgeHandler(
        instaweb_handler.original_url_, server_context->cache_path(),
        instaweb_handler.MakeFetch(true /* buffered */, "purge"));
    ret = APACHE_OK;
  } else if (request_handler_str == kConsoleHandler &&
             global_config->ConsoleAccessAllowed(gurl, client_is_loopback)) {
    WarnIfNonLoopbackAdminAccess(ClientIpForAdminWarning(request),
                                 AdminHandlerFamily::kConsole, kConsoleHandler,
                                 message_handler);
    InstawebHandler instaweb_handler(request);
    server_context->ConsoleHandler(
        *instaweb_handler.options(), AdminSite::kOther,
        instaweb_handler.query_params(),
        instaweb_handler.MakeFetch(false /* unbuffered */, "console"));
    ret = APACHE_OK;
  } else if (request_handler_str == kMessageHandler &&
             global_config->MessagesAccessAllowed(gurl, client_is_loopback)) {
    WarnIfNonLoopbackAdminAccess(ClientIpForAdminWarning(request),
                                 AdminHandlerFamily::kMessages, kMessageHandler,
                                 message_handler);
    InstawebHandler instaweb_handler(request);
    server_context->MessageHistoryHandler(
        *instaweb_handler.options(), AdminSite::kOther,
        instaweb_handler.MakeFetch(false /* unbuffered */, "messages"));
    ret = APACHE_OK;
  } else if (request_handler_str == kLogRequestHeadersHandler) {
    // For testing CustomFetchHeader.
    GoogleString output;
    StringWriter writer(&output);
    HeaderLoggingData header_logging_data(&writer, message_handler);
    apr_table_do(&log_request_headers, &header_logging_data,
                 request->headers_in, NULL);

    write_handler_response(output, request, kContentTypeJavascript, "public");
    ret = APACHE_OK;
  } else if (strcmp(request->handler, kGenerateResponseWithOptionsHandler) ==
                 0 &&
             request->uri != nullptr) {
    // This handler is only needed for apache_system_test. It adds headers to
    // headers_out and/or err_headers_out to test handling of parameters in
    // those resources.
    if (strstr(request->parsed_uri.query, "headers_out") != nullptr) {
      apr_table_add(request->headers_out, "PageSpeed", "off");
    } else if (strstr(request->parsed_uri.query, "headers_errout") != nullptr) {
      apr_table_add(request->err_headers_out, "PageSpeed", "off");
    } else if (strstr(request->parsed_uri.query, "headers_override") !=
               nullptr) {
      apr_table_add(request->headers_out, "PageSpeed", "off");
      apr_table_add(request->headers_out, "PageSpeedFilters",
                    "-remove_comments");
      apr_table_add(request->err_headers_out, "PageSpeed", "on");
      apr_table_add(request->err_headers_out, "PageSpeedFilters",
                    "+remove_comments");
    } else if (strstr(request->parsed_uri.query, "headers_combine") !=
               nullptr) {
      apr_table_add(request->headers_out, "PageSpeed", "on");
      apr_table_add(request->err_headers_out, "PageSpeedFilters",
                    "+remove_comments");
    }
  } else {
    const char* url = InstawebContext::MakeRequestUrl(*global_config, request);
    // Do not try to rewrite our own sub-request.
    if (url != nullptr) {
      GoogleUrl gurl(url);
      if (!gurl.IsWebValid()) {
        ap_log_rerror(APLOG_MARK, APLOG_DEBUG, APR_SUCCESS, request,
                      "Ignoring invalid URL: %s", gurl.spec_c_str());
      } else if (IsBeaconUrl(global_config->beacon_url(), gurl)) {
        ret = instaweb_beacon_handler(request, server_context);
        // For the beacon accept any method; for all others only allow GETs.
      } else if (request->method_number != M_GET) {
        ap_log_rerror(APLOG_MARK, APLOG_DEBUG, APR_SUCCESS, request,
                      "Not rewriting non-GET %d of %s", request->method_number,
                      gurl.spec_c_str());
      } else if (gurl.PathSansLeaf() ==
                 server_context->apache_factory()->static_asset_prefix()) {
        instaweb_static_handler(request, server_context);
        ret = APACHE_OK;
      } else if (!is_pagespeed_subrequest(request) &&
                 handle_as_resource(server_context, request, &gurl)) {
        ret = APACHE_OK;
      }

      // Check for HTTP_NO_CONTENT here since that's the status used for a
      // successfully handled beacon.
      if (ret != APACHE_OK && ret != HTTP_NO_CONTENT &&
          gurl.Host() != "localhost" &&
          (global_config->slurping_enabled() || global_config->test_proxy() ||
           !global_config->domain_lawyer()->proxy_suffix().empty())) {
        // TODO(jmarantz): Consider moving the InstawebHandler up above
        // where we assign 'const char* url' above because we are repeating
        // a bunch of string-hacking here in the constructor.  However, we
        // really want the query-param evaluation happening inside the
        // constructor here.
        InstawebHandler instaweb_handler(request);
        if (instaweb_handler.ProxyUrl()) {
          ret = APACHE_OK;
        }
      }
    }
  }
  return ret;
}

// This translator must be inserted into the translate_name chain
// prior to mod_rewrite.  By saving the original URL in a
// request->notes and using that in our handler, we prevent
// mod_rewrite from borking URL names that need to be handled by
// mod_pagespeed.
//
// This hack seems to be the most robust way to immunize mod_pagespeed
// from when mod_rewrite rewrites the URL.  We still need mod_rewrite
// to do required complex processing of the filename (e.g. prepending
// the DocumentRoot) so mod_authz_host is happy, so we return DECLINED
// even for mod_pagespeed resources.
//
// One alternative strategy is to return OK to bypass mod_rewrite
// entirely, but then we'd have to duplicate the functionality in
// mod_rewrite that prepends the DocumentRoot, which is itself
// complex.  See mod_rewrite.c:hook_fixup(), and look for calls to
// ap_document_root().
//
// Or we could return DECLINED but set a note "mod_rewrite_rewritten"
// to try to convince mod_rewrite to leave our URLs alone, which seems
// fragile as that's an internal string literal in mod_rewrite.c and
// is not documented anywhere.
//
// Another strategy is to return OK but leave request->filename NULL.
// In that case, the server kernel generates an ominous 'info' message:
//
//     [info] [client ::1] Module bug?  Request filename is missing for URI
//     /mod_pagespeed_statistics
//
// This is generated by httpd/src/server/request.c line 486, and right
// above that is this comment:
//
//     "OK" as a response to a real problem is not _OK_, but to
//     allow broken modules to proceed, we will permit the
//     not-a-path filename to pass the following two tests.  This
//     behavior may be revoked in future versions of Apache.  We
//     still must catch it later if it's heading for the core
//     handler.  Leave INFO notes here for module debugging.
//
// It seems like the simplest, most robust approach is to squirrel
// away the original URL *before* mod_rewrite sees it in
// kPagespeedOriginalUrl "mod_pagespeed_url" and use *that* rather than
// request->unparsed_uri (which mod_rewrite might have mangled) when
// processing the request.
//
// Additionally we store whether or not this request is a pagespeed
// resource or not in kResourceUrlNote.
/* static */
apr_status_t InstawebHandler::save_url_hook(request_rec* request) {
  ApacheServerContext* server_context =
      InstawebContext::ServerContextFromServerRec(request->server);
  return save_url_in_note(request, server_context);
}

/* static */
apr_status_t InstawebHandler::save_url_in_note(
    request_rec* request, ApacheServerContext* server_context) {
  // Escape ASAP if we're in unplugged mode.
  if (server_context->global_config()->unplugged()) {
    return DECLINED;
  }

  // This call to MakeRequestUrl() not only returns the url but also
  // saves it for future use so that if another module changes the
  // url in the request, we still have the original one.
  const char* url = InstawebContext::MakeRequestUrl(
      *server_context->global_options(), request);
  GoogleUrl gurl(url);

  bool bypass_mod_rewrite = false;
  if (gurl.IsWebValid()) {
    // Note: We cannot use request->handler because it may not be set yet :(
    // TODO(sligocki): Make this robust to custom statistics and beacon URLs.
    StringPiece leaf = gurl.LeafSansQuery();
    if (leaf == kStatisticsHandler || leaf == kConsoleHandler ||
        leaf == kGlobalStatisticsHandler || leaf == kMessageHandler ||
        leaf == kAdminHandler ||
        gurl.PathSansLeaf() ==
            server_context->apache_factory()->static_asset_prefix() ||
        IsBeaconUrl(server_context->global_options()->beacon_url(), gurl) ||
        server_context->IsPagespeedResource(gurl)) {
      bypass_mod_rewrite = true;
    }
  }

  if (bypass_mod_rewrite) {
    apr_table_set(request->notes, kResourceUrlNote, kResourceUrlYes);
  } else {
    // Leave behind a note for non-instaweb requests that says that
    // our handler got called and we decided to pass.  This gives us
    // one final chance at serving resources in the presence of a
    // module that intercepted 'translate_name' before mod_pagespeed.
    // The absence of this marker indicates that translate_name did
    // not get a chance to run, and thus we should try to look at
    // the URI directly.
    apr_table_set(request->notes, kResourceUrlNote, kResourceUrlNo);
  }
  return DECLINED;
}

// Override core_map_to_storage for pagespeed resources.
/* static */
apr_status_t InstawebHandler::instaweb_map_to_storage(request_rec* request) {
  if (request->proxyreq == PROXYREQ_REVERSE) {
    // If Apache is acting as a reverse proxy for this request there is no
    // point in walking the directory because it doesn't apply to this
    // server's htdocs tree, it applies to the server we are proxying to.
    // This can result in it raising a 403 because some path doesn't exist.
    // Note that experimenting shows that it doesn't matter if we return OK
    // or DECLINED here, at least with URLs that aren't overly long; also,
    // we actually fetch the DECODED URL (no .pagespeed. etc) from the proxy
    // server and we rewrite it ourselves.
    return DECLINED;
  }

  if (request->filename == nullptr) {
    // We set filename to NULL below, and it appears other modules do too
    // (the WebSphere plugin for example; see issue 610), so to prevent a
    // dereference of NULL.
    return DECLINED;
  }

  ApacheServerContext* server_context =
      InstawebContext::ServerContextFromServerRec(request->server);
  if (server_context->global_config()->unplugged()) {
    // If we're in unplugged mode then none of our hooks apply so escape ASAP.
    return DECLINED;
  }

  if (get_instaweb_resource_url(request, server_context) == nullptr) {
    return DECLINED;
  }

  // core_map_to_storage does at least two things:
  //  1) checks filename length limits
  //  2) determines directory specific options
  // We want (2) but not (1).  If we simply return OK we will keep
  // core_map_to_storage from running and let through our long filenames but
  // resource requests that require regeneration will not respect directory
  // specific options.
  //
  // To fix this we need to be more dependent on apache internals than we
  // would like.  core_map_to_storage always calls ap_directory_walk(request),
  // which does both (1) and (2) and appears to work entirely off of
  // request->filename.  But ap_directory_walk doesn't care whether the last
  // request->segment of the path actually exists.  So if we change the
  // request->filename from something like:
  //    /var/www/path/to/LEAF_WHICH_MAY_BE_HUGE.pagespeed.FILTER.HASH.EXT
  // to:
  //    /var/www/path/to/A
  // then we will bypass the filename length limit without harming the load of
  // directory specific options.
  //
  // So: modify request->filename in place to cut it off after the last '/'
  // character and replace the whole leaf with 'A', and then call
  // ap_directory_walk to figure out custom options.
  char* filename_starting_at_last_slash = strrchr(request->filename, '/');
  if (filename_starting_at_last_slash != nullptr &&
      filename_starting_at_last_slash[1] != '\0') {
    filename_starting_at_last_slash[1] = 'A';
    filename_starting_at_last_slash[2] = '\0';
  }
  ap_directory_walk(request);

  // mod_speling, if enabled, looks for the filename on the file system,
  // and tries to "correct" the spelling.  This is not desired for
  // mod_pagesped resources, but mod_speling will not do this damage
  // when request->filename == NULL.  See line 219 of
  // http://svn.apache.org/viewvc/httpd/httpd/trunk/modules/mappers/
  // mod_speling.c?revision=983065&view=markup
  //
  // Note that mod_speling runs 'hook_fixups' at APR_HOOK_LAST, and
  // we are currently running instaweb_map_to_storage in map_to_storage
  // HOOK_FIRST-2, which is a couple of phases before hook_fixups.
  //
  // If at some point we stop NULLing the filename here we need to modify the
  // code above that mangles it to use a temporary buffer instead.
  request->filename = nullptr;

  // While setting request->filename helps get mod_speling (as well as
  // mod_mime and mod_mime_magic) out of our hair, it causes crashes
  // in mod_negotiation (if on) when finfo.filetype is APR_NOFILE.
  // So we give it a type that's something other than APR_NOFILE (plus we
  // also don't want APR_DIR, since that would make mod_mime to set the
  // mimetype to httpd/unix-directory).
  request->finfo.filetype = APR_UNKFILE;

  // Keep core_map_to_storage from running and rejecting our long filenames.
  return APACHE_OK;
}

/* static */
void InstawebHandler::AboutToBeDoneWithRecorder(request_rec* request,
                                                IproRecorder* recorder) {
  apr_pool_cleanup_kill(request->pool, recorder, DeleteInPlaceRecorder);
}

}  // namespace net_instaweb
