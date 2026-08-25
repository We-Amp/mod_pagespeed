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

#ifndef PAGESPEED_SYSTEM_DAEMON_IPRO_RECORDER_H_
#define PAGESPEED_SYSTEM_DAEMON_IPRO_RECORDER_H_

#include <cstdint>

#include "pagespeed/kernel/base/atomic_int32.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/http_options.h"
#include "pagespeed/kernel/http/request_headers.h"
#include "pagespeed/system/daemon_record_arm.h"
#include "pagespeed/system/ipro_recorder.h"

namespace net_instaweb {

class DaemonAbi;
class MessageHandler;
class ResponseHeaders;
class Timer;

// What a serving seam must gather about the REQUEST before the response
// starts, because none of it survives to the end of the response.
//
// The recorder outlives the request handler: it is driven by output filters
// that run after the handler has returned.  Everything here is therefore
// COPIED at construction rather than referenced, and the options context in
// particular is rendered up front -- the resolved options it is rendered from
// may be owned by a rewrite driver that is gone by then.
struct DaemonRecordRequest {
  GoogleString url;  // Path and query; see UrlIsKeySafe.
  GoogleString hostname;
  GoogleString scheme;

  GoogleString accept;
  GoogleString user_agent;
  GoogleString save_data;
  GoogleString accept_encoding;

  // The canonical options context for this request and its signature, or both
  // empty when it could not be rendered.  Both or neither.
  GoogleString option_context;
  GoogleString option_signature;

  // What the REQUEST carried on the axes that decide whether a response may
  // be kept in a cache shared with other requests: cookies, and above all
  // authorization.  Not derivable from the response, and not defaultable --
  // the default-constructed value says "no authorization", which is the
  // permissive answer on the one axis where being wrong means putting an
  // authenticated user's response where another user's request can reach it.
  RequestHeaders::Properties request_properties;
};

// What one recorder did, copied out before it deletes itself.
//
// A recorder is self-owned and DoneAndSetHeaders destroys it, so an accessor
// on the object is unreadable exactly when the answer exists. A sink the
// caller owns is the only shape that works. Test-only: nothing shipped reads
// this.
struct DaemonRecordOutcome {
  // False until DoneAndSetHeaders has run.
  bool done = false;
  // True once the gate was actually consulted -- distinguishes "the gate said
  // no" from "the response never reached the gate", which the checks before
  // it (status, cacheability, truncation) produce and which would otherwise
  // be indistinguishable from a refusal.
  bool gate_ran = false;
  DaemonRecordVerdict verdict = DaemonRecordVerdict::kRefuse;
  const char* reason = nullptr;
  uint32_t notify_mask = 0;
  bool stored = false;
  bool notified = false;
};

// Records an origin response into the optimizer daemon's shared cache as the
// durable ORIGINAL, and asks the daemon to optimize it.
//
// THE BODY IS ACCUMULATED, not streamed straight through, for a reason that
// is the server's and not this class's: the final response headers are not
// known until after the body has passed, and the write cannot be opened
// without them -- the Cache-Control state and the validators that decide how
// long the entry lives are stamped when the write begins.
//
// The content cap is applied WHILE accumulating, not to a finished buffer.
// The point of a cap is not to discover after holding 16 MB that it was 16 MB.
class DaemonIproRecorder : public IproRecorder {
 public:
  // `abi`, `cache`, `timer` and `handler` are borrowed and must outlive this
  // object.  `cache` may be null, which is the "nothing is recorded" state and
  // is not an error here: the health verdict is what decides whether a
  // recorder is built at all.
  //
  // Self-owned in the same way the classic recorder is: DoneAndSetHeaders
  // deletes it.
  DaemonIproRecorder(const DaemonAbi* abi, void* cache, StringPiece socket_path,
                     const DaemonRecordRequest& request,
                     const HttpOptions& http_options, Timer* timer,
                     MessageHandler* handler);
  ~DaemonIproRecorder() override;

  // Number of daemon recorders this process has ever CONSTRUCTED.  The
  // counterpart of the classic recorder's, and for the same reason: it is the
  // only way to assert that a seam did not build one, as opposed to building
  // one and not using it.  Monotonic; never reset.
  static int64 num_constructed();

  bool Write(const StringPiece& contents, MessageHandler* handler) override;
  bool Flush(MessageHandler* handler) override { return true; }

  void ConsiderResponseHeaders(HeadersKind headers_kind,
                               ResponseHeaders* response_headers) override;
  // Part of the IproRecorder contract. On Apache's daemon substrate the only
  // caller is the fix-headers filter, which is deliberately not attached
  // there, so this does not run in production on that seam today; other seams
  // that do rewrite caching headers need it.
  void SaveCacheControl(const char* cache_control) override;

  void Fail() override { failure_ = true; }
  bool failed() const override { return failure_; }

  MessageHandler* handler() override { return handler_; }
  const HttpOptions& http_options() const override { return http_options_; }

  void DoneAndSetHeaders(ResponseHeaders* response_headers,
                         bool entire_response_received) override;

  const GoogleString& url() const override { return request_.url; }

  // Where to copy the outcome just before this object destroys itself.
  // Borrowed; must outlive the response.  Test seam only.
  //
  // A sink rather than accessors, because DoneAndSetHeaders destroys this
  // object -- so an accessor on it is unreadable at exactly the moment the
  // answer exists.
  void set_outcome_sink_for_testing(DaemonRecordOutcome* sink) {
    outcome_ = sink;
  }

 private:
  // Builds the gate's input from the final response headers.  `input` borrows
  // from this object and from `response_headers`, so it must not outlive
  // either.
  void BuildInput(const ResponseHeaders& response_headers,
                  DaemonRecordInput* input);

  const DaemonAbi* abi_;
  void* cache_;
  const GoogleString socket_path_;
  const DaemonRecordRequest request_;
  const HttpOptions http_options_;
  Timer* timer_;
  MessageHandler* handler_;

  GoogleString body_;
  // The response's Vary lines, joined.  A MEMBER, not a local: the gate's
  // input borrows it as a StringPiece and reads it after the call that fills
  // it returns, so a temporary here is a read of freed storage as soon as the
  // joined value is longer than a small string.  Two `Vary: Accept-Encoding`
  // lines -- an ordinary result of two modules each adding one -- are already
  // long enough.
  GoogleString vary_storage_;
  bool failure_ = false;
  // Set when the body ran past the durable-originals cap.  Distinct from
  // `failure_` so the outcome can be reported as the ordinary thing it is
  // rather than as a malfunction.
  bool over_cap_ = false;

  // Whether SaveCacheControl was called, and with what.  Both are needed:
  // "the serving path did not rewrite Cache-Control" and "the origin sent an
  // empty Cache-Control" are different responses.
  bool cache_control_saved_ = false;
  GoogleString saved_cache_control_;

  DaemonRecordOutcome* outcome_ = nullptr;

  static AtomicInt32 num_constructed_;
};

class DaemonAdapter;

// The ONE place in this tree from which a serving seam may construct a
// daemon-side recorder -- the counterpart of the classic recorder's gate, and
// held to the same rule: nullptr is returned WITHOUT constructing a recorder
// first and discarding it, so num_constructed() pins non-instantiation rather
// than non-use.
//
// Returns nullptr unless the adapter's health says the daemon owns the
// in-place cache for this server AND this process has a usable handle on its
// volume.  The caller owns the returned recorder in the same way it owns the
// classic one: DoneAndSetHeaders deletes it.
IproRecorder* MakeDaemonIproRecorderIfReady(DaemonAdapter* adapter,
                                            const DaemonRecordRequest& request,
                                            const HttpOptions& http_options,
                                            Timer* timer,
                                            MessageHandler* handler);

}  // namespace net_instaweb

#endif  // PAGESPEED_SYSTEM_DAEMON_IPRO_RECORDER_H_
