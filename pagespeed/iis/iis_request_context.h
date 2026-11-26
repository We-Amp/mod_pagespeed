// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef IIS_REQUEST_CONTEXT_H_
#define IIS_REQUEST_CONTEXT_H_

#include <httpserv.h>
#include <memory>
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/kernel/http/google_url.h"
#include "pagespeed/automatic/proxy_fetch.h"
#include "pagespeed/kernel/thread/win_mutex.h"
#include "pagespeed/iis/iis_base_fetch.h"
#include "pagespeed/iis/iis_experiment_assignment.h"
#include "pagespeed/iis/iis_process_context.h"
#include "pagespeed/iis/iis_server_context.h"
#include "pagespeed/iis/iis_message_handler.h"
#include "pagespeed/iis/iis_psol_request_context.h"
#include "pagespeed/iis/iis_rewrite_driver_factory.h"
#include "pagespeed/iis/dechunker.h"
#include "pagespeed/iis/iis_daemon_html_capture.h"
#include "pagespeed/iis/iis_daemon_html_serve.h"
#include "pagespeed/system/module_entity_tags.h"
//KS: makes things not portable
#include "pagespeed/iis/iis_module_request_context.h"


namespace net_instaweb {

class IisServerContext;
class IproRecorder;

class IisInnerRequestContext 
{
public:
	IisInnerRequestContext(bool is_resource, const char * url, size_t url_len, GoogleUrl* gurl
		, IisModuleRequestContext *outer_context,  IisProcessContext* process_context, IisServerContext* server_context, int local_port, GoogleString local_ip_address, GoogleString local_scheme) :
		 is_resource_(is_resource)
		, url_(url)
		, url_len_(url_len)
		, gurl_(gurl)
	    , proxy_fetch_(nullptr)
		, base_fetch_(nullptr)
		, leave_(false)
		, send_response_seen_(false)
		
		, process_context_(process_context)
		, chunked_(false)
		, server_context_(server_context)
		, local_port_(local_port)
		, local_ip_address_(local_ip_address)
		, local_scheme_(local_scheme)
		, in_place_(false)
		, in_place_candidate_(false)
		, driver_(nullptr)
		, recorder_(nullptr)
		, rewrite_html_(false)
		, outer_context_(outer_context)
		, created_ms_(server_context->timer()->NowMs())
	{
		//server_context->message_handler()->Message(kInfo,"create request context for connection to ip %s:%d", local_ip_address.c_str(), local_port);
		pagespeedRequestPointer.reset(new IisPsolRequestContext(
				server_context->thread_system()->NewMutex(),
				server_context->timer(),
//WEAMPKS: 1.9 needs host
				gurl->Host(),

				local_port,
				local_ip_address,
				local_scheme)
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
			local_ip_address_,
			local_scheme_)
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
	// The request entered the in-place branch: whatever answers it, a
	// response the module could optimize is kept out of IIS's caches.
	bool in_place_candidate() const { return in_place_candidate_; }
	void set_in_place_candidate(bool x) { in_place_candidate_ = x; }
	RewriteDriver* driver() { return driver_; }
	void set_driver(RewriteDriver* x) { driver_ = x; }
	void set_recorder(IproRecorder* x) { recorder_ = x; }
	IproRecorder* recorder() { return recorder_; }
	IisExperimentAssignment* experiment_assignment() { return &experiment_assignment_; }
	bool rewrite_html() { return rewrite_html_;}
	void set_rewrite_html(bool x) { rewrite_html_ = x; }
	IisModuleRequestContext* outer_context() { return outer_context_; }

	// DaemonHtmlCache: the HTML cache engine's capture of this HTML response
	// (iis_daemon_html_capture.h), or nullptr.  Fed by the main
	// registration's send and by the module's own header edits, given its
	// final view by the final-view registration's send, and finished when
	// IIS releases the request (CleanupStoredContext).
	IisDaemonHtmlCapture* daemon_html_capture() { return daemon_html_capture_.get(); }
	void set_daemon_html_capture(std::unique_ptr<IisDaemonHtmlCapture> capture) {
		daemon_html_capture_ = std::move(capture);
	}
	// The module's own header edits are done (IisModuleBaseFetch::
	// CollectHeaders): the module view of the capture, read off `response`.
	void NoteDaemonHtmlModuleView(IHttpResponse* response);
	// When this context was created, at the module's first notification for
	// the request (Unix milliseconds): the fallback capture start when the
	// one stamped at GL_PRE_BEGIN_REQUEST cannot be read.
	int64 created_ms() const { return created_ms_; }

	// The client's If-None-Match and If-Modified-Since as they arrived,
	// before the begin-request notification removed a module tag from the
	// request (IisHttpModule::OnBeginRequest), copied here from the module
	// object that took them when this context is created.  A child request
	// of this one finds this context, not its own (already stripped)
	// request.
	const ClientConditionalStash& client_conditionals() const { return client_conditionals_; }
	void set_client_conditionals(const ClientConditionalStash& stash) { client_conditionals_ = stash; }

	// DaemonHtmlCache serve-variants: the Markdown answer the main
	// registration kept for the final-view registration to send
	// (iis_daemon_html_serve.h).  Taken once.
	bool markdown_answer_pending() const { return markdown_answer_ != nullptr; }
	void set_markdown_answer(std::unique_ptr<IisMarkdownAnswer> answer) { markdown_answer_ = std::move(answer); }
	std::unique_ptr<IisMarkdownAnswer> take_markdown_answer() { return std::move(markdown_answer_); }

	// The capture finished its body, so a commit follows at cleanup
	// (IisServerContext::NoteDaemonHtmlCommitStarted).
	void NoteDaemonHtmlBodyEnded();
	// The capture's cache URL, for TraceDaemonHtml.
	void set_daemon_html_trace_url(const GoogleString& url) { daemon_html_trace_url_ = url; }
protected:
	virtual ~IisInnerRequestContext();
private:
	std::string siteConfigFile_;
	RequestContextPtr pagespeedRequestPointer;
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
	bool send_response_seen_;
	IisProcessContext* process_context_;
	bool chunked_;
	IisServerContext* server_context_;
	IisModuleRequestContext* outer_context_;
	int local_port_;
	GoogleString local_ip_address_;
	GoogleString local_scheme_;
	bool in_place_;
	bool in_place_candidate_;
	RewriteDriver* driver_;
	IproRecorder* recorder_;
	IisExperimentAssignment experiment_assignment_;
	bool rewrite_html_;
	int64 created_ms_;
	std::unique_ptr<IisDaemonHtmlCapture> daemon_html_capture_;
	ClientConditionalStash client_conditionals_;
	std::unique_ptr<IisMarkdownAnswer> markdown_answer_;
	bool daemon_html_commit_started_ = false;
	GoogleString daemon_html_trace_url_;

	IisInnerRequestContext(const IisInnerRequestContext&) = delete;
	IisInnerRequestContext& operator=(const IisInnerRequestContext&) = delete;
};

}

#endif
