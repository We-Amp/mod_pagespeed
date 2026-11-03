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

#ifndef PAGESPEED_SYSTEM_IPRO_RECORDER_H_
#define PAGESPEED_SYSTEM_IPRO_RECORDER_H_

#include <memory>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/writer.h"
#include "pagespeed/kernel/http/http_options.h"
#include "pagespeed/kernel/http/response_headers.h"

namespace net_instaweb {

class MessageHandler;

// What a serving seam drives while an origin response streams past it, so
// that the response can be recorded for later in-place optimization.
//
// WHY THIS IS AN INTERFACE.  There are two substrates now.  The classic one
// records into this module's own HTTP cache; the other records into the
// optimizer daemon's shared cache and tells the daemon about it.  They differ
// entirely in what recording MEANS and share nothing but this lifecycle --
// which is dictated by the server's filter order, not by either
// implementation, and is therefore exactly the right seam to name.
//
// THE LIFECYCLE, in the order a server drives it:
//
//   ConsiderResponseHeaders(kPreliminaryHeaders, …)  as soon as anything is
//       known about the response.  May shift the recorder into its failed
//       state, which stops the work early.
//   Write(...) / Flush(...)                          per body chunk.
//   SaveCacheControl(...)                            iff the server is about
//       to rewrite the response's Cache-Control, with the ORIGINAL value --
//       so what is recorded is the origin's answer and not ours.
//   SetOriginCachingHeaders(...)                     whenever the seam can
//       see the origin's caching headers before the server's own directives
//       act on them.  Optional; at any point before DoneAndSetHeaders.
//   DoneAndSetHeaders(final headers, complete)       exactly once, at the end
//       of the response.  DELETES THE RECORDER; nothing may touch it after.
//
// Apache splits the body and the final headers across two filters, so the
// preliminary/full split is not an implementation convenience: the body is
// captured before compression and the headers only exist after it.
class IproRecorder : public Writer {
 public:
  enum HeadersKind {
    // Enough to tell whether an upstream already encoded the content.
    kPreliminaryHeaders,

    // Headers are complete.
    kFullHeaders,
  };

  ~IproRecorder() override;

  // Offers what is known about the response so far.  Does not take ownership.
  virtual void ConsiderResponseHeaders(HeadersKind headers_kind,
                                       ResponseHeaders* response_headers) = 0;

  // The Cache-Control the ORIGIN sent, captured before the serving path
  // rewrites it.  nullptr means the response carried no Cache-Control at all,
  // which is not the same as an empty one.  Stores a copy.
  virtual void SaveCacheControl(const char* cache_control) = 0;

  // Something went wrong; nothing is recorded.  DoneAndSetHeaders must still
  // be called.
  virtual void Fail() = 0;
  virtual bool failed() const = 0;

  virtual MessageHandler* handler() = 0;
  virtual const HttpOptions& http_options() const = 0;

  // The response is over.  `entire_response_received` is false when the body
  // fed in was cut short, which is what keeps a truncated response from being
  // recorded as if it were whole.  Does not take ownership of the headers.
  //
  // DELETES THIS OBJECT.  Do not use it afterwards.
  virtual void DoneAndSetHeaders(ResponseHeaders* response_headers,
                                 bool entire_response_received) = 0;

  virtual const GoogleString& url() const = 0;

  // The caching headers as the ORIGIN sent them: Cache-Control, Expires,
  // Pragma, Set-Cookie and Vary, taken from `origin` line by line, before the
  // server's own caching directives acted on them.  Store decisions then use
  // the stricter of the origin's and the server's caching headers: the
  // response is stored only when both allow it, never for longer than the
  // server states when it states a lifetime, and the final headers supply
  // everything else.  A seam that cannot see them does not call this, and
  // the final headers decide alone; so do they when the origin stated none
  // of Cache-Control, Expires and Pragma.  Stores a copy; a later call
  // replaces an earlier one.
  void SetOriginCachingHeaders(const ResponseHeaders& origin);
  bool has_origin_caching_headers() const {
    return origin_caching_headers_ != nullptr;
  }

  // The names SetOriginCachingHeaders takes.  Vary is last: it is merged
  // into the final headers rather than replacing them.
  static const char* const kOriginCachingHeaderNames[];
  static const int kNumOriginCachingHeaderNames;

 protected:
  // Replaces the caching fields of `headers` with the origin's, when the
  // seam supplied them, and recomputes its caching.  Vary becomes the union
  // of both: a Vary the server added is still a Vary.  When the server's
  // headers state a lifetime, the stored one is never longer: the origin's
  // stated lifetime, or the implicit one when it states none, is clamped.
  // The server's lifetime is read from `headers` as given, with its
  // Cache-Control replaced by `saved_cache_control` when that is not null
  // (the value SaveCacheControl received; empty means no Cache-Control).
  // Returns false, and leaves `headers` alone, when no origin headers were
  // supplied.  Callers judge `headers` with IsProxyCacheable BEFORE this
  // call as well, and refuse when either verdict refuses.
  bool ApplyOriginCachingHeaders(ResponseHeaders* headers,
                                 const GoogleString* saved_cache_control) const;

 private:
  std::unique_ptr<ResponseHeaders> origin_caching_headers_;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_SYSTEM_IPRO_RECORDER_H_
