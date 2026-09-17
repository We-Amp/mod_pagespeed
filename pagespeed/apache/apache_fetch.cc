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

#include "pagespeed/apache/apache_fetch.h"

#include <algorithm>
#include <atomic>

#include "base/logging.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "pagespeed/apache/apache_mmap_bucket.h"
#include "pagespeed/kernel/base/abstract_mutex.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/mapped_shared_string.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/thread/scheduler_sequence.h"
#include "pagespeed/system/system_rewrite_options.h"

namespace net_instaweb {

namespace {

// Emit-time forced-wrap margin for the lease-pinned zero-copy serve: if a
// ceiling-forced wrap -- which ignores the read lease -- is already
// reachable within this window when the body is emitted, the stripe is too
// contended to alias; serve a verified copy instead.  Strictly larger than
// the bucket's read-barrier margin so an admitted serve is never de-aliased
// (and double-counted) by its very first read.
const uint64_t kEmitMarginNs = 2 * kMmapAliasReadBarrierMarginNs;

// Whether the opted-in-but-ineligible diagnosis was already logged.  An
// ineligible chain disqualifies EVERY serve on it, so the log names the
// first blocker once per process and the zerocopy_serve_ineligible counter
// carries the ongoing signal (without either, the degrade was
// indistinguishable from a dead code path).
std::atomic<bool> ineligible_serve_logged{false};

}  // namespace

ApacheFetch::ApacheFetch(const GoogleString& mapped_url, StringPiece debug_info,
                         RewriteDriver* driver, ApacheWriter* apache_writer,
                         RequestHeaders* request_headers,
                         const RequestContextPtr& request_context,
                         const RewriteOptions* options, MessageHandler* handler)
    : AsyncFetch(request_context),
      mapped_url_(mapped_url),
      apache_writer_(apache_writer),
      options_(options),
      message_handler_(handler),
      done_(false),
      wait_called_(false),
      handle_error_(true),
      squelch_output_(false),
      headers_sent_(false),
      status_ok_(false),
      is_proxy_(false),
      buffered_(true),
      driver_(driver),
      scheduler_(nullptr) {
  // We are proxying content, and the caching in the http configuration
  // should not apply; we want to use the caching from the proxy.
  apache_writer_->set_disable_downstream_header_filters(true);
  // TODO(jefftk): ApacheWriter has a bug where it doesn't actually strip the
  // cookies when we ask it to.  This is hard to fix because we're not sure
  // which uses depend on cookies being passed through.
  apache_writer_->set_strip_cookies(true);

  request_headers->RemoveAll(HttpAttributes::kCookie);
  request_headers->RemoveAll(HttpAttributes::kCookie2);
  SetRequestHeadersTakingOwnership(request_headers);

  debug_info.CopyToString(&debug_info_);

  driver_->SetRequestHeaders(*request_headers);
  driver_->IncrementAsyncEventsCount();
  driver_->RunTasksOnRequestThread();
  scheduler_ = driver_->scheduler();
}

ApacheFetch::~ApacheFetch() { driver_->DecrementAsyncEventsCount(); }

// Called by other threads unless buffered=false.
void ApacheFetch::HandleHeadersComplete() {
  if (buffered_) {
    // Do nothing on this thread right now.  When done waiting we'll deal with
    // headers on the request thread.
    return;
  }
  SendOutHeaders();
}

// Called on the request thread.
void ApacheFetch::SendOutHeaders() {
  int status_code = response_headers()->status_code();

  // Setting status_ok = true tells InstawebHandler that we've handled this
  // request and sent out the response.  If we leave it as false InstawebHandler
  // will DECLINE the request and another handler will deal with it.
  status_ok_ = (status_code != 0) && (status_code < 400);

  if (handle_error_ || status_ok_) {
    StringPiece error_message = "";  // No error by default.
    // 304 and 204 responses shouldn't have content lengths and aren't expected
    // to have Content-Types.  All other responses should.
    if ((status_code != HttpStatus::kNotModified) &&
        (status_code != HttpStatus::kNoContent) &&
        !response_headers()->Has(HttpAttributes::kContentType)) {
      status_code = HttpStatus::kForbidden;
      status_ok_ = false;
      response_headers()->SetStatusAndReason(HttpStatus::kForbidden);
      response_headers()->Add(HttpAttributes::kContentType, "text/html");
      response_headers()->RemoveAll(HttpAttributes::kCacheControl);
      error_message = "Missing Content-Type required for proxied resource";
    }

    int64 now_ms = scheduler_->timer()->NowMs();
    response_headers()->SetDate(now_ms);

    // http://msdn.microsoft.com/en-us/library/ie/gg622941(v=vs.85).aspx
    // Script and styleSheet elements will reject responses with
    // incorrect MIME types if the server sends the response header
    // "X-Content-Type-Options: nosniff". This is a security feature
    // that helps prevent attacks based on MIME-type confusion.
    if (!is_proxy_) {
      response_headers()->Add("X-Content-Type-Options", "nosniff");
    }

    // TODO(sligocki): Add X-Mod-Pagespeed header.

    // Default cache-control to nocache.  304 and 204 responses are exempt,
    // like they are for the Content-Type check above: introducing a
    // Cache-Control on a 304 that the full response never had would make
    // clients update their stored response with it (RFC 7232 section 4.1),
    // e.g. poisoning a long-TTL .pagespeed. resource with no-cache.
    if ((status_code != HttpStatus::kNotModified) &&
        (status_code != HttpStatus::kNoContent) &&
        !response_headers()->Has(HttpAttributes::kCacheControl)) {
      response_headers()->Add(HttpAttributes::kCacheControl,
                              HttpAttributes::kNoCacheMaxAge0);
    }
    response_headers()->ComputeCaching();

    if (content_length_known() && error_message.empty()) {
      apache_writer_->set_content_length(content_length());
    }
    apache_writer_->OutputHeaders(response_headers());
    headers_sent_ = true;
    if (!error_message.empty()) {
      if (buffered_) {
        error_message.CopyToString(&output_bytes_);
      } else {
        apache_writer_->Write(error_message, message_handler_);
      }
      squelch_output_ = true;
    }
  } else {
    // We are not handling this error response; the caller will answer the
    // request itself after Wait().  Suppress any output the fetch may still
    // produce: nothing may be written to the ApacheWriter before
    // OutputHeaders(), and we must not emit a second response's bytes.
    squelch_output_ = true;
  }
}

// Called by other threads.
void ApacheFetch::HandleDone(bool success) {
  {
    ScopedMutex lock(scheduler_->mutex());
    done_ = true;

    if (status_ok_ && !success) {
      message_handler_->Message(
          kWarning,
          "Response for url %s issued with status %d %s but "
          "failed to complete.",
          mapped_url_.c_str(), response_headers()->status_code(),
          response_headers()->reason_phrase());
    }

    if (buffered_) {
      // Let our owner on the apache request thread know we're done and they
      // will send out anything that still needs sending and then delete us.
      scheduler_->Signal();
    }
  }
}

bool ApacheFetch::WriteMapped(const StringPiece& mmap_sp,
                              const MappedSharedString& keepalive,
                              MessageHandler* handler) {
  // Complete headers even for an empty body, mirroring AsyncFetch::Write's
  // contract (headers-complete precedes any body decision).
  if (!headers_complete()) {
    HeadersComplete();
  }
  if (mmap_sp.empty()) {
    return true;  // Mirror Write(): empty writes are no-ops.
  }
  if (request_headers()->method() == RequestHeaders::kHead) {
    return true;  // Mirror Write(): no body bytes on a HEAD response.
  }
  if (squelch_output_) {
    return true;  // Suppressing further output after an error message.
  }
  // Apache posture: the aliased sink is opt-in experimental.  The engine
  // routes mapped bytes here whenever CycloneZeroCopyServe is enabled
  // (default on for the nginx GA), but Apache aliases only when the option
  // was EXPLICITLY configured on; otherwise it serves a verified copy.
  bool alias_opted_in = false;
  {
    ScopedMutex lock(scheduler_->mutex());
    // Plain dynamic_cast, not SystemRewriteOptions::DynamicCast: the
    // latter DCHECKs non-null, but a bare RewriteOptions (no system scope,
    // e.g. under test) simply means there is no opt-in.
    const SystemRewriteOptions* system_options =
        dynamic_cast<const SystemRewriteOptions*>(options_);
    alias_opted_in = system_options != nullptr &&
                     system_options->cyclone_zero_copy_serve() &&
                     system_options->has_cyclone_zero_copy_serve();
  }
  Statistics* stats = driver_->server_context()->statistics();
  Variable* aliased_stat = stats != nullptr
                               ? stats->FindVariable("zerocopy_serve_aliased")
                               : nullptr;
  Variable* copied_stat = stats != nullptr
                              ? stats->FindVariable("zerocopy_serve_copied_out")
                              : nullptr;
  Variable* renew_fail_stat =
      stats != nullptr ? stats->FindVariable("zerocopy_serve_renew_fail_reset")
                       : nullptr;
  // Alias only a committed, verbatim 200 on the unbuffered request-thread
  // streaming path.  RequestServesBodyVerbatim() excludes subrequests,
  // header-only, Range, and any output filter (deflate/ssl/http2/unknown)
  // that could transform, re-slice, or retain the aliased bytes.  An
  // opted-in serve that fails these gates is counted (and its first
  // blocker logged once per process): the operator asked for aliasing and
  // is not getting it, and without the signal that degrade reads as
  // aliased=0/copied_out=0 -- a dead path.
  bool alias_eligible = false;
  if (alias_opted_in) {
    GoogleString blocker;
    if (buffered_) {
      blocker = "buffered (non-streaming) fetch";
    } else if (!headers_sent_) {
      blocker = "headers not sent before body";
    } else if (response_headers()->status_code() != HttpStatus::kOK) {
      blocker =
          StrCat("status ", IntegerToString(response_headers()->status_code()),
                 " (only 200 aliases)");
    } else {
      // Let self-dispatching filters settle against the committed
      // response before the walk: a mod_filter harness
      // (AddOutputFilterByType / FilterChain) sits in EVERY request's
      // output chain until the first brigade, then dispatches on the
      // response and removes itself when no provider matches.  One empty
      // brigade runs exactly that dispatch, so an unmatched harness
      // (e.g. a by-type DEFLATE harness on an image response) leaves the
      // chain before the walk, while a matched harness stays and
      // correctly disqualifies aliasing.
      apache_writer_->SettleOutputFilters();
      if (apache_writer_->RequestServesBodyVerbatim(&blocker)) {
        alias_eligible = true;
      }
    }
    if (!alias_eligible) {
      Variable* ineligible_stat =
          stats != nullptr ? stats->FindVariable("zerocopy_serve_ineligible")
                           : nullptr;
      if (ineligible_stat != nullptr) {
        ineligible_stat->Add(1);
      }
      if (!ineligible_serve_logged.exchange(true)) {
        message_handler_->Message(
            kInfo,
            "CycloneZeroCopyServe: serve for %s is not alias-eligible (%s); "
            "serving a verified copy. Counted in zerocopy_serve_ineligible; "
            "logged once per process.",
            mapped_url_.c_str(), blocker.c_str());
      }
    }
  }
  if (alias_eligible) {
    // Emit-time aliasing decision (intent-checked).  RenewLeaseStrict()
    // stamps a fresh lease AND revalidates the borrow, the same protocol
    // the initial cache read used; kOk means no wrap can overwrite the
    // region while the lease stays renewed, which the bucket's read()
    // barrier does before every send.
    const LeaseRenewal verdict = keepalive.RenewLeaseStrict();
    if (verdict == LeaseRenewal::kOk &&
        keepalive.NsUntilForcedWrap() > kEmitMarginNs) {
      if (aliased_stat != nullptr) {
        aliased_stat->Add(1);
      }
      return apache_writer_->WriteMappedAliased(mmap_sp, keepalive, copied_stat,
                                                renew_fail_stat, handler);
    }
    if (verdict == LeaseRenewal::kTorn) {
      // The borrow was already overwritten: there are no correct bytes to
      // serve.  Headers (with Content-Length) are on the wire, so fail
      // closed by aborting the connection -- never a corrupt-but-complete
      // body.
      if (renew_fail_stat != nullptr) {
        renew_fail_stat->Add(1);
      }
      apache_writer_->AbortConnection();
      return false;
    }
    // kCopyNow (wrap in flight, region intact), kLeasesOff (no lease
    // protection), or a forced wrap within the margin: fall through to the
    // verified copy.
  }
  // De-alias by copying, with the copy-then-verify protocol
  // (CopyMappedVerified): a forced wrap ignores the lease and can race the
  // memcpy, so the borrow is re-checked AFTER the bytes were copied.  kTorn
  // fails closed; kOk/kCopyNow (region intact) and kLeasesOff (legacy, no
  // protection) serve the copy -- leases-off must never turn every serve
  // into a reset.
  GoogleString owned;
  if (!CopyMappedVerified(mmap_sp, keepalive, &owned)) {
    if (renew_fail_stat != nullptr) {
      renew_fail_stat->Add(1);
    }
    if (headers_sent_) {
      // The response (and its Content-Length promise) is committed: abort
      // the connection so the truncation is unambiguous on the wire.
      apache_writer_->AbortConnection();
    } else {
      // Uncommitted (buffered, or a suppressed-error flow that never sent
      // headers): poison the response so Wait()/SendOutHeaders can never
      // emit the promised 200 over a missing body.  A torn borrow has no
      // correct bytes; a clean 200 + Content-Length + empty body would be
      // a corrupt-but-complete response, the exact class this feature must
      // never produce.
      response_headers()->SetStatusAndReason(HttpStatus::kInternalServerError);
      // Drop the cached 200's freshness headers: a poisoned 500 carrying
      // "public, max-age=..." could be stored by an intermediary.  With
      // Cache-Control absent, SendOutHeaders adds no-cache.
      response_headers()->RemoveAll(HttpAttributes::kCacheControl);
      response_headers()->RemoveAll(HttpAttributes::kExpires);
      set_content_length(kContentLengthUnknown);
      output_bytes_.clear();
      squelch_output_ = true;
    }
    return false;
  }
  // The copied-out counter tracks the read-side degradation of
  // alias-ELIGIBLE serves only (mirroring the nginx sink); the ordinary
  // not-opted-in verified copy is this port's classic serve, not a
  // degrade.
  if (alias_eligible && copied_stat != nullptr) {
    copied_stat->Add(1);
  }
  return Write(owned, handler);
}

bool ApacheFetch::HandleWrite(const StringPiece& sp, MessageHandler* handler) {
  if (squelch_output_) {
    return true;  // Suppressing further output after writing error message.
  } else if (buffered_) {
    sp.AppendToString(&output_bytes_);
    return true;
  }
  return apache_writer_->Write(sp, handler);
}

bool ApacheFetch::HandleFlush(MessageHandler* handler) {
  if (squelch_output_ || buffered_) {
    // Don't pass flushes through when buffering, and never touch the
    // ApacheWriter for a squelched (suppressed error) response.
    return true;
  }
  return apache_writer_->Flush(handler);
}

// Called on the apache request thread.  Blocks until the request is retired.
void ApacheFetch::Wait() {
  if (wait_called_) {
    return;
  }
  wait_called_ = true;
  Timer* timer = scheduler_->timer();
  int64 start_ms = timer->NowMs();

  {
    ScopedMutex lock(scheduler_->mutex());

    // Compute the time we want to block on each call to RunTasksUntil
    // below, based on the in-place rewrite deadline and the
    // configured FetcherTimeoutMs.
    //
    // The role of this timeout here is to dictate how often we'll log
    // "Waiting for completion" messages.  The loop will not actually exit
    // until the request is completed, and we are dependent on timeouts
    // configured elsewhere in the code to guarantee that a completion will
    // come at some point.
    //
    // We pick the 'max' of those two values because that will ensure we don't
    // get a large number of spurious messages as a result of (say) configuring
    // the in-place rewrite deadline to be much higher than the fetcher timeout.
    int64 fetch_timeout_ms =
        std::max(options_->blocking_fetch_timeout_ms(),
                 static_cast<int64>(options_->in_place_rewrite_deadline_ms()));
    Scheduler::Sequence* scheduler_sequence = driver_->scheduler_sequence();
    while (!scheduler_sequence->RunTasksUntil(fetch_timeout_ms, &done_)) {
      int64 elapsed_ms = timer->NowMs() - start_ms;
      message_handler_->Message(kWarning,
                                "Waiting for completion of URL %s for %g sec.",
                                mapped_url_.c_str(), elapsed_ms / 1000.0);
    }
    CHECK(done_);

    // A 'true' return from RunTasksUntil means done_==true, but it
    // does not mean all tasks are exhausted.  For example, an
    // in-place rewrite deadline timeout will successfully break out
    // of RunTasksUntil, and we'll want to continue processing even
    // though we are going to retire the request.
    driver_->SwitchToQueuedWorkerPool();
  }
  if (buffered_) {
    SendOutHeaders();
    // Only write the body if headers actually went out; when handle_error_
    // is false and the response was an error, nothing may be sent.
    if (headers_sent_ && !output_bytes_.empty()) {
      apache_writer_->Write(output_bytes_, message_handler_);
    }
  }
}

bool ApacheFetch::IsCachedResultValid(const ResponseHeaders& headers) {
  ScopedMutex lock(scheduler_->mutex());
  return OptionsAwareHTTPCacheCallback::IsCacheValid(
      mapped_url_, *options_, request_context(), headers);
}

}  // namespace net_instaweb
