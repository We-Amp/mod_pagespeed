#ifndef ASYNCWINHTTP
#define ASYNCWINHTTP
#ifndef _WINSOCKAPI_
#define _WINSOCKAPI_
#endif
#include <Windows.h>
#include <winhttp.h>
#include <string>
#include <list>
#include <vector>
#include <iostream>

namespace ASyncWinHTTP
{

enum WinHTTPStatus {NotStarted,Ok,Error,Running,Cancelled,Timeout};
enum WinHTTPContentType {Content,Headers};


class WinHTTPEvents
{
public:
	virtual void OnData(const char *data,int length,WinHTTPContentType type) // the data
	{
		std::cout << "data " << type << length << std::endl;
	}
	
	virtual void OnCompleted(WinHTTPStatus status) 
	{
		std::cout << "completed" << std::endl;
	}
};

class WinHTTPVectorHandler:public WinHTTPEvents
{
	std::vector<char> myBuffer;
	WinHTTPStatus status;

public:
	WinHTTPVectorHandler():WinHTTPEvents() {status=WinHTTPStatus::NotStarted;}
	void Clear() {myBuffer.clear();status=WinHTTPStatus::NotStarted;}
	virtual void OnData(const char *data,int length,WinHTTPContentType type) // the data
	{
		if (type==Content)
		{
			for (int c=0;c<length;c++)
				myBuffer.push_back(*data++);
		}
	}
	virtual void OnCompleted(WinHTTPStatus status) // 
	{
		this->status=status;
	}
	WinHTTPStatus GetStatus() {return status;}
	std::vector<char> *GetBuffer() {return &myBuffer;}
};


class WinHTTP
{
	WinHTTPStatus status;
	HANDLE endEvent;
	std::wstring useragent;
	HINTERNET session;
	HINTERNET connectSession;
	HINTERNET requestHandle;
	std::list<std::wstring> headers;
	std::list<std::wstring> headervalues;
	DWORD firstError;
	DWORD dwResult;
	DWORD dwError;

	DWORD _bytes;
	DWORD starttime;
	int resolveTimeout,connectTimeout,sendTimeout,receiveTimeout,totalTimeout;
	char *tempbuf;
	DWORD tempbufSize;
	WinHTTPEvents *eventhandler;
	bool allow_self_signed_;
public:
	WinHTTP();
	void SetEventHandler(WinHTTPEvents *eventhandler) {this->eventhandler=eventhandler;}
	// Relax TLS cert validation for https requests (FetchHttps allow_self_signed).
	void SetAllowSelfSigned(bool v) { allow_self_signed_ = v; }
	void SetUserAgent(std::string useragent);
	void SetUserAgent(std::wstring useragent);

	void AddHeader(std::wstring header,std::wstring value);
	void AddHeader(std::string header,std::string value); 
	void SetResolveTimeout(int ms); 
	void SetConnectTimeout(int ms); 
	void SetSendTimeout(int ms); 
	void SetReceiveTimeout(int ms); 
	void SetTotalTimeout(int ms); 
	int GetTotalTimeout(); 
	int GetResolveTimeout(); 
	int GetConnectTimeout(); 
	int GetSendTimeout();
	int GetReceiveTimeout();

	bool GetUrl(std::string url, std::string host);
	void Wait();		
	void CleanUp();
	virtual void Cancel() {status=WinHTTPStatus::Cancelled;CleanUp();}
	~WinHTTP();

	static void WinHttpCallback(
	_In_  HINTERNET hInternet,
  _In_  DWORD_PTR dwContext,
  _In_  DWORD dwInternetStatus,
  _In_  LPVOID lpvStatusInformation,
  _In_  DWORD dwStatusInformationLength);

	
	
protected:
	void SetError(DWORD result,DWORD error);
	void EventHandlerCompleted();
	virtual bool TotalTimeValid();
	/*Events */
	virtual void OnHandleClosing(HINTERNET handle);
	virtual void OnHeadersAvailable();	
	virtual void OnIntermediateResponse();
	virtual void OnReceivingResponse();
	virtual void OnRedirect(std::wstring url);
	virtual void OnRequestError(WINHTTP_ASYNC_RESULT *result);
	virtual void OnResponseReceived();
	virtual void OnSecureFailure();
	virtual void OnSendRequestComplete();
	virtual void OnWriteComplete(DWORD length);
	virtual void OnGetProxyForUrlComplete();
	virtual void OnCloseComplete() ;
	virtual void OnShutdownComplete();
	virtual void OnDataAvailable(DWORD bytes);
	virtual void OnHandleCreated(HINTERNET handle);

	virtual void OnResolvingName(std::wstring name);
	virtual void OnNameResolved(std::wstring name);
	virtual void OnConnectingToServer(std::wstring ip);
	virtual void OnConnectedToServer(std::wstring ip);
	virtual void OnSendingRequest(); 
	virtual void OnConnectionClosed(); 
	virtual void OnRequestSent(DWORD bytes);
	virtual void OnReadComplete(char *buffer,DWORD length) ;

};
}
#endif