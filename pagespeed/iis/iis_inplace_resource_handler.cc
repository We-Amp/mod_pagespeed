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

#include "pagespeed/iis/iis_inplace_resource_handler.h"

#include <unordered_set>
#include <vector>

#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/content_type.h"
#include "pagespeed/kernel/http/http_names.h"

#ifdef _WIN32
#include "net/instaweb/http/public/http_cache.h"
#include "net/instaweb/http/public/http_value.h"
#include "net/instaweb/http/public/request_context.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "pagespeed/kernel/cache/cache_interface.h"
#include "pagespeed/kernel/http/request_headers.h"
#endif

namespace net_instaweb {

namespace {
const char kPageSpeedMarker[] = ".pagespeed.";
const size_t kPageSpeedMarkerLen = sizeof(kPageSpeedMarker) - 1;

// kOptimizedResourceCacheTtlMs: Cache TTL for optimized resources (1 year).
// PageSpeed URLs contain a content hash (e.g., .pagespeed.cf.AbCdEf.css), so
// the URL changes whenever the content changes. This enables aggressive caching
// with a 1-year TTL, following HTTP caching best practices for immutable,
// content-addressed resources. The hash ensures cache invalidation happens
// automatically when the source content changes.
const int64 kOptimizedResourceCacheTtlMs = 365 * 24 * 60 * 60 * 1000LL;

#ifdef _WIN32
// Simple synchronous callback for HTTPCache lookups.
// Used to check if an optimized resource is available in cache.
class IproCacheLookupCallback : public HTTPCache::Callback {
 public:
  explicit IproCacheLookupCallback(const RequestContextPtr& request_ctx)
      : HTTPCache::Callback(request_ctx),
        done_(false),
        found_(false) {}

  void Done(HTTPCache::FindResult result) override {
    done_ = true;
    found_ = (result.status == HTTPCache::kFound);
  }

  bool IsCacheValid(const GoogleString& key,
                    const ResponseHeaders& headers) override {
    return true;  // Accept all valid entries
  }

  ResponseHeaders::VaryOption RespectVaryOnResources() const override {
    return ResponseHeaders::kRespectVaryOnResources;
  }

  bool done() const { return done_; }
  bool found() const { return found_; }

 private:
  bool done_;
  bool found_;

  IproCacheLookupCallback(const IproCacheLookupCallback&) = delete;
  IproCacheLookupCallback& operator=(const IproCacheLookupCallback&) = delete;
};
#endif  // _WIN32

}  // namespace

bool IisInPlaceResourceHandler::IsPageSpeedUrl(const StringPiece& url) {
  return url.find(kPageSpeedMarker) != StringPiece::npos;
}

bool IisInPlaceResourceHandler::DecodeUrl(const StringPiece& url,
                                           GoogleString* original_url,
                                           GoogleString* filter_id,
                                           GoogleString* hash) {
  // Find .pagespeed. marker
  size_t marker_pos = url.find(kPageSpeedMarker);
  if (marker_pos == StringPiece::npos) {
    return false;
  }

  // Get base path before .pagespeed.
  StringPiece base = url.substr(0, marker_pos);

  // Get the rest after .pagespeed.
  StringPiece rest = url.substr(marker_pos + kPageSpeedMarkerLen);

  // Parse FILTER_ID.HASH[.EXT]
  // The format is: cf.AbCdEf.css or jm.XyZ123.js
  std::vector<StringPiece> parts;
  SplitStringPieceToVector(rest, ".", &parts, false);

  if (parts.size() < 2) {
    return false;
  }

  filter_id->assign(parts[0].data(), parts[0].size());
  hash->assign(parts[1].data(), parts[1].size());

  // Reconstruct original URL
  if (parts.size() > 2) {
    // Has extension
    original_url->assign(base.data(), base.size());
    StrAppend(original_url, ".", parts[2].as_string());
  } else {
    original_url->assign(base.data(), base.size());
  }

  return true;
}

GoogleString IisInPlaceResourceHandler::EncodeUrl(
    const StringPiece& original_url,
    const StringPiece& filter_id,
    const StringPiece& hash) {
  // Find the extension
  size_t dot_pos = original_url.rfind('.');

  if (dot_pos == StringPiece::npos) {
    // No extension
    return StrCat(original_url, kPageSpeedMarker, filter_id, ".", hash);
  }

  StringPiece base = original_url.substr(0, dot_pos);
  StringPiece ext = original_url.substr(dot_pos);

  return StrCat(base, kPageSpeedMarker, filter_id, ".", hash, ext);
}

#ifdef _WIN32

bool IisInPlaceResourceHandler::HandleRequest(
    IHttpContext* context,
    ServerContext* server_context,
    RewriteOptions* options,
    MessageHandler* handler) {
  IHttpRequest* request = context->GetRequest();
  if (request == nullptr) {
    return false;
  }

  // Get request URL from raw HTTP request
  HTTP_REQUEST* http_request = request->GetRawHttpRequest();
  if (http_request == nullptr) {
    return false;
  }

  PCSTR abs_path = http_request->pRawUrl;
  if (abs_path == nullptr) {
    return false;
  }

  GoogleString url(abs_path);

  // Decode the PageSpeed URL
  GoogleString original_url, filter_id, hash;
  if (!DecodeUrl(url, &original_url, &filter_id, &hash)) {
    handler->Message(kWarning, "Failed to decode PageSpeed URL: %s",
                     url.c_str());
    return false;
  }

  handler->Message(kInfo, "IPRO request: %s -> %s (filter: %s, hash: %s)",
                   url.c_str(), original_url.c_str(),
                   filter_id.c_str(), hash.c_str());

  // Check for conditional request first
  USHORT if_none_match_len = 0;
  PCSTR if_none_match = request->GetHeader("If-None-Match", &if_none_match_len);
  if (if_none_match != nullptr && if_none_match_len > 0) {
    GoogleString etag(if_none_match, if_none_match_len);
    // If the ETag matches the hash, return 304
    if (etag.find(hash) != GoogleString::npos) {
      if (HandleConditionalRequest(context, hash, 0)) {
        return true;
      }
    }
  }

  // Build cache key: original_url is the key, and we'll use the host as fragment
  // This matches how InPlaceRewriteContext stores optimized resources
  GoogleString cache_key = original_url;

  // Get cache fragment from the URL's host
  GoogleUrl gurl(original_url);
  GoogleString cache_fragment;
  if (gurl.IsWebValid()) {
    cache_fragment = gurl.Host().as_string();
  }

  // Create request context for the cache lookup
  RequestContextPtr request_context(new RequestContext(
      options->ComputeHttpOptions(),
      server_context->thread_system()->NewMutex(),
      server_context->timer()));

  // Perform cache lookup
  HTTPCache* http_cache = server_context->http_cache();
  if (http_cache != nullptr) {
    IproCacheLookupCallback* callback =
        new IproCacheLookupCallback(request_context);

    http_cache->Find(cache_key, cache_fragment, handler, callback);

    // For synchronous caches (like LRU), the callback is called immediately.
    // For async caches, we fall back to serving original.
    if (callback->done() && callback->found()) {
      // Cache hit - extract and serve the optimized content
      StringPiece contents;
      if (callback->http_value()->ExtractContents(&contents)) {
        handler->Message(kInfo, "IPRO cache hit for: %s", original_url.c_str());
        ServeFromCache(context, *callback->response_headers(), contents,
                       handler);
        delete callback;
        return true;
      }
    }
    delete callback;
  }

  // Cache miss - serve original content
  handler->Message(kInfo, "IPRO cache miss for: %s, serving original",
                   original_url.c_str());
  ServeOriginal(context, original_url, server_context, handler);

  // Queue background optimization using a new RewriteDriver
  RewriteDriver* driver =
      server_context->NewRewriteDriver(request_context);
  if (driver != nullptr) {
    // Set up the driver with request headers from the IIS request
    RequestHeaders driver_request_headers;
    if (http_request != nullptr && http_request->Headers.UnknownHeaderCount > 0) {
      // Copy headers from IIS request (basic headers only for now)
      // Full header copying would iterate through all headers
    }
    driver->SetRequestHeaders(driver_request_headers);

    // Queue the optimization
    QueueOptimization(original_url, filter_id, server_context, driver);

    // Cleanup will happen asynchronously when optimization completes
    driver->Cleanup();
  }

  return true;
}

void IisInPlaceResourceHandler::ServeFromCache(
    IHttpContext* context,
    const ResponseHeaders& headers,
    const StringPiece& content,
    MessageHandler* handler) {
  IHttpResponse* response = context->GetResponse();
  if (response == nullptr) {
    return;
  }

  // Set status
  response->SetStatus(200, "OK");

  // Set headers. Use fReplace=TRUE for the first occurrence of each header
  // name, FALSE for subsequent ones to preserve multi-valued headers.
  std::unordered_set<GoogleString> seen_headers;
  for (int i = 0; i < headers.NumAttributes(); ++i) {
    const GoogleString& name = headers.Name(i);
    const GoogleString& value = headers.Value(i);
    BOOL replace = seen_headers.insert(name).second ? TRUE : FALSE;
    response->SetHeader(name.c_str(),
                        value.c_str(),
                        static_cast<USHORT>(value.length()),
                        replace);
  }

  // Set cache headers
  SetCacheHeaders(response, kOptimizedResourceCacheTtlMs);

  // Write body
  if (!content.empty()) {
    HTTP_DATA_CHUNK chunk;
    chunk.DataChunkType = HttpDataChunkFromMemory;
    chunk.FromMemory.pBuffer = const_cast<char*>(content.data());
    chunk.FromMemory.BufferLength = static_cast<ULONG>(content.size());

    DWORD bytes_sent = 0;
    response->WriteEntityChunks(&chunk, 1, FALSE, TRUE, &bytes_sent);
  }

  handler->Message(kInfo, "Served %zu bytes from cache", content.size());
}

void IisInPlaceResourceHandler::ServeOriginal(
    IHttpContext* context,
    const GoogleString& original_url,
    ServerContext* server_context,
    MessageHandler* handler) {
  // For now, we'll let IIS serve the original file
  // In a full implementation, we would:
  // 1. Map the URL to a physical path
  // 2. Read the file
  // 3. Apply optimization
  // 4. Send the optimized content
  // 5. Cache the result

  IHttpResponse* response = context->GetResponse();
  if (response != nullptr) {
    // Set a header to indicate we processed this but served original
    response->SetHeader("X-PageSpeed-IPRO", "pending", 7, FALSE);
  }

  handler->Message(kInfo, "Serving original for: %s (optimization pending)",
                   original_url.c_str());

  // Let IIS handle the actual file serving by continuing the pipeline
  // The caller should return RQ_NOTIFICATION_CONTINUE
}

void IisInPlaceResourceHandler::QueueOptimization(
    const GoogleString& original_url,
    const GoogleString& filter_id,
    ServerContext* server_context,
    RewriteDriver* driver) {
  // Queue the resource for background optimization
  // This uses PageSpeed's standard optimization infrastructure
  if (driver == nullptr) {
    return;
  }

  // Start async fetch of the original resource
  // The optimization will happen in the background
  driver->FetchInPlaceResource(
      GoogleUrl(original_url),
      false,  // proxy_mode
      nullptr);  // async_fetch callback
}

void IisInPlaceResourceHandler::SetCacheHeaders(
    IHttpResponse* response,
    int64 cache_ttl_ms) {
  if (response == nullptr) {
    return;
  }

  int64 cache_ttl_sec = cache_ttl_ms / 1000;

  // Set Cache-Control
  GoogleString cache_control = StrCat("public, max-age=",
                                       Integer64ToString(cache_ttl_sec));
  response->SetHeader(HttpAttributes::kCacheControl,
                      cache_control.c_str(),
                      static_cast<USHORT>(cache_control.length()),
                      TRUE);

  // Set Expires header (far future)
  // TODO(iis): Compute proper Expires date
}

bool IisInPlaceResourceHandler::HandleConditionalRequest(
    IHttpContext* context,
    const GoogleString& etag,
    int64 last_modified_time_ms) {
  IHttpResponse* response = context->GetResponse();
  if (response == nullptr) {
    return false;
  }

  // Send 304 Not Modified
  response->SetStatus(304, "Not Modified");

  // Set ETag header
  GoogleString etag_value = StrCat("\"", etag, "\"");
  response->SetHeader(HttpAttributes::kEtag,
                      etag_value.c_str(),
                      static_cast<USHORT>(etag_value.length()),
                      TRUE);

  // Finish the response with no body
  response->Flush(TRUE, TRUE, nullptr);

  return true;
}

#endif  // _WIN32

}  // namespace net_instaweb
