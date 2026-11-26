// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

// The final-view registration of the module image (iis_registration.h).
//
// IIS runs this module's send notification at PRIORITY_ALIAS_FIRST, after
// ProtocolSupportModule has written the site's customHeaders and after
// dynamic compression: the latest point a module sees the response headers
// before they go out, and the only one that sees them as the client will.
// The main registration's send runs well before those stages, so it cannot.
//
// For DaemonHtmlCache it reads that view for the HTML cache engine and
// changes nothing: no header, no body, no status, no completion.
//
// What it shares with the main registration it reads only through the
// main registration's own module context (IisModuleRequestContext, keyed by
// the main registration's module id), never through a header or a server
// variable a client could set.  Its send can run on the thread that resumes
// a request the rewriter held, inside that thread's IndicateCompletion, so it
// reads nothing a thread set earlier for itself and takes no lock.

#ifndef PAGESPEED_IIS_IIS_FINAL_VIEW_MODULE_H_
#define PAGESPEED_IIS_IIS_FINAL_VIEW_MODULE_H_

// clang-format off
#include <windows.h>
#include <sal.h>
#include <httpserv.h>
// clang-format on

#include "pagespeed/system/daemon_html_engine.h"

namespace net_instaweb {

// Every header line of an IIS response, known headers in their table order
// and then the unknown ones, as copies.
DaemonHtmlHeaderLines IisResponseHeaderLines(const HTTP_RESPONSE& response);

class IisFinalViewModule : public CHttpModule {
 public:
  IisFinalViewModule() = default;

  REQUEST_NOTIFICATION_STATUS OnSendResponse(
      IN IHttpContext* pHttpContext,
      IN ISendResponseProvider* pProvider) override;

  // Allocated in request memory; IIS frees it with the request.
  VOID Dispose() override { this->~IisFinalViewModule(); }

 private:
  IisFinalViewModule(const IisFinalViewModule&) = delete;
  IisFinalViewModule& operator=(const IisFinalViewModule&) = delete;
};

class IisFinalViewModuleFactory : public IHttpModuleFactory {
 public:
  IisFinalViewModuleFactory() = default;
  HRESULT GetHttpModule(OUT CHttpModule** ppModule,
                        IN IModuleAllocator* pAllocator) override;
  void Terminate() override { delete this; }
};

}  // namespace net_instaweb

#endif  // PAGESPEED_IIS_IIS_FINAL_VIEW_MODULE_H_
