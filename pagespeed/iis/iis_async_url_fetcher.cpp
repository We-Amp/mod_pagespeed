
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


IisAsyncUrlFetcher::IisAsyncUrlFetcher():UrlAsyncFetcher(),fetcher_supports_https_(true)
{
	InitializeCriticalSection(&cs);
}

IisAsyncUrlFetcher::~IisAsyncUrlFetcher()
{
	ShutDown();
	DeleteCriticalSection(&cs);
}

void IisAsyncUrlFetcher::Fetch(const GoogleString& url,
                     MessageHandler* message_handler,
                     AsyncFetch* fetch) 
{
	EnterCriticalSection(&cs);
	this->shutting_down_ = false;
	IisAsyncWorker *worker = new IisAsyncWorker(this, fetch, message_handler);
	asyncworkers.push_back(worker);

	const char* host = fetch->request_headers()->Lookup1(HttpAttributes::kHost);
	
	//if no explicit host header is passed in, we derive it from the url
	if (host == NULL) {
		GoogleUrl gurl(url);		
		host = gurl.HostAndPort().as_string().c_str();
	}
	message_handler->Message(kInfo, "Native Fetching of [%s] (host:[%s])", url.c_str(), host);
	worker->GetUrl(url, host);
	LeaveCriticalSection(&cs);
}
void IisAsyncUrlFetcher::StopFetch(IisAsyncWorker *worker)
{
	EnterCriticalSection(&cs);
	if (this->shutting_down_) {
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
