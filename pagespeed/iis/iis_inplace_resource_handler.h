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

#ifndef PAGESPEED_IIS_IIS_INPLACE_RESOURCE_HANDLER_H_
#define PAGESPEED_IIS_IIS_INPLACE_RESOURCE_HANDLER_H_

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/google_url.h"
#include "pagespeed/kernel/http/response_headers.h"

#ifdef _WIN32
// Windows headers - winsock2.h must come before windows.h and httpserv.h
// to avoid redefinition errors with winsock.h
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <httpserv.h>
#endif

namespace net_instaweb {

class AsyncFetch;
class MessageHandler;
class RewriteDriver;
class RewriteOptions;
class ServerContext;

// Handles In-Place Resource Optimization (IPRO) requests.
// IPRO allows PageSpeed to optimize resources (CSS, JS, images) without
// modifying the HTML that references them. The optimized resources are
// served from URLs containing ".pagespeed." markers.
class IisInPlaceResourceHandler {
 public:
  // URL format: /path/to/file.pagespeed.FILTER_ID.HASH.EXTENSION
  // Example: /styles/main.pagespeed.cf.AbCdEf.css
  //
  // Filter IDs:
  //   cf - CSS filter (combine, minify)
  //   jm - JavaScript minify
  //   jc - JavaScript combine
  //   ic - Image compress/convert
  //   ce - Cache extend

  // Decode a .pagespeed. URL to extract the original resource info.
  // Returns true if the URL is a valid PageSpeed resource URL.
  static bool DecodeUrl(const StringPiece& url,
                        GoogleString* original_url,
                        GoogleString* filter_id,
                        GoogleString* hash);

  // Check if a URL is a PageSpeed resource URL (contains .pagespeed.)
  static bool IsPageSpeedUrl(const StringPiece& url);

  // Encode an original URL into a PageSpeed resource URL.
  static GoogleString EncodeUrl(const StringPiece& original_url,
                                const StringPiece& filter_id,
                                const StringPiece& hash);

#ifdef _WIN32
  // Handle an IPRO request.
  // Returns true if the request was handled (response sent).
  // Returns false if the request should fall through to normal handling.
  static bool HandleRequest(IHttpContext* context,
                            ServerContext* server_context,
                            RewriteOptions* options,
                            MessageHandler* handler);

 private:
  // Serve optimized content from cache.
  static void ServeFromCache(IHttpContext* context,
                             const ResponseHeaders& headers,
                             const StringPiece& content,
                             MessageHandler* handler);

  // Serve the original resource (on cache miss or optimization failure).
  static void ServeOriginal(IHttpContext* context,
                            const GoogleString& original_url,
                            ServerContext* server_context,
                            MessageHandler* handler);

  // Queue background optimization for the resource.
  static void QueueOptimization(const GoogleString& original_url,
                                const GoogleString& filter_id,
                                ServerContext* server_context,
                                RewriteDriver* driver);

  // Set appropriate cache headers for optimized resources.
  static void SetCacheHeaders(IHttpResponse* response,
                              int64 cache_ttl_ms);

  // Handle conditional request (If-None-Match, If-Modified-Since).
  // Returns true if 304 response was sent.
  static bool HandleConditionalRequest(IHttpContext* context,
                                       const GoogleString& etag,
                                       int64 last_modified_time_ms);
#endif  // _WIN32

  IisInPlaceResourceHandler(const IisInPlaceResourceHandler&) = delete;
  IisInPlaceResourceHandler& operator=(const IisInPlaceResourceHandler&) = delete;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_IIS_IIS_INPLACE_RESOURCE_HANDLER_H_
