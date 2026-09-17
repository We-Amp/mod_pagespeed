// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef IIS_MODULE_BASE_FETCH_H_
#define IIS_MODULE_BASE_FETCH_H_

#define _WINSOCKAPI_
#ifndef _WINSOCKAPI_
#define _WINSOCKAPI_
#endif
#include <windows.h>
#include <sal.h>
#include <httpserv.h>

extern "C" {

}

#include "net/instaweb/http/public/async_fetch.h"
#include "pagespeed/kernel/http/headers.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/mapped_shared_string.h"
#include "pagespeed/iis/iis_base_fetch.h"
#include "pagespeed/iis/iis_zerocopy_serve.h"
#include <atomic>
#include <memory>
#include <vector>

namespace net_instaweb {

	#define MY_CUSTOM_NOTIFICATION L"MyCustomNotification"
class MyCustomProvider : public ICustomNotificationProvider
	{
	public:
		// Create the method that will identify the custom notification.
		PCWSTR QueryNotificationType(VOID)
		{
			// Return the unique identifier string for the custom notification.
			return MY_CUSTOM_NOTIFICATION;
		}
		// Create the method that will process errors.
		VOID SetErrorStatus(HRESULT hrError)
		{
			return;
		}
	};


enum FetchType {
 kUnknown = 0,
 kHtml = 1,
 kResource = 2,
 kInPlace = 3,
};

class IisInnerRequestContext;
class InPlaceResourceRecorder;
class Variable;
class IisModuleBaseFetch : public IisBaseFetch {
 public:

  IisModuleBaseFetch (IHttpContext* http_context, FetchType fetch_type, MessageHandler * handler,const RequestContextPtr &ptr, IisInnerRequestContext* ctx);

  virtual ~IisModuleBaseFetch ();
  static void PopulateRequestHeaders(IHttpContext* http_context, RequestHeaders* request_headers);
  static void PopulateResponseHeaders(IHttpContext* http_context, ResponseHeaders* response_headers);

  int CollectHeaders();
  bool headers_collected() { return headers_collected_; }
  void SetLastWrite(DWORD lastwrite) {this->lastwritetime=lastwrite;}
  DWORD RequestTime() {return lastwritetime-donetime;}
  static volatile LONG fetchount;
  bool need_flush() { return need_flush_; }
  void set_need_flush(bool x) { need_flush_ = x; }
  void set_pending(bool x) { pending_ = x; }
  void set_flush_html(bool x) { flush_html_ = x; }  
  bool get_flush_html() { return flush_html_; }  
  void SendData(MessageHandler* handler, bool flush);  
  bool HasChunk(HTTP_DATA_CHUNK* chunk);

  // ==== Zero-copy aliased serve (CycloneZeroCopyServe) ====================
  // The '.pagespeed.' resource cache-hit serve (rewrite_driver.cc
  // CacheCallback::DeliverDone) calls WriteMapped with a StringPiece
  // aliasing a Cyclone mmap region and a MappedSharedString pin (read-lease
  // keep-alive carrying the lease renew / force-wrap hooks).  Instead of
  // the request-pool copy HandleWrite would do, this override records the
  // borrowed span; HandleDone then serves it as a sequence of aliased
  // chunks, each pushed with WriteEntityChunkByReference + an async Flush
  // whose completion (OnZeroCopyCompletion, routed from the module's
  // OnAsyncCompletion) is the per-chunk "HTTP.sys is done reading this
  // buffer" barrier.  The pin is held until the serve finishes, aborts, or
  // -- as a backstop -- this fetch is destroyed at request cleanup (the
  // inner request context holds the last reference until
  // CleanupStoredContext, which runs strictly after the response drain or
  // client disconnect).  Chunking/copy-out/abort policy lives in
  // IisZeroCopyServePlanner (iis_zerocopy_serve.h); guards that must
  // disqualify aliasing (compression, Range, non-GET) live in
  // ZeroCopyGuardsAllowAlias().  Anything disqualified degrades to the
  // classic copying Write() -- never to a failed request while correct
  // bytes can still be produced.
  bool WriteMapped(const StringPiece& mmap_sp,
                   const MappedSharedString& keepalive,
                   MessageHandler* handler) override;
  // True while the async chunked serve owns request completion signaling.
  bool HasActiveZeroCopyServe() const {
    return zc_active_.load(std::memory_order_acquire);
  }
  // Called from IisHttpModule::OnAsyncCompletion (IIS thread-pool thread)
  // for each per-chunk flush completion.  Returns the notification status
  // the module must return (PENDING while more chunks are in flight).
  REQUEST_NOTIFICATION_STATUS OnZeroCopyCompletion(HRESULT completion_status);

 private:
  virtual bool HandleWrite(const StringPiece& sp, MessageHandler* handler);
  virtual bool HandleFlush(MessageHandler* handler);
  virtual void HandleHeadersComplete();
  virtual void HandleDone(bool success);
  void AddChunk(char *data,int len,bool lock=true);
  virtual void log(const char * msg, ...);

  // Zero-copy serve internals (see the public section above).
  enum class ZcDrive {
    kPending,   // an async flush is in flight; a completion will resume
    kFinished,  // all body bytes verified and handed to IIS
    kAborted,   // connection reset (torn bytes / unrecoverable failure)
  };
  bool ZeroCopyGuardsAllowAlias();
  void StartZeroCopyServe();
  ZcDrive DriveZeroCopyServe();
  // epoch_torn selects the stat: torn borrows count as
  // zerocopy_serve_renew_fail_reset (nginx-compatible meaning);
  // allocation/submit failures count as zerocopy_serve_aborted.
  ZcDrive AbortZeroCopyServe(const char* reason, bool epoch_torn);

  IHttpContext* http_context_;
  
  FetchType fetch_type_;
  bool keep_alive_;
  bool headers_collected_;
  char *currentblock;
  int currentblocksize;
  int currentblockpos;
  DWORD lastwritetime;
  DWORD donetime;
  DWORD alive;
  
  std::vector<HTTP_DATA_CHUNK*> chunks_;
  bool need_flush_;
  size_t chunk_send_index_;
  bool pending_;
  bool flush_html_;
  IisInnerRequestContext* ctx_;
  MessageHandler* handler_;

  // Zero-copy serve state.  Mutation is strictly alternating: WriteMapped/
  // HandleDone on the PSOL delivery thread first, then exactly one thread
  // at a time in submit -> completion -> submit order (IIS allows a single
  // outstanding async operation per request).  zc_active_ is the atomic
  // handoff flag the module's completion dispatch keys on.
  const char* zc_data_;
  size_t zc_size_;
  bool zc_has_borrowed_;
  bool zc_aliased_counted_;
  MappedSharedString zc_pin_;
  std::unique_ptr<IisZeroCopyServePlanner> zc_planner_;
  std::atomic<bool> zc_active_;
  // Statistics (registered centrally in rewrite_driver_factory.cc; shared
  // names with the nginx sink).  Looked up once in StartZeroCopyServe.
  Variable* zc_aliased_stat_;
  Variable* zc_copied_out_stat_;
  Variable* zc_renew_fail_stat_;
  Variable* zc_aborted_stat_;
  IisModuleBaseFetch(const IisModuleBaseFetch&) = delete;
  IisModuleBaseFetch& operator=(const IisModuleBaseFetch&) = delete;
}; 

} // namespace net_instaweb

#endif  // IIS_MODULE_BASE_FETCH_H_