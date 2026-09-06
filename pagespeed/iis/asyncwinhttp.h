// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

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
	// loopback pinning state (see LoopbackConnectHost / StartRequest):
	// a pinned first attempt that fails before any response data is restarted
	// once against the other loopback family from OnHandleClosing.
	std::string url_;			// as passed to GetUrl; re-cracked per attempt
	std::string host_;			// explicit Host header value ("" = derive)
	bool loopback_pinned_;		// current attempt connects to a pinned loopback address
	bool retry_scheduled_;		// failed pinned attempt: restart when the session closes
	bool retry_attempted_;		// the fallback attempt is running (or finished)
	bool response_data_seen_;	// response data reached this attempt's handler
	// diagnostics: per-attempt trail (connect family, failure point,
	// real WinHTTP error codes). Appended only on the request/callback path,
	// read by the owner after completion; surfaces in the failure warning so
	// "WinHTTP status=2" is never the only evidence again.
	std::string diag_;
public:
	const std::string &Diag() const {return diag_;}
	WinHTTP();
	void SetEventHandler(WinHTTPEvents *eventhandler) {this->eventhandler=eventhandler;}
	// Relax TLS cert validation for https requests (FetchHttps allow_self_signed).
	void SetAllowSelfSigned(bool v) { allow_self_signed_ = v; }
	// the connect address handed to WinHttpConnect for `host`.
	// "localhost" (case-insensitive) pins to 127.0.0.1 and retries the IPv6
	// loopback; an explicit ::1/[::1] target retries 127.0.0.1. The IPv6
	// loopback is always returned bracketed ("[::1]") because WinHttpConnect
	// rejects the bare form (ERROR_WINHTTP_INVALID_URL). Returns L"" when the
	// host gets no loopback pinning. Pure and static so the policy is
	// unit-testable without a WinHTTP session (iis_async_url_fetcher_test.cc).
	static std::wstring LoopbackConnectHost(const std::wstring &host,bool retry_attempt);
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
	virtual void Cancel() {diag_+="|ext_cancel";status=WinHTTPStatus::Cancelled;CleanUp();}
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
	// one connection attempt for url_/host_ (factored out of GetUrl
	// so OnHandleClosing can restart the request for the loopback fallback).
	bool StartRequest();
	// True while a failed pinned attempt may still retry the other family.
	bool LoopbackRetryEligible();
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