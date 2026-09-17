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

#include "pagespeed/apache/apache_writer.h"

#include "apr_buckets.h"  // NOLINT
#include "apr_strings.h"  // for apr_pstrdup    // NOLINT
#include "base/logging.h"
#include "http_protocol.h"  // NOLINT
#include "net/instaweb/http/public/async_fetch.h"
#include "pagespeed/apache/apache_mmap_bucket.h"
#include "pagespeed/apache/header_util.h"
#include "pagespeed/kernel/base/mapped_shared_string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "util_filter.h"  // NOLINT - for ap_pass_brigade

namespace net_instaweb {

namespace {

// Output filters known to hand DATA buckets to the network without
// transforming, re-slicing, or retaining raw pointers into their bytes
// across blocking waits: the stock httpd 2.4 straight-line serve chain.
// Everything else (deflate/brotli, ssl, http2, substitute, includes,
// ratelimit, third-party filters) disables aliasing, fail-closed.  httpd
// stores registered filter names lowercased; compare case-insensitively
// anyway.
const char* const kVerbatimOutputFilters[] = {
    "byterange",         // partitions via bucket split(), never reads.  In
                         // practice unreachable for aliasing: the Range
                         // check in RequestServesBodyVerbatim() already
                         // forces the copy path for any ranged request;
                         // listed so the ever-present filter does not
                         // disqualify plain requests.
    "chunk",             // brackets data buckets with metadata only.
    "content_length",    // counts known-length buckets without reading.
    "core",              // the send engine; re-reads before every writev.
    "http_header",       // emits headers, passes body through.
    "http_outerror",     // error bookkeeping passthrough.
    "log_input_output",  // mod_logio byte counting, no data access.
    "logio_ttfb_out",    // mod_logio first-byte timestamp passthrough.
    "old_write",         // ap_rwrite shim; flushes its own buffer, then
                         // passes foreign brigades through untouched.
    "reqtimeout",        // mod_reqtimeout's output filter (reqtimeout_eor,
                         // httpd 2.4 modules/filters/mod_reqtimeout.c):
                         // input-timeout bookkeeping only.  It looks at
                         // whether the brigade's LAST bucket is the EOR
                         // metadata bucket (to reset its per-connection
                         // timeout stage) and then ap_pass_brigade()s the
                         // brigade on untouched -- it never reads,
                         // re-slices, or retains data buckets.  Enabled by
                         // default on Debian/Ubuntu; omitting it disabled
                         // aliasing on every stock install.
};

bool IsVerbatimOutputFilter(const char* name) {
  for (size_t i = 0; i < arraysize(kVerbatimOutputFilters); ++i) {
    if (StringCaseEqual(name, kVerbatimOutputFilters[i])) {
      return true;
    }
  }
  return false;
}

}  // namespace

ApacheWriter::ApacheWriter(request_rec* r, ThreadSystem* thread_system)
    : request_(r),
      headers_out_(false),
      disable_downstream_header_filters_(false),
      strip_cookies_(false),
      content_length_(AsyncFetch::kContentLengthUnknown),
      thread_system_(thread_system) {
  apache_request_thread_.reset(thread_system->GetThreadId());
}

ApacheWriter::~ApacheWriter() {}

bool ApacheWriter::Write(const StringPiece& str, MessageHandler* handler) {
  DCHECK(apache_request_thread_->IsCurrentThread());
  DCHECK(headers_out_);
  ap_rwrite(str.data(), str.size(), request_);
  return true;
}

bool ApacheWriter::Flush(MessageHandler* handler) {
  DCHECK(apache_request_thread_->IsCurrentThread());
  DCHECK(headers_out_);
  ap_rflush(request_);
  return true;
}

void ApacheWriter::SettleOutputFilters() {
  DCHECK(apache_request_thread_->IsCurrentThread());
  DCHECK(headers_out_);
  conn_rec* c = request_->connection;
  if (c == nullptr) {
    return;  // Nothing to settle; the eligibility walk fails closed.
  }
  apr_bucket_brigade* bb = apr_brigade_create(request_->pool, c->bucket_alloc);
  // The return status is deliberately ignored: no body bytes were sent,
  // and a filter that errors on an empty brigade will error again on the
  // body pass, where the serve's normal failure handling applies.
  ap_pass_brigade(request_->output_filters, bb);
  apr_brigade_destroy(bb);
}

bool ApacheWriter::RequestServesBodyVerbatim(GoogleString* blocker) const {
  request_rec* r = request_;
  if (r->main != nullptr || r->prev != nullptr || r->next != nullptr) {
    if (blocker != nullptr) {
      *blocker = "subrequest or internal-redirect chain";
    }
    return false;
  }
  if (r->header_only) {
    if (blocker != nullptr) {
      *blocker = "header-only request";
    }
    return false;
  }
  if (r->connection == nullptr) {
    if (blocker != nullptr) {
      *blocker = "no connection";
    }
    return false;
  }
  if (apr_table_get(r->headers_in, "Range") != nullptr) {
    if (blocker != nullptr) {
      *blocker = "Range request";  // The byterange filter would re-slice.
    }
    return false;
  }
  // Walk the output chain: the request-level list links onward into the
  // connection-level filters, ending at the core network filter.  Every
  // hop must be a known verbatim pass-through; ssl / http2 / deflate /
  // anything unrecognized disables aliasing (fail-closed).
  for (ap_filter_t* f = r->output_filters; f != nullptr; f = f->next) {
    if (f->frec == nullptr || f->frec->name == nullptr ||
        !IsVerbatimOutputFilter(f->frec->name)) {
      if (blocker != nullptr) {
        *blocker = StrCat("output filter '",
                          (f->frec != nullptr && f->frec->name != nullptr)
                              ? f->frec->name
                              : "(unnamed)",
                          "' is not a known verbatim pass-through");
      }
      return false;
    }
  }
  return true;
}

bool ApacheWriter::WriteMappedAliased(const StringPiece& span,
                                      const MappedSharedString& pin,
                                      Variable* copied_out_stat,
                                      Variable* renew_fail_stat,
                                      MessageHandler* handler) {
  DCHECK(apache_request_thread_->IsCurrentThread());
  DCHECK(headers_out_);
  conn_rec* c = request_->connection;
  apr_bucket_brigade* bb = apr_brigade_create(request_->pool, c->bucket_alloc);
  apr_bucket* b = CreateMmapAliasBucket(span, pin, copied_out_stat,
                                        renew_fail_stat, c->bucket_alloc);
  if (b == nullptr) {
    AbortConnection();
    return false;
  }
  APR_BRIGADE_INSERT_TAIL(bb, b);
  // Synchronous on the request thread.  Downstream, the bucket's read()
  // barrier revalidates the lease before every writev burst and the
  // setaside() hook copies out anything parked past this pass; the pin is
  // released by the bucket's destroy() when the last reference dies.
  const apr_status_t rv = ap_pass_brigade(request_->output_filters, bb);
  // Filters must consume the brigade (send, buffer, or setaside).  Buckets
  // left behind mean some downstream hop bailed without an error httpd
  // propagated -- the swallowed-failure class -- and those bytes will never
  // reach the client; treat it as a failed serve rather than report success
  // with a silently truncated body.
  const bool leftover = (rv == APR_SUCCESS) && !APR_BRIGADE_EMPTY(bb);
  apr_brigade_destroy(bb);
  if (rv != APR_SUCCESS || leftover) {
    // Either a downstream write error or the barrier detected a torn
    // borrow.  The Content-Length promise cannot be met any more; abort
    // the connection rather than let a truncated-or-corrupt body pass as
    // complete.
    AbortConnection();
    return false;
  }
  return true;
}

void ApacheWriter::AbortConnection() {
  if (request_->connection != nullptr) {
    request_->connection->aborted = 1;
  }
}

void ApacheWriter::OutputHeaders(ResponseHeaders* response_headers) {
  DCHECK(apache_request_thread_->IsCurrentThread());
  DCHECK(!headers_out_);
  if (headers_out_) {
    return;
  }
  headers_out_ = true;

  // Apache2 defaults to set the status line as HTTP/1.1.  If the
  // original content was HTTP/1.0, we need to force the server to use
  // HTTP/1.0.  I'm not sure why/whether we need to do this; it was in
  // mod_static from the sdpy project, which is where I copied this
  // code from.
  if ((response_headers->major_version() == 1) &&
      (response_headers->minor_version() == 0)) {
    apr_table_set(request_->subprocess_env, "force-response-1.0", "1");
  }

  // It doesn't matter how the origin transferred the request to us;
  // Apache will fill this data in when it issues the response.
  response_headers->RemoveAll(HttpAttributes::kTransferEncoding);
  response_headers->RemoveAll(HttpAttributes::kContentLength);
  if (content_length_ != AsyncFetch::kContentLengthUnknown) {
    ap_set_content_length(request_, content_length_);
  }
  ResponseHeadersToApacheRequest(*response_headers, request_);
  request_->status = response_headers->status_code();
  if (disable_downstream_header_filters_) {
    DisableDownstreamHeaderFilters(request_);
  }

  // TODO(jefftk): Sanitize strips cookies and a lot of other headers.  It's
  // being run after ResponseHeadersToApacheRequest(), however, which means it's
  // not actually doing anything.  This code is not doing what it says it does,
  // and should be fixed, but it's possible some users like the mobilizing proxy
  // are depending on the current behavior.
  if (strip_cookies_ && response_headers->Sanitize()) {
    response_headers->ComputeCaching();
  }

  const char* content_type =
      response_headers->Lookup1(HttpAttributes::kContentType);
  if (content_type != nullptr) {
    // ap_set_content_type does not make a copy of the string, we need
    // to duplicate it.  Note that we will update the content type below,
    // after transforming the headers.
    ap_set_content_type(request_, apr_pstrdup(request_->pool, content_type));
  }

  // We don't set the content-length here, because we don't know it yet.
}

}  // namespace net_instaweb
