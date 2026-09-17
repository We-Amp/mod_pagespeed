// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef IISMODULEREQUESTCONTEXT_H
#define IISMODULEREQUESTCONTEXT_H
#include <httpserv.h>
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/http/google_url.h"
#include "pagespeed/automatic/proxy_fetch.h"
#include "pagespeed/kernel/thread/win_mutex.h"
#include "pagespeed/iis/iis_module_base_fetch.h"
#include "pagespeed/iis/iis_process_context.h"
#include "pagespeed/iis/dechunker.h"

namespace net_instaweb
{

	class IisInnerRequestContext;
	class GoogleUrl;
	class IisProcessContext;
	class IisServerContext;
	class GzipInflater;
	class IisModuleRequestContext: public IHttpStoredContext
	{
	public:
		IisModuleRequestContext(IHttpContext *pContext,bool is_resource, const char * url, size_t url_len, GoogleUrl* gurl
						, IisProcessContext* process_context, IisServerContext* server_context, int local_port, GoogleString local_ip_address);
		IisModuleRequestContext();
		virtual ~IisModuleRequestContext();
		virtual void CleanupStoredContext();

		void set_base_fetch(IisModuleBaseFetch *base_fetcher);
		IisModuleBaseFetch* base_fetch();

		IisInnerRequestContext *GetInnerContext();

	// specific IIS module functions
		bool HideAcceptEncoding() {return hideacceptencoding_;};
		void SetHideAcceptEncoding(bool value) {hideacceptencoding_=value;}

		void StripAndStoreAcceptEncoding(IHttpContext *c,bool stripforever=false);
		void AddStoredAcceptEncoding(IHttpContext *c);
		GzipInflater* inflater() { return inflater_; }
		void set_inflater(GzipInflater* x) { inflater_ = x; }
		void Destroy() {this->CleanupStoredContext();delete this;}
		bool empty() { return empty_; }
		void set_nextsendresponsestatus(REQUEST_NOTIFICATION_STATUS status) { hasNextStatus = true; nextsendresponsestatus_ = status; }
		void reset_nextsendresponsestatus() { hasNextStatus = false; }
		bool has_nextstatus() { return hasNextStatus; }
		REQUEST_NOTIFICATION_STATUS nextsendresponsestatus() { return nextsendresponsestatus_; }
		static IisModuleRequestContext* GetRequestContext(IHttpContext* pHttpContext);
	protected:
		bool empty_;
		bool hasNextStatus;
		REQUEST_NOTIFICATION_STATUS nextsendresponsestatus_;
		IisInnerRequestContext *requestContext_;
		bool hideacceptencoding_;
		PCSTR pRawValueAcceptEncoding;
		USHORT pRawValueLengthAcceptEncoding;
		GzipInflater* inflater_;
	};

}
#endif