#include "pagespeed/iis/iis_http_module.h"

#define _WINSOCKAPI_
#define _WINSOCKAPI_
#include <windows.h>
#include <sal.h>
#include <httpserv.h>
#include <winternl.h>
#include <ntstatus.h>
#include <memory>

#include <time.h>


#include "base/logging.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/resource_fetch.h"
#include "net/instaweb/rewriter/public/static_asset_manager.h"
#include "net/instaweb/public/global_constants.h"
#include "pagespeed/kernel/http/google_url.h"
#include "pagespeed/kernel/base/std_timer.h"
#include "pagespeed/kernel/base/string_writer.h"
#include "pagespeed/kernel/base/google_message_handler.h"
#include "pagespeed/kernel/html/html_keywords.h"
#include "pagespeed/automatic/proxy_fetch.h"
#include "pagespeed/automatic/proxy_interface.h"
#include "pagespeed/kernel/util/gzip_inflater.h"
#include "pagespeed/kernel/http/query_params.h"

#include "pagespeed/system/in_place_resource_recorder.h"

#include "pagespeed/kernel/base/stack_buffer.h"
#include "pagespeed/kernel/util/statistics_logger.h"
#include "pagespeed/kernel/base/statistics.h"

#include "pagespeed/iis/iis_global_constants.h"
#include "pagespeed/iis/iis_config_util.h"
#include "pagespeed/iis/iis_proxy_fetch_completion.h"
#include "pagespeed/iis/iis_process_context.h"
#include "pagespeed/iis/iis_rewrite_driver_factory.h"
#include "pagespeed/iis/iis_misc.h"
#include "pagespeed/iis/iis_message_handler.h"
#include "pagespeed/iis/iis_server_context.h"
#include "pagespeed/iis/iis_rewrite_options.h"
#include "pagespeed/iis/iis_module_request_context.h"
#include "pagespeed/iis/iis_configuration.h"
#include "pagespeed/iis/iis_module_misc.h"
#include "pagespeed/iis/iis_request_context.h"
#include "pagespeed/iis/iis_ip_string.h"

namespace net_instaweb
{
	// Writes text wrapped in a <pre> block
	void IisHttpModule::WritePre(StringPiece str, Writer* writer, MessageHandler* handler) {
		writer->Write("<pre>\n", handler);
		writer->Write(str, handler);
		writer->Write("</pre>\n", handler);
	}

	// Write response headers and send out headers and output, including the option
	//     for a custom Content-Type.
	void IisHttpModule::WriteHandlerResponse(IN IHttpContext * pHttpContext, const StringPiece& output, ContentType content_type, const StringPiece& cache_control) {
		ResponseHeaders response_headers;
		response_headers.SetStatusAndReason(HttpStatus::kOK);
		response_headers.set_major_version(1);
		response_headers.set_minor_version(1);

		response_headers.Add(HttpAttributes::kContentType, content_type.mime_type());
		StdTimer timer;
		int64 now_ms = timer.NowMs();
		response_headers.SetDate(now_ms);
		response_headers.SetLastModified(now_ms);
		response_headers.Add(HttpAttributes::kCacheControl, cache_control);


		IHttpResponse* r = pHttpContext->GetResponse();
		//now copy pagespeed response header to iis output
		r->SetStatus(response_headers.status_code(), response_headers.reason_phrase());
		for (size_t i = 0; i < response_headers.NumAttributes(); i++)
		{
			const GoogleString& name_gs = response_headers.Name(i);
			const GoogleString& value_gs = response_headers.Value(i);
			r->SetHeader(name_gs.c_str(), value_gs.c_str(), value_gs.length(), true);
		}

		WriteResponseMessage(pHttpContext, (PVOID)output.data(), output.length(), true);

	}

	void IisHttpModule::WriteHandlerResponse(IN IHttpContext * pHttpContext, const StringPiece& output, ContentType content_type) {
		WriteHandlerResponse(pHttpContext, output, kContentTypeHtml, HttpAttributes::kNoCache);
	}

	void IisHttpModule::WriteHandlerResponse(IN IHttpContext * pHttpContext, const StringPiece& output) {
		WriteHandlerResponse(pHttpContext, output, kContentTypeHtml);
	}

	bool has_own_iispeed_config(IHttpContext *pHttpContext)
	{
		// Engage gate: a site engages when it has its OWN per-site config
		//. Routed through the shared resolver's tier-3
		// primitive so the gate, the request-time merge, and the FileID watch
		// probe the exact same file. Deliberately scoped to the per-site file
		// (not the %ProgramData% base): a site without its own config stays
		// disengaged, so single-site behavior — including opt-out by removing
		// the per-site file — is unchanged.
		return iis_config_util::SiteConfigExists(
			ws2s(pHttpContext->GetApplication()->GetApplicationPhysicalPath()));
	}

	bool IisHttpModule::IsLocalRequest(IN IHttpContext* pHttpContext)
	{
		GoogleString remote_ip_address = GetIPString(pHttpContext->GetRequest()->GetRemoteAddress());
		GoogleString local_ip_address = GetIPString(pHttpContext->GetRequest()->GetLocalAddress());

		return remote_ip_address == "127.0.0.1" || remote_ip_address == local_ip_address;
	}
	namespace RequestRouting {
		enum Response {
			kError,
			kNotUnderstood,
			kStaticContent,
			kInvalidUrl,
			kPagespeedDisabled,
			kBeacon,
			kStatistics,
			kGlobalStatistics,
			kConsole,
			kMessages,
			kAdmin,
			kCachePurge,
			kGlobalAdmin,
			kPagespeedSubrequest,
			kNotHeadOrGet,
			kErrorResponse,
			kResource,
		};
	}  // namespace RequestRouting


	// Set us up for processing a request.  Creates a request context and determines
	// which handler should deal with the request.
	RequestRouting::Response ps_route_request(IN IHttpContext* pHttpContext,
		IisModuleRequestContext* request_context,
		bool is_resource_fetch) {
		auto inner_context = request_context->GetInnerContext();
		IisServerContext* server_context = inner_context->server_context();
		if (!server_context->global_options()->enabled()) {
			// Not enabled for this server block.
			return RequestRouting::kPagespeedDisabled;
		}

		//if (r->err_status != 0) {
		//	return RequestRouting::kErrorResponse;
		//}

		// TODO(oschaaf): was via determine_url
		GoogleString url_string(inner_context->url());
		GoogleUrl url(url_string);;

		if (!url.IsWebValid()) {
			//ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "invalid url");

			// Let nginx deal with the error however it wants; we will see a NULL ctx in
			// the body filter or content handler and do nothing.
			return RequestRouting::kInvalidUrl;
		}
		StringPiece user_agent = StringPiece(pHttpContext->GetRequest()->GetHeader("User-Agent"));
		if (user_agent.find(kModPagespeedSubrequestUserAgent) != user_agent.npos) {
			return RequestRouting::kPagespeedSubrequest;
		}
		else if (
			url.PathSansLeaf() == dynamic_cast<IisRewriteDriverFactory*>(
			server_context->factory())->static_asset_prefix()) {
			return RequestRouting::kStaticContent;
		}

		const IisRewriteOptions* global_options = server_context->config();

		StringPiece path = url.PathSansQuery();
		auto stat_path = global_options->statistics_path();
		if (StringCaseEqual(path, global_options->statistics_path())) {
			return RequestRouting::kStatistics;
		}
		else if (StringCaseEqual(path, global_options->global_statistics_path())) {
			return RequestRouting::kGlobalStatistics;
		}
		else if (StringCaseEqual(path, global_options->console_path())) {
			return RequestRouting::kConsole;
		}
		else if (StringCaseEqual(path, global_options->messages_path())) {
			return RequestRouting::kMessages;
		}
		else if (
			// The admin handlers get everything under a path (/path/*) while all the
			// other handlers only get exact matches (/path).  So match all paths
			// starting with the handler path.
			!global_options->admin_path().empty() &&
			StringCaseStartsWith(path, global_options->admin_path())) {
			return RequestRouting::kAdmin;
		}
		else if (!global_options->global_admin_path().empty() &&
			StringCaseStartsWith(path, global_options->global_admin_path())) {
			return RequestRouting::kGlobalAdmin;
		}
		else if (global_options->enable_cache_purge() &&
			!global_options->purge_method().empty() &&
			(global_options->purge_method() ==
			pHttpContext->GetRequest()->GetHttpMethod())) {
			return RequestRouting::kCachePurge;
		}

		const GoogleString* beacon_url;
		if (pHttpContext->GetRequest()->GetRawHttpRequest()->pSslInfo != NULL) {
			beacon_url = &(global_options->beacon_url().https);
		}
		else {
			beacon_url = &(global_options->beacon_url().http);
		}

		if (url.PathSansQuery() == StringPiece(*beacon_url)) {
			return RequestRouting::kBeacon;
		}

		return RequestRouting::kResource;
	}

	const char* SyncReadPostBody(IN IHttpContext* pHttpContext)
	{
		static const off_t kMaxPostSizeBytes = 131072;
		DWORD cbBytesReceived = 0;
		DWORD pos = 0;
		DWORD bufferSize = kMaxPostSizeBytes;
		void * pvRequestBody = pHttpContext->AllocateRequestMemory(kMaxPostSizeBytes + 1); //+1 for \0

		if (NULL == pvRequestBody)
		{
			OutputDebugStringA("failed to allocate request memory for the post body");
			DCHECK(false);
			return NULL;
		}
		while (pHttpContext->GetRequest()->GetRemainingEntityBytes() != 0)
		{
			if (bufferSize == 0)
			{
				OutputDebugStringA("beacon post too large, bailing out");
				//DCHECK(false);
				return NULL;
			}
			HRESULT hr = pHttpContext->GetRequest()->ReadEntityBody(
				((char *)pvRequestBody) + pos, bufferSize, false, &cbBytesReceived, NULL);


			if (FAILED(hr))
			{
				// End of data is okay.
				if (ERROR_HANDLE_EOF != (hr & 0x0000FFFF))
				{
					OutputDebugStringA("failed to allocate request memory for the post body");
					DCHECK(false);
					return NULL;
				}
				pos += cbBytesReceived;
				break;
			}
			bufferSize -= cbBytesReceived;
			pos += cbBytesReceived;

		}


		((char*)pvRequestBody)[pos] = 0;

		return (const char*)pvRequestBody;
	}

	REQUEST_NOTIFICATION_STATUS
		IisHttpModule::OnBeginRequest(IN IHttpContext * pHttpContext, IN IHttpEventProvider * pProvider)
	{
		if (!has_own_iispeed_config(pHttpContext))
		{
			log(pHttpContext, "no configuration - bail");
			return RQ_NOTIFICATION_CONTINUE;
		}
		if (!pHttpContext->GetConnection()->IsConnected()) {
			log(pHttpContext, "Connection not connected - bail");
			return RQ_NOTIFICATION_CONTINUE;
		}


		auto checkContext = pHttpContext;
		while (checkContext) // if we have a context here we will bail
		{
			auto container = checkContext->GetModuleContextContainer();
			if (container)
			{
				auto currentModuleContext = container->GetModuleContext(module_creator_->module_id());
				if (currentModuleContext)
					return RQ_NOTIFICATION_CONTINUE;
			}
			checkContext = checkContext->GetParentContext();
		} 

		auto ret = OnBeginRequestPageSpeed(pHttpContext, pProvider);
		if (ret == RQ_NOTIFICATION_PENDING)
			return ret;
		if (!IisModuleRequestContext::GetRequestContext(pHttpContext))
		{
			auto container = pHttpContext->GetModuleContextContainer();
			if (container)
			{
				container->SetModuleContext(new IisModuleRequestContext(), module_creator_->module_id());
			}
		}
		return ret;
	}



	REQUEST_NOTIFICATION_STATUS
		IisHttpModule::OnBeginRequestPageSpeed(IN IHttpContext * pHttpContext, IN IHttpEventProvider * pProvider)
	{
		// oschaaf: state is to keep it thread safe
		std::mbstate_t state = std::mbstate_t();
		auto request = pHttpContext->GetRootContext()->GetRequest();
		PCWSTR tmpx;
		DWORD len;
		// We set this in our global prebegin request hook, in an attempt to beat
		// any other modules/filters that modify the url
		pHttpContext->GetRootContext()->GetServerVariable("X-PRISTINE-URL", &tmpx, &len);
		if (tmpx == NULL) {
			log(pHttpContext, "Missing X-PRISTINE-URL -> retry on raw http request url..");
			tmpx = pHttpContext->GetRootContext()->GetRequest()->GetRawHttpRequest()->CookedUrl.pFullUrl;
		}
		// Note: we have actually seen this happen at Lee's machine.
		const wchar_t* wstr = tmpx;
		if (wstr == NULL)
		{
			log(pHttpContext, "CookedUrl was NULL. Bail!");
			return RQ_NOTIFICATION_CONTINUE;
		}

		StringPiece range = StringPiece(pHttpContext->GetRequest()->GetHeader("Range"));
		if (range.length() > 0) {
			log(pHttpContext, "Range request. Bail");
			return RQ_NOTIFICATION_CONTINUE;
		}
		StringPiece if_range = StringPiece(pHttpContext->GetRequest()->GetHeader("If-Range"));
		if (if_range.length() > 0) {
			log(pHttpContext, "If-Range request. Bail");
			return RQ_NOTIFICATION_CONTINUE;
		}

		int url_len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, NULL, 0, 0, NULL);
		if (url_len == 0)
		{
			log(pHttpContext, "Could not convert url 1");
			return RQ_NOTIFICATION_CONTINUE;
		}
		char *url = (char*)pHttpContext->AllocateRequestMemory(url_len + 32); // we could add https or a port
		url_len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, url, url_len, 0, NULL);
		if (url_len == 0)
		{
			log(pHttpContext, "Could not convert url 2");
			return RQ_NOTIFICATION_CONTINUE;
		}


		/*size_t url_len;
		url_len=wcsrtombs(NULL,&wstr,MAX_URL_LEN*2,&state);
		if (url_len==((size_t) -1))
		{
		log(pHttpContext, "Something went wrong with converting. Bail");
		return RQ_NOTIFICATION_CONTINUE;
		}
		char * url = (char*)pHttpContext->AllocateRequestMemory(url_len+7);
		url_len=wcsrtombs(url,&wstr,MAX_URL_LEN*2,&state);			*/

		log(pHttpContext, "pristine url [%s]", url);

		StringPiece xf = StringPiece(pHttpContext->GetRequest()->GetHeader("X-Forwarded-Proto"));

		bool https = pHttpContext->GetRequest()->GetRawHttpRequest()->pSslInfo != NULL;
		int port = ntohs(((PSOCKADDR_IN)pHttpContext->GetRequest()->GetLocalAddress())->sin_port);
		bool noport = (https && port == 443) || (!https && port == 80) || xf != NULL;
		if (noport) {
			int cc = 0;
			int n_idx = 0;
			int countslashes = 0;
			for (int i = 0; i < url_len; i++) {
				countslashes += (url[i] == '/') ? 1 : 0;
				if (url[i] == ':' && countslashes == 2)
					cc++;
				if (cc == 1) {
					if (url[i] == '/') {
						cc++;
						url[n_idx++] = url[i];
					}
				}
				else {
					url[n_idx++] = url[i];
				}
			}
			url_len = n_idx;
			url[n_idx] = 0;
			log(pHttpContext, "noport processing url [%s]", url);
		}

		if (xf != NULL && xf.length() <= 6) {
			StringPiece url_sp(url);
			StringPiece::size_type colon_pos = url_sp.find(":");
			if (colon_pos != StringPiece::npos)
			{
				snprintf(url, url_len + 32, "%s%s",
				         std::string(xf.data(), xf.size()).c_str(),
				         std::string(url_sp.substr(colon_pos)).c_str());
				log(pHttpContext, "xf-proto processed url [%s]", url);
			}
		}

		log(pHttpContext, "processed url [%s]", url);

		GoogleUrl* gurl = new GoogleUrl(url);
		bool valid = !gurl->is_empty() && gurl->IsWebValid();
		if (!valid)
		{
			log(pHttpContext, "Invalid url, bailing out");
			delete gurl;
			return RQ_NOTIFICATION_CONTINUE;
		}
		StringPiece ua = StringPiece(pHttpContext->GetRequest()->GetHeader("User-Agent"));
		// check asyncwinhttp.cpp when you need to change this.
		// TODO(oschaaf): mod_pagespeed is actually defined in net/instaweb/public/version.h
		// bail out early for iispeed subrequests. this can possibly cause recursion when resource requests end up being
		// html when served
		if (ua.find("mod_pagespeed") != ua.npos)
		{
			log(pHttpContext, "Pagespeed user agent detected, bailing out");
			delete gurl;
			return RQ_NOTIFICATION_CONTINUE;
		}


		log(pHttpContext, "OnBeginRequest: [%s]", gurl->spec_c_str());
		GoogleString checkpath = FindConfigFile(ws2s(pHttpContext->GetApplication()->GetApplicationPhysicalPath()));
		IisProcessContext* failed_pc = nullptr;
		IisModuleRequestContext* ctx = CreateRequestContext(pHttpContext, checkpath, url, url_len, gurl, &failed_pc);
		if (ctx == NULL) {
			if (!IsLocalRequest(pHttpContext)) {
				log(pHttpContext, "OnBeginRequest: Failed to create a request context, bail");
				delete gurl;
				return RQ_NOTIFICATION_CONTINUE;
			}

			// Local-only diagnostic page. We keep HTTP 200 so anything
			// (browser, curl, monitoring) renders the body, and emit a
			// machine-readable X-Pagespeed-Init-Status header that test
			// fixtures and CI can match on without scraping prose. The
			// exact status values are part of the test/operator contract:
			//   cache-path-empty | cache-path-missing |
			//   cache-path-not-writable | cache-path-create-failed |
			//   log-dir-create-failed | post-config-failed | startup-failed
			GoogleString body;
			StringWriter writer(&body);
			GoogleMessageHandler handler;
			GoogleString site_id = ws2s(pHttpContext->GetApplication()->GetApplicationId());
			GoogleString init_status;

			if (failed_pc == nullptr) {
				init_status = "startup-failed";
				writer.Write(StrCat(
					"IISpeed failed to start.\n\n",
					"Site: ", site_id, "\n",
					"(no detail captured — check the Windows Application "
					"event log and pagespeed LogDir)\n"), &handler);
			} else {
				// The rename: failed_init_path() supersedes
				// failed_cache_path(). For cache-path failure kinds it
				// still holds the cache path; for kLogDirCreateFailed
				// it holds the LogDir path. The local var name stays
				// `cache_path` on the cache cases for minimum diff;
				// the kLogDirCreateFailed case below names it
				// `log_dir` for clarity at the writer call.
				const GoogleString& cache_path = failed_pc->failed_init_path();
				const GoogleString& identity   = failed_pc->app_pool_identity();
				const GoogleString& os_err     = failed_pc->init_error_message();

				switch (failed_pc->init_failure_kind()) {
				case IisProcessContext::InitFailureKind::kCachePathEmpty:
					init_status = "cache-path-empty";
					writer.Write(StrCat(
						"IISpeed is not running: no FileCachePath is configured.\n\n",
						"Site:        ", site_id, "\n",
						"Config file: ", checkpath.empty() ? GoogleString("(none found)") : checkpath, "\n\n",
						"Add to pagespeed.config:\n",
						"  pagespeed FileCachePath \"C:\\ProgramData\\We-Amp\\PageSpeed\\cache\"\n",
						"...then recycle the application pool.\n"), &handler);
					break;
				case IisProcessContext::InitFailureKind::kCachePathMissing:
					init_status = "cache-path-missing";
					writer.Write(StrCat(
						"IISpeed is not running: the configured FileCachePath does not exist\n",
						"and we did not auto-create it.\n\n",
						"FileCachePath: ", cache_path, "\n",
						"Identity:      ", identity, "\n",
						"OS error:      ", os_err, "\n\n",
						"We only auto-create paths under:\n",
						"  C:\\ProgramData\\We-Amp\\PageSpeed\\cache\\\n",
						"  C:\\ProgramData\\We-Amp\\IISWebSpeed\\cache\\\n",
						"This is your own path - please create it manually:\n",
						"  mkdir \"", cache_path, "\"\n",
						"  icacls \"", cache_path, "\" /grant \"", identity, "\":(OI)(CI)M\n",
						"...then recycle the application pool.\n"), &handler);
					break;
				case IisProcessContext::InitFailureKind::kCachePathUnwritable:
					init_status = "cache-path-not-writable";
					writer.Write(StrCat(
						"IISpeed is not running: the FileCachePath is not writable by the worker identity.\n\n",
						"FileCachePath: ", cache_path, "\n",
						"Identity:      ", identity, "\n",
						"OS error:      ", os_err, "\n\n",
						"Grant Modify to the worker identity:\n",
						"  icacls \"", cache_path, "\" /grant \"", identity, "\":(OI)(CI)M\n",
						"...then recycle the application pool.\n"), &handler);
					break;
				case IisProcessContext::InitFailureKind::kCachePathCreateFailed:
					init_status = "cache-path-create-failed";
					writer.Write(StrCat(
						"IISpeed is not running: auto-create of the per-site cache directory failed.\n\n",
						"FileCachePath: ", cache_path, "\n",
						"Identity:      ", identity, "\n",
						"OS error:      ", os_err, "\n\n",
						"This usually indicates EDR/AV blocking a CreateDirectoryW or\n",
						"DACL write on the canonical product tree. Check your endpoint\n",
						"security product's block log for the OS error above. As a\n",
						"manual fallback, create the directory and grant Modify to the\n",
						"worker identity:\n",
						"  mkdir \"", cache_path, "\"\n",
						"  icacls \"", cache_path, "\" /grant \"", identity, "\":(OI)(CI)M\n",
						"...then recycle the application pool.\n"), &handler);
					break;
				case IisProcessContext::InitFailureKind::kLogDirCreateFailed: {
					// the referenced issue, the design record §Operational. Parallel to
					// kCachePathCreateFailed above but for the LogDir
					// path. The narrower icacls hint — (RX,W) instead
					// of M — mirrors Product.wxs GrantLogAcl: workers
					// append to logs but admin owns rotation, so the
					// runtime auto-create grant withholds DELETE.
					init_status = "log-dir-create-failed";
					// failed_init_path() holds the LogDir on this
					// failure kind; aliased locally for clarity.
					const GoogleString& log_dir = cache_path;
					writer.Write(StrCat(
						"IISpeed is not running: auto-create of the log directory failed.\n\n",
						"LogDir:    ", log_dir, "\n",
						"Identity:  ", identity, "\n",
						"OS error:  ", os_err, "\n\n",
						"This usually indicates EDR/AV blocking a CreateDirectoryW or\n",
						"DACL write on the canonical product tree. Check your endpoint\n",
						"security product's block log for the OS error above. As a\n",
						"manual fallback, create the directory and grant Read+Execute+Write\n",
						"(without Modify/Delete) to the worker identity:\n",
						"  mkdir \"", log_dir, "\"\n",
						"  icacls \"", log_dir, "\" /grant \"", identity, "\":(OI)(CI)(RX,W)\n",
						"...then recycle the application pool.\n"), &handler);
					break;
				}
				case IisProcessContext::InitFailureKind::kPostConfigFailed:
					init_status = "post-config-failed";
					writer.Write(StrCat(
						"IISpeed failed to start.\n\n",
						"Site:   ", site_id, "\n",
						"Reason: ", os_err, "\n"), &handler);
					break;
				case IisProcessContext::InitFailureKind::kOk:
				case IisProcessContext::InitFailureKind::kUnknown:
				default:
					init_status = "startup-failed";
					writer.Write(StrCat(
						"IISpeed failed to start.\n\n",
						"Site: ", site_id, "\n",
						"(see pagespeed LogDir for details)\n"), &handler);
					break;
				}
			}

			writer.Write(
				"\n(this page is only shown when viewing on the local machine — "
				"your visitors will see the normal, but unoptimized website)\n",
				&handler);

			IHttpResponse* r = pHttpContext->GetResponse();
			r->SetStatus(200, "OK");
			// Machine-readable status header; stable across body wording.
			r->SetHeader("X-Pagespeed-Init-Status",
				init_status.c_str(), (USHORT)init_status.size(), true);
			GoogleString scl = size_t_to_str(body.size());
			r->SetHeader("Content-Length", scl.c_str(), (USHORT)scl.size(), true);
			r->SetHeader("Content-Type", "text/plain; charset=utf-8",
				(USHORT)strlen("text/plain; charset=utf-8"), true);
			r->SetHeader(HttpAttributes::kCacheControl, "no-store",
				(USHORT)strlen("no-store"), true);
			WriteResponseMessage(pHttpContext, (PVOID)body.data(), body.size(), true);
			return RQ_NOTIFICATION_FINISH_REQUEST;
		}


		bool is_resource = ctx->GetInnerContext()->is_resource();
		// TODO(oschaaf): 1.9-> is_resource arg not used?!
		auto response_category = ps_route_request(pHttpContext, ctx, is_resource);
		bool is_an_admin_handler =
			response_category == RequestRouting::kStatistics ||
			response_category == RequestRouting::kGlobalStatistics ||
			response_category == RequestRouting::kConsole ||
			response_category == RequestRouting::kAdmin ||
			response_category == RequestRouting::kGlobalAdmin ||
			response_category == RequestRouting::kCachePurge;

		auto innerContext = ctx->GetInnerContext();
		bool accept_post = !is_resource || (response_category == RequestRouting::kBeacon || response_category == RequestRouting::kCachePurge);
		if (!accept_post && StringCaseEqual(pHttpContext->GetRequest()->GetHttpMethod(), "POST")){
			log(pHttpContext, "POST to non-beacon url - bail");
			innerContext->set_leave(true);
			if (innerContext->base_fetch() != NULL) {
				delete innerContext->base_fetch();
				innerContext->set_base_fetch(NULL);
			}
			return RQ_NOTIFICATION_CONTINUE;
		}

		if (!is_resource && IisModuleBaseFetch::fetchount > 250) {
			log(pHttpContext, "Too many outstanding rewrites - bail");
			innerContext->set_leave(true);
			if (innerContext->base_fetch() != NULL) {
				delete innerContext->base_fetch();
				innerContext->set_base_fetch(NULL);
			}
			return RQ_NOTIFICATION_CONTINUE;
		}

		innerContext->server_context()->FlushCacheIfNecessary();


		pHttpContext->GetModuleContextContainer()->SetModuleContext(ctx, module_creator_->module_id());

		auto server_context = innerContext->server_context();
		// If null, that means use global options.
		net_instaweb::RewriteOptions* tmp = NULL;
		std::unique_ptr<RequestHeaders> request_headers(new RequestHeaders);
		IisModuleBaseFetch::PopulateRequestHeaders(pHttpContext, request_headers.get());
		// TODO(oschaaf): this is a relatively expensive hack. Since the IPRO change, it now
		// becomes apparent that determine_options modified innerContext->gurl(), to remove
		// the PageSpeed/PageSpeedFilters querystring options. So we can't re-use that here again.
		// So we recreate the original GoogleUrl se we can determine again.
		GoogleUrl xgurl(innerContext->url());
		GoogleString pagespeed_query_params;
		GoogleString pagespeed_option_cookies;
		bool ok = true;
		ok = determine_options(pHttpContext, innerContext, request_headers.get(), NULL, &tmp, innerContext->GetPagespeedRequestContext(),
		     &xgurl, &pagespeed_query_params, &pagespeed_option_cookies);
		std::unique_ptr<net_instaweb::RewriteOptions> custom_options(tmp);
		if (!ok)
		{
			log(pHttpContext, "determine options failed!");
			if (custom_options.get())
			{
				innerContext->set_leave(true);
			}
			if (innerContext->base_fetch() != NULL) {
				delete innerContext->base_fetch();
				innerContext->set_base_fetch(NULL);
			}
			return RQ_NOTIFICATION_CONTINUE;
		}
		
		PSOCKADDR_IN addr_in = (PSOCKADDR_IN)pHttpContext->GetRequest()->GetLocalAddress();
		GoogleString ip_address = GetIPString((PSOCKADDR)addr_in);
		RewriteOptions* remote_options = ps_determine_remote_options(server_context, ip_address, *innerContext->gurl());

		if (remote_options != nullptr) {
			if (custom_options.get() == nullptr) {
				custom_options.reset(innerContext->server_context()->global_options()->Clone());
			}
			custom_options->Merge(*remote_options);
			delete remote_options;
			remote_options = nullptr;
		}

		net_instaweb::RewriteOptions* options;
		if (custom_options.get() == NULL)
		{
			options = server_context->global_options();
		}
		else
		{
			server_context->ComputeSignature(custom_options.get());
			options = custom_options.get();
		}

		if (is_an_admin_handler && !IsLocalRequest(pHttpContext) && INFO_URLS_LOCAL_ONLY) {
			log(pHttpContext, "Declining serving of an admin page: not a local request");
			is_an_admin_handler = false;
		} 
		else if (is_an_admin_handler) {
			log(pHttpContext, "Serving admin page");
			QueryParams query_params;
			query_params.ParseFromUrl(*ctx->GetInnerContext()->gurl());
			innerContext->set_leave(true);
			innerContext->GetPagespeedRequestContext()->set_options(options->ComputeHttpOptions());
			ctx->set_base_fetch(new IisModuleBaseFetch(pHttpContext, FetchType::kResource, innerContext->process_context()->handler(), innerContext->GetPagespeedRequestContext(), NULL));

			StdTimer timer;
			int64 now_ms = timer.NowMs();
			ctx->base_fetch()->response_headers()->SetDateAndCaching(
				now_ms, 0 /* max-age */, ", no-cache");
			if (!server_context->ShouldOptimize()) {
				ctx->base_fetch()->response_headers()->Add("x-need-renew", "1");
			}

			if (response_category == RequestRouting::kStatistics ||
				response_category == RequestRouting::kGlobalStatistics) {
				server_context->StatisticsPage(
					response_category == RequestRouting::kGlobalStatistics,
					query_params,
					server_context->config(),
					ctx->base_fetch());
			}
			else if (response_category == RequestRouting::kConsole) {
				server_context->ConsoleHandler(
					*server_context->config(),
					AdminSite::kStatistics,
					query_params,
					ctx->base_fetch());
			}
			else if (response_category == RequestRouting::kAdmin ||
				response_category == RequestRouting::kGlobalAdmin) {
				// Read POST body for JSON-API endpoints (/v1/license/*).
				// AdminLicenseHandler's mutation handlers (apply, activate,
				// trial, consent) ExtractJsonStringField from this body;
				// without forwarding it, every license POST 400s with a
				// missing-field error. The Apache, nginx, and Envoy ports
				// already read+pass the body here -- IIS was missing parity.
				StringPiece request_body;
				if (StringCaseEqual(pHttpContext->GetRequest()->GetHttpMethod(),
				                    "POST")) {
					const char* body = SyncReadPostBody(pHttpContext);
					if (body != NULL) {
						request_body = StringPiece(body);
					}
				}
				server_context->AdminPage(
					response_category == RequestRouting::kGlobalAdmin,
					*ctx->GetInnerContext()->gurl(),
					query_params,
					custom_options == NULL ? server_context->config()
					: custom_options.get(),
					ctx->base_fetch(),
					request_body);
			}
			else if (response_category == RequestRouting::kCachePurge) {
				AdminSite* admin_site = server_context->admin_site();
				admin_site->PurgeHandler(ctx->GetInnerContext()->url(),
					server_context->cache_path(),
					ctx->base_fetch());
			}
			else {
				CHECK(false);
			}

			return RQ_NOTIFICATION_PENDING;
		} else if (is_resource)
		{
			innerContext->set_leave(true);
			innerContext->GetPagespeedRequestContext()->set_options(options->ComputeHttpOptions());
			ctx->set_base_fetch(new IisModuleBaseFetch(pHttpContext, FetchType::kResource, innerContext->process_context()->handler(), innerContext->GetPagespeedRequestContext(), NULL));

			std::unique_ptr<ProxyFetchPropertyCallbackCollector>
				property_callback(
				ProxyFetchFactory::InitiatePropertyCacheLookup(
				true /* is_resource_fetch */,
				*innerContext->gurl(),
				innerContext->server_context(),
				options,
				innerContext->base_fetch()));

			ResourceFetch::Start(
				*gurl, custom_options.release(),
				server_context, innerContext->base_fetch());
			return RQ_NOTIFICATION_PENDING;
		}
		else if (response_category == RequestRouting::kStaticContent)
		{
			innerContext->set_leave(true);
			// Strip out the common prefix url before sending to
			// StaticJavascriptManager.
			StringPiece path_and_leaf = gurl->PathAndLeaf();
			log(pHttpContext, "%s", path_and_leaf.as_string().c_str());

			IisRewriteDriverFactory* fac = (IisRewriteDriverFactory*)server_context->factory();

			StringPiece file_name(path_and_leaf.substr(fac->static_asset_prefix().size()));
			log(pHttpContext, "%s", file_name.as_string().c_str());

			StringPiece file_contents;
			StringPiece cache_header;
			ContentType content_type;
			bool found = server_context->static_asset_manager()->GetAsset(
				file_name, &file_contents, &content_type, &cache_header);
			if (!found) {
				return RQ_NOTIFICATION_CONTINUE;
			}

			IHttpResponse* r = pHttpContext->GetResponse();

			r->SetStatus(200, "OK");
			GoogleString scl = size_t_to_str(file_contents.size());
			r->SetHeader("Content-Length", scl.c_str(), scl.size(), true);
			r->SetHeader("Content-Type", content_type.mime_type(), strlen(content_type.mime_type()), true);
			r->SetHeader("Cache-Control", cache_header.as_string().c_str(), cache_header.size(), true);
			r->SetHeader("ETag", "W/\"0\"", 5, true);
			WriteResponseMessage(pHttpContext, (PVOID)file_contents.data(), file_contents.size(), true);
			return RQ_NOTIFICATION_FINISH_REQUEST;
		}
		else if (response_category == RequestRouting::kBeacon) {
			bool beacon_handler_ok = false;

			StringPiece unparsed_uri(url, url_len);
			stringpiece_ssize_type question_mark_index = unparsed_uri.find("?");
			StringPiece beacon_get_data;
			StringPiece beacon_post_data;

			if (question_mark_index == StringPiece::npos) {
				beacon_get_data = "";
			}
			else {
				beacon_get_data = unparsed_uri.substr(
					question_mark_index + 1, unparsed_uri.size() - (question_mark_index + 1));
			}

			if (!strcmp(pHttpContext->GetRequest()->GetHttpMethod(), "POST"))
			{
				const char* data = SyncReadPostBody(pHttpContext);
				if (data != NULL)
				{
					beacon_post_data = StringPiece(data);
				}
				else {
					innerContext->server_context()->message_handler()->Message(kInfo, "NO Beacon post data read??");
				}
			}

			GoogleString beacon_data = StrCat(
				beacon_get_data, "&", beacon_post_data);

			beacon_handler_ok = server_context->HandleBeacon(beacon_data, StringPiece(pHttpContext->GetRequest()->GetHeader("User-Agent")), innerContext->GetPagespeedRequestContext());

			IHttpResponse* r = pHttpContext->GetResponse();
			r->ClearHeaders();
			r->SetHeader("Cache-Control", "max-age=0, no-cache", strlen("max-age=0, no-cache"), true);

			if (beacon_handler_ok)
			{
				r->SetStatus(204, "No Content");
			}
			else
			{
				// OS: Serve up the beacon as a 204 anyhow. This is a temporary fix to make sure the beacon does not
				// show up as a http/500 in the webmaster tools. Apparently, Google's crawler attempts to find the 
				// reference it sees in javascript
				innerContext->server_context()->message_handler()->Message(kWarning, "Beacon not handled OK!");
				r->SetStatus(204, "No Content");
			}
			return RQ_NOTIFICATION_FINISH_REQUEST;
		}
		else if (options->in_place_rewriting_enabled() &&
			options->enabled() &&
			options->IsAllowed(innerContext->gurl()->Spec()) &&
			(StringCaseEqual(pHttpContext->GetRequest()->GetHttpMethod(), "GET") || StringCaseEqual(pHttpContext->GetRequest()->GetHttpMethod(), "HEAD"))
			) {
			ctx->set_base_fetch(new IisModuleBaseFetch(pHttpContext, FetchType::kInPlace, innerContext->process_context()->handler(), innerContext->GetPagespeedRequestContext(), innerContext));
			innerContext->base_fetch()->request_context()->set_options(options->ComputeHttpOptions());
			// TODO(oschaaf): uncommented in 1.9
			//ctx->base_fetch()->SetRequestHeadersTakingOwnership(request_headers.release());
			// Do not store driver in request_context, it's not safe.
			RewriteDriver* driver;
			if (custom_options.get() == NULL) {
				driver = innerContext->server_context()->NewRewriteDriver(
					innerContext->base_fetch()->request_context());
			}
			else {
				// NewCustomRewriteDriver takes ownership of custom_options.
				driver = innerContext->server_context()->NewCustomRewriteDriver(
					custom_options.release(), innerContext->base_fetch()->request_context());
			}
			driver->SetRequestHeaders(*innerContext->base_fetch()->request_headers());

			innerContext->set_driver(driver);
			innerContext->server_context()->message_handler()->Message(
				kInfo, "Trying to serve rewritten resource in-place: %s",
				innerContext->gurl()->spec_c_str());

			// TODO(oschaaf): seems unused
			innerContext->set_in_place(true);
				// TODO(oschaaf):
			//innerContext->base_fetch()->set_handle_error(false);
			innerContext->driver()->FetchInPlaceResource(
				*innerContext->gurl(), false /* proxy_mode */, innerContext->base_fetch());

			return RQ_NOTIFICATION_PENDING;
		}
		return RQ_NOTIFICATION_CONTINUE;
	}

	void IisHttpModule::HideAcceptEncodingIfNeeded(IN IHttpContext* pHttpContext)
	{
		IisModuleRequestContext* request_context = IisModuleRequestContext::GetRequestContext(pHttpContext);
		if (!request_context) return;
		if (request_context->HideAcceptEncoding())  request_context->StripAndStoreAcceptEncoding(pHttpContext);
	}

	void IisHttpModule::RestoreAcceptEncoding(IN IHttpContext* pHttpContext)
	{
		IisModuleRequestContext* request_context = IisModuleRequestContext::GetRequestContext(pHttpContext);
		if (!request_context) return;
		if (request_context->HideAcceptEncoding()) request_context->AddStoredAcceptEncoding(pHttpContext);
	}


	REQUEST_NOTIFICATION_STATUS IisHttpModule::OnPostBeginRequest(IN IHttpContext* pHttpContext, IN IHttpEventProvider* pProvider)
	{
		log(pHttpContext, "OnPostBeginRequest");
		RestoreAcceptEncoding(pHttpContext);
		return RQ_NOTIFICATION_CONTINUE;
	}

	MyCustomProvider cp2;
	REQUEST_NOTIFICATION_STATUS
		IisHttpModule::OnSendResponse(IN IHttpContext * pHttpContext, IN ISendResponseProvider * pProvider)
	{
		IisModuleRequestContext* request_context = IisModuleRequestContext::GetRequestContext(pHttpContext);

		if (!request_context){
			log(pHttpContext, "OnSendResponse - no request_context, bail");
			return RQ_NOTIFICATION_CONTINUE;
		}

		// the design record: soft enforcement — do NOT bail on the optimized response
		// path when unlicensed. Optimization always proceeds; the unlicensed
		// state is signalled softly via the "x-pagespeed-warn: unlicensed"
		// header added in IisModuleBaseFetch::CollectHeaders.

		if (request_context->has_nextstatus())
		{			
			auto ret = request_context->nextsendresponsestatus();
			request_context->reset_nextsendresponsestatus();
			return ret;
		}
		auto innerContext = request_context->GetInnerContext();

		if (innerContext == nullptr) {
			log(pHttpContext, "OnSendResponse - no innerContext, bail");
			return RQ_NOTIFICATION_CONTINUE;
		}

		log(pHttpContext, "OnSendResponse: [%s]", innerContext->url());
		innerContext->set_send_response_seen(true);


		//if we get here, we can only be rewriting html
		IHttpResponse * pHttpResponse = pHttpContext->GetResponse();



		if (pHttpResponse == NULL)
		{
			log(pHttpContext, "OnSendResponse - no pHttpResponse, bail");
			DCHECK(false);
			return RQ_NOTIFICATION_CONTINUE;
		}

		bool more_data = (pProvider->GetFlags() & HTTP_SEND_RESPONSE_FLAG_MORE_DATA) != 0;
		bool mx = pHttpResponse->GetHeadersSuppressed();
		HTTP_RESPONSE *pResponseStruct = pHttpResponse->GetRawHttpResponse();
		if (pResponseStruct == NULL)
		{
			log(pHttpContext, "OnSendResponse - no pResponseStruct, bail");
			DCHECK(false);
			return RQ_NOTIFICATION_CONTINUE;
		}

		if (!pHttpContext->GetConnection()->IsConnected()) {
			// stop rewriting for disconnected clients.
			if (innerContext->proxy_fetch() != NULL) {
				log(pHttpContext, "SendResponse -> Connection not connected - bail");
				// proxy_fetch()->Done() can synchronously tear the request down
				// and free innerContext (HandleDone -> IndicateCompletion ->
				// CleanupStoredContext). Clear state first, Done() last, never
				// touch innerContext afterwards. See iis_proxy_fetch_completion.h.
				FinishProxyFetchAndLeave<IisInnerRequestContext, ProxyFetch>(innerContext);
				return RQ_NOTIFICATION_PENDING;
			}
		}


		if (innerContext->is_resource() || innerContext->leave())
		{
			log(pHttpContext, "OnSendResponse --- bail");
			//resources are handled completely in onbeginrequest
			return RQ_NOTIFICATION_CONTINUE;
		}
		else
		{
			PCSTR http_request_method = pHttpContext->GetRequest()->GetHttpMethod();
			if (!(StringCaseEqual(http_request_method, "GET") || StringCaseEqual(http_request_method, "POST")))
			{
				log(pHttpContext, "OnSendResponse - not GET or HEAD, bail");
				innerContext->set_leave(true);
				return RQ_NOTIFICATION_CONTINUE;
			}

			if (innerContext->proxy_fetch() == NULL && !innerContext->end_request_seen())
			{
				USHORT ct_len;
				PCSTR ct = pHttpContext->GetResponse()->GetHeader(HttpHeaderContentType, &ct_len);

				if (ct && ct_len > 0)
				{
					log(pHttpContext, "OnSendResponse --- content-type: [%.*s]", ct_len, ct);
					std::string scontent_type;
					scontent_type.append(ct, ct_len);
					// We don't know what this request is, but we only want to send html through
					// to pagespeed.  Check the content type header and find out.
					const net_instaweb::ContentType* content_type = net_instaweb::MimeTypeToContentType(scontent_type);
					if (content_type != NULL && content_type->IsHtmlLike())
					{
						StringPiece xmicrosoftajax = StringPiece(pHttpContext->GetRequest()->GetHeader("X-MicrosoftAjax"));
						StringPiece xrequestedwith = StringPiece(pHttpContext->GetRequest()->GetHeader("X-Requested-With"));
						StringPiece xprototype = StringPiece(pHttpContext->GetRequest()->GetHeader("X-Prototype-Version"));

						if (xmicrosoftajax.length() > 0 || (xrequestedwith.length() > 0 && StringCaseEqual(xrequestedwith, "XMLHttpRequest")) || xprototype.size() > 0)
						{
							log(pHttpContext, "OnSendResponse --- AJAX request -> no html rewriting.");
						}
						else
						{
							innerContext->set_rewrite_html(true);
						}
					}
				}
				else
				{
					log(pHttpContext, "OnSendResponse --- no content type");
				}

				if (!innerContext->rewrite_html() && innerContext->recorder() == NULL) {
					log(pHttpContext, "OnSendResponse --- no recorder, no html - bail");
					innerContext->set_leave(true);
					return RQ_NOTIFICATION_CONTINUE;
				}

				pHttpResponse->DisableKernelCache();
				auto cp = pHttpResponse->GetCachePolicy();
				auto kcp = cp->GetKernelCachePolicy();
				if (kcp) {
					kcp->Policy = HttpCachePolicyNocache;
					kcp->SecondsToLive = 0;
				}
				auto vbh = cp->GetVaryByHeaders();
				auto vbq = cp->GetVaryByQueryStrings();
				auto vbv = cp->GetVaryByValue();
				auto kcis = cp->GetKernelCacheInvalidatorSet();

				auto ucp = cp->GetUserCachePolicy();
				if (ucp) {
					ucp->Policy = HttpCachePolicyNocache;
				}

				
			}

			// TODO(oschaaF): if first time, and we want to rewrite html, etc
			if (innerContext->rewrite_html() && innerContext->proxy_fetch() == NULL && !innerContext->end_request_seen())
			{
				if (innerContext->recorder() != NULL) {
					innerContext->recorder()->Fail();
					innerContext->recorder()->DoneAndSetHeaders(NULL, false);
					innerContext->set_recorder(NULL);
					// release any pagespeed request pointer we have from IPRO here.
				}

				log(pHttpContext, "OnSendResponse --- Create base fetch for sending HTML into PSOL");
				// TODO(oschaaf): we can create the base fetch later, we just need the request headers.
				// Fix that later
				innerContext->createNewPageSpeedRequestPointer();
				request_context->set_base_fetch(new IisModuleBaseFetch(pHttpContext, FetchType::kHtml, innerContext->process_context()->handler(), innerContext->GetPagespeedRequestContext(), innerContext));

				size_t len = 0;
				for (int c = 0; c < pResponseStruct->EntityChunkCount; c++)
				{
					char * data = NULL;


					HTTP_DATA_CHUNK *currentchunk = pResponseStruct->pEntityChunks + c;
					switch (currentchunk->DataChunkType)
					{
					case HTTP_DATA_CHUNK_TYPE::HttpDataChunkFromMemory:
						len += currentchunk->FromMemory.BufferLength;
						break;
					case HTTP_DATA_CHUNK_TYPE::HttpDataChunkFromFragmentCache:
						len += 10; // should have content
						break;
					case HTTP_DATA_CHUNK_TYPE::HttpDataChunkFromFragmentCacheEx:
						len += 10; // should have content
						break;
					case HTTP_DATA_CHUNK_TYPE::HttpDataChunkFromFileHandle:
						/*auto lng=currentchunk->FromFileHandle.ByteRange.Length.QuadPart;
						if (lng==-1) lng=100;*/

						len += 10; // should have content
						break;
					}

				}
				if (len == 0 && !pProvider->GetHeadersBeingSent())
				{
					log(pHttpContext, "no content,no headers!");
				}

				ResponseHeaders hdrs;
				{
					HTTP_RESPONSE *resp = pResponseStruct;
					for (int i = resp->Headers.UnknownHeaderCount - 1; i >= 0 && resp->Headers.pUnknownHeaders != NULL; i--)
					{
						HTTP_UNKNOWN_HEADER *hdr = &resp->Headers.pUnknownHeaders[i];
						std::string header(hdr->pName);
						if (header == "PageSpeed" ||
							header == "PageSpeedFilters" ||
							header == "PageSpeedCssFlattenMaxBytes" ||
							header == "PageSpeedCssImageInlineMaxBytes" ||
							header == "PageSpeedCssInlineMaxBytes" ||
							header == "PageSpeedImageInlineMaxBytes" ||
							header == "PageSpeedJsInlineMaxBytes"
							)
						{
							hdrs.Add(hdr->pName, hdr->pRawValue);
							pHttpContext->GetResponse()->DeleteHeader(hdr->pName);
						}
					}
				}

				// If null, that means use global options.
				net_instaweb::RewriteOptions* tmp = NULL;
				GoogleString pagespeed_query_params;
				GoogleString pagespeed_option_cookies;

				bool ok = determine_options(pHttpContext, innerContext, innerContext->base_fetch()->request_headers()
					, &hdrs
					//innerContext->base_fetch()->response_headers()
					, &tmp, innerContext->base_fetch()->request_context(), innerContext->gurl(), &pagespeed_query_params, &pagespeed_option_cookies);
				std::unique_ptr<net_instaweb::RewriteOptions> custom_options(tmp);

				if (!ok)
				{
					log(pHttpContext, "determine options failed!");
					innerContext->set_leave(true);
					delete innerContext->base_fetch();
					innerContext->set_base_fetch(NULL);
					return RQ_NOTIFICATION_FINISH_REQUEST;
				}

				PSOCKADDR_IN addr_in = (PSOCKADDR_IN)pHttpContext->GetRequest()->GetLocalAddress();
				GoogleString ip_address = GetIPString((PSOCKADDR)addr_in);

				RewriteOptions* remote_options = ps_determine_remote_options(innerContext->server_context(), ip_address, *innerContext->gurl());
				if (remote_options != nullptr) {
					if (custom_options.get() == nullptr) {
						custom_options.reset(innerContext->server_context()->global_options()->Clone());
					}
					custom_options->Merge(*remote_options);
					delete remote_options;
					remote_options = nullptr;
				}

				net_instaweb::RewriteOptions* options;
				if (custom_options.get() == NULL)
				{
					options = innerContext->server_context()->global_options();

				}
				else
				{
					innerContext->server_context()->ComputeSignature(custom_options.get());
					options = custom_options.get();
				}

				RequestContextPtr ctx = innerContext->GetPagespeedRequestContext();
				ctx->set_options(options->ComputeHttpOptions());
				if (!options->enabled() || !options->IsAllowed(innerContext->gurl()->spec_c_str()))
				{
					log(pHttpContext, "pagespeed disabled");
					delete innerContext->base_fetch();
					innerContext->set_base_fetch(NULL);
					innerContext->set_leave(true);
					return RQ_NOTIFICATION_CONTINUE;
				}
				else {
					
					if (pProvider->GetHeadersBeingSent())
					{
						//remove the transfer-encoding header before sending it to pagespeed,
						//as we will decode it.
						HTTP_KNOWN_HEADER transfer_encoding
							= pHttpContext->GetResponse()->GetRawHttpResponse()->Headers.KnownHeaders[HttpHeaderTransferEncoding];

						HTTP_KNOWN_HEADER content_encoding
							= pHttpContext->GetResponse()->GetRawHttpResponse()->Headers.KnownHeaders[HttpHeaderContentEncoding];

						if (pHttpContext->GetResponse()->GetHeader("X-Page-Speed"))
						{
							log(pHttpContext, "OnSendResponse ---  content is already rewritten, don't rewrite");
							delete innerContext->base_fetch();
							innerContext->set_base_fetch(NULL);
							innerContext->set_leave(true);
							return RQ_NOTIFICATION_CONTINUE;
						}
						USHORT http_status = pHttpContext->GetResponse()->GetRawHttpResponse()->StatusCode;
						if (http_status != 200)
						{
							log(pHttpContext, "OnSendResponse ---  http response status code <> 200, don't rewrite");
							delete innerContext->base_fetch();
							innerContext->set_base_fetch(NULL);
							innerContext->set_leave(true);
							return RQ_NOTIFICATION_CONTINUE;
						}

						if (content_encoding.pRawValue != NULL && content_encoding.RawValueLength > 0)
						{
							char *encoding = new char[content_encoding.RawValueLength + 1];
							strncpy(encoding, content_encoding.pRawValue, content_encoding.RawValueLength);
							encoding[content_encoding.RawValueLength] = 0;

							GzipInflater* inflater = NULL;

							if (StringCaseEqual(StringPiece(encoding), "gzip")) {
								inflater = new GzipInflater(GzipInflater::InflateType::kGzip);
							}
							else if (StringCaseEqual(StringPiece(encoding), "deflate")) {
								inflater = new GzipInflater(GzipInflater::InflateType::kDeflate);
							}

							if (inflater == NULL)
							{
								log(pHttpContext, "OnSendResponse ---  unknown encoding %s", encoding);
								delete[] encoding;
								delete innerContext->base_fetch();
								innerContext->set_base_fetch(NULL);
								
								innerContext->set_leave(true);
								return RQ_NOTIFICATION_CONTINUE;
							}
							else
							{
								log(pHttpContext, "adding inflater for [%s], and removing content encoding header", encoding);
								inflater->Init();
								request_context->set_inflater(inflater);
								pHttpContext->GetResponse()->DeleteHeader("Content-Encoding");
							}
						}

						if (transfer_encoding.pRawValue != NULL || transfer_encoding.RawValueLength > 0)
						{
							innerContext->set_chunked(true);
							pHttpContext->GetResponse()->DeleteHeader("Transfer-Encoding");
						}
						request_context->base_fetch()->PopulateResponseHeaders(pHttpContext, request_context->base_fetch()->response_headers());
					}
				}

				net_instaweb::RewriteDriver* driver;
				bool page_callback_added = false;
				std::unique_ptr<ProxyFetchPropertyCallbackCollector>
					property_callback(
					ProxyFetchFactory::InitiatePropertyCacheLookup(
					false /* is_resource_fetch */,
					*innerContext->gurl(),
					innerContext->server_context(),
					options,
					innerContext->base_fetch()));


				if (custom_options.get() == NULL)
				{
					driver = innerContext->server_context()->NewRewriteDriver(ctx);
				}
				else
				{
					// NewCustomRewriteDriver takes ownership of custom_options.
					driver = innerContext->server_context()->NewCustomRewriteDriver(custom_options.release(), ctx);
				}
				IisModuleBaseFetch* bf = (IisModuleBaseFetch*)innerContext->base_fetch();
				driver->SetRequestHeaders(*innerContext->base_fetch()->request_headers());
#ifdef IISPEEDHASFLUSH
				bf->set_flush_html(driver->options()->flush_html());
#endif
				ProxyFetch* f = innerContext->server_context()->fetch_factory()->CreateNewProxyFetch(
					innerContext->gurl()->Spec().as_string(), innerContext->base_fetch(), driver,
					property_callback.release() /* property_callback */,
					NULL /* original_content_fetch */);

				innerContext->set_proxy_fetch(f);
				log(pHttpContext, "OnSendResponse ---  proxy fetch set - [%p]", innerContext->proxy_fetch());
				} // if (innerContext->proxy_fetch() == NULL && innerContext->base_fetch() != NULL) 



			if (pProvider->GetHeadersBeingSent() && innerContext->recorder() != NULL)
			{
				ResponseHeaders response_headers;
				response_headers.SetDate(innerContext->server_context()->timer()->NowMs());
				IisModuleBaseFetch::PopulateResponseHeaders(pHttpContext, &response_headers);
				GoogleString s;
				StringWriter sw(&s);
				response_headers.WriteAsHttp(&sw, NULL);
				if (response_headers.status_code() == 200) {
					log(pHttpContext, "IPRO->consider response headers: %s", s.c_str());
					innerContext->recorder()->ConsiderResponseHeaders(InPlaceResourceRecorder::HeadersKind::kFullHeaders, &response_headers);
				}
				else {
					// TODO(oschaaf): for 304, we should be able to cancel out the recorder 
					// without updating any stats. Probably goes for status >200 && < 399.
					// E.g. this is a benign failure, which should just be retried on the next 
					// opportunity. No seriuous consequences here AFAICT, so low priority.
					log(pHttpContext, "IPRO-> abort recording due to status <> 200 (%d)", response_headers.status_code());
					innerContext->recorder()->Fail();
					innerContext->recorder()->DoneAndSetHeaders(NULL, false);
					innerContext->set_recorder(NULL);

				}
			}


			log(pHttpContext, "inspect ---  %d chunks", (int)(pResponseStruct->EntityChunkCount));

			for (int c = 0; c < pResponseStruct->EntityChunkCount; c++)
			{
				char * data = NULL;
				size_t len=0;

				HTTP_DATA_CHUNK *currentchunk=pResponseStruct->pEntityChunks+c;
				IisModuleBaseFetch* bf = (IisModuleBaseFetch*) innerContext->base_fetch();
#ifdef IISPEEDHASFLUSH
				if (bf->HasChunk(currentchunk)) {
					continue;
				}
#endif
				log(pHttpContext, "OnSendResponse --- chunk address - [%p]", currentchunk);

				switch (currentchunk->DataChunkType)
				{
				case HttpDataChunkFromMemory:
					data = (char*)currentchunk->FromMemory.pBuffer;
					len = currentchunk->FromMemory.BufferLength;
					break;
				case HttpDataChunkFromFileHandle: {
					HTTP_BYTE_RANGE *pFileByteRange = &currentchunk->FromFileHandle.ByteRange;
					LARGE_INTEGER  lFileSize;
					ULONGLONG ulTotalLength = 0;
					//
					// File chunks may contain by ranges with unspecified length 
					// (HTTP_BYTE_RANGE_TO_EOF).  In order to send parts of such a chunk, 
					// its necessary to know when the chunk is finished, and
					// we need to move to the next chunk.
					//              
					if (pFileByteRange->Length.QuadPart == HTTP_BYTE_RANGE_TO_EOF)
					{
						if (GetFileType(currentchunk->FromFileHandle.FileHandle) ==
							FILE_TYPE_DISK)
						{
							if (!GetFileSizeEx(currentchunk->FromFileHandle.FileHandle,
								&lFileSize))
							{
								CHECK(false) << "Could not determine file size";
								return RQ_NOTIFICATION_CONTINUE;

								//								DWORD dwError = GetLastError();
								//								hr = HRESULT_FROM_WIN32(dwError);
								//								goto Finished;
							}

							// put the resolved file length in the chunk, replacing 
							// HTTP_BYTE_RANGE_TO_EOF
							pFileByteRange->Length.QuadPart =
								lFileSize.QuadPart - pFileByteRange->StartingOffset.QuadPart;
						}
						else
						{
							CHECK(false) << "Could not determine file type";
							return RQ_NOTIFICATION_CONTINUE;
							//hr = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
							//goto Finished;                        
						}
					}

					ulTotalLength = pFileByteRange->Length.QuadPart;
					data = (char*)pHttpContext->AllocateRequestMemory(ulTotalLength);
					len = ulTotalLength;
					log(pHttpContext, "read file chunk");

					if (ReadFileChunk(currentchunk, data) != S_OK)
					{
						CHECK(false) << "Failed to read file chunk";
						return RQ_NOTIFICATION_CONTINUE;

						//DWORD dwErr = GetLastError();
						//hr = HRESULT_FROM_WIN32(dwErr);
						//goto Finished;
					}
					log(pHttpContext, "done reading file chunk");

				} break;

				default:
					// TODO(oschaaf):
					CHECK(false);
					continue;
				}

				if (len > 0)
				{
					Dechunker *dechunker = innerContext->get_dechunker(); // dechunker does pass-through when not in chu nk mode
					dechunker->FeedChunk(data, len);
					while (dechunker->ParseNext())
					{
						if (dechunker->HasData())
						{
							size_t datablocklength;
							char *datablock;
							dechunker->GetDataBlock(&datablock, &datablocklength);
							if (datablocklength)
							{
								log(pHttpContext, "send chunk [%p] of %ld bytes into psa", datablock, datablocklength);

								//request_context->base_fetch()->SetLastWrite(GetTickCount());
								GzipInflater* inflater = request_context->inflater();
								if (inflater == NULL)
								{
									if (innerContext->proxy_fetch() != NULL) {
										innerContext->proxy_fetch()->Write(StringPiece(datablock, datablocklength), innerContext->process_context()->handler());
									}
									if (innerContext->recorder() != NULL) {
										innerContext->recorder()->Write(StringPiece(datablock, datablocklength), innerContext->process_context()->handler());
									}
								}
								else
								{
									char buf[net_instaweb::kStackBufferSize];
									inflater->SetInput(datablock, datablocklength);

									while (inflater->HasUnconsumedInput())
									{
										int num_inflated_bytes = inflater->InflateBytes(buf, net_instaweb::kStackBufferSize);
										if (num_inflated_bytes < 0)
										{
											innerContext->process_context()->handler()->Message(net_instaweb::kWarning, "Corrupted inflation");
										}
										else if (num_inflated_bytes > 0)
										{
											if (innerContext->proxy_fetch() != NULL) {
												innerContext->proxy_fetch()->Write(StringPiece(buf, num_inflated_bytes), innerContext->process_context()->handler());
											}
											if (innerContext->recorder() != NULL) {
												innerContext->recorder()->Write(StringPiece(buf, num_inflated_bytes), innerContext->process_context()->handler());
											}

										}
									}

								}
							}
								}
							}
#ifdef IISPEEDHASFLUSH
					innerContext->proxy_fetch()->Flush(innerContext->process_context()->handler());
#endif
						}
				else
				{
					log(pHttpContext, "0 length chunk received");
				}
					} //for (int c=0;c<pResponseStruct->EntityChunkCount;c++)


			if (!more_data && innerContext->recorder() != NULL)  {
				ResponseHeaders* response_headers = new ResponseHeaders();
				response_headers->SetDate(innerContext->server_context()->timer()->NowMs());
				IisModuleBaseFetch::PopulateResponseHeaders(pHttpContext, response_headers);
				GoogleString s;
				StringWriter sw(&s);
				response_headers->WriteAsHttp(&sw, NULL);
				log(pHttpContext, "IPRO recorded. Final response headers: %s", s.c_str());
				innerContext->recorder()->DoneAndSetHeaders(response_headers, true);
				innerContext->set_recorder(NULL);
			}

			if (innerContext->proxy_fetch() != NULL)
			{
				if (true)
				{
					IisModuleBaseFetch* bf = (IisModuleBaseFetch*)innerContext->base_fetch();
					log(pHttpContext, "Clearing response");
					pHttpResponse->Clear();
					// oschaaf: not sure if this adds any value:
					pResponseStruct->EntityChunkCount=0;
#ifdef IISPEEDHASFLUSH
					if (bf->need_flush() ) { 
						log(pHttpContext, "Need flush");
						DWORD bytessent=0;
						bf->SendData(innerContext->process_context()->handler(), true);
						pHttpResponse->Flush(false,true,&bytessent);
						bf->set_need_flush(false);
					}
					else
					{
						log(pHttpContext, "Do not need flush");
					}
#endif
				}
				else
					log(pHttpContext, "Not clearing response");

				// oschaaf: when iis decides to sent the headers, but pagespeed hasn't indicated they are complete yet, 
				// this hack force pulls them, and sends that out. 
				// that is the best we can do. we should probably disable certain filters in this case (move_meta_tags_to_head)
				if (pProvider->GetHeadersBeingSent() && !request_context->base_fetch()->headers_collected())
				{
					log(pHttpContext, "force collect headers");
					IisModuleBaseFetch* bf = (IisModuleBaseFetch*)innerContext->base_fetch();
					if (true || bf->get_flush_html()) { // if we want to flush, buffering needs to be turned off						
						log(pHttpContext, "OnSendResponse --- disable buffering [%p]", innerContext->proxy_fetch());
						pHttpContext->GetResponse()->DisableBuffering();
					}
					request_context->base_fetch()->CollectHeaders();
				}

				if (!more_data)/*not chunked, seen final chunk*/
				{
					log(pHttpContext, "OnSendResponse: call proxy_fetch->Done(), return pending");
					// set_pending() must precede Done(): it reads innerContext's
					// base_fetch, and Done() can synchronously free innerContext.
					request_context->base_fetch()->set_pending(true);
					// proxy_fetch()->Done() can synchronously run the request
					// teardown (HandleDone -> IndicateCompletion ->
					// CleanupStoredContext) which deletes innerContext. Clear state
					// first, Done() last, and touch neither innerContext nor
					// request_context afterwards. See iis_proxy_fetch_completion.h.
					FinishProxyFetchAndLeave<IisInnerRequestContext, ProxyFetch>(innerContext);
					return RQ_NOTIFICATION_PENDING;
				}
				else {
					CHECK(!innerContext->leave());
					log(pHttpContext, "expecting more data");
				}
				/*else
				return RQ_NOTIFICATION_PENDING;*/
			} //proxy_fetch != NULL
				} // !innerContext->is_resource() && !innerContext->leave()


		return RQ_NOTIFICATION_CONTINUE;
			}

	REQUEST_NOTIFICATION_STATUS
		IisHttpModule::OnAsyncCompletion(
		IN IHttpContext * pHttpContext,
		IN DWORD dwNotification,
		IN BOOL fPostNotification,
		IN IHttpEventProvider * pProvider,
		IN IHttpCompletionInfo * pCompletionInfo
		)
	{
		// Zero-copy aliased serve (CycloneZeroCopyServe): the per-chunk
		// async flush completions of the resource serve land here (the
		// request is parked in RQ_BEGIN_REQUEST -- hence the notification
		// check, a cheap guard against any spurious completion from a
		// different pipeline stage); route them to the serve driver.  The
		// base fetch is guaranteed alive: the inner request context holds
		// a reference until CleanupStoredContext, which cannot run while a
		// notification/completion is outstanding.
		if (dwNotification == RQ_BEGIN_REQUEST)
		{
			IisModuleRequestContext* request_context =
			    IisModuleRequestContext::GetRequestContext(pHttpContext);
			if (request_context != NULL &&
			    request_context->GetInnerContext() != NULL)
			{
				IisModuleBaseFetch* bf = request_context->base_fetch();
				if (bf != NULL && bf->HasActiveZeroCopyServe())
				{
					HRESULT completion_status =
					    (pCompletionInfo != NULL)
					        ? pCompletionInfo->GetCompletionStatus()
					        : S_OK;
					return bf->OnZeroCopyCompletion(completion_status);
				}
			}
		}
		if (NULL != pCompletionInfo)
		{
			// Create strings for completion information.
			char szNotification[256] = "";
			char szBytes[256] = "";
			char szStatus[256] = "";

			// Retrieve and format the completion information.
			sprintf_s(szNotification, 255, "Notification: %u",
				dwNotification);
			sprintf_s(szBytes, 255, "Completion Bytes: %u",
				pCompletionInfo->GetCompletionBytes());
			sprintf_s(szStatus, 255, "Completion Status: 0x%08x",
				pCompletionInfo->GetCompletionStatus());
			// Create an array of strings.
			LPCSTR szBuffer[3] = { szNotification, szBytes, szStatus };
			log(pHttpContext, "%s %s %s", szBuffer[0], szBuffer[1], szBuffer[2]);
			
			
			return RQ_NOTIFICATION_CONTINUE;
			//CHECK(false);

			// Write the strings to the Event Viewer.
		}

		// Return processing to the pipeline.
		return RQ_NOTIFICATION_CONTINUE;
	}

	

	HRESULT IisHttpModule::ReadFileChunk(HTTP_DATA_CHUNK *chunk, char *buf)
	{
		OVERLAPPED ovl;
		DWORD dwDataStartOffset;
		ULONGLONG bytesTotal = 0;
		ULONGLONG bytesToRead = chunk->FromFileHandle.ByteRange.Length.QuadPart;
		BYTE *	pIoBuffer = NULL;
		HANDLE	hIoEvent = INVALID_HANDLE_VALUE;
		HRESULT hr = S_OK;

		pIoBuffer = (BYTE *)VirtualAlloc(NULL,
			1,
			MEM_COMMIT | MEM_RESERVE,
			PAGE_READWRITE);
		if (pIoBuffer == NULL)
		{
			hr = HRESULT_FROM_WIN32(GetLastError());
			goto Done;
		}

		hIoEvent = CreateEvent(NULL,  // security attr
			FALSE, // manual reset
			FALSE, // initial state
			NULL); // name
		if (hIoEvent == NULL)
		{
			hr = HRESULT_FROM_WIN32(GetLastError());
			goto Done;
		}
		DWORD m_dwPageSize = 4096;//Replace with call to  process_context()->page_size();

		while (bytesTotal < chunk->FromFileHandle.ByteRange.Length.QuadPart)
		{
			DWORD bytesRead = 0;
			int was_eof = 0;
			ULONGLONG offset = chunk->FromFileHandle.ByteRange.StartingOffset.QuadPart + bytesTotal;

			ZeroMemory(&ovl, sizeof ovl);
			ovl.hEvent = hIoEvent;
			ovl.Offset = (DWORD)offset;
			dwDataStartOffset = ovl.Offset & (m_dwPageSize - 1);
			ovl.Offset &= ~(m_dwPageSize - 1);
			ovl.OffsetHigh = offset >> 32;

			if (!ReadFile(chunk->FromFileHandle.FileHandle,
				pIoBuffer,
				m_dwPageSize,
				&bytesRead,
				&ovl))
			{
				DWORD dwErr = GetLastError();

				switch (dwErr)
				{
				case ERROR_IO_PENDING:
					//
					// GetOverlappedResult can return without waiting for the
					// event thus leaving it signalled and causing problems
					// with future use of that event handle, so just wait ourselves
					//
					WaitForSingleObject(ovl.hEvent, INFINITE); // == WAIT_OBJECT_0);

					if (!GetOverlappedResult(
						chunk->FromFileHandle.FileHandle,
						&ovl,
						&bytesRead,
						TRUE))
					{
						dwErr = GetLastError();

						switch (dwErr)
						{
						case ERROR_HANDLE_EOF:
							was_eof = 1;
							break;

						default:
							hr = HRESULT_FROM_WIN32(dwErr);
							goto Done;
						}
					}
					break;

				case ERROR_HANDLE_EOF:
					was_eof = 1;
					break;

				default:
					hr = HRESULT_FROM_WIN32(dwErr);
					goto Done;
				}
			}

			bytesRead -= dwDataStartOffset;

			if (bytesRead > chunk->FromFileHandle.ByteRange.Length.QuadPart)
			{
				bytesRead = (DWORD)chunk->FromFileHandle.ByteRange.Length.QuadPart;
			}
			if (bytesToRead < (bytesTotal + bytesRead)) { bytesRead = bytesToRead - bytesTotal; }
			memcpy(buf, pIoBuffer + dwDataStartOffset, bytesRead);

			buf += bytesRead;
			bytesTotal += bytesRead;

			if (was_eof != 0)
				chunk->FromFileHandle.ByteRange.Length.QuadPart = bytesToRead;
		}

	Done:
		if (NULL != pIoBuffer)
		{
			VirtualFree(pIoBuffer, 0, MEM_RELEASE);
		}

		if (INVALID_HANDLE_VALUE != hIoEvent)
		{
			CloseHandle(hIoEvent);
		}

		return hr;
	}
	/*::ReadFileChunk(HTTP_DATA_CHUNK *chunk, char *buf)
	{
	OutputDebugStringA("ReadFileChunk!");

	OVERLAPPED ovl;
	DWORD dwDataStartOffset;
	ULONGLONG bytesTotal = 0;
	BYTE *	pIoBuffer = NULL;
	HANDLE	hIoEvent = INVALID_HANDLE_VALUE;
	HRESULT hr = S_OK;

	hIoEvent = CreateEvent(NULL,  // security attr
	FALSE, // manual reset
	FALSE, // initial state
	NULL); // name
	if (hIoEvent == NULL)
	{
	hr = HRESULT_FROM_WIN32(GetLastError());
	goto Done;
	}


	DWORD m_dwPageSize = process_context()->page_size();

	ULONGLONG offset = chunk->FromFileHandle.ByteRange.StartingOffset.QuadPart;
	ULONGLONG toreadtotal= chunk->FromFileHandle.ByteRange.Length.QuadPart;


	while(toreadtotal>0)
	{
	DWORD bytesRead = 0;
	DWORD toRead=m_dwPageSize; // get this with
	GetFileInformationByHandleEx(chunk->FromFileHandle.FileHandle
	if (toreadtotal<toRead) toRead=toreadtotal;
	int was_eof = 0;
	ZeroMemory(&ovl, sizeof ovl);
	ovl.hEvent     = hIoEvent;
	ovl.Offset = (DWORD)offset;
	ovl.OffsetHigh = offset >> 32;



	if (!ReadFile(chunk->FromFileHandle.FileHandle,
	buf,
	toRead,
	NULL,
	&ovl))
	{
	DWORD dwErr = GetLastError();


	switch (dwErr)
	{
	case ERROR_IO_PENDING:
	//
	// GetOverlappedResult can return without waiting for the
	// event thus leaving it signalled and causing problems
	// with future use of that event handle, so just wait ourselves
	//
	WaitForSingleObject(ovl.hEvent, INFINITE); // == WAIT_OBJECT_0);

	if (!GetOverlappedResult(
	chunk->FromFileHandle.FileHandle,
	&ovl,
	&bytesRead,
	TRUE))
	{
	dwErr = GetLastError();

	switch(dwErr)
	{
	case ERROR_HANDLE_EOF:
	was_eof = 1;
	break;

	default:
	hr = HRESULT_FROM_WIN32(dwErr);
	goto Done;
	}
	}
	break;

	case ERROR_HANDLE_EOF:
	was_eof = 1;
	break;

	default:
	hr = HRESULT_FROM_WIN32(dwErr);
	goto Done;
	}
	}

	buf+=bytesRead;
	toreadtotal-=bytesRead;
	offset+=bytesRead;

	if(was_eof != 0)
	{
	chunk->FromFileHandle.ByteRange.Length.QuadPart = bytesTotal;
	break;
	}
	}

	Done:

	if(INVALID_HANDLE_VALUE != hIoEvent)
	{
	CloseHandle(hIoEvent);
	}

	return hr;
	}*/



	void IisHttpModule::log(IHttpContext* c, const char * msg, ...)
	{

		if (REDUCE_LOG)
			return;

		int lvl = 0;
		IHttpContext * p = c;
		while (p != p->GetRootContext())
		{
			lvl++;
			p = p->GetParentContext();
		}

		std::string tabs;
		while (lvl > 0) {
			tabs.append("    ");
			--lvl;
		}
		char *m = new char[1024 * 32];
		va_list vl;
		va_start(vl, msg);
		vsnprintf(m, 1024*32, msg, vl);
		va_end(vl);

		module_creator_->message_handler()->Message(net_instaweb::kInfo, "%s[%p]: %s", tabs.c_str(), c, m);
		delete[] m;
	}






	REQUEST_NOTIFICATION_STATUS
		IisHttpModule::OnCustomRequestNotification(
		IN IHttpContext * pHttpContext,
		IN ICustomNotificationProvider * pProvider
		)
	{
		Sleep(1000);
		log(pHttpContext, "custom!!");
		return RQ_NOTIFICATION_CONTINUE;
	}

	IisModuleRequestContext* IisHttpModule::CreateRequestContext(IHttpContext *pHttpContext, std::string site_root, char *url, int url_len, GoogleUrl *gurl, IisProcessContext** failed_pc)
	{
		if (failed_pc != nullptr) {
			*failed_pc = nullptr;
		}
		GoogleString site_app_id = ws2s(pHttpContext->GetApplication()->GetApplicationId());
		GoogleString id;
		int slashes = 0;
		for (size_t i = 0; i < site_app_id.size(); i++) {
			if (site_app_id[i] == '/') {
				slashes++;
				if (slashes > 3) {
					id.push_back('.');
				}
			}
			else {
				if (slashes > 2) {
					id.push_back(site_app_id[i]);
				}
			}
		}

		GoogleString path = FindConfigFile(ws2s(pHttpContext->GetApplication()->GetApplicationPhysicalPath()));
		auto process_context = this->process_context(id, site_root, path);
		if (!process_context) {
			// No process context — module-factory failed to even allocate
			// one. Leave failed_pc null; caller falls back to the generic
			// "startup-failed" page.
			return NULL;
		}
		if (!process_context->ok()) {
			log(pHttpContext, "process_context->ok() returned false");
			if (failed_pc != nullptr) {
				*failed_pc = process_context;
			}
			return NULL;
		}

		auto server_context = process_context->GetServerContext(id, gurl->Host().as_string().c_str(), gurl->EffectiveIntPort(),
			path.c_str());
		if (server_context == NULL) {
			log(pHttpContext, "process_context->GetServerContext() returned null");
			// GetServerContext sets init_failure_kind_ on the diagnostic
			// failure paths (PostConfig / cache path); surface that.
			if (failed_pc != nullptr) {
				*failed_pc = process_context;
			}
			return NULL;
		}


		bool is_resource = server_context->IsPagespeedResource(*gurl);

		PSOCKADDR_IN addr_in = (PSOCKADDR_IN)pHttpContext->GetRequest()->GetLocalAddress();
		GoogleString ip_address = GetIPString((PSOCKADDR)addr_in);

		// TODO:!!!!! is_resource => kHtml, etc?
		IisModuleRequestContext* ctx = new IisModuleRequestContext(pHttpContext, is_resource, url, url_len, gurl, process_context, server_context,
			(int)ntohs(addr_in->sin_port), ip_address);
		return ctx;

	}

}