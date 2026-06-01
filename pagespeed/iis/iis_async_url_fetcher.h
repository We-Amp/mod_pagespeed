#ifndef IISASYNCURLFETCHER_H
#define IISASYNCURLFETCHER_H
#include "pagespeed/iis/asyncwinhttp.h" 
#include "net/instaweb/http/public/http_response_parser.h"
#include "net/instaweb/http/public/async_fetch.h"
#include "net/instaweb/http/public/url_async_fetcher.h"
//#include "net/instaweb/http/public/url_pollable_async_fetcher.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"

#include "pagespeed/iis/iis_message_handler.h"

using namespace ASyncWinHTTP;

namespace net_instaweb {

class IisAsyncWorker;

class IisAsyncUrlFetcher: public UrlAsyncFetcher
{
	CRITICAL_SECTION cs;
	std::list<IisAsyncWorker *> asyncworkers;
	bool fetcher_supports_https_;
public:
	IisAsyncUrlFetcher();
	void StopFetch(IisAsyncWorker *worker);
	virtual ~IisAsyncUrlFetcher();
	virtual bool SupportsHttps() const { return fetcher_supports_https_; }
	virtual void Fetch(const GoogleString& url,
                     MessageHandler* message_handler,
                     AsyncFetch* fetch);
	// Since the underlying fetcher is blocking, there can never be
	// any outstanding fetches.  
	virtual int Poll(int64 max_wait_ms);

	void set_fetcher_supports_https(bool val) { fetcher_supports_https_ = val; }

	bool shutting_down_;
	virtual void ShutDown();


};


class IisAsyncWorker :public ASyncWinHTTP::WinHTTPEvents
{
	net_instaweb::IisAsyncUrlFetcher *fetcher;
	ASyncWinHTTP::WinHTTP client;
	AsyncFetch *fetch;
	MessageHandler *messagehandler;
	HttpResponseParser *parser;
	std::vector<char> myData;
public:
	IisAsyncWorker(net_instaweb::IisAsyncUrlFetcher * fetcher, AsyncFetch *fetch, MessageHandler* messagehandler);
	virtual void OnData(const char *data, int length, WinHTTPContentType type);// the data
	virtual void OnCompleted(WinHTTPStatus status);
	void Cancel();
	bool GetUrl(GoogleString url, const char* host)
	{
		CHECK(host != NULL);
		bool res = client.GetUrl(std::string(url.c_str()), std::string(host == NULL ? "" : host));
		messagehandler->Message(kInfo, "Request to [%s] with host header [%s] -> %s", url.c_str(), host, res ? "OK":"Fail");
		return res;
	}
	~IisAsyncWorker();

};

} // namespace net_instaweb


#endif
