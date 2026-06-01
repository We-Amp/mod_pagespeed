#ifndef IIS_MISC_H_
#define IIS_MISC_H_

#include <string>
#ifndef _WINSOCKAPI_
#define _WINSOCKAPI_
#endif
#include <Windows.h>

#include <sal.h>
#include <httpserv.h>

#include "pagespeed/kernel/base/string_util.h"     
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/http/google_url.h"
#include "pagespeed/kernel/base/string_util.h"


#include "pagespeed/iis/iis_request_context.h"

namespace net_instaweb 
{
	std::wstring s2ws(const std::string& s);
	std::string ws2s(const std::wstring& s);

	bool determine_request_options(
		//IHttpContext * r,
		IisInnerRequestContext* ctx,
		const RewriteOptions* domain_options, /* may be null */
		net_instaweb::RequestContextPtr request_context,
		net_instaweb::RequestHeaders* request_headers,
		net_instaweb::ResponseHeaders* response_headers,
		net_instaweb::ServerContext* server_context,
		net_instaweb::GoogleUrl* url,
		GoogleString* pagespeed_query_params,
		GoogleString* pagespeed_option_cookie);

	 bool determine_options(IHttpContext * r,
						 IisInnerRequestContext* ctx,
						 net_instaweb::RequestHeaders* request_headers,
						 net_instaweb::ResponseHeaders* response_headers,
						 net_instaweb::RewriteOptions** options,
						 net_instaweb::RequestContextPtr request_context,
						 net_instaweb::GoogleUrl* url,
						 GoogleString* pagespeed_query_params,
						 GoogleString* pagespeed_option_cookies);
	RewriteOptions* ps_determine_remote_options(ServerContext* server_context, const GoogleString& in_ip, const GoogleUrl& url);
	bool set_experiment_state_and_cookie(IHttpContext * pHttpContext, net_instaweb::IisInnerRequestContext* ctx,
		net_instaweb::RewriteOptions* options,
		const StringPiece& host);

	 GoogleString size_t_to_str(size_t i);
	 
	 HRESULT WriteResponseMessage(IHttpContext * pHttpContext, PVOID buf, size_t len, bool is_last);
}

#endif