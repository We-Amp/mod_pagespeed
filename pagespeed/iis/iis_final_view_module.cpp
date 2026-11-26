// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

#include "pagespeed/iis/iis_final_view_module.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <new>

#include "pagespeed/iis/iis_daemon_html_capture.h"
#include "pagespeed/iis/iis_daemon_html_serve.h"
#include "pagespeed/iis/iis_global_constants.h"
#include "pagespeed/iis/iis_misc.h"
#include "pagespeed/iis/iis_module_factory.h"
#include "pagespeed/iis/iis_module_request_context.h"
#include "pagespeed/iis/iis_request_context.h"
#include "pagespeed/iis/iis_server_context.h"
#include "pagespeed/kernel/base/message_handler.h"

namespace net_instaweb {

HTTP_MODULE_ID IisFinalViewModuleFactory::final_view_module_id_ = nullptr;

namespace {

// The mark IisFinalViewRunsFor reads: one object for every request, which
// IIS only ever asks to clean up.
class IisFinalViewMark : public IHttpStoredContext {
 public:
  void CleanupStoredContext() override {}
};
IisFinalViewMark final_view_mark;

// Replaces the pending error response with the Markdown answer the main
// registration kept (iis_daemon_html_serve.h).  Runs at the first send that
// carries the headers, on the thread that made the answer.
void FinalizeIisMarkdownAnswer(IHttpContext* http_context,
                               IisInnerRequestContext* inner) {
  std::unique_ptr<IisMarkdownAnswer> answer = inner->take_markdown_answer();
  IHttpResponse* response = http_context->GetResponse();
  HTTP_RESPONSE* raw =
      response == nullptr ? nullptr : response->GetRawHttpResponse();
  if (answer == nullptr || raw == nullptr) return;
  USHORT status = 0;
  USHORT substatus = 0;
  response->GetStatus(&status, &substatus);
  const IisMarkdownFinalPlan plan =
      PlanIisMarkdownFinal(status, substatus, raw->EntityChunkCount,
                           IisResponseHeaderLines(*raw), *answer);
  IisServerContext* server_context = inner->server_context();
  if (!plan.finalize) {
    // Another module replaced the response (a custom error page): it goes
    // out as that module made it.
    static std::atomic<bool> logged(false);
    if (!logged.exchange(true)) {
      server_context->message_handler()->Message(
          kWarning,
          "DaemonHtmlCache: a Markdown answer was not sent because another "
          "module replaced the response first (status %u.%u); the client got "
          "that response",
          static_cast<unsigned>(status), static_cast<unsigned>(substatus));
    }
    return;
  }
  for (const GoogleString& name : plan.remove) {
    response->DeleteHeader(name.c_str());
  }
  if (plan.transport_conflict) {
    DaemonHtmlEngine* engine = server_context->daemon_html_engine_if_built();
    if (engine != nullptr) engine->NoteTransportConflict();
  }
  response->SetStatus(static_cast<USHORT>(answer->status),
                      answer->status == 304 ? "Not Modified" : "OK", 0, S_OK,
                      nullptr, TRUE);
  for (const auto& [name, value] : answer->headers) {
    response->SetHeader(name.c_str(), value.c_str(),
                        static_cast<USHORT>(value.size()), TRUE);
  }
  if (!answer->body.empty()) {
    // In request memory: IIS sends it after this function returns.
    const DWORD size = static_cast<DWORD>(answer->body.size());
    char* body =
        static_cast<char*>(http_context->AllocateRequestMemory(size));
    HTTP_DATA_CHUNK* chunk = static_cast<HTTP_DATA_CHUNK*>(
        http_context->AllocateRequestMemory(sizeof(HTTP_DATA_CHUNK)));
    if (body == nullptr || chunk == nullptr) {
      // Headers that promise a body it cannot carry never go out.
      response->Clear();
      response->SetStatus(500, "Internal Server Error", 0, S_OK, nullptr,
                          TRUE);
      return;
    }
    memcpy(body, answer->body.data(), size);
    memset(chunk, 0, sizeof(*chunk));
    chunk->DataChunkType = HttpDataChunkFromMemory;
    chunk->FromMemory.pBuffer = body;
    chunk->FromMemory.BufferLength = size;
    raw->pEntityChunks = chunk;
    raw->EntityChunkCount = 1;
  }
  DisarmResponseCaches(response);
}

}  // namespace

bool IisFinalViewRunsFor(IHttpContext* http_context) {
  const HTTP_MODULE_ID id = IisFinalViewModuleFactory::final_view_module_id();
  if (id == nullptr || http_context == nullptr) return false;
  IHttpModuleContextContainer* container =
      http_context->GetModuleContextContainer();
  return container != nullptr && container->GetModuleContext(id) != nullptr;
}

REQUEST_NOTIFICATION_STATUS IisFinalViewModule::OnResolveRequestCache(
    IN IHttpContext* pHttpContext, IN IHttpEventProvider* pProvider) {
  UNREFERENCED_PARAMETER(pProvider);
  const HTTP_MODULE_ID id = IisFinalViewModuleFactory::final_view_module_id();
  IHttpModuleContextContainer* container =
      pHttpContext->GetModuleContextContainer();
  if (id != nullptr && container != nullptr) {
    container->SetModuleContext(&final_view_mark, id);
  }
  return RQ_NOTIFICATION_CONTINUE;
}

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
  // one the module records.  Without the main entry (it was removed while
  // this one stayed) there is no such context to read.
  if (IisModuleFactory::module_id() == nullptr) return RQ_NOTIFICATION_CONTINUE;
  IisModuleRequestContext* request_context =
      IisModuleRequestContext::GetRequestContext(pHttpContext);
  if (request_context == nullptr) return RQ_NOTIFICATION_CONTINUE;
  IisInnerRequestContext* inner = request_context->GetInnerContext();
  if (inner == nullptr) return RQ_NOTIFICATION_CONTINUE;
  // A Markdown answer the main registration kept for this send.
  if (inner->markdown_answer_pending()) {
    if (pProvider->GetHeadersBeingSent()) {
      FinalizeIisMarkdownAnswer(pHttpContext, inner);
    }
    return RQ_NOTIFICATION_CONTINUE;
  }
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
