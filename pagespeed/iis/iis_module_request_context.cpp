#include <httpserv.h>
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/http/google_url.h"
#include "pagespeed/kernel/util/gzip_inflater.h"
#include "pagespeed/automatic/proxy_fetch.h"
#include "pagespeed/kernel/thread/win_mutex.h"
#include "pagespeed/iis/iis_module_base_fetch.h"
#include "pagespeed/iis/iis_process_context.h"
#include "pagespeed/iis/iis_module_request_context.h"
#include "pagespeed/iis/iis_request_context.h"
#include "pagespeed/iis/iis_misc.h"
#include "pagespeed/iis/iis_module_misc.h"
#include "pagespeed/iis/iis_server_context.h"
#include "pagespeed/iis/iis_configuration.h"
#include "pagespeed/iis/iis_module_factory.h"

namespace net_instaweb
{

	IisModuleRequestContext::IisModuleRequestContext(IHttpContext *pContext, bool is_resource, const char * url, size_t url_len, GoogleUrl* gurl
		, IisProcessContext* process_context, IisServerContext* server_context, int local_port, GoogleString local_ip_address)
		:
		hideacceptencoding_(false),
		inflater_(NULL),
		empty_(false),
		hasNextStatus(false)
		
	{
		pRawValueAcceptEncoding = 0;
		pRawValueLengthAcceptEncoding = 0;
		// Transport scheme of the incoming connection — what a loopback fetch
		// to local_port must speak. The request URL's scheme can differ when
		// X-Forwarded-Proto is honored (defect B).
		GoogleString local_scheme =
			(pContext->GetRequest()->GetRawHttpRequest()->pSslInfo != NULL) ? "https" : "http";
		requestContext_ = new IisInnerRequestContext(is_resource, url, url_len, gurl,this, process_context, server_context, local_port, local_ip_address, local_scheme);
		GoogleString checkpath = FindConfigFile(ws2s(pContext->GetApplication()->GetApplicationPhysicalPath()));
		requestContext_->SetSiteConfigFile(checkpath);
	}
	IisModuleRequestContext::IisModuleRequestContext() :requestContext_(NULL), empty_(true), pRawValueAcceptEncoding(NULL), pRawValueLengthAcceptEncoding(0),
		inflater_(NULL),
		hideacceptencoding_(false),
		hasNextStatus(false)
	{

	}




	IisModuleRequestContext* IisModuleRequestContext::GetRequestContext(IN IHttpContext* pHttpContext)
	{
		auto checkContext = pHttpContext;
		while (checkContext) // if we have a context here we will bail
		{
			auto container=checkContext->GetModuleContextContainer();
			if (container)
			{
				auto currentModuleContext = container->GetModuleContext(IisModuleFactory::module_id());
				if (currentModuleContext)
				{
					IisModuleRequestContext* r = (IisModuleRequestContext*)(currentModuleContext);
					if (r->empty())
						return NULL;
					return r;
				}
			}

			checkContext = checkContext->GetParentContext();
		}
		return NULL;


		/*
		IHttpModuleContextContainer* context_container;

		if (!process_context__) return NULL;
		CHECK(context_container != NULL);

		IHttpStoredContext* storedContext = context_container->GetModuleContext(module_creator_->module_id());
		if (!storedContext) return NULL;
		IisModuleRequestContext* r = (IisModuleRequestContext*)(storedContext);
		if (r == NULL || r->empty())
		return NULL;
		//DCHECK(r != NULL);
		return r;
		*/
	}

	IisInnerRequestContext *IisModuleRequestContext::GetInnerContext()
	{
		return requestContext_;
	}

	void IisModuleRequestContext::CleanupStoredContext()
	{
		if (requestContext_) {
			requestContext_->CleanupStoredContext();
		}
		requestContext_ = 0;
		if (inflater_ != NULL)
		{
			delete inflater_;
			inflater_ = NULL;
		}
		delete this;
	}
	IisModuleRequestContext::~IisModuleRequestContext()
	{
		if (requestContext_) requestContext_->CleanupStoredContext();
	}

	void IisModuleRequestContext::StripAndStoreAcceptEncoding(IHttpContext *c, bool stripforever)
	{
		if (!hideacceptencoding_) return;
		auto rr = c->GetRequest()->GetRawHttpRequest();
		if (!stripforever && rr->Headers.KnownHeaders[HttpHeaderAcceptEncoding].pRawValue)
		{
			pRawValueAcceptEncoding = rr->Headers.KnownHeaders[HttpHeaderAcceptEncoding].pRawValue;
			pRawValueLengthAcceptEncoding = rr->Headers.KnownHeaders[HttpHeaderAcceptEncoding].RawValueLength;
		}
		rr->Headers.KnownHeaders[HttpHeaderAcceptEncoding].RawValueLength = 0;
		rr->Headers.KnownHeaders[HttpHeaderAcceptEncoding].pRawValue = 0;
	}
	void IisModuleRequestContext::AddStoredAcceptEncoding(IHttpContext *c)
	{
		if (!hideacceptencoding_) return;
		if (pRawValueAcceptEncoding)
		{
			auto rr = c->GetRequest()->GetRawHttpRequest();
			if (rr->Headers.KnownHeaders[HttpHeaderAcceptEncoding].pRawValue == NULL)
			{
				rr->Headers.KnownHeaders[HttpHeaderAcceptEncoding].pRawValue = pRawValueAcceptEncoding;
				rr->Headers.KnownHeaders[HttpHeaderAcceptEncoding].RawValueLength = pRawValueLengthAcceptEncoding;
			}
			//pRawValueAcceptEncoding=0;
			//pRawValueLengthAcceptEncoding=0;
		}
	}
	void IisModuleRequestContext::set_base_fetch(IisModuleBaseFetch *base_fetcher)
	{
		// When switching from the IPRO cache lookup to html rewriting, 
		// a fresh base fetch will be set. We need to release to old one.
		if (this->GetInnerContext()->base_fetch() != NULL) {
			this->GetInnerContext()->base_fetch()->ReleaseRef();
		}
		this->GetInnerContext()->set_base_fetch(base_fetcher);
	}
	IisModuleBaseFetch* IisModuleRequestContext::base_fetch()
	{
		return (IisModuleBaseFetch*) this->GetInnerContext()->base_fetch();
	}




	




}