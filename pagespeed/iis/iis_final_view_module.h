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
// For DaemonHtmlCache it does two things there:
//
//   * for an HTML response the module records, it reads that view for the
//     HTML cache engine and changes nothing;
//   * for a Markdown answer the main registration kept for it
//     (serve-variants, iis_daemon_html_serve.h), it replaces the empty
//     error response the main registration left with that answer and its
//     exact header set, removing every other line IIS and other modules
//     added, and only while the response is still that untouched error
//     response.  Without this registration the client gets the error,
//     never an unfinished Markdown answer.
//
// What it shares with the main registration it reads only through the
// main registration's own module context (IisModuleRequestContext, keyed by
// the main registration's module id), never through a header or a server
// variable a client could set.  Its send can run on the thread that resumes
// a request the rewriter held, inside that thread's IndicateCompletion, so it
// reads nothing a thread set earlier for itself and takes no lock.  (A
// Markdown answer is never held: it is finalised on the thread that made
// it.)
//
// At RQ_RESOLVE_REQUEST_CACHE, where the installer's order runs it before
// the main registration, it only marks the request as one it will see at
// its send (IisFinalViewRunsFor): the main registration keeps a Markdown
// answer only for such a request, and answers any other with its HTML.

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

// Whether the final-view registration ran its resolve-cache notification for
// this request, so it will run its send too.  False when the entry is not in
// the request's module list, or not ahead of the main one.
bool IisFinalViewRunsFor(IHttpContext* http_context);

class IisFinalViewModule : public CHttpModule {
 public:
  IisFinalViewModule() = default;

  REQUEST_NOTIFICATION_STATUS OnResolveRequestCache(
      IN IHttpContext* pHttpContext, IN IHttpEventProvider* pProvider) override;

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

  // The final-view registration's own module id, which keys the mark
  // IisFinalViewRunsFor reads; set once at its registration.
  static HTTP_MODULE_ID final_view_module_id() { return final_view_module_id_; }
  static void set_final_view_module_id(HTTP_MODULE_ID id) {
    final_view_module_id_ = id;
  }

 private:
  static HTTP_MODULE_ID final_view_module_id_;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_IIS_IIS_FINAL_VIEW_MODULE_H_
