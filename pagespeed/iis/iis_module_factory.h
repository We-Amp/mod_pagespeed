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

 private:
  // Driver factory
  std::unique_ptr<IisRewriteDriverFactory> driver_factory_;

  // Server context for all requests
  std::unique_ptr<IisServerContext> default_context_;

  IHttpServer* iis_server_;  // Not owned

  // Module ID for context storage via IHttpModuleContextContainer
  HTTP_MODULE_ID module_id_ = nullptr;

  // Static module ID for use in handlers (IISpeed pattern).
  // This provides a single source of truth accessible without factory reference.
  static HTTP_MODULE_ID s_module_id_;

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
