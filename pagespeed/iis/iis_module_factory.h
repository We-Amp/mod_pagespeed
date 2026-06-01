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

#ifndef PAGESPEED_IIS_IIS_MODULE_FACTORY_H_
#define PAGESPEED_IIS_IIS_MODULE_FACTORY_H_

// IIS Native Module API headers
#include <httpserv.h>

#include <list>
#include <map>
#include <memory>
#include <mutex>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "net/instaweb/rewriter/public/process_context.h"

namespace net_instaweb {

class IisConfig;
class IisRewriteDriverFactory;
class IisServerContext;

// IIS HTTP Module Factory implementing IHttpModuleFactory.
//
// This factory is responsible for:
// 1. Creating IisHttpModule instances for each request
// 2. Managing per-site IisServerContext instances for multi-site support
// 3. Terminating the module when IIS recycles the app pool
//
// Multi-site support:
// Each IIS site/application can have its own web.config with PageSpeed settings.
// The factory maintains a cache of server contexts per site, keyed by the
// application path. This allows different sites to have different optimization
// settings while sharing the underlying driver factory.
class IisModuleFactory : public IHttpModuleFactory {
 public:
  IisModuleFactory();
  ~IisModuleFactory();

  // Initialize the factory with IIS server context
  HRESULT Initialize(IHttpServer* server);

  // IHttpModuleFactory interface
  HRESULT GetHttpModule(
      CHttpModule** module,
      IModuleAllocator* allocator) override;

  void Terminate() override;

  // Get or create a server context for the given IIS context.
  // The context is cached by site/application path.
  IisServerContext* GetServerContext(IHttpContext* context);

  // Access the default server context (for backward compatibility)
  IisServerContext* server_context() { return default_context_.get(); }

  // Access the driver factory
  IisRewriteDriverFactory* factory() { return driver_factory_.get(); }

  // Get the module ID for context storage
  HTTP_MODULE_ID module_id() const { return module_id_; }

  // Set the module ID (called by RegisterModule)
  void set_module_id(HTTP_MODULE_ID id) { module_id_ = id; }

  // Invalidate cached configuration for a site
  void InvalidateSiteConfig(const GoogleString& site_id);

  // Invalidate all cached configurations
  void InvalidateAllConfigs();

  // Default maximum number of site contexts to cache before evicting oldest.
  // Can be overridden via max_site_contexts configuration.
  static constexpr size_t kDefaultMaxSiteContexts = 100;

 private:
  // Evict oldest site contexts when cache exceeds max_site_contexts_.
  // Must be called while holding site_contexts_mutex_.
  void EvictOldestIfNeeded();

  // Update LRU tracking for a site. Moves the site to the front of the LRU
  // list (most recently used). Must be called while holding site_contexts_mutex_.
  void TouchLru(const GoogleString& site_id);
  // Get a unique identifier for the site/application
  GoogleString GetSiteId(IHttpContext* context);

  // Create a new server context for a site
  IisServerContext* CreateServerContext(const GoogleString& site_id,
                                          std::unique_ptr<IisConfig> config);

  // Shared driver factory for all sites
  std::unique_ptr<IisRewriteDriverFactory> driver_factory_;

  // Default server context for backward compatibility
  std::unique_ptr<IisServerContext> default_context_;

  // Per-site server context cache
  std::map<GoogleString, std::unique_ptr<IisServerContext>> site_contexts_;
  std::mutex site_contexts_mutex_;

  // LRU eviction support:
  // - site_contexts_lru_: ordered list with most recently used at front
  // - lru_positions_: maps site_id to its position in site_contexts_lru_ for O(1) updates
  std::list<GoogleString> site_contexts_lru_;
  std::map<GoogleString, std::list<GoogleString>::iterator> lru_positions_;
  size_t max_site_contexts_ = kDefaultMaxSiteContexts;

  IHttpServer* iis_server_;  // Not owned

  // Module ID for context storage via IHttpModuleContextContainer
  HTTP_MODULE_ID module_id_ = nullptr;

  // Process context must outlive the driver factory since the factory
  // stores a pointer to data inside it (js_tokenizer_patterns).
  std::unique_ptr<ProcessContext> process_context_;

  DISALLOW_COPY_AND_ASSIGN(IisModuleFactory);
};

}  // namespace net_instaweb

// DLL entry point for IIS module registration
extern "C" __declspec(dllexport) HRESULT __stdcall RegisterModule(
    DWORD version,
    IHttpModuleRegistrationInfo* info,
    IHttpServer* server);

#endif  // PAGESPEED_IIS_IIS_MODULE_FACTORY_H_
