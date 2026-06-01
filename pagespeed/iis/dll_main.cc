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

// DLL entry point and IIS RegisterModule for PageSpeed IIS module.
// Merges IISpeed's iisps.cpp initialization into 1.1's dll_main.cc.

#define _WINSOCKAPI_
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <httpserv.h>

#include "pagespeed/kernel/html/html_keywords.h"
#include "net/instaweb/rewriter/public/process_context.h"
#include "pagespeed/system/system_thread_system.h"
#include "pagespeed/iis/iis_message_handler.h"
#include "pagespeed/iis/iis_rewrite_driver_factory.h"
#include "pagespeed/iis/iis_rewrite_options.h"
#include "pagespeed/iis/iis_module_factory.h"
#include "pagespeed/iis/iis_configuration.h"
#include "pagespeed/iis/iis_global_constants.h"
#include "pagespeed/iis/log_message_handler.h"

#include <cstdio>
#include <google/protobuf/stubs/common.h>

// ASan runtime configuration for IIS testing.
// Embedded in the DLL so IIS worker processes pick up these options
// automatically without requiring environment variable configuration.
#if defined(__SANITIZE_ADDRESS__) || \
    (defined(__has_feature) && __has_feature(address_sanitizer))
extern "C" const char* __asan_default_options() {
  return "halt_on_error=0"
         ":log_path=C:\\pagespeed_asan"
         ":detect_leaks=0";
}
#endif

// ---------------------------------------------------------------------------
// Global state (formerly in IISpeed's iisps.cpp)
// ---------------------------------------------------------------------------
namespace net_instaweb {

HMODULE currentModule = NULL;
std::string modulePath;
std::string moduleFilename;
IisMessageHandler* message_handler = NULL;
ConfigFactory* cf = NULL;

}  // namespace net_instaweb


// ---------------------------------------------------------------------------
// MyGlobalModule — preserves the pristine URL before other modules modify it
// ---------------------------------------------------------------------------
namespace net_instaweb {

class MyGlobalModule : public CGlobalModule {
 public:
  MyGlobalModule() {}
  ~MyGlobalModule() {}

  GLOBAL_NOTIFICATION_STATUS
  OnGlobalPreBeginRequest(IN IPreBeginRequestProvider* pProvider) {
    UNREFERENCED_PARAMETER(pProvider);
    auto rq = pProvider->GetHttpContext()->GetRequest();

    PCWSTR tmpx = NULL;
    DWORD len;
    // Set X-PRISTINE-URL before any other module can modify the URL.
    pProvider->GetHttpContext()->GetRootContext()->GetServerVariable(
        "X-PRISTINE-URL", &tmpx, &len);
    if (tmpx == NULL) {
      auto url = rq->GetRawHttpRequest()->CookedUrl.pFullUrl;
      pProvider->GetHttpContext()->GetRootContext()->SetServerVariable(
          "X-PRISTINE-URL", url);
    }
    return GL_NOTIFICATION_CONTINUE;
  }

  VOID Terminate() {
    delete this;
  }
};

}  // namespace net_instaweb


// ---------------------------------------------------------------------------
// DllMain — process attach/detach initialization
// ---------------------------------------------------------------------------
BOOL APIENTRY DllMain(HMODULE hModule,
                      DWORD ul_reason_for_call,
                      LPVOID lpReserved) {
  switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH: {
      DisableThreadLibraryCalls(hModule);

      net_instaweb::HtmlKeywords::Init();

      net_instaweb::IisProcessContext::PSOL_PROCESS_CONTEXT =
          new net_instaweb::ProcessContext();


      // Create thread system and message handler.
      // SystemThreadSystem uses WinThreadSystem on Windows.
      auto ts = new net_instaweb::SystemThreadSystem();
      net_instaweb::message_handler =
          new net_instaweb::IisMessageHandler(ts->NewTimer(), ts->NewMutex());

      net_instaweb::log_message_handler::Install(net_instaweb::message_handler);
      net_instaweb::IisRewriteDriverFactory::Initialize();
      net_instaweb::IisRewriteOptions::Initialize();
      net_instaweb::currentModule = hModule;
      break;
    }

    case DLL_PROCESS_DETACH:
      delete net_instaweb::cf;
      net_instaweb::cf = NULL;

      net_instaweb::IisRewriteOptions::Terminate();
      net_instaweb::IisRewriteDriverFactory::Terminate();
      google::protobuf::ShutdownProtobufLibrary();
      net_instaweb::HtmlKeywords::ShutDown();

      delete net_instaweb::message_handler;
      net_instaweb::message_handler = NULL;
      break;

    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
      break;
  }
  return TRUE;
}

// ---------------------------------------------------------------------------
// RegisterModule — IIS calls this to register the HTTP module
// ---------------------------------------------------------------------------
extern "C" __declspec(dllexport) HRESULT __stdcall RegisterModule(
    DWORD dwServerVersion,
    IHttpModuleRegistrationInfo* pModuleInfo,
    IHttpServer* pGlobalInfo) {
  UNREFERENCED_PARAMETER(dwServerVersion);

  char mypath[MAX_PATH];
  GetModuleFileNameA(net_instaweb::currentModule, mypath, MAX_PATH);
  net_instaweb::moduleFilename = std::string(mypath);

  char* lastslash = strrchr(mypath, '\\');
  if (lastslash) lastslash[1] = 0;
  net_instaweb::modulePath = std::string(mypath);

  // Initialize the empty data chunk used for zero-length responses.
  emptydatachunk.DataChunkType = HttpDataChunkFromMemory;
  emptydatachunk.FromMemory.BufferLength = 0;
  emptydatachunk.FromMemory.pBuffer = const_cast<char*>(emptystring);

  net_instaweb::cf = new ConfigFactory();

  // Register global module for preserving pristine URLs.
  auto pGlobalModule = new net_instaweb::MyGlobalModule();
  HRESULT hr = pModuleInfo->SetGlobalNotifications(
      pGlobalModule, GL_PRE_BEGIN_REQUEST);
  if (FAILED(hr)) return hr;

  hr = pModuleInfo->SetPriorityForGlobalNotification(
      GL_PRE_BEGIN_REQUEST, PRIORITY_ALIAS_FIRST);
  if (FAILED(hr)) return hr;

  // Register the HTTP module factory.
  net_instaweb::IisModuleFactory::set_module_id(pModuleInfo->GetId());
  hr = pModuleInfo->SetRequestNotifications(
      new net_instaweb::IisModuleFactory(
          net_instaweb::modulePath,
          pGlobalInfo->GetAppPoolName(),
          net_instaweb::message_handler),
      RQ_SEND_RESPONSE | RQ_BEGIN_REQUEST | RQ_CUSTOM_NOTIFICATION,
      0);
  if (FAILED(hr)) return hr;

  hr = pModuleInfo->SetPriorityForRequestNotification(
      RQ_SEND_RESPONSE, PRIORITY_ALIAS_LAST);
  if (FAILED(hr)) return hr;

  hr = pModuleInfo->SetPriorityForRequestNotification(
      RQ_BEGIN_REQUEST, PRIORITY_ALIAS_FIRST);
  if (FAILED(hr)) return hr;

  return hr;
}
