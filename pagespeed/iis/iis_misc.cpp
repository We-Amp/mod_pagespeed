// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// TODO(oschaaf): cleanup header mess in here and in misc.h.
#include "pagespeed/iis/iis_misc.h"

#include <string>

#include <algorithm>
#include <functional>
#include <cctype>
#include <locale>
#include <vector>



#include "pagespeed/iis/iis_global_constants.h"

#include "pagespeed/kernel/base/string_util.h"     
#include "net/instaweb/rewriter/public/experiment_matcher.h"
#include "net/instaweb/rewriter/public/experiment_util.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/rewrite_query.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/http/google_url.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/time_util.h"
#include "pagespeed/kernel/base/std_timer.h"


#include "pagespeed/iis/iis_request_context.h"
#include "pagespeed/iis/iis_configuration.h"
#include "pagespeed/iis/iis_config_util.h"
#include "pagespeed/iis/iis_server_context.h"

#include "Shlobj.h"

namespace net_instaweb 
{
	// ugly, quick fix
	//extern HMODULE currentModule=NULL;
	extern std::string modulePath;
	extern std::string moduleFilename;
	extern ConfigFactory *cf;


	void ltrim_if(GoogleString& s, int(*fp) (int)) {
		for (size_t i = 0; i < s.size();) {
			if (fp(s[i])) {
				s.erase(i, 1);
			}
			else  {
				break;
			}
		}
	}

	void rtrim_if(GoogleString& s, int(*fp) (int)) {
		for (size_t i = (size_t)s.size() - 1; i >= 0; i--) {
			if (fp(s[i])) {
				s.erase(i, 1);
			}
			else  {
				break;
			}
		}
	}

	void trim_if(GoogleString& s, int(*fp) (int)) {
		ltrim_if(s, fp);
		rtrim_if(s, fp);
	}

	int notalnum(int c) {
		return !isalnum(c);
	}


	std::wstring s2ws(const std::string& s)
	{
		int slength = (int)s.length() + 1;
		int len = MultiByteToWideChar(CP_ACP, 0, s.c_str(), slength, 0, 0);
		if (len <= 0) return std::wstring();
		std::wstring r(len - 1, L'\0');
		MultiByteToWideChar(CP_ACP, 0, s.c_str(), slength, &r[0], len);
		return r;
	}

	std::string ws2s(const std::wstring& s)
	{
		int slength = (int)s.length() + 1;
		int len = WideCharToMultiByte(CP_ACP, 0, s.c_str(), slength, 0, 0, 0, 0);
		if (len <= 0) return std::string();
		std::string r(len - 1, '\0');
		WideCharToMultiByte(CP_ACP, 0, s.c_str(), slength, &r[0], len, 0, 0);
		return r;
	}


	// Wrapper around GetQueryOptions()
	RewriteOptions* ps_determine_request_options(
		IisInnerRequestContext* ctx,
		const RewriteOptions* domain_options, /* may be null */
		RequestHeaders* request_headers,
		ResponseHeaders* response_headers,
		RequestContextPtr request_context,
		ServerContext* server_context,
		GoogleUrl* url,
		GoogleString* pagespeed_query_params,
		GoogleString* pagespeed_option_cookies) {
		// Sets option from request headers and url.
		RewriteQuery rewrite_query;
		if (!server_context->GetQueryOptions(
			request_context, domain_options, url, request_headers,
			response_headers, &rewrite_query)) {
			// Failed to parse query params or request headers.  Treat this as if there
			// were no query params given.
			//ngx_log_error(NGX_LOG_INFO, r->connection->log, 0,
			//	"ps_route request: parsing headers or query params failed.");
			return NULL;
		}

		*pagespeed_query_params =
			rewrite_query.pagespeed_query_params().ToEscapedString();
		*pagespeed_option_cookies =
			rewrite_query.pagespeed_option_cookies().ToEscapedString();

		// Will be NULL if there aren't any options set with query params or in
		// headers.
		return rewrite_query.ReleaseOptions();
	}
	
	bool set_experiment_state_and_cookie(IHttpContext * pHttpContext, IisInnerRequestContext* ctx, RequestHeaders* request_headers,
		net_instaweb::RewriteOptions* options,
		const StringPiece& host)
	{
		CHECK(options->running_experiment());
		if (ctx->experiment_classified()) {
			return true;
		}
		ctx->set_experiment_classified(true);
		
		bool need_cookie = ctx->server_context()->experiment_matcher()->ClassifyIntoExperiment(*request_headers, *ctx->server_context()->user_agent_matcher(), options);

		if (need_cookie && host.length() > 0) {
			StdTimer posix_timer;
			int64 time_now_us = posix_timer.NowUs();
			int64 expiration_time_ms = (time_now_us/1000 +
										options->experiment_cookie_duration_ms());

			int state = options->experiment_id();
			GoogleString expires;
			net_instaweb::ConvertTimeToString(expiration_time_ms, &expires);
			GoogleString value = absl::StrCat(
				net_instaweb::experiment::kExperimentCookie,
				"=", net_instaweb::experiment::ExperimentStateToCookieString(state),
				"; Expires=", expires,
				"; Domain=.", host.as_string(),
				"; Path=/");

			pHttpContext->GetResponse()->SetHeader("Set-Cookie", value.c_str(), value.length(), false);
		  }
		  return true;

	}
	

	// TODO(oschaaf): now that determine_options requires the pHttpContext,
	// it doesn't make sense to place it here in a file in /shared anymore
	// remove it, or refactor it.
	 bool determine_options(
						 IHttpContext * pHttpContext,
						 IisInnerRequestContext* ctx,
						 net_instaweb::RequestHeaders* request_headers,
						 net_instaweb::ResponseHeaders* response_headers,
						 net_instaweb::RewriteOptions** options,
						 net_instaweb::RequestContextPtr request_context,
						 net_instaweb::GoogleUrl* url,
						 GoogleString* pagespeed_query_params,
						 GoogleString* pagespeed_option_cookie) {
	IisServerContext* server_context = ctx->server_context();

	  const net_instaweb::RewriteOptions* global_options =
		  //ctx->process_context()->root_options();
	  // Global options for this server.  Never null.
	  // global options will be root config
		  server_context->global_options();

	  // Directory-specific options, usually null.  They've already been rebased off
	  // of the global options as part of the configuration process.
	  //net_instaweb::RewriteOptions* directory_options = cfg_l->options;

	  // Request-specific options, nearly always null.  If set they need to be
	  // rebased on the directory options or the global options.
	  net_instaweb::RewriteOptions* request_options = 
		  ps_determine_request_options(ctx, NULL, request_headers, response_headers, request_context,
		  server_context, url, pagespeed_query_params, pagespeed_option_cookie);

	  // Because the caller takes memory ownership of any options we return, the
	  // only situation in which we can avoid allocating a new RewriteOptions is if
	  // the global options are ok as are.
	  if (/*directory_options == NULL*/ request_options == NULL &&
		  !global_options->running_experiment()) {
		//*options = NULL;
		//return true;
	  }

	  // Config resolution: the SAME shared precedence
	  // chain the factory and the engage gate use — %ProgramData% base
	  // (tiers 1/2, canonical-first) then the per-site override (tier 3),
	  // existing files only, layered in list order so the per-site file wins
	  // (ConfigFactory::GetConfig applies each in order). This replaces the
	  // hand-copied %ProgramData% probe that used to live here and could drift
	  // from the factory's copy.
	  std::string site_physical =
		  ws2s(pHttpContext->GetApplication()->GetApplicationPhysicalPath());
	  std::vector<std::string> resolved =
		  iis_config_util::ResolveConfigPaths(site_physical);
	  if (!resolved.empty())
	  {
		  *options=global_options->Clone();
		  std::map<std::string,std::string> input;
		  auto path=url->PathSansQuery();
		  auto fullpath=url->PathAndLeaf();
		  auto hostname=url->Host();


		  /* this is what we will match, add IP and other data please */
		  input["path"]=std::string(path.data(),path.length());
		  input["fullpath"]=std::string(fullpath.data(),fullpath.length());
		  input["hostname"]=std::string(hostname.data(),hostname.length());
		  input["config"]=std::string("request");

		  /* this is where we get our options: base first, per-site override last */
		  std::list<std::string> paths(resolved.begin(), resolved.end());
		  cf->GetConfig(paths,input,**options,server_context->message_handler(), NULL);
	  }
	  else
		  *options = global_options->Clone(); // get global options

	  //delete [] wFileName;
	  

	  // Modify our options in response to request options or experiment settings,
	  // if we need to.  If there are request options then ignore the experiment
	  // because we don't want experiments to be contaminated with unexpected
	  // settings.
	  bool ok = true;
	  if (request_options != NULL) {
		(*options)->Merge(*request_options);
		delete request_options;
		request_options=NULL;
	  } else if ((*options)->running_experiment()) {
		ok = set_experiment_state_and_cookie(pHttpContext, ctx, request_headers, *options, url->Host());
		if (!ok) {
		  if (*options != NULL) {
			delete *options;
			*options = NULL;
		  }
		  return false;
		}
	  }
	   
	  return true;
	}
	
	GoogleString getComputerName()
	{
		char buffer[MAX_COMPUTERNAME_LENGTH + 1];
		DWORD len = MAX_COMPUTERNAME_LENGTH + 1;
		if (GetComputerNameA(buffer, &len))
			return GoogleString(buffer, len);
		return GoogleString("UNKNOWN");
	}

	RewriteOptions* ps_determine_remote_options(ServerContext* server_context, const GoogleString& in_ip, const GoogleUrl& url) {
		if (!server_context || !server_context->global_options()) {
			return NULL;
		}
		RewriteOptions* customized_options = nullptr;

		if (!server_context->global_options()->remote_configuration_url().empty()) {
			RewriteOptions* remote_options = server_context->global_options()->Clone();
			// This fetch is blocking for up to remote_configuration_timeout_ms ms.
			server_context->GetRemoteOptions(remote_options, false);
			return remote_options;
		}

		return NULL;
	}


	//todo: efficiency
	GoogleString size_t_to_str(size_t i)
	{
		std::stringstream ss;
		ss << i;
		GoogleString tmp;
		ss >> tmp;
		return tmp;
	}

	HRESULT WriteResponseMessage(IHttpContext * pHttpContext, PVOID buf, size_t len, bool is_last)
	{
		HRESULT hr;
		HTTP_DATA_CHUNK dataChunk;
		dataChunk.DataChunkType = HttpDataChunkFromMemory;
		DWORD cbSent;

		dataChunk.FromMemory.pBuffer =(PVOID) buf;
		dataChunk.FromMemory.BufferLength =(ULONG) len;
	
		hr = pHttpContext->GetResponse()->WriteEntityChunks(
			&dataChunk,1,FALSE,!is_last,&cbSent);

		if (FAILED(hr))
		{
			return hr;
		}

		return S_OK;
	}
}
