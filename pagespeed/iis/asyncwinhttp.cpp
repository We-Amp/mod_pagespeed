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
	resolveTimeout=32000;
	connectTimeout=30000;
	sendTimeout=35000;
	receiveTimeout=30000;
	totalTimeout=75000;
	eventhandler=NULL;
	status=WinHTTPStatus::NotStarted;
	allow_self_signed_=false;
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
		SetEvent(endEvent);
		if(eventhandler)
			eventhandler->OnCompleted(status);
	}
}

		
void WinHTTP::OnHeadersAvailable() 
{
	if (!TotalTimeValid()) return;
		
	DWORD dwSize=0;
	if (!WinHttpQueryHeaders( requestHandle, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                            WINHTTP_HEADER_NAME_BY_INDEX, NULL,
                            &dwSize, WINHTTP_NO_HEADER_INDEX))
	{
		DWORD err=GetLastError();
		if (err!=ERROR_INSUFFICIENT_BUFFER)
		{
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
	 
	WinHttpQueryDataAvailable(requestHandle,&_bytes);  
	
}	
void WinHTTP::OnIntermediateResponse() {}
void WinHTTP::OnReadComplete(char *buffer,DWORD length) 
{
	if (!TotalTimeValid()) return;
	//std::cout << "read complete " << length << std::endl;
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
	//std::cout<<"send request complete" << std::endl;
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
	//std::cout << "data available" << bytes << std::endl;
	char *temp=new char [bytes];
	DWORD bytesRead;
	WinHttpReadData(requestHandle,(LPVOID) temp,bytes,&bytesRead);
	if (eventhandler) eventhandler->OnData(temp,bytesRead,Content);
	delete [] temp;
	WinHttpQueryDataAvailable(requestHandle,&_bytes);
		
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
	if (!TotalTimeValid()) return;
	//std::cout << "request sent"<< bytes << std::endl;
	if (!WinHttpReceiveResponse(requestHandle,NULL))
	{
		dwResult=1000;
		status=WinHTTPStatus::Error;
		CleanUp();

	}	
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

bool WinHTTP::GetUrl(std::string url, std::string host)
{
	status=WinHTTPStatus::Running;
	starttime=GetTickCount();
	URL_COMPONENTS urlComp;
	ZeroMemory(&urlComp, sizeof(urlComp));
	urlComp.dwStructSize = sizeof(urlComp);
	urlComp.dwSchemeLength    = (DWORD)-1;
	urlComp.dwHostNameLength  = (DWORD)-1;
	urlComp.dwUrlPathLength   = (DWORD)-1;
	urlComp.dwExtraInfoLength = (DWORD)-1;
	
	std::wstring wUrl=s2ws(url);
	
	if (!WinHttpCrackUrl(wUrl.c_str(),wcslen(wUrl.c_str()),0,&urlComp))
	{
		
		return false;
	}
	std::wstring server=std::wstring(urlComp.lpszHostName,urlComp.dwHostNameLength);
	int port=urlComp.nPort;
	std::wstring urlpath=std::wstring(urlComp.lpszUrlPath,urlComp.dwUrlPathLength)
		+
		std::wstring(urlComp.lpszExtraInfo,urlComp.dwExtraInfoLength)
		;

	
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


		connectSession=WinHttpConnect(session,server.c_str(),port,0);
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
			if (host != "") {
				std::wstring w_host = s2ws(host);
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
					CleanUp();
					return false;
				}
				return true;
				
			}
			else
			{
				firstError=GetLastError();
				CleanUp();
				return false;
			}
		}
		else
		{
			firstError=GetLastError();
			CleanUp();
			return false;
		}
	}
	else
		return false;

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
	
}





}