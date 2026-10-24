// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef IIS_HTTP_MODULE_H_
#define IIS_HTTP_MODULE_H_

#include <windows.h>
#include <sal.h>
#include <httpserv.h>
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/iis/iis_process_context.h"
#include "pagespeed/iis/iis_request_context.h"
#include "pagespeed/iis/iis_module_request_context.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/iis/iis_module_factory.h"
#include "pagespeed/iis/iis_unclaimed_ref.h"

namespace net_instaweb
{



class IisHttpModule  : public CHttpModule
{
public:
	IisHttpModule(IisModuleFactory *module_creator) 
	:
		module_creator_(module_creator)
	{
	}

	REQUEST_NOTIFICATION_STATUS
    OnCustomRequestNotification(
        IN IHttpContext * pHttpContext,
        IN ICustomNotificationProvider * pProvider
    ) override;


	REQUEST_NOTIFICATION_STATUS OnBeginRequest(IN IHttpContext * pHttpContext, IN IHttpEventProvider * pProvider) override;
	REQUEST_NOTIFICATION_STATUS OnBeginRequestPageSpeed(IN IHttpContext * pHttpContext, IN IHttpEventProvider * pProvider);

	
	REQUEST_NOTIFICATION_STATUS OnSendResponse(IN IHttpContext * pHttpContext, IN ISendResponseProvider * pProvider) override;
	



	
	
	bool IsLocalRequest(IN IHttpContext* pHttpContext);

	// Sets the local-only X-Pagespeed-Init-Status value for an unusable
	// optimizer service (DaemonInitStatusToken: every kUnavailable
	// verdict, not only an unreachable service) on the response, when
	// the request is local and the live daemon verdict is kUnavailable,
	// and disarms the kernel and user caches for that response so the
	// header can never reach a visitor through a cached copy.
	void SetDaemonInitStatusHeaderIfLocal(IN IHttpContext* pHttpContext,
	                                      IisInnerRequestContext* innerContext);

	void log(IHttpContext* c, const char * msg, ...);

	IisProcessContext* process_context(const GoogleString& site_app_id,std::string site_root, const GoogleString& app_url_base) { 
		if (!process_context__.get()) 
		{
			process_context__.Adopt(module_creator_->GetProcessContext(site_app_id,site_root,app_url_base));
		}
		return process_context__.get();
	}
   REQUEST_NOTIFICATION_STATUS
        OnAsyncCompletion(
        IN IHttpContext * pHttpContext,
        IN DWORD dwNotification,
        IN BOOL fPostNotification,
        IN IHttpEventProvider * pProvider,
        IN IHttpCompletionInfo * pCompletionInfo
        ) override;

private:
	// failed_pc (out): on NULL-return, populated with the IisProcessContext
	// whose initialization failed if one exists (e.g. cache-path unwritable),
	// or nullptr if no process context could even be constructed. The
	// local-only diagnostic page in OnBeginRequest uses this to render a
	// per-failure-mode error with the resolved cache path + worker identity
	// instead of the legacy generic "missing configuration" message.
	IisModuleRequestContext* CreateRequestContext(IHttpContext *pHttpContext,
	    std::string site_root, char *url, int url_len, GoogleUrl *gurl,
	    IisProcessContext** failed_pc);
	void WritePre(StringPiece str, Writer* writer, MessageHandler* handler);
	void WriteHandlerResponse(IN IHttpContext * pHttpContext, const StringPiece& output, ContentType content_type, const StringPiece& cache_control);
	void WriteHandlerResponse(IN IHttpContext * pHttpContext, const StringPiece& output, ContentType content_type);
	void WriteHandlerResponse(IN IHttpContext * pHttpContext, const StringPiece& output);

	IisModuleFactory *module_creator_;
	// The reference GetProcessContext adds for this request. A request
	// context that is created claims it; otherwise it is released when the
	// module goes away at the end of the request.
	UnclaimedRef<IisProcessContext> process_context__;
	IisHttpModule(const IisHttpModule&) = delete;
	IisHttpModule& operator=(const IisHttpModule&) = delete;
};


}
#endif
