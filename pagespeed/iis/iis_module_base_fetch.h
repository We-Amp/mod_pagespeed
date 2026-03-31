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
#include "pagespeed/iis/iis_base_fetch.h"
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
  
 private:
  virtual bool HandleWrite(const StringPiece& sp, MessageHandler* handler);
  virtual bool HandleFlush(MessageHandler* handler);
  virtual void HandleHeadersComplete();
  virtual void HandleDone(bool success);
  void AddChunk(char *data,int len,bool lock=true);
  virtual void log(const char * msg, ...);

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
  IisModuleBaseFetch(const IisModuleBaseFetch&) = delete;
  IisModuleBaseFetch& operator=(const IisModuleBaseFetch&) = delete;
}; 

} // namespace net_instaweb

#endif  // IIS_MODULE_BASE_FETCH_H_