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

#ifndef PAGESPEED_APACHE_STREAMING_PAGESPEED_RESOURCE_FETCH_H_
#define PAGESPEED_APACHE_STREAMING_PAGESPEED_RESOURCE_FETCH_H_

#include "net/instaweb/http/public/async_fetch.h"
#include "pagespeed/apache/apache_httpd_includes.h"

namespace net_instaweb {

// Sits between ResourceFetch and the unbuffered ApacheFetch on the
// streaming .pagespeed. resource path, and reproduces the header/wire
// fixups the old buffered path applied in
// InstawebHandler::HandleAsPagespeedResource and
// send_out_headers_and_body:
//  1. removes the X-Page-Speed header that ResourceFetch adds (old
//     mod_pagespeed never sent it on resources);
//  2. enables Apache's DEFLATE output filter for compressible 200
//     responses, before the first body byte is written.
//
// All calls arrive on the Apache request thread: the unbuffered
// ApacheFetch underneath switched the driver to run its tasks on the
// request thread (RewriteDriver::RunTasksOnRequestThread), and
// InstawebHandler::WaitForFetch pumps them there.
class StreamingPagespeedResourceFetch : public SharedAsyncFetch {
 public:
  StreamingPagespeedResourceFetch(request_rec* request, AsyncFetch* base_fetch);

  // Whether responses of this content type are worth passing through
  // Apache's DEFLATE output filter.
  static bool IsCompressibleContentType(const char* content_type);

 protected:
  void HandleHeadersComplete() override;

 private:
  request_rec* request_;

  StreamingPagespeedResourceFetch(const StreamingPagespeedResourceFetch&) =
      delete;
  StreamingPagespeedResourceFetch& operator=(
      const StreamingPagespeedResourceFetch&) = delete;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_APACHE_STREAMING_PAGESPEED_RESOURCE_FETCH_H_
