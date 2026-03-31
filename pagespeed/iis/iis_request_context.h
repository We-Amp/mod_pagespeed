#ifndef IIS_REQUEST_CONTEXT_H_
#define IIS_REQUEST_CONTEXT_H_

#include <httpserv.h>
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/http/google_url.h"
#include "pagespeed/automatic/proxy_fetch.h"
#include "pagespeed/kernel/thread/win_mutex.h"
#include "pagespeed/iis/iis_base_fetch.h"
#include "pagespeed/iis/iis_process_context.h"
#include "pagespeed/iis/iis_server_context.h"
#include "pagespeed/iis/iis_message_handler.h"
#include "pagespeed/iis/iis_psol_request_context.h"
#include "pagespeed/iis/iis_rewrite_driver_factory.h"
#include "pagespeed/iis/dechunker.h"
//KS: makes things not portable
#include "pagespeed/iis/iis_module_request_context.h"


namespace net_instaweb {

class IisServerContext;
class InPlaceResourceRecorder;

class IisInnerRequestContext 
{
public:
	IisInnerRequestContext(bool is_resource, const char * url, size_t url_len, GoogleUrl* gurl
		, IisModuleRequestContext *outer_context,  IisProcessContext* process_context, IisServerContext* server_context, int local_port, GoogleString local_ip_address) :
		 is_resource_(is_resource)
		, url_(url)
		, url_len_(url_len)
		, gurl_(gurl)
	    , proxy_fetch_(NULL)
		, base_fetch_(NULL)
		, leave_(false)
		, end_request_seen_(false)
		, send_response_seen_(false)
		
		, process_context_(process_context)
		, chunked_(false)
		, server_context_(server_context)
		, local_port_(local_port)
		, local_ip_address_(local_ip_address)
		, in_place_(false)
		, driver_(NULL)
		, recorder_(NULL)
		, experiment_classified_(false)
		, rewrite_html_(false)
		, outer_context_(outer_context)
	{
		//server_context->message_handler()->Message(kInfo,"create request context for connection to ip %s:%d", local_ip_address.c_str(), local_port);
		pagespeedRequestPointer.reset(new IisPsolRequestContext(
				server_context->thread_system()->NewMutex(),
				server_context->timer(),
//WEAMPKS: 1.9 needs host
				gurl->Host(),

				local_port,
				local_ip_address)
			);
	}

	Dechunker *GetDechunker();
	void createNewPageSpeedRequestPointer() {
		pagespeedRequestPointer.reset(new IisPsolRequestContext(
			server_context_->thread_system()->NewMutex(),
			server_context_->timer(),
			//WEAMPKS: 1.9 needs host
			gurl_->Host(),

			local_port_,
			local_ip_address_)
			);
	}
	virtual void CleanupStoredContext();
	//void DetermineData(char *data,size_t datalength,char **senddata,size_t *senddatalength); 

	IisBaseFetch* base_fetch() { return base_fetch_;}
	void set_base_fetch(IisBaseFetch* x){ base_fetch_=x; }
	bool is_resource() { return is_resource_;} 
	ProxyFetch* proxy_fetch() { return proxy_fetch_;} 
	void set_proxy_fetch(ProxyFetch* x) { proxy_fetch_ = x; }
	const char * url() { return url_;}
	void set_leave(bool x) { leave_ = x; }
	bool leave() { return leave_;}
	void set_end_request_seen(bool x) { end_request_seen_= x; }
	bool end_request_seen() { return end_request_seen_; }
	bool send_response_seen() { return  send_response_seen_; }
	void set_send_response_seen(bool x) { send_response_seen_ = x; }
	GoogleUrl* gurl() { return gurl_;} 
	IisProcessContext* process_context() { return process_context_; }
	void set_chunked(bool x){ chunked_=x;dechunker.Initialize(x);}
	Dechunker *get_dechunker() {return &dechunker;}
	bool chunked() { return chunked_; }
	const RequestContextPtr &GetPagespeedRequestContext() {return pagespeedRequestPointer;}
	void SetSiteConfigFile(std::string siteConfigFile) {siteConfigFile_.assign(siteConfigFile);}
	std::string GetSiteConfigFile() {return siteConfigFile_;}
	IisServerContext* server_context() { return server_context_;}
	int local_port() { return local_port_;}
	GoogleString local_ip() { return local_ip_address_;}
	int in_place() { return in_place_;}
	void set_in_place(bool x) { in_place_ = x; }
	RewriteDriver* driver() { return driver_; }
	void set_driver(RewriteDriver* x) { driver_ = x; }
	void set_recorder(InPlaceResourceRecorder* x) { recorder_ = x; }
	InPlaceResourceRecorder* recorder() { return recorder_; }
	bool experiment_classified() { return experiment_classified_;}
	void set_experiment_classified(bool x) { experiment_classified_ = x; }
	bool rewrite_html() { return rewrite_html_;}
	void set_rewrite_html(bool x) { rewrite_html_ = x; }
	IisModuleRequestContext* outer_context() { return outer_context_; }
protected:
	virtual ~IisInnerRequestContext();
private:
	std::string siteConfigFile_;
	RequestContextPtr pagespeedRequestPointer;
	PCSTR pRawValueAcceptEncoding;
	USHORT pRawValueLengthAcceptEncoding;
	bool is_resource_;

	const char * url_;
	size_t url_len_;

	//owned
	GoogleUrl* gurl_;
	Dechunker dechunker;

	//not owned
	ProxyFetch* proxy_fetch_;
	IisBaseFetch* base_fetch_;
	bool leave_;
	bool end_request_seen_;
	bool send_response_seen_;
	IisProcessContext* process_context_;
	bool chunked_;
	IisServerContext* server_context_;
	IisModuleRequestContext* outer_context_;
	int local_port_;
	GoogleString local_ip_address_;
	bool in_place_;
	RewriteDriver* driver_;
	InPlaceResourceRecorder* recorder_;
	bool experiment_classified_;
	bool rewrite_html_;

	IisInnerRequestContext(const IisInnerRequestContext&) = delete;
	IisInnerRequestContext& operator=(const IisInnerRequestContext&) = delete;
};

}

#endif
