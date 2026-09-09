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

#ifndef PAGESPEED_APACHE_APACHE_WRITER_H_
#define PAGESPEED_APACHE_APACHE_WRITER_H_

#include <memory>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/base/writer.h"

struct request_rec;

namespace net_instaweb {

class MappedSharedString;
class MessageHandler;
class ResponseHeaders;
class Variable;

// Writer object that writes to an Apache Request stream.  Should only be used
// from a single apache request thread, not from a rewrite thread or anything
// else.
class ApacheWriter : public Writer {
 public:
  ApacheWriter(request_rec* r, ThreadSystem* thread_system);
  ~ApacheWriter() override;

  bool Write(const StringPiece& str, MessageHandler* handler) override;
  bool Flush(MessageHandler* handler) override;

  // Zero-copy ALIASED serve (CycloneZeroCopyServe).  True when
  // this request's body bytes travel to the network verbatim: a main
  // (non-sub, non-internal-redirect) request, not header-only, no Range,
  // and every filter on the output chain -- request-level through the
  // connection-level core -- is one of the known verbatim pass-through
  // filters.  Anything else (mod_deflate, mod_ssl, mod_http2, third-party
  // filters) may re-slice, transform, or retain raw pointers into the body
  // across blocking waits, where a mapped alias could be overwritten
  // underneath it, so the caller must serve a copy instead.  Fail-closed:
  // an unknown filter name disables aliasing.
  //
  // On a false return, if 'blocker' is non-null it is set to a
  // human-readable description of the FIRST disqualifying condition
  // (e.g. "output filter 'deflate' is not a known verbatim pass-through"),
  // for the caller's opted-in-but-ineligible diagnostics (the
  // silent degrade was indistinguishable from a dead code path).
  bool RequestServesBodyVerbatim(GoogleString* blocker = nullptr) const;

  // Lets self-dispatching output filters settle against the committed
  // response before the eligibility walk.  A mod_filter harness
  // (AddOutputFilterByType / FilterChain, httpd 2.4 mod_filter.c) sits in
  // EVERY request's output chain until the first brigade passes, then
  // evaluates its provider conditions against the response and removes
  // itself when none matches.  Passing one empty brigade runs exactly that
  // dispatch, so an unmatched harness (e.g. a by-type DEFLATE harness on
  // an image response) leaves the chain before
  // RequestServesBodyVerbatim() walks it, while a MATCHED harness stays
  // and correctly disqualifies aliasing.  Must be called on
  // the request thread after OutputHeaders(); the headers this may commit
  // to the wire are identical on the aliased and copy paths.
  void SettleOutputFilters();

  // Sends 'span' -- bytes aliasing the Cyclone mapped region pinned by
  // 'pin' -- as a single PAGESPEED_MMAP bucket brigade (see
  // apache_mmap_bucket.h for the barrier/copy-out/pin semantics).  Must be
  // called on the request thread after OutputHeaders(), and only when
  // RequestServesBodyVerbatim().  Returns false on failure, in which case
  // the connection has been aborted: a torn borrow mid-serve means the
  // Content-Length promise can no longer be met, and a corrupt-but-complete
  // body must never reach the client.
  bool WriteMappedAliased(const StringPiece& span,
                          const MappedSharedString& pin,
                          Variable* copied_out_stat, Variable* renew_fail_stat,
                          MessageHandler* handler);

  // Marks the underlying connection aborted, used when a committed
  // response (headers and Content-Length already on the wire) can no
  // longer be completed correctly.
  void AbortConnection();

  // Copies the contents of the specified response_headers to the Apache
  // headers_out structure.  This must be done before any bytes are flushed.
  //
  // Note: if strip_cokies is set, the cookies will be stripped here.
  //
  // If set_content_length was previously called, this will set a
  // content length to avoid chunked encoding, otherwise it will clear
  // any content-length specified in the response headers.
  void OutputHeaders(ResponseHeaders* response_headers);
  void set_content_length(int64 x) { content_length_ = x; }

  // Disables mod_expires and mod_headers to allow the headers to
  // be under control of mod_pagespeed.  Default is false.
  void set_disable_downstream_header_filters(bool x) {
    disable_downstream_header_filters_ = x;
  }

  // Removes 'Set-Cookie' and 'Set-Cookie2' from the response headers
  // once they are complete.  Default is false.
  // TODO(jefftk): Doesn't actually do anything, because of an old bug.
  void set_strip_cookies(bool x) { strip_cookies_ = x; }

 private:
  request_rec* request_;
  bool headers_out_;
  bool disable_downstream_header_filters_;
  bool strip_cookies_;
  int64 content_length_;
  ThreadSystem* thread_system_;
  std::unique_ptr<ThreadSystem::ThreadId> apache_request_thread_;

  ApacheWriter(const ApacheWriter&) = delete;
  ApacheWriter& operator=(const ApacheWriter&) = delete;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_APACHE_APACHE_WRITER_H_
