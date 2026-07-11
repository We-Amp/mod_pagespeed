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

#ifndef PAGESPEED_APACHE_FETCH_H_
#define PAGESPEED_APACHE_FETCH_H_

#include <memory>

#include "net/instaweb/http/public/async_fetch.h"
#include "net/instaweb/http/public/request_context.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "pagespeed/apache/apache_writer.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/thread_annotations.h"
#include "pagespeed/kernel/http/request_headers.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "pagespeed/kernel/thread/scheduler.h"

namespace net_instaweb {

// Links an apache request_rec* to an AsyncFetch, adding the ability to
// block based on a condition variable.
//
// If you need this to stream writes and flushes out to apache, call
// set_buffered(false) and make sure all calls are on the request thread.
// ApacheWriter will DCHECK-fail if it's called on another thread.
class ApacheFetch : public AsyncFetch {
 public:
  // Takes ownership of apache_writer, but the underlying request_rec needs to
  // stay around until Wait returns false or the caller finishes handling a true
  // return and deletes the ApacheFetch.
  // Also takes ownership of request_headers.
  ApacheFetch(const GoogleString& mapped_url, StringPiece debug_info,
              RewriteDriver* driver, ApacheWriter* apache_writer,
              RequestHeaders* request_headers,
              const RequestContextPtr& request_context,
              const RewriteOptions* options, MessageHandler* handler);
  ~ApacheFetch() override;

  // When used for in-place resource optimization in mod_pagespeed, we have
  // disabled fetching resources that are not in cache, otherwise we may wind
  // up doing a loopback fetch to the same Apache server.  So the
  // CacheUrlAsyncFetcher will return a 501 or 404 but we do not want to
  // send that to the client.  So for ipro we suppress reporting errors
  // in this flow.
  //
  // TODO(jmarantz): consider allowing fetches in ipro when running as
  // a reverse-proxy.
  void set_handle_error(bool x) { handle_error_ = x; }

  // If all calls to an ApacheFetch are on the apache request thread it is safe
  // to set buffered to false.  All calls Write()/Flush() will be passed through
  // to the underlying writer immediately.  Otherwise, leave it as true and they
  // will be delayed until Wait().
  void set_buffered(bool x) { buffered_ = x; }

  // Blocks waiting for the fetch to complete.
  void Wait() LOCKS_EXCLUDED(scheduler_->mutex());

  bool status_ok() const { return status_ok_; }

  // Whether this fetch has already sent response headers (and possibly body
  // bytes) to the client.  Once true, the caller must not emit a response of
  // its own (e.g. an error page), even if status_ok() is false:
  // SendOutHeaders() can commit a 403 for a response that lacked a
  // Content-Type regardless of handle_error.  Only meaningful on the request
  // thread; for the buffered case only after Wait() has returned.
  bool response_committed() const { return headers_sent_; }

  bool IsCachedResultValid(const ResponseHeaders& headers) override
      LOCKS_EXCLUDED(scheduler_->mutex());

  // Zero-copy ALIASED serve (CycloneZeroCopyServe, the design record).  Called on
  // the request thread by the '.pagespeed.' cache-hit serve
  // (rewrite_driver.cc CacheCallback::DeliverDone) with a body StringPiece
  // aliasing a Cyclone mmap region and its pinning keepalive.  When the
  // Apache opt-in gate and the verbatim-serve guards hold, the body goes
  // out as a PAGESPEED_MMAP bucket that revalidates the read lease before
  // every send and copies out on setaside (apache_mmap_bucket.h);
  // otherwise the bytes are de-aliased here by a verified copy
  // (copy-then-verify) and served through the classic Write path.  A torn
  // borrow (epoch moved) fails the serve closed instead of completing a
  // corrupt body.
  bool WriteMapped(const StringPiece& mmap_sp,
                   const MappedSharedString& keepalive,
                   MessageHandler* handler) override
      LOCKS_EXCLUDED(scheduler_->mutex());

  // By default ApacheFetch is not intended for proxying third party content.
  // When it is to be used for proxying third party content, we must avoid
  // sending a 'nosniff' header.
  void set_is_proxy(bool x) { is_proxy_ = x; }

 protected:
  void HandleHeadersComplete() override LOCKS_EXCLUDED(scheduler_->mutex());
  void HandleDone(bool success) override LOCKS_EXCLUDED(scheduler_->mutex());
  bool HandleFlush(MessageHandler* handler) override
      LOCKS_EXCLUDED(scheduler_->mutex());
  bool HandleWrite(const StringPiece& sp, MessageHandler* handler) override
      LOCKS_EXCLUDED(scheduler_->mutex());

 private:
  void SendOutHeaders();

  GoogleString mapped_url_;
  std::unique_ptr<ApacheWriter> apache_writer_;
  const RewriteOptions* options_ GUARDED_BY(scheduler_->mutex());

  MessageHandler* message_handler_;

  bool done_ GUARDED_BY(scheduler_->mutex());
  bool wait_called_;
  bool handle_error_;
  bool squelch_output_;
  // Whether SendOutHeaders() actually handed headers to the ApacheWriter.
  // False when handle_error_ is false and the response was an error: in
  // that case the caller answers the request itself and we must not write
  // anything (ApacheWriter requires OutputHeaders() before Write()).
  // Only read/written on the request thread.
  bool headers_sent_;
  bool status_ok_;
  bool is_proxy_;
  bool buffered_;
  GoogleString debug_info_;
  GoogleString output_bytes_;
  RewriteDriver* driver_;
  Scheduler* scheduler_;

  ApacheFetch(const ApacheFetch&) = delete;
  ApacheFetch& operator=(const ApacheFetch&) = delete;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_APACHE_FETCH_H_
