
#include "pagespeed/iis/iis_async_url_fetcher.h"

#include "pagespeed/iis/iis_message_handler.h"

#include "pagespeed/kernel/http/google_url.h"

using namespace ASyncWinHTTP;

namespace net_instaweb {

IisAsyncWorker::IisAsyncWorker(IisAsyncUrlFetcher * fetcher,AsyncFetch *fetch,MessageHandler *messagehandler)
{
	parser=new HttpResponseParser(fetch->response_headers(),fetch,messagehandler);
	client.SetEventHandler(this);
	this->fetch=fetch;
	this->fetcher=fetcher;
	this->messagehandler=messagehandler;
}

IisAsyncWorker::~IisAsyncWorker()
{
	delete parser; 
}

void IisAsyncWorker::OnData(const char *data,int length,WinHTTPContentType type)
{
	parser->ParseChunk(StringPiece(data,length));
	/*for (int i=0;i<length;i++)
	
		myData.push_back(*data++);*/
	//
	//this->fetch->Write(StringPiece(data,length),messagehandler);
}
void IisAsyncWorker::OnCompleted(WinHTTPStatus status) 
{
	/*
	if (status==Ok)
	{ 
		std::string html(myData.begin(), myData.end());
		parser->ParseChunk(StringPiece(html.c_str(), html.size()));
	}*/
	fetch->Done(status==Ok);
	fetcher->StopFetch(this);	
}
void IisAsyncWorker::Cancel()
{
	client.SetEventHandler(NULL);
	client.Cancel();
	client.Wait();
	//fetch->Done(false);
}


IisAsyncUrlFetcher::IisAsyncUrlFetcher():UrlAsyncFetcher(),fetcher_supports_https_(true),allow_self_signed_(false)
{
	InitializeCriticalSection(&cs);
}

IisAsyncUrlFetcher::~IisAsyncUrlFetcher()
{
	ShutDown();
	DeleteCriticalSection(&cs);
}

// Resolve the host header used for an outbound WinHTTP request.
//
// Preference order: an explicit Host header on the AsyncFetch (e.g. forwarded
// from the client request), otherwise derived from the URL.
//
// CAUTION: previous code wrote
//   host = gurl.HostAndPort().as_string().c_str();
// which returned a pointer into a temporary GoogleString that died at the end
// of the statement, leaving `host` dangling. Reading through it (via `%s` in
// Message() and the std::string ctor in worker->GetUrl()) triggered
// AppVerifier's Heaps provider FIRST_CHANCE_ACCESS_VIOLATION (Sig[8]=0x13)
// on every internal image-rewrite fetch — the page-heap guard page on the
// released allocation raised STATUS_ACCESS_VIOLATION, surfacing as
// `vrfcore.dll 0x80000003` Application Error events on the Windows AppVerif
// runners and intermittent w3wp recycles that broke `test_config_isolation`
// and 8 background image-rewriting tests.
//
// `explicit_host_header` is the const char* returned by
// RequestHeaders::Lookup1(kHost) (NULL when absent). Tested independently
// from Fetch() in iis_async_url_fetcher_test.cc.
GoogleString IisAsyncUrlFetcher::DeriveHost(const char* explicit_host_header,
                                            const GoogleString& url) {
	if (explicit_host_header != nullptr) {
		return GoogleString(explicit_host_header);
	}
	GoogleUrl gurl(url);
	return gurl.HostAndPort().as_string();
}

void IisAsyncUrlFetcher::Fetch(const GoogleString& url,
                     MessageHandler* message_handler,
                     AsyncFetch* fetch)
{
	EnterCriticalSection(&cs);
	this->shutting_down_ = false;
	IisAsyncWorker *worker = new IisAsyncWorker(this, fetch, message_handler);
	asyncworkers.push_back(worker);

	GoogleString host_storage = DeriveHost(
	    fetch->request_headers()->Lookup1(HttpAttributes::kHost), url);
	const char* host = host_storage.c_str();
	message_handler->Message(kInfo, "Native Fetching of [%s] (host:[%s])", url.c_str(), host);
	worker->GetUrl(url, host);
	LeaveCriticalSection(&cs);
}
void IisAsyncUrlFetcher::StopFetch(IisAsyncWorker *worker)
{
	EnterCriticalSection(&cs);
	if (this->shutting_down_) {
		LeaveCriticalSection(&cs);
		return;
	}
	asyncworkers.remove(worker);
	delete worker;
	LeaveCriticalSection(&cs);
}

void IisAsyncUrlFetcher::ShutDown()
{
	VLOG(1) << "Shutting down IisAsyncUrlFetcher instance";

	EnterCriticalSection(&cs);
	if (this->shutting_down_) {
		LeaveCriticalSection(&cs);
		return;
	}

	this->shutting_down_ = true;
	while (asyncworkers.size())
	{
		
		IisAsyncWorker *worker=asyncworkers.front();
		asyncworkers.pop_front();
		worker->Cancel();
		delete worker;
	}
	LeaveCriticalSection(&cs);
}

int IisAsyncUrlFetcher::Poll(int64 max_wait_ms)
{
	int ret;
	EnterCriticalSection(&cs);
	ret=asyncworkers.size();
	LeaveCriticalSection(&cs);
	return ret;
}

} // namespace net_instaweb
