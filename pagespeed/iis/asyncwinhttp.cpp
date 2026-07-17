#include "pagespeed/iis/asyncwinhttp.h"
#include "pagespeed/iis/util.h"

namespace ASyncWinHTTP
{



WinHTTP::WinHTTP()
{
	// TODO(oschaaf): mod_pagespeed is actually defined in 
	// "net/instaweb/public/global_constants.h"
	// This useragent is used in iis_http_module.cpp to check for 
	// subrequests from iispeed, so it can bail out instead of endlessly recurse 
	// in that case.
	// TODO(oschaaf): append version and lastchang string as well
	useragent=L"WinHttp mod_pagespeed ";
	endEvent=CreateEventA(NULL,TRUE,FALSE,NULL);
	session=NULL;
	requestHandle=NULL;
	connectSession=NULL;
	firstError=0;
	tempbuf=NULL;
	tempbufSize=0;
	resolveTimeout=32000;
	connectTimeout=30000;
	sendTimeout=35000;
	receiveTimeout=30000;
	totalTimeout=75000;
	eventhandler=NULL;
	status=WinHTTPStatus::NotStarted;
	allow_self_signed_=false;
	loopback_pinned_=false;
	retry_scheduled_=false;
	retry_attempted_=false;
	response_data_seen_=false;
}
void WinHTTP::SetUserAgent(std::string useragent)
{
	SetUserAgent(s2ws(useragent));
}
void WinHTTP::SetUserAgent(std::wstring useragent)
{
	this->useragent=useragent;
}
void WinHTTP::AddHeader(std::wstring header,std::wstring value) {headers.push_back(header);headervalues.push_back(value);}
void WinHTTP::AddHeader(std::string header,std::string value) {AddHeader(s2ws(header),s2ws(value));}
void WinHTTP::SetResolveTimeout(int ms) {resolveTimeout=ms;}
void WinHTTP::SetConnectTimeout(int ms) {connectTimeout=ms;}
void WinHTTP::SetSendTimeout(int ms) {sendTimeout=ms;}
void WinHTTP::SetReceiveTimeout(int ms) {receiveTimeout=ms;}
void WinHTTP::SetTotalTimeout(int ms) {totalTimeout=ms;}
int WinHTTP::GetTotalTimeout() {return totalTimeout;}
int WinHTTP::GetResolveTimeout() {return resolveTimeout;}
int WinHTTP::GetConnectTimeout() {return connectTimeout;}
int WinHTTP::GetSendTimeout() {return sendTimeout;}
int WinHTTP::GetReceiveTimeout() {return receiveTimeout;}

void WinHTTP::SetError(DWORD result,DWORD error) {dwResult=result;dwError=error;status=WinHTTPStatus::Error;}
void WinHTTP::EventHandlerCompleted() {if (eventhandler) eventhandler->OnCompleted(status);}
bool WinHTTP::TotalTimeValid()
{
	if ((GetTickCount()-starttime)>totalTimeout)

	{
		//std::cout << "total timeout" << std::endl;
		diag_+="|total_timeout elapsed_ms="+std::to_string((unsigned long)(GetTickCount()-starttime));
		status=WinHTTPStatus::Timeout;
		CleanUp();
		return false;
	}
	return true;
}
/*Events */
void WinHTTP::OnHandleClosing(HINTERNET handle) {
	//std::cout<<"onhandleclosing" << handle << std::endl;
	if (handle==requestHandle) 
	{
		//std::cout << "closing connectsession" << std::endl;
		requestHandle=NULL;
		WinHttpCloseHandle(connectSession);
	}
	if (handle==connectSession) 
	{
		//std::cout << "closing session" << std::endl;
		connectSession=NULL;
		WinHttpCloseHandle(session);
	}
	if (handle==session) 
	{
		//std::cout << "setting event" << std::endl;
		session=NULL;
		if (retry_scheduled_ && status!=WinHTTPStatus::Cancelled)
		{
			// the pinned loopback attempt failed before any response
			// data and every handle is now gone -- restart the request once
			// against the other loopback family. endEvent stays unset until
			// that attempt completes, keeping Wait()/CleanUp semantics intact.
			retry_scheduled_=false;
			retry_attempted_=true;
			if (StartRequest())
				return;
			if (session)
				return; // retry failed mid-setup; its own close cascade finishes
			diag_+="|retry_start_fail";
			status=WinHTTPStatus::Error;
		}
		retry_scheduled_=false;
		SetEvent(endEvent);
		if(eventhandler)
			eventhandler->OnCompleted(status);
	}
}

		
void WinHTTP::OnHeadersAvailable()
{
	if (!TotalTimeValid()) return;
	// Headers arrived, so the pinned connect address worked; from here on a
	// loopback retry would re-deliver response data downstream.
	if (!response_data_seen_) diag_+="|hdrs";
	response_data_seen_=true;
		
	DWORD dwSize=0;
	if (!WinHttpQueryHeaders( requestHandle, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                            WINHTTP_HEADER_NAME_BY_INDEX, NULL,
                            &dwSize, WINHTTP_NO_HEADER_INDEX))
	{
		DWORD err=GetLastError();
		if (err!=ERROR_INSUFFICIENT_BUFFER)
		{
			diag_+="|qhdrs_size_fail="+std::to_string((unsigned long)err);
			SetError(1012,0);
			CleanUp();
			return;
			//EventHandlerCompleted();
		}
		
		
	}
	WCHAR * lpOutBuffer = new WCHAR[(dwSize+4)/sizeof(WCHAR)];

        // Now, use WinHttpQueryHeaders to retrieve the header.
        if (!WinHttpQueryHeaders( requestHandle,
                                    WINHTTP_QUERY_RAW_HEADERS_CRLF,
                                    WINHTTP_HEADER_NAME_BY_INDEX,
                                    lpOutBuffer, &dwSize,
                                    WINHTTP_NO_HEADER_INDEX))
		{
			diag_+="|qhdrs_read_fail="+std::to_string((unsigned long)GetLastError());
			delete [] lpOutBuffer;
			SetError(1002,0);
			CleanUp();
			//EventHandlerCompleted();

			return;
		}
	std::string headers=ws2s(lpOutBuffer) ;



	// Security: removed OutputDebugStringA that leaked response headers

	//std::cout << headers << std::endl ;
	if (eventhandler) 
		eventhandler->OnData(headers.c_str(),strlen(headers.c_str()),Headers);
	delete [] lpOutBuffer;

	// Async session: the available-byte count arrives via
	// WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE, so lpdwNumberOfBytesAvailable must be
	// NULL -- WinHTTP does not write it, and would race the callback if it did.
	if (!WinHttpQueryDataAvailable(requestHandle,NULL))
	{
		diag_+="|qda_hdrs_fail="+std::to_string((unsigned long)GetLastError());
		SetError(1014,GetLastError());
		CleanUp();
	}
}	
void WinHTTP::OnIntermediateResponse() {}
void WinHTTP::OnReadComplete(char *buffer,DWORD length) 
{
	if (!TotalTimeValid()) return;
	// This is the data path for an async handle: `buffer` is the buffer handed to
	// WinHttpReadData and `length` is how much of it WinHTTP actually filled.
	if (length==0)
	{
		// End of the response body.
		status=WinHTTPStatus::Ok;
		CleanUp();
		return;
	}
	if (eventhandler) eventhandler->OnData(buffer,length,Content);
	if (!WinHttpQueryDataAvailable(requestHandle,NULL))
	{
		diag_+="|qda_read_fail="+std::to_string((unsigned long)GetLastError());
		SetError(1014,GetLastError());
		CleanUp();
	}
}
void WinHTTP::OnReceivingResponse() {
	if (!TotalTimeValid()) return;
	std::cout<<"Receiving response"<<std::endl;
}
void WinHTTP::OnRedirect(std::wstring url) 
{
	if (!TotalTimeValid()) return;
	std::cout<<"redirect"<<ws2s(url)<<std::endl;
}
void WinHTTP::OnRequestError(WINHTTP_ASYNC_RESULT *result)
{
	status=WinHTTPStatus::Error;
	dwError=result->dwError;
	dwResult=result->dwResult;
	// a pinned loopback attempt that failed before any response
	// data (typically ERROR_WINHTTP_CANNOT_CONNECT when only the other
	// loopback family is listening) is retried once; OnHandleClosing
	// restarts it after the close cascade below releases the handles.
	if (LoopbackRetryEligible())
		retry_scheduled_=true;
	diag_+="|async_err api="+std::to_string((unsigned long)result->dwResult)
		+" err="+std::to_string((unsigned long)result->dwError)
		+(retry_scheduled_?"+retry_sched":"");
	CleanUp();
}
void WinHTTP::OnResponseReceived() 
{
	if (!TotalTimeValid()) return;
	//std::cout<<"response received"<<std::endl;
}
void WinHTTP::OnSecureFailure() 
{
	if (!TotalTimeValid()) return;
}
void WinHTTP::OnSendRequestComplete()
{
	if (!TotalTimeValid()) return;
	// WinHttpReceiveResponse belongs HERE, after the async
	// WinHttpSendRequest completes (SENDREQUEST_COMPLETE). It used to be
	// driven from OnRequestSent (REQUEST_SENT, an informational
	// notification that fires while the send operation is still active
	// inside WinHTTP), overlapping the receive pipeline with the live send
	// op -- a race that Cuzz collapses into a spurious request abort:
	// REQUEST_ERROR api=API_SEND_REQUEST err=12017 OPERATION_CANCELLED
	// after headers already arrived, with no handle closed by this class
	// (every CleanUp path was instrumented and silent).
	if (!WinHttpReceiveResponse(requestHandle,NULL))
	{
		diag_+="|recvresp_fail="+std::to_string((unsigned long)GetLastError());
		dwResult=1000;
		status=WinHTTPStatus::Error;
		CleanUp();
	}
}
void WinHTTP::OnWriteComplete(DWORD length) 
{
	if (!TotalTimeValid()) return;
	//std::cout << "write complete"<<std::endl;
}
void WinHTTP::OnGetProxyForUrlComplete() {}
void WinHTTP::OnCloseComplete() 
{
	if (!TotalTimeValid()) return;
	//std::cout << "close complete" <<std::endl; 
}
void WinHTTP::OnShutdownComplete() 
{
	//std::cout << "shutdown"<<std::endl;
}
void WinHTTP::OnDataAvailable(DWORD bytes) 
{
	if (bytes==0)
	{
		status=WinHTTPStatus::Ok;
		CleanUp();
		//std::cout << "async after close" << std::endl;
		return;
	}
	if (!TotalTimeValid()) return;
	// WINHTTP_FLAG_ASYNC: WinHttpReadData completes asynchronously. The buffer must
	// outlive this call and stay untouched until WINHTTP_CALLBACK_STATUS_READ_COMPLETE
	// (-> OnReadComplete) reports the real byte count; lpdwNumberOfBytesRead is never
	// filled in and must be NULL. Reading it, and freeing the buffer here, handed PSOL
	// an empty (or garbage) response body whenever the read did not complete inline.
	if (tempbufSize < bytes)
	{
		delete [] tempbuf;
		tempbuf = new char[bytes];
		tempbufSize = bytes;
	}
	if (!WinHttpReadData(requestHandle,(LPVOID) tempbuf,bytes,NULL))
	{
		diag_+="|readdata_fail="+std::to_string((unsigned long)GetLastError());
		SetError(1013,GetLastError());
		CleanUp();
	}
		
}
void WinHTTP::OnHandleCreated(HINTERNET handle) 
{
	//std::cout << "handle created" << handle << std::endl;
}

void WinHTTP::OnResolvingName(std::wstring name) {/*std::cout << "resolving name " << ws2s(name) << std::endl;*/}
void WinHTTP::OnNameResolved(std::wstring name) {/*std::cout << "name resolved" << ws2s(name) << std::endl;*/}
void WinHTTP::OnConnectingToServer(std::wstring ip) {/*std::cout << "connecting to server " << ws2s(ip) << std::endl;*/}
void WinHTTP::OnConnectedToServer(std::wstring ip) {/*std::cout << "connected to server " << ws2s(ip) << std::endl;*/}
void WinHTTP::OnSendingRequest() {/*std::cout << "sending request " << std::endl;*/}
void WinHTTP::OnConnectionClosed() {/*std::cout << "connection closed" << std::endl;*/}
	

void WinHTTP::OnRequestSent(DWORD bytes) {
	// Informational only (bytes reached the wire). WinHttpReceiveResponse
	// moved to OnSendRequestComplete -- see the note there.
	if (!TotalTimeValid()) return;
}


void CALLBACK WinHttpCallback(_In_  HINTERNET hInternet,
  _In_  DWORD_PTR dwContext,
  _In_  DWORD dwInternetStatus,
  _In_  LPVOID lpvStatusInformation,
  _In_  DWORD dwStatusInformationLength)
{
	WinHTTP::WinHttpCallback(hInternet,dwContext,dwInternetStatus,lpvStatusInformation,dwStatusInformationLength);
}


void WinHTTP::WinHttpCallback(
	_In_  HINTERNET hInternet,
  _In_  DWORD_PTR dwContext,
  _In_  DWORD dwInternetStatus,
  _In_  LPVOID lpvStatusInformation,
  _In_  DWORD dwStatusInformationLength)
{
	WinHTTP *cb=(WinHTTP *) dwContext;
	switch(dwInternetStatus)
	{
	case WINHTTP_CALLBACK_STATUS_CONNECTION_CLOSED:
		cb->OnConnectionClosed();
		break;
	case WINHTTP_CALLBACK_STATUS_RESPONSE_RECEIVED:
		cb->OnResponseReceived();
		break;
	case WINHTTP_CALLBACK_STATUS_RECEIVING_RESPONSE:
		cb->OnReceivingResponse();
		return;
	case WINHTTP_CALLBACK_STATUS_RESOLVING_NAME:
		cb->OnResolvingName((LPCWSTR) lpvStatusInformation);
		return;
	case WINHTTP_CALLBACK_STATUS_NAME_RESOLVED:
		cb->OnNameResolved((LPCWSTR) lpvStatusInformation);
		return;
	case WINHTTP_CALLBACK_STATUS_CONNECTING_TO_SERVER:
		cb->OnConnectingToServer((LPCWSTR) lpvStatusInformation);
		return;
	case WINHTTP_CALLBACK_STATUS_CONNECTED_TO_SERVER:
		cb->OnConnectedToServer((LPCWSTR) lpvStatusInformation);
		return;
	case WINHTTP_CALLBACK_STATUS_SENDING_REQUEST:
		cb->OnSendingRequest();
		return;
	case WINHTTP_CALLBACK_STATUS_REQUEST_SENT:
		cb->OnRequestSent(*((DWORD*)lpvStatusInformation));
		return;
	case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE:
		{
			DWORD bytes=*(DWORD*) lpvStatusInformation;
			cb->OnDataAvailable(bytes);
			return;
		}
	case WINHTTP_CALLBACK_STATUS_HANDLE_CREATED:
		{
			HINTERNET handle=*(HINTERNET *) lpvStatusInformation;
			cb->OnHandleCreated(handle);
			return;
		}
	case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
		{
			HINTERNET handle=*(HINTERNET *) lpvStatusInformation;
			cb->OnHandleClosing(handle);
			return;
		}
	case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
		{
			cb->OnHeadersAvailable();
			return;
		}
	case WINHTTP_CALLBACK_STATUS_INTERMEDIATE_RESPONSE:
		{
			cb->OnIntermediateResponse();
			return;
		}
	case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
		{
			char *buffer=(char*)lpvStatusInformation;		
			cb->OnReadComplete(buffer,dwStatusInformationLength);
			return;
		}
	case WINHTTP_CALLBACK_STATUS_REDIRECT:
		{
			cb->OnRedirect((LPWSTR) lpvStatusInformation);
			return;
		}
	case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
		cb->OnRequestError((WINHTTP_ASYNC_RESULT *) lpvStatusInformation);
		return;
	case WINHTTP_CALLBACK_STATUS_SECURE_FAILURE:
		cb->OnSecureFailure();
		return;
	case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
		cb->OnSendRequestComplete();
		return;
	case WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE:
		cb->OnWriteComplete(*((DWORD *) lpvStatusInformation));
		return;
	case WINHTTP_CALLBACK_STATUS_CLOSING_CONNECTION:

		return;
	default:
		// should not happen
		return;
	}
}

std::wstring WinHTTP::LoopbackConnectHost(const std::wstring &host,bool retry_attempt)
{
	// The IPv6 loopback is always returned in BRACKETED form: WinHttpConnect
	// rejects an unbracketed "::1" synchronously with
	// ERROR_WINHTTP_INVALID_URL (12005), and a synchronous connect failure
	// tears the session down before the retry plumbing can engage, so every
	// [::1]-target fetch would fail outright (WinHttpCrackUrl itself keeps
	// the brackets in the host component it returns).
	if (_wcsicmp(host.c_str(),L"localhost")==0)
		return retry_attempt ? L"[::1]" : L"127.0.0.1";
	if (host==L"::1" || host==L"[::1]")
		return retry_attempt ? L"127.0.0.1" : L"[::1]";
	return L"";
}

bool WinHTTP::LoopbackRetryEligible()
{
	return loopback_pinned_ && !retry_attempted_ && !response_data_seen_;
}

bool WinHTTP::GetUrl(std::string url, std::string host)
{
	url_=url;
	host_=host;
	retry_scheduled_=false;
	retry_attempted_=false;
	return StartRequest();
}

bool WinHTTP::StartRequest()
{
	status=WinHTTPStatus::Running;
	starttime=GetTickCount();
	response_data_seen_=false;
	URL_COMPONENTS urlComp;
	ZeroMemory(&urlComp, sizeof(urlComp));
	urlComp.dwStructSize = sizeof(urlComp);
	urlComp.dwSchemeLength    = (DWORD)-1;
	urlComp.dwHostNameLength  = (DWORD)-1;
	urlComp.dwUrlPathLength   = (DWORD)-1;
	urlComp.dwExtraInfoLength = (DWORD)-1;
	
	std::wstring wUrl=s2ws(url_);
	
	if (!WinHttpCrackUrl(wUrl.c_str(),wcslen(wUrl.c_str()),0,&urlComp))
	{
		diag_+="|crackurl_fail="+std::to_string((unsigned long)GetLastError());
		return false;
	}
	std::wstring server=std::wstring(urlComp.lpszHostName,urlComp.dwHostNameLength);
	int port=urlComp.nPort;
	std::wstring urlpath=std::wstring(urlComp.lpszUrlPath,urlComp.dwUrlPathLength)
		+
		std::wstring(urlComp.lpszExtraInfo,urlComp.dwExtraInfoLength)
		;

	// WinHTTP hands "localhost" to the OS resolver, which can prefer
	// the IPv6 loopback while the origin site only answers on IPv4; the async
	// connect then fails and PSOL remembers the fetch failure for minutes, so
	// rewrites of local sub-resources never converge. Pin the connect address
	// for loopback hosts and retry the other loopback family once on failure
	// (OnRequestError / OnHandleClosing). Only the address handed to
	// WinHttpConnect changes; the Host request header keeps naming the
	// original authority.
	std::wstring connectHost=LoopbackConnectHost(server,retry_attempted_);
	loopback_pinned_=(connectHost!=L"");
	if (!loopback_pinned_)
		connectHost=server;
	diag_+=(retry_attempted_?"|retry(":"|first(")+ws2s(connectHost)+")";
	std::string hostHeader=host_;
	if (loopback_pinned_ && hostHeader=="" && connectHost!=server)
	{
		// Without an explicit Host header WinHTTP derives Host from the
		// WinHttpConnect server name -- now the pinned address. Preserve the
		// URL's own authority instead.
		std::string authority=ws2s(server);
		// WinHttpCrackUrl keeps the brackets of an IPv6 literal, so only wrap
		// a bare colon-hex address (never double-bracket).
		if (authority.find(':')!=std::string::npos && authority[0]!='[')
			authority="["+authority+"]";
		int defaultPort=(urlComp.nScheme==INTERNET_SCHEME_HTTPS)?INTERNET_DEFAULT_HTTPS_PORT:INTERNET_DEFAULT_HTTP_PORT;
		if (port!=defaultPort)
			authority+=":"+std::to_string(port);
		hostHeader=authority;
	}

	
	WinHTTP *thisptr=this;
	session=WinHttpOpen(useragent.c_str(),WINHTTP_ACCESS_TYPE_NO_PROXY,NULL,NULL,WINHTTP_FLAG_ASYNC);
	WinHttpSetTimeouts(session,resolveTimeout,connectTimeout,sendTimeout,receiveTimeout);
	if (session)
	{
		DWORD setting=WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
		WinHttpSetOption(session,WINHTTP_OPTION_CONTEXT_VALUE,&thisptr,sizeof(thisptr));
		WinHttpSetOption(session,WINHTTP_OPTION_REDIRECT_POLICY,&setting,sizeof(setting));
		
		WINHTTP_STATUS_CALLBACK callback=WinHttpSetStatusCallback(session, (WINHTTP_STATUS_CALLBACK) ASyncWinHTTP::WinHttpCallback,WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS|WINHTTP_CALLBACK_FLAG_ALL_NOTIFICATIONS,NULL);

		// Security: removed OutputDebugStringW that leaked connection details


		connectSession=WinHttpConnect(session,connectHost.c_str(),port,0);
		if (connectSession)
		{
			// Use TLS for https URLs. WinHttpCrackUrl populated urlComp.nScheme;
			// without WINHTTP_FLAG_SECURE the request goes out as cleartext on
			// the TLS port and the fetch fails -- so the module could not fetch
			// any https-origin sub-resource (loopback or remote).
			DWORD requestFlags=WINHTTP_FLAG_REFRESH;
			if (urlComp.nScheme==INTERNET_SCHEME_HTTPS)
			{
				requestFlags|=WINHTTP_FLAG_SECURE;
			}
			requestHandle=WinHttpOpenRequest(connectSession,L"GET",urlpath.c_str(),NULL,NULL,NULL,requestFlags);
			if (hostHeader != "") {
				std::wstring w_host = s2ws(hostHeader);
				w_host = L"Host: " + w_host;
				WinHttpAddRequestHeaders(requestHandle, w_host.c_str(), (ULONG)-1L, WINHTTP_ADDREQ_FLAG_REPLACE | WINHTTP_ADDREQ_FLAG_ADD);

			}
			if (requestHandle)
			{
				// When configured (FetchHttps allow_self_signed), relax TLS
				// validation for https requests -- mirrors the curl-based system
				// fetcher's allow_self_signed on nginx/Apache. Off by default, so
				// production https origins are validated normally.
				if (allow_self_signed_ && (requestFlags & WINHTTP_FLAG_SECURE))
				{
					DWORD secFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
						SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
						SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
						SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
					WinHttpSetOption(requestHandle, WINHTTP_OPTION_SECURITY_FLAGS, &secFlags, sizeof(secFlags));
				}
				std::list<std::wstring>::iterator itheaders=headers.begin();
				std::list<std::wstring>::iterator itvalues=headervalues.begin();
				while(itheaders!=headers.end())
				{
					std::wstring header=(*itheaders+L": ")+*itvalues;
					bool ok = WinHttpAddRequestHeaders(requestHandle, header.c_str(), (ULONG)-1L, WINHTTP_ADDREQ_FLAG_REPLACE | WINHTTP_ADDREQ_FLAG_ADD);
					itheaders++;
					itvalues++;
				}
				bool result=WinHttpSendRequest(requestHandle,WINHTTP_NO_ADDITIONAL_HEADERS,0,NULL,NULL,0,(DWORD_PTR) this);
				if (!result)
				{
					firstError=GetLastError();
					// schedule the loopback fallback before CleanUp --
					// the close cascade may run inline and consult the flag.
					bool retrying=LoopbackRetryEligible();
					if (retrying)
						retry_scheduled_=true;
					diag_+="|send_fail="+std::to_string((unsigned long)firstError)
						+(retrying?"+retry_sched":"");
					CleanUp();
					return retrying;
				}
				return true;

			}
			else
			{
				firstError=GetLastError();
				diag_+="|openreq_fail="+std::to_string((unsigned long)firstError);
				CleanUp();
				return false;
			}
		}
		else
		{
			firstError=GetLastError();
			diag_+="|connect_fail="+std::to_string((unsigned long)firstError);
			CleanUp();
			return false;
		}
	}
	else
	{
		diag_+="|open_fail="+std::to_string((unsigned long)GetLastError());
		return false;
	}

}
void WinHTTP::CleanUp()
{
	bool ret=true;
	if (requestHandle)
		if(WinHttpCloseHandle(requestHandle)) return;
	
	if (connectSession)
	{
		if (WinHttpCloseHandle(connectSession)) return;
	}
	
	if (session)
	{
		if (WinHttpCloseHandle(session)) return;
	}

	WaitForSingleObject(endEvent,INFINITE);
}

void WinHTTP::Wait()
{
	WaitForSingleObject(endEvent,INFINITE);
}


WinHTTP::~WinHTTP()
{
	CleanUp();
	/*if (WinHttpCloseHandle(session))
	{*/
		WaitForSingleObject(endEvent,INFINITE);
	//}
	CloseHandle(endEvent);
	// Only safe after the endEvent wait: a cancelled in-flight WinHttpReadData may
	// still own this buffer.
	delete [] tempbuf;
	tempbuf=NULL;
	tempbufSize=0;
	
}





}