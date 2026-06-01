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

namespace net_instaweb
{



class IisHttpModule  : public CHttpModule
{
public:
	IisHttpModule(IisModuleFactory *module_creator) 
	:
		module_creator_(module_creator),
		process_context__(NULL)
	{
	}

	REQUEST_NOTIFICATION_STATUS
    OnCustomRequestNotification(
        IN IHttpContext * pHttpContext,
        IN ICustomNotificationProvider * pProvider
    );


	REQUEST_NOTIFICATION_STATUS OnBeginRequest(IN IHttpContext * pHttpContext, IN IHttpEventProvider * pProvider);
	REQUEST_NOTIFICATION_STATUS OnBeginRequestPageSpeed(IN IHttpContext * pHttpContext, IN IHttpEventProvider * pProvider);
	REQUEST_NOTIFICATION_STATUS OnPostBeginRequest(IN IHttpContext* pHttpContext,IN IHttpEventProvider* pProvider);

	
	REQUEST_NOTIFICATION_STATUS OnSendResponse(IN IHttpContext * pHttpContext, IN ISendResponseProvider * pProvider);
	

	REQUEST_NOTIFICATION_STATUS OnEndRequest(IN IHttpContext * pHttpContext, IN IHttpEventProvider  * pProvider) {
		log(pHttpContext,"OnEndRequest");
		HideAcceptEncodingIfNeeded(pHttpContext);
		return RQ_NOTIFICATION_CONTINUE;}
	REQUEST_NOTIFICATION_STATUS OnPostEndRequest(IN IHttpContext* pHttpContext, IN IHttpEventProvider* pProvider) {
		log(pHttpContext,"OnPostEndRequest");
		RestoreAcceptEncoding(pHttpContext);
		return RQ_NOTIFICATION_CONTINUE;
	}

	// RQ_AUTHENTICATE_REQUEST
	REQUEST_NOTIFICATION_STATUS OnAuthenticateRequest(IN IHttpContext* pHttpContext,IN IAuthenticationProvider* pProvider) {
		log(pHttpContext,"OnAuthenticateRequest");
		HideAcceptEncodingIfNeeded(pHttpContext);
		return RQ_NOTIFICATION_CONTINUE;}
	REQUEST_NOTIFICATION_STATUS OnPostAuthenticateRequest(IN IHttpContext* pHttpContext,IN IHttpEventProvider* pProvider) {
		log(pHttpContext,"OnPostAuthenticateRequest");
		RestoreAcceptEncoding(pHttpContext);
		return RQ_NOTIFICATION_CONTINUE;
	}
	
	//RQ_AUTHORIZE_REQUEST
	REQUEST_NOTIFICATION_STATUS OnAuthorizeRequest(IN IHttpContext* pHttpContext,IN IHttpEventProvider* pProvider) {
		log(pHttpContext,"OnAuthorizeRequest");
		HideAcceptEncodingIfNeeded(pHttpContext);
		return RQ_NOTIFICATION_CONTINUE;}
	REQUEST_NOTIFICATION_STATUS OnPostAuthorizeRequest(IN IHttpContext* pHttpContext,IN IHttpEventProvider* pProvider) {
		log(pHttpContext,"OnPostAuthorizeRequest");
		RestoreAcceptEncoding(pHttpContext);
		return RQ_NOTIFICATION_CONTINUE;}

	//RQ_RESOLVE_REQUEST_CACHE
	REQUEST_NOTIFICATION_STATUS OnResolveRequestCache(IN IHttpContext* pHttpContext,IN IHttpEventProvider* pProvider) 
	{
		log(pHttpContext,"OnResolveRequestCache");
		HideAcceptEncodingIfNeeded(pHttpContext);
		return RQ_NOTIFICATION_CONTINUE;
	}
	REQUEST_NOTIFICATION_STATUS OnPostResolveRequestCache(IN IHttpContext* pHttpContext,IN IHttpEventProvider* pProvider) {
		log(pHttpContext,"OnPostResolveRequestCache");
		//RestoreAcceptEncoding(pHttpContext);
		return RQ_NOTIFICATION_CONTINUE;
	}

	//RQ_MAP_REQUEST_HANDLER
	REQUEST_NOTIFICATION_STATUS OnMapRequestHandler(IN IHttpContext* pHttpContext,IN IMapHandlerProvider* pProvider) {
		log(pHttpContext,"OnMapRequestHandler");
		//HideAcceptEncodingIfNeeded(pHttpContext);
		return RQ_NOTIFICATION_CONTINUE;}
	REQUEST_NOTIFICATION_STATUS OnPostMapRequestHandler(IN IHttpContext* pHttpContext,IN IHttpEventProvider* pProvider) {
		log(pHttpContext,"OnPostMapRequestHandler");
		RestoreAcceptEncoding(pHttpContext);
		return RQ_NOTIFICATION_CONTINUE;}

	//RQ_ACQUIRE_REQUEST_STATE
	REQUEST_NOTIFICATION_STATUS OnAcquireRequestState(IN IHttpContext* pHttpContext,IN IHttpEventProvider* pProvider) {
		log(pHttpContext,"OnAcquireRequestState");
		HideAcceptEncodingIfNeeded(pHttpContext);
		return RQ_NOTIFICATION_CONTINUE;}
	REQUEST_NOTIFICATION_STATUS OnPostAcquireRequestState(IN IHttpContext* pHttpContext,IN IHttpEventProvider* pProvider) {
		log(pHttpContext,"OnPostAcquireRequestState");
		RestoreAcceptEncoding(pHttpContext);
		return RQ_NOTIFICATION_CONTINUE;}

	//RQ_PRE_EXECUTE_REQUEST_HANDLER
	REQUEST_NOTIFICATION_STATUS OnPreExecuteRequestHandler(IN IHttpContext* pHttpContext,IN IHttpEventProvider* pProvider) {
		log(pHttpContext,"OnPreExecuteRequestHandler");
		HideAcceptEncodingIfNeeded(pHttpContext);
		return RQ_NOTIFICATION_CONTINUE;
	}
	REQUEST_NOTIFICATION_STATUS OnPostPreExecuteRequestHandler(IN IHttpContext* pHttpContext,IN IHttpEventProvider* pProvider) {
		log(pHttpContext,"OnPostPreExecuteRequestHandler");
		//RestoreAcceptEncoding(pHttpContext);
		return RQ_NOTIFICATION_CONTINUE;}

	//RQ_EXECUTE_REQUEST_HANDLER
	REQUEST_NOTIFICATION_STATUS OnExecuteRequestHandler(IN IHttpContext* pHttpContext,IN IHttpEventProvider* pProvider) 
	{
		log(pHttpContext,"OnExecuteRequestHandler");
		//HideAcceptEncodingIfNeeded(pHttpContext);
		return RQ_NOTIFICATION_CONTINUE;
	}
	REQUEST_NOTIFICATION_STATUS OnPostExecuteRequestHandler(IN IHttpContext* pHttpContext,IN IHttpEventProvider* pProvider) {
		log(pHttpContext,"OnPostExecuteRequestHandler");
		RestoreAcceptEncoding(pHttpContext);
		return RQ_NOTIFICATION_CONTINUE;}

	//RQ_RELEASE_REQUEST_STATE
	REQUEST_NOTIFICATION_STATUS OnReleaseRequestState(IN IHttpContext* pHttpContext,IN IHttpEventProvider* pProvider) {
		log(pHttpContext,"OnReleaseRequestState");
		HideAcceptEncodingIfNeeded(pHttpContext);
		return RQ_NOTIFICATION_CONTINUE;}
	REQUEST_NOTIFICATION_STATUS OnPostReleaseRequestState(IN IHttpContext* pHttpContext,IN IHttpEventProvider* pProvider) {
		log(pHttpContext,"OnPostReleaseRequestState");
		RestoreAcceptEncoding(pHttpContext);
		return RQ_NOTIFICATION_CONTINUE;}

	//RQ_UPDATE_REQUEST_CACHE
	REQUEST_NOTIFICATION_STATUS OnUpdateRequestCache(IN IHttpContext* pHttpContext,IN IHttpEventProvider* pProvider) {
		log(pHttpContext,"OnUpdateRequestCache");
		HideAcceptEncodingIfNeeded(pHttpContext);
		return RQ_NOTIFICATION_CONTINUE;
	}
	REQUEST_NOTIFICATION_STATUS OnPostUpdateRequestCache(IN IHttpContext* pHttpContext,IN IHttpEventProvider* pProvider) {
		log(pHttpContext,"OnPostUpdateRequestCache");
		RestoreAcceptEncoding(pHttpContext);
		return RQ_NOTIFICATION_CONTINUE;
	}

	//RQ_LOG_REQUEST
	REQUEST_NOTIFICATION_STATUS OnLogRequest(IN IHttpContext* pHttpContext,IN IHttpEventProvider* pProvider) {
		log(pHttpContext,"OnLogRequest");
		HideAcceptEncodingIfNeeded(pHttpContext);
		return RQ_NOTIFICATION_CONTINUE;}
	REQUEST_NOTIFICATION_STATUS OnPostLogRequest(IN IHttpContext* pHttpContext,IN IHttpEventProvider* pProvider) {
		log(pHttpContext,"OnPostLogRequest");
		RestoreAcceptEncoding(pHttpContext);
		return RQ_NOTIFICATION_CONTINUE;}

	//RQ_READ_ENTITY |
	REQUEST_NOTIFICATION_STATUS OnReadEntity(IN IHttpContext* pHttpContext,IN IReadEntityProvider* pProvider) {
		log(pHttpContext,"OnReadEntity");
		return RQ_NOTIFICATION_CONTINUE;}

	//RQ_MAP_PATH
	REQUEST_NOTIFICATION_STATUS OnMapPath(IN IHttpContext* pHttpContext,IN IMapPathProvider* pProvider) {
		log(pHttpContext,"OnMapPath");
		return RQ_NOTIFICATION_CONTINUE;}


	
	
	bool IsLocalRequest(IN IHttpContext* pHttpContext);
	
	HRESULT ReadFileChunk(HTTP_DATA_CHUNK *chunk, char *buf);
	void log(IHttpContext* c, const char * msg, ...);

	IisProcessContext* process_context(const GoogleString& site_app_id,std::string site_root, const GoogleString& app_url_base) { 
		if (!process_context__) 
		{
			process_context__=module_creator_->GetProcessContext(site_app_id,site_root,app_url_base);
		}
		return process_context__;
	}
   REQUEST_NOTIFICATION_STATUS
        OnAsyncCompletion(
        IN IHttpContext * pHttpContext,
        IN DWORD dwNotification,
        IN BOOL fPostNotification,
        IN IHttpEventProvider * pProvider,
        IN IHttpCompletionInfo * pCompletionInfo
        );

private:
	IisModuleRequestContext* CreateRequestContext(IHttpContext *pHttpContext,std::string site_root,char *url,int url_len,GoogleUrl *gurl);
	void WritePre(StringPiece str, Writer* writer, MessageHandler* handler);
	void HideAcceptEncodingIfNeeded(IN IHttpContext* pHttpContext);
	void RestoreAcceptEncoding(IN IHttpContext* pHttpContext);
	void WriteHandlerResponse(IN IHttpContext * pHttpContext, const StringPiece& output, ContentType content_type, const StringPiece& cache_control);
	void WriteHandlerResponse(IN IHttpContext * pHttpContext, const StringPiece& output, ContentType content_type);
	void WriteHandlerResponse(IN IHttpContext * pHttpContext, const StringPiece& output);

	IisModuleFactory *module_creator_;
	IisProcessContext* process_context__;
	DISALLOW_COPY_AND_ASSIGN(IisHttpModule);	
};


}
#endif
