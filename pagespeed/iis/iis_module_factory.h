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

// Windows headers - winsock2.h must come before windows.h and httpserv.h
// to avoid redefinition errors with winsock.h
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>

// IIS Native Module API headers
#include <httpserv.h>

#include <atomic>
#include <memory>

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
// 2. Managing the single IisServerContext for all requests
// 3. Terminating the module when IIS recycles the app pool
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

  // Access the server context
  IisServerContext* server_context() { return default_context_.get(); }

  // Access the driver factory
  IisRewriteDriverFactory* factory() { return driver_factory_.get(); }

  // Get the module ID for context storage (instance accessor)
  HTTP_MODULE_ID module_id() const { return module_id_; }

  // Set the module ID (called by RegisterModule)
  void set_module_id(HTTP_MODULE_ID id) {
    module_id_ = id;
    s_module_id_ = id;  // Also set static for use in handlers
  }

  // Static accessor for use in handlers (IISpeed pattern).
  // This avoids accessing module_factory_ which may not be set in all contexts.
  static HTTP_MODULE_ID GetModuleId() { return s_module_id_; }

  // Shutdown coordination: prevents crashes from in-flight requests during
  // app pool recycle. OnBeginRequest checks IsShuttingDown() and calls
  // IncrementActiveRequests(); CleanupStoredContext calls
  // DecrementActiveRequests() when the request fully completes.
  bool IsShuttingDown() const;
  void IncrementActiveRequests();
  void DecrementActiveRequests();

 protected:
  // Hook for heavy system initialization (caches, threads, Redis, etc.).
  // Called by Initialize() after creating the factory and server context.
  // Override in tests to skip the slow Init/PostConfig/RootInit/ChildInit
  // sequence that requires running services.
  virtual HRESULT SetupSystemCaches();

 protected:
  // Driver factory - protected so test subclasses can set up lightweight init
  std::unique_ptr<IisRewriteDriverFactory> driver_factory_;

  // Server context for all requests
  std::unique_ptr<IisServerContext> default_context_;

 private:

  IHttpServer* iis_server_;  // Not owned
  bool initialized_;         // Whether Initialize() completed successfully
  bool caches_initialized_;  // Whether SetupSystemCaches() ran full init

  // Module ID for context storage via IHttpModuleContextContainer
  HTTP_MODULE_ID module_id_ = nullptr;

  // Static module ID for use in handlers (IISpeed pattern).
  // This provides a single source of truth accessible without factory reference.
  static HTTP_MODULE_ID s_module_id_;

  // Process context must outlive the driver factory since the factory
  // stores a pointer to data inside it (js_tokenizer_patterns).
  std::unique_ptr<ProcessContext> process_context_;

  // Shutdown coordination.
  std::atomic<bool> shutting_down_{false};
  std::atomic<int> active_request_count_{0};

  DISALLOW_COPY_AND_ASSIGN(IisModuleFactory);
};

// Global module that receives GL_APPLICATION_START notifications.
// Per-site JS library loading has moved to SetupSystemCaches() via
// IisConfig::LoadAllSiteLibraries(). This handler is retained for
// future per-site notifications.
class IisGlobalModule : public CGlobalModule {
 public:
  explicit IisGlobalModule(IisModuleFactory* factory) : factory_(factory) {}

  GLOBAL_NOTIFICATION_STATUS OnGlobalApplicationStart(
      IHttpApplicationStartProvider* provider) override;

  void Terminate() override {}

 private:
  IisModuleFactory* factory_;  // Not owned
};

}  // namespace net_instaweb

// DLL entry point for IIS module registration
extern "C" __declspec(dllexport) HRESULT __stdcall RegisterModule(
    DWORD version,
    IHttpModuleRegistrationInfo* info,
    IHttpServer* server);

#endif  // PAGESPEED_IIS_IIS_MODULE_FACTORY_H_
