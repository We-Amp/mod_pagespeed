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

#include "pagespeed/system/daemon_ipro_recorder.h"

#include <vector>

#include "pagespeed/kernel/base/atomic_int32.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/http_options.h"
#include "pagespeed/kernel/http/request_headers.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "pagespeed/system/daemon_abi.h"
#include "pagespeed/system/daemon_adapter.h"
#include "pagespeed/system/daemon_health.h"

namespace net_instaweb {

AtomicInt32 DaemonIproRecorder::num_constructed_(0);

namespace {

// Joins every value of a repeated header into one comma-separated field
// value.  RFC 9110 says a repeated field IS one list, and the predicates on
// the other side are written against the list -- offering them one line at a
// time accepts combinations the list as a whole is refused for.
GoogleString JoinHeader(const ResponseHeaders& headers, StringPiece name) {
  ConstStringStarVector values;
  if (!headers.Lookup(name, &values)) {
    return GoogleString();
  }
  GoogleString joined;
  for (const GoogleString* value : values) {
    if (value == nullptr) {
      continue;
    }
    if (!joined.empty()) {
      StrAppend(&joined, ",");
    }
    StrAppend(&joined, *value);
  }
  return joined;
}

}  // namespace

DaemonIproRecorder::DaemonIproRecorder(const DaemonAbi* abi, void* cache,
                                       StringPiece socket_path,
                                       const DaemonRecordRequest& request,
                                       const HttpOptions& http_options,
                                       Timer* timer, MessageHandler* handler)
    : abi_(abi),
      cache_(cache),
      socket_path_(socket_path.data(), socket_path.size()),
      request_(request),
      http_options_(http_options),
      timer_(timer),
      handler_(handler) {
  num_constructed_.BarrierIncrement(1);
}

DaemonIproRecorder::~DaemonIproRecorder() = default;

int64 DaemonIproRecorder::num_constructed() { return num_constructed_.value(); }

bool DaemonIproRecorder::Write(const StringPiece& contents,
                               MessageHandler* /*handler*/) {
  if (failure_ || over_cap_) {
    // Already decided; keep taking the bytes so the response still reaches
    // the client, but stop growing a buffer nothing will use.
    return true;
  }
  if (static_cast<int64>(body_.size() + contents.size()) >
      kDaemonOriginalContentCapBytes) {
    // The cap is applied HERE, at the byte that crosses it, rather than to a
    // finished buffer: the whole point is not to hold the response in memory
    // in order to find out it was too big.  Not a failure -- a response that
    // is too large to keep as a durable original is an ordinary outcome, and
    // the response itself is unaffected.
    over_cap_ = true;
    body_.clear();
    return true;
  }
  contents.AppendToString(&body_);
  return true;
}

void DaemonIproRecorder::ConsiderResponseHeaders(
    HeadersKind headers_kind, ResponseHeaders* response_headers) {
  if (headers_kind != kPreliminaryHeaders || response_headers == nullptr) {
    return;
  }
  // The one thing worth knowing before the body arrives: whether what is
  // about to stream past is the origin's bytes or an encoded form of them.
  // Deciding now costs nothing; deciding at the end costs a buffered copy of
  // a body that was never recordable.
  const char* encoding =
      response_headers->Lookup1(HttpAttributes::kContentEncoding);
  if (encoding != nullptr && !StringCaseEqual(encoding, "identity")) {
    failure_ = true;
  }
}

void DaemonIproRecorder::SaveCacheControl(const char* cache_control) {
  cache_control_saved_ = true;
  saved_cache_control_ = cache_control == nullptr ? "" : cache_control;
}

void DaemonIproRecorder::BuildInput(const ResponseHeaders& response_headers,
                                    DaemonRecordInput* input) {
  input->url = request_.url;
  input->hostname = request_.hostname;
  input->scheme = request_.scheme;

  const char* content_type =
      response_headers.Lookup1(HttpAttributes::kContentType);
  if (content_type != nullptr) {
    input->content_type = content_type;
  }
  const char* content_encoding =
      response_headers.Lookup1(HttpAttributes::kContentEncoding);
  if (content_encoding != nullptr) {
    input->content_encoding = content_encoding;
  }
  const char* etag = response_headers.Lookup1(HttpAttributes::kEtag);
  if (etag != nullptr) {
    input->etag = etag;
  }

  // Into a MEMBER, and then borrowed.  See the declaration: the gate reads
  // this through a StringPiece after BuildInput returns.
  vary_storage_ = JoinHeader(response_headers, HttpAttributes::kVary);
  input->vary = vary_storage_;

  // THE ORIGIN'S Cache-Control, not the one on the wire.  The serving path
  // rewrites Cache-Control on an unoptimized response, and recording that
  // would record our own answer as the origin's.  SaveCacheControl is how the
  // original reaches us; only when it was never called does the header itself
  // stand.
  input->cache_control_lines.clear();
  if (cache_control_saved_) {
    if (!saved_cache_control_.empty()) {
      input->cache_control_lines.push_back(saved_cache_control_);
    }
  } else {
    ConstStringStarVector values;
    if (response_headers.Lookup(HttpAttributes::kCacheControl, &values)) {
      for (const GoogleString* value : values) {
        if (value != nullptr) {
          input->cache_control_lines.push_back(*value);
        }
      }
    }
  }

  input->last_modified_ms = response_headers.last_modified_time_ms();

  int64 age = 0;
  const char* age_header = response_headers.Lookup1(HttpAttributes::kAge);
  if (age_header != nullptr && StringToInt64(age_header, &age) && age > 0) {
    input->age_seconds = age;
  }

  input->content_length = static_cast<int64>(body_.size());

  // EVERY response header, verbatim, for the reproducible-set predicate.  The
  // pieces point into `response_headers`, which outlives the gate call --
  // DoneAndSetHeaders holds it for the whole of the record.
  //
  // THE WHOLE SET, not a filtered one: the predicate is fail-closed over
  // whatever it is handed, and a seam that pre-filtered would be answering the
  // rule here with a list that drifts from the one the predicate keeps.
  //
  // THESE ARE THE SERVER'S FINALIZED RESPONSE HEADERS, NOT THE ORIGIN'S, and
  // the predicate is written against that fact rather than in spite of it.
  // This seam runs after the server has merged everything it adds to a
  // response -- its own `Date` (set here, a line below the gather, because the
  // server does not put one in the outgoing table), the resource headers it
  // fixes for a static response, and anything a site-wide configuration
  // directive contributes.  So the list below is a superset of what the origin
  // sent, and the members that are not the origin's are exactly the ones the
  // SERVING side will add again on the way out -- which is why they belong in
  // the predicate's serve-generated class rather than being filtered here.
  //
  // Two of those are worth naming because they are the ones that would
  // otherwise mark every response on this server for ever: `Accept-Ranges`,
  // which rides on essentially every static resource, and `Age`, which rides
  // on every response from a proxy- or CDN-fronted origin.  A `Cache-Control`
  // the serving path rewrote (the case SaveCacheControl exists for) is a third
  // and costs nothing: the field is reproducible whatever its value.
  //
  // What this seam CANNOT do is tell a header a site-wide directive added from
  // one the origin sent for this URL alone; see the note on
  // OriginHeadersAreReproducible.  Filtering here would not help -- it would
  // move the same undecidable question to a list that then drifts from the
  // predicate's.
  input->response_headers.clear();
  input->response_headers.reserve(response_headers.NumAttributes());
  for (int i = 0, n = response_headers.NumAttributes(); i < n; ++i) {
    input->response_headers.push_back(DaemonOriginHeader{
        response_headers.Name(i), response_headers.Value(i)});
  }

  input->accept = request_.accept;
  input->user_agent = request_.user_agent;
  input->save_data = request_.save_data;
  input->accept_encoding = request_.accept_encoding;
  input->option_context = request_.option_context;
  input->option_signature = request_.option_signature;
}

void DaemonIproRecorder::DoneAndSetHeaders(ResponseHeaders* response_headers,
                                           bool entire_response_received) {
  DaemonRecordOutcome outcome;
  outcome.done = true;

  // Every path below must reach the delete.  This object is self-owned in the
  // same way the classic recorder is, and a return that skipped it would leak
  // one buffered response body per request.
  do {
    if (abi_ == nullptr || cache_ == nullptr || response_headers == nullptr) {
      break;
    }
    if (failure_ || over_cap_ || !entire_response_received) {
      // A truncated response is not the origin's answer, and storing it as the
      // durable original would replace a good one with a partial one -- the
      // class has one slot per URL, so there is nowhere for a doubtful copy to
      // sit harmlessly.
      break;
    }
    // ---- status ----------------------------------------------------
    // The same two checks the classic recorder makes, in the same order and
    // for the same reasons.
    //
    // The second one is not redundant with the first, and on THIS substrate
    // it is the load-bearing one. A 301 is a cacheable resource status here,
    // so without it a redirect's body -- often empty, never the resource --
    // would be stored as the durable ORIGINAL for that URL, with no Location
    // anywhere in the entry, and the serving side would later hand it out as
    // if it were the resource.
    if (response_headers->IsErrorStatus() ||
        response_headers->status_code() != HttpStatus::kOK) {
      break;
    }

    // ---- this module's own cacheability judgement --------------------
    // EXACTLY the call the classic recorder makes, and each argument matters:
    //
    //   request_properties  what the REQUEST carried. The default-constructed
    //       value assumes no authorization, which is the permissive answer on
    //       the one axis where being wrong means an authenticated user's
    //       response reaching another user's request -- through a cache
    //       shared across virtual hosts, at that.
    //   respect_vary        the configured option, not a constant. Hard-coding
    //       the strict end here would refuse every Vary but Accept-Encoding
    //       module-side, which is both a regression against the classic path
    //       and a way of making the peer's own predicate unreachable.
    //   kNoValidator        this path revalidates nothing, so it must not
    //       claim it can.
    //
    // This is the FIRST of two gates. The peer's store-side predicate is the
    // second, and it is applied inside the record gate over the same
    // response. Strictest of both, and on the axes where they differ it is
    // the peer's that is strict.
    if (!response_headers->IsProxyCacheable(
            request_.request_properties,
            ResponseHeaders::GetVaryOption(http_options_.respect_vary),
            ResponseHeaders::kNoValidator)) {
      break;
    }

    DaemonRecordInput input;
    BuildInput(*response_headers, &input);

    const DaemonRecordDecision decision = EvaluateDaemonRecordGate(
        *abi_, input, timer_ == nullptr ? 0 : timer_->NowMs() / 1000);
    outcome.gate_ran = true;
    outcome.verdict = decision.verdict;
    outcome.reason = decision.reason;
    outcome.notify_mask = decision.notify_mask;
    if (decision.verdict == DaemonRecordVerdict::kRefuse) {
      break;
    }

    DaemonRecordWriter writer(abi_, cache_);
    if (writer.Begin(input, decision) != kPsOk) {
      break;
    }
    if (writer.Append(body_.data(), body_.size()) != kPsOk) {
      writer.Abandon();
      break;
    }
    if (writer.Commit() != kPsOk) {
      break;
    }
    outcome.stored = true;

    if (decision.verdict != DaemonRecordVerdict::kStoreAndNotify) {
      break;
    }
    // The original is durable at this point.  A notification that does not
    // arrive costs this response its optimization and nothing else, so it is
    // not an error path -- and it deliberately runs AFTER the commit, so the
    // optimizer is never told about an entry that is not there yet.
    outcome.notified =
        NotifyDaemon(*abi_, socket_path_, input, decision) == kPsOk;
  } while (false);

  if (outcome_ != nullptr) {
    *outcome_ = outcome;
  }
  delete this;
}

IproRecorder* MakeDaemonIproRecorderIfReady(DaemonAdapter* adapter,
                                            const DaemonRecordRequest& request,
                                            const HttpOptions& http_options,
                                            Timer* timer,
                                            MessageHandler* handler) {
  if (adapter == nullptr || adapter->health() != DaemonHealth::kReady) {
    return nullptr;
  }
  // Asked for BEFORE the recorder exists, not after: a recorder built against
  // a volume this process cannot open would buffer a whole response body and
  // then throw it away, once per request.
  void* cache = adapter->RecordCache();
  if (cache == nullptr || adapter->abi() == nullptr) {
    return nullptr;
  }
  return new DaemonIproRecorder(adapter->abi(), cache, adapter->socket_path(),
                                request, http_options, timer, handler);
}

}  // namespace net_instaweb
