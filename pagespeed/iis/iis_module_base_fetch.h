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
#include <string>
#include <vector>

namespace net_instaweb {

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

  ~IisModuleBaseFetch () override;
  static void PopulateRequestHeaders(IHttpContext* http_context, RequestHeaders* request_headers);
  static void PopulateResponseHeaders(IHttpContext* http_context, ResponseHeaders* response_headers);

  int CollectHeaders();
  bool headers_collected() {
    Lock();
    bool collected = headers_collected_;
    Unlock();
    return collected;
  }
  void SetLastWrite(DWORD lastwrite) {this->lastwritetime=lastwrite;}
  DWORD RequestTime() {return lastwritetime-donetime;}
  bool need_flush() { return need_flush_; }
  void set_need_flush(bool x) { need_flush_ = x; }
  void set_flush_html(bool x) { flush_html_ = x; }  
  bool get_flush_html() { return flush_html_; }  
  // Caching-header policy for CollectHeaders.  Set on the HTML base fetch
  // from the options resolved for the response being rewritten (the HTML
  // driver is not stored on the inner context, so CollectHeaders cannot
  // read the flag from there).
  void set_modify_cache_headers(bool x) { modify_cache_headers_ = x; }
  void SendData(MessageHandler* handler, bool flush);  

  // ==== Thread rule for rewritten HTML (kHtml) =============================
  // IIS request objects are not thread safe and IIS itself uses them between
  // notifications.  For an HTML response they are touched only by the thread
  // that is running this module's OnSendResponse before it calls Done()
  // (EnterNotification .. LeaveNotification), or by HandleDone after the
  // hand-off (HandOffToCompletion).  PSOL worker threads only append to a
  // module-owned buffer under the fetch lock; HandleDone copies that buffer
  // into request memory.  Other fetch types are not subject to the rule and
  // these calls do nothing for them.
  //
  // The calling thread becomes the one that may touch IIS.
  void EnterNotification();
  // The calling thread gives the right up again.
  void LeaveNotification();
  // The final send is done: HandleDone, on whichever thread runs it, is now
  // the only code that touches IIS.  Call once, before Done().
  void HandOffToCompletion();
  // The request is going away without HandleDone having run (IIS cleaned the
  // request up first): drop the request pointer so later worker output can
  // never reach IIS.  HandleDone then only releases its reference.
  void DetachFromRequest() override;
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
  bool HandleWrite(const StringPiece& sp, MessageHandler* handler) override;
  bool HandleFlush(MessageHandler* handler) override;
  void HandleHeadersComplete() override;
  void HandleDone(bool success) override;
  void AddChunk(char *data,int len,bool lock=true);
  void log(const char * msg, ...) override;

  // Request-memory buffering of body bytes.  Caller holds the fetch lock;
  // sp is copied into blocks from the request pool.  Returns false (and
  // latches write_failed_) when the pool cannot satisfy an allocation.
  bool BufferIntoRequestMemory(const StringPiece& sp);
  // True when the calling thread may call into IIS for this fetch (always for
  // fetch types other than kHtml).  Caller holds the fetch lock.
  bool MayTouchIisLocked() const;
  bool MayTouchIis();

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

  // kHtml thread rule state; written only under the fetch lock.
  GoogleString html_body_;       // rewritten bytes not yet in request memory
  DWORD iis_owner_thread_;       // thread inside OnSendResponse, 0 if none
  bool detached_;                // request is gone; never touch IIS again
  // Log indentation and root context, computed on construction so log() never
  // walks the IIS context from another thread.
  std::string log_indent_;
  void* log_root_;

  FetchType fetch_type_;
  bool keep_alive_;
  bool headers_collected_;
  char *currentblock;
  int currentblocksize;
  int currentblockpos;
  // Set once request memory could not be had for the body: every later
  // write fails and the response is reset instead of completed.
  bool write_failed_;
  DWORD lastwritetime;
  DWORD donetime;
  DWORD alive;
  
  std::vector<HTTP_DATA_CHUNK*> chunks_;
  bool need_flush_;
  size_t chunk_send_index_;
  bool pending_;
  bool flush_html_;
  bool modify_cache_headers_;
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