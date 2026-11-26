// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

#include "pagespeed/iis/iis_final_view_module.h"

#include <new>

#include "pagespeed/iis/iis_daemon_html_capture.h"
#include "pagespeed/iis/iis_global_constants.h"
#include "pagespeed/iis/iis_module_request_context.h"
#include "pagespeed/iis/iis_request_context.h"

namespace net_instaweb {

DaemonHtmlHeaderLines IisResponseHeaderLines(const HTTP_RESPONSE& response) {
  DaemonHtmlHeaderLines lines;
  for (int i = 0; i < HttpHeaderResponseMaximum; ++i) {
    const HTTP_KNOWN_HEADER& known = response.Headers.KnownHeaders[i];
    if (known.pRawValue != nullptr && known.RawValueLength > 0) {
      lines.emplace_back(HTTP_HEADER_ID_STRINGS_RESPONSE[i],
                         GoogleString(known.pRawValue, known.RawValueLength));
    }
  }
  if (response.Headers.pUnknownHeaders != nullptr) {
    for (USHORT i = 0; i < response.Headers.UnknownHeaderCount; ++i) {
      const HTTP_UNKNOWN_HEADER& unknown = response.Headers.pUnknownHeaders[i];
      if (unknown.pName == nullptr || unknown.NameLength == 0) continue;
      lines.emplace_back(
          GoogleString(unknown.pName, unknown.NameLength),
          unknown.pRawValue == nullptr
              ? GoogleString()
              : GoogleString(unknown.pRawValue, unknown.RawValueLength));
    }
  }
  return lines;
}

REQUEST_NOTIFICATION_STATUS IisFinalViewModule::OnSendResponse(
    IN IHttpContext* pHttpContext, IN ISendResponseProvider* pProvider) {
  // Only the main registration's own context says whether this response is
  // one the module records.
  IisModuleRequestContext* request_context =
      IisModuleRequestContext::GetRequestContext(pHttpContext);
  if (request_context == nullptr) return RQ_NOTIFICATION_CONTINUE;
  IisInnerRequestContext* inner = request_context->GetInnerContext();
  if (inner == nullptr) return RQ_NOTIFICATION_CONTINUE;
  IisDaemonHtmlCapture* capture = inner->daemon_html_capture();
  if (capture == nullptr) return RQ_NOTIFICATION_CONTINUE;

  // The headers go out with the first send that carries them; later sends
  // carry none.
  if (pProvider->GetHeadersBeingSent() && capture->needs_final_view()) {
    IHttpResponse* response = pHttpContext->GetResponse();
    const HTTP_RESPONSE* raw =
        response == nullptr ? nullptr : response->GetRawHttpResponse();
    if (raw != nullptr) {
      capture->NoteFinalView(raw->StatusCode, IisResponseHeaderLines(*raw));
    }
  }
  IHttpConnection* connection = pHttpContext->GetConnection();
  if (connection == nullptr || !connection->IsConnected()) {
    capture->NoteAborted();
  }
  return RQ_NOTIFICATION_CONTINUE;
}

HRESULT IisFinalViewModuleFactory::GetHttpModule(
    OUT CHttpModule** ppModule, IN IModuleAllocator* pAllocator) {
  if (ppModule == nullptr || pAllocator == nullptr) return E_INVALIDARG;
  void* memory = pAllocator->AllocateMemory(sizeof(IisFinalViewModule));
  if (memory == nullptr) return HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY);
  *ppModule = new (memory) IisFinalViewModule();
  return S_OK;
}

}  // namespace net_instaweb
