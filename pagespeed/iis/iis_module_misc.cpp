#define _WINSOCKAPI_
#include <Windows.h>
#include <string>
#include <httpserv.h>

#include "pagespeed/iis/util.h"




std::string ServerMapPath(IHttpContext *pContext,std::string frompath)
{
	
	WCHAR *to=new WCHAR[MAX_PATH+1];
	DWORD size=MAX_PATH;
	std::string result;	
	auto wFrompath=s2ws(frompath);
	if (!FAILED(pContext->MapPath(wFrompath.c_str() ,to,&size)))
	{
		to[size]=0;
		result=ws2s(to);
	}
	
	delete [] to;
	return result;
}

