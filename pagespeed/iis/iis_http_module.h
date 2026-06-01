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

#ifndef PAGESPEED_IIS_IIS_HTTP_MODULE_H_
#define PAGESPEED_IIS_IIS_HTTP_MODULE_H_

// IIS Native Module API headers
#include <httpserv.h>

#include <memory>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/shared_string.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/http/request_headers.h"

namespace net_instaweb {

class IisAsyncFetch;
class ProxyFetch;
class ProxyFetchFactory;
class IisModuleFactory;
class IisRequestContext;
class IisServerContext;
class InPlaceResourceRecorder;

// IIS HTTP Module implementing the CHttpModule interface.
//
// This module intercepts HTTP requests and responses to:
// 1. Handle .pagespeed. resource requests (IPRO)
// 2. Rewrite HTML responses through PageSpeed filters
// 3. Serve admin UI and statistics endpoints
//
// The module registers for these IIS pipeline events:
// - OnBeginRequest: IPRO resource handling, license validation
// - OnResolveRequestCache: Check for cached optimized resources
// - OnSendResponse: Buffer and rewrite HTML responses
// - OnEndRequest: Cleanup per-request state
//
// Multi-site support:
// The module uses the IisModuleFactory to get the appropriate server context
// for each request, allowing different sites to have different configurations.
//
// Thread safety:
// IIS may process concurrent requests on the same module instance. Per-request
// state is stored in IisModuleContext via IIS's IHttpModuleContextContainer.
// This ensures the context survives across different module instances for the
// same request (IIS may create new module instances for each event).
class IisHttpModule : public CHttpModule {
 public:
  // Per-request context stored in IIS's module context container.
  // Inherits from IHttpStoredContext so it can be stored/retrieved via
  // IHttpContext::GetModuleContextContainer().
  // This class holds all per-request state.
  class IisModuleContext : public IHttpStoredContext {
   public:
    // Constructor and destructor defined in .cc file to allow incomplete types
    // with unique_ptr
    IisModuleContext();
    ~IisModuleContext();

    // Required by IHttpStoredContext - called by IIS when cleaning up
    void CleanupStoredContext() override { delete this; }

    IisServerContext* active_context_;
    std::unique_ptr<IisRequestContext> request_context_;

    // ProxyFetch-based HTML rewriting.
    // IisAsyncFetch receives output from ProxyFetch.
    std::unique_ptr<IisAsyncFetch> iis_async_fetch_;
    // ProxyFetch handles HTML parsing and rewriting on worker threads.
    // Note: ProxyFetch deletes itself after Done() is called.
    ProxyFetch* proxy_fetch_{nullptr};

    // Cache lookup results
    bool cache_hit_;
    SharedString cached_content_;
    GoogleString cached_content_type_;

    // IPRO background recording state.
    // When a non-.pagespeed. resource URL is requested and IPRO is enabled,
    // we record the response body and cache it for future optimization.
    // The recorder is self-deleting after DoneAndSetHeaders() is called.
    InPlaceResourceRecorder* ipro_recorder_;  // Not owned after creation
    bool ipro_recording_started_;
    RequestHeaders::Properties ipro_request_properties_;
  };


  // Constructor for multi-site support (preferred)
  IisHttpModule(IisModuleFactory* factory, IisServerContext* default_context);

  // Legacy constructor for backward compatibility
  explicit IisHttpModule(IisServerContext* server_context);

  ~IisHttpModule() override;

  // IIS request pipeline events
  REQUEST_NOTIFICATION_STATUS OnBeginRequest(
      IHttpContext* context,
      IHttpEventProvider* provider) override;

  REQUEST_NOTIFICATION_STATUS OnResolveRequestCache(
      IHttpContext* context,
      IHttpEventProvider* provider) override;

  REQUEST_NOTIFICATION_STATUS OnSendResponse(
      IHttpContext* context,
      ISendResponseProvider* provider) override;

  REQUEST_NOTIFICATION_STATUS OnEndRequest(
      IHttpContext* context,
      IHttpEventProvider* provider) override;

 private:
  // Check if this is a .pagespeed. resource URL
  bool IsPageSpeedResource(const GoogleString& url);

  // Handle .pagespeed. resource request (IPRO)
  REQUEST_NOTIFICATION_STATUS HandlePageSpeedResource(
      IHttpContext* context,
      const GoogleString& url);

  // Handle admin/statistics requests
  REQUEST_NOTIFICATION_STATUS HandleAdminRequest(
      IHttpContext* context,
      const GoogleString& path);

  // Serve optimized content from cache.
  // Called when cache lookup succeeds in OnResolveRequestCache.
  REQUEST_NOTIFICATION_STATUS ServeFromCache(
      IHttpContext* context,
      const SharedString& cached_content,
      const char* content_type);

  // Check if response is HTML and should be rewritten
  bool ShouldRewriteResponse(IHttpContext* context);

  // Perform HTML rewriting via ProxyFetch.
  // Returns RQ_NOTIFICATION_FINISH_REQUEST on success,
  // RQ_NOTIFICATION_CONTINUE on failure (pass through original).
  REQUEST_NOTIFICATION_STATUS HandleHtmlRewriting(
      IHttpContext* context,
      ISendResponseProvider* provider);

  // Check if response is an IPRO-eligible resource (CSS, JS, or image)
  bool IsIproEligibleResponse(IHttpContext* context);

  // Check if the URL should be recorded for IPRO optimization.
  // Returns true if IPRO is enabled and the URL is not already a .pagespeed.
  // resource.
  bool ShouldRecordForIpro(IHttpContext* context, const GoogleString& url);

  // Start IPRO recording for a resource that was not found in cache.
  // Creates an InPlaceResourceRecorder and attaches it to the module context.
  void StartIproRecording(IHttpContext* context, const GoogleString& url);

  // Record response data during OnSendResponse.
  // Returns true if recording should continue, false if recording failed.
  bool RecordIproResponseData(IHttpContext* context,
                               ISendResponseProvider* provider);

  // Finish IPRO recording and cache the resource.
  void FinishIproRecording(IHttpContext* context, bool success);

  // Get request URL from IIS context
  GoogleString GetRequestUrl(IHttpContext* context);

  // Get request path from IIS context
  GoogleString GetRequestPath(IHttpContext* context);

  IisModuleFactory* module_factory_;  // Not owned, may be null for legacy
  IisServerContext* server_context_;  // Not owned, default context

  // Get the appropriate server context for this request
  IisServerContext* GetActiveContext(IHttpContext* context);

  // Get or create the per-request module context from the static map.
  // Returns nullptr if context is null.
  IisModuleContext* GetOrCreateModuleContext(IHttpContext* context);

  // Get the existing module context from IIS's context container.
  // Returns nullptr if not found.
  IisModuleContext* GetModuleContext(IHttpContext* context);

  // Remove the module context (cleanup is handled by IIS via CleanupStoredContext).
  void RemoveModuleContext(IHttpContext* context);

  DISALLOW_COPY_AND_ASSIGN(IisHttpModule);
};

}  // namespace net_instaweb

#endif  // PAGESPEED_IIS_IIS_HTTP_MODULE_H_
