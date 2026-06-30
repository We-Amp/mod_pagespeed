#include "pagespeed/iis/iis_module_base_fetch.h"
#include "pagespeed/iis/iis_global_constants.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "pagespeed/kernel/http/request_headers.h"
#include "pagespeed/system/in_place_resource_recorder.h"
#include "pagespeed/system/system_rewrite_options.h"
#include "net/instaweb/rewriter/public/rewrite_stats.h"
#include "pagespeed/kernel/base/string_writer.h"

//#include "pagespeed/kernel/base/google_message_handler.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_query.h"
#include "pagespeed/iis/iis_message_handler.h"
#include "pagespeed/iis/iis_misc.h"
#include "pagespeed/iis/iis_server_context.h"
#include <algorithm>    // std::transform


namespace net_instaweb {

volatile LONG IisModuleBaseFetch::fetchount = 0;

IisModuleBaseFetch::IisModuleBaseFetch(IHttpContext* http_context, FetchType fetch_type, MessageHandler* handler,const RequestContextPtr &ptr, IisInnerRequestContext* ctx)
    : IisBaseFetch(ptr)
	,http_context_(http_context)
	
	, fetch_type_(fetch_type)
	, keep_alive_(false)
	, headers_collected_(false)
	, need_flush_(false)
	, chunk_send_index_(0)
	, pending_(false)
	, alive(0x12345678)
	, flush_html_(false)
	, ctx_(ctx)
	, handler_(handler)
	
{
	lastwritetime=0;
	donetime=0;
	currentblock=0;
	currentblockpos=NULL;
	currentblocksize=0;
	PopulateRequestHeaders(http_context_, request_headers());
}

IisModuleBaseFetch::~IisModuleBaseFetch() 
{
	for (size_t i = 0; i < chunks_.size(); i++)
	{
		delete chunks_[i];
	}
	chunks_.clear();
}

//WEAMPKS: 1.9 now need this local
const char* kPassThroughRequestAttributes[10] = {
	HttpAttributes::kIfModifiedSince,
	HttpAttributes::kReferer,
	HttpAttributes::kUserAgent,
	HttpAttributes::kAccept,
	HttpAttributes::kCookie,
	// Content-Type is needed by AdminLicenseHandler's CSRF gate
	// (pagespeed/system/admin_license_handler.cc): mutation endpoints
	// require Content-Type: application/json. Without forwarding it
	// here, the gate rejects every license POST regardless of what
	// the browser sent.
	HttpAttributes::kContentType,
	// Note: These headers are listed so that the headers we see contain them,
	// but should immediately be detected and removed by RewriteQuery::Scan().
	RewriteQuery::kModPagespeed,
	RewriteQuery::kPageSpeed,
	RewriteQuery::kModPagespeedFilters,
	RewriteQuery::kPageSpeedFilters
};


void IisModuleBaseFetch::PopulateRequestHeaders(IHttpContext* http_context, RequestHeaders* request_headers) 
{
	
	USHORT major,minor;
	http_context->GetRequest()->GetHttpVersion(&major,&minor);
	request_headers->set_major_version(major);
	request_headers->set_minor_version(minor);

	//WEAMPKS: 1.9 kPassThroughRequestAttributes is now local
	// TODO(oschaaf): 1.9 -> need to make sure, but I think we should now pass all of them.
	int n = arraysize(kPassThroughRequestAttributes);                                                                                                                                                 
	for (int i = 0; i < n; ++i) 
	{
		PCSTR val = http_context->GetRequest()->GetHeader( kPassThroughRequestAttributes[i] );
		if (val != NULL)
		{
			request_headers->Add(kPassThroughRequestAttributes[i], val);
		}
	}

	// Pass through all PageSpeed/ModPagespeed option headers (not just the whitelist).
	// Also pass through X-Requested-With, which AdminLicenseHandler's CSRF gate
	// requires on mutation endpoints. X-Requested-With is an HTTP Unknown Header
	// in IIS (not in HTTP_HEADER_ID), so it falls into pUnknownHeaders rather
	// than KnownHeaders.
	HTTP_REQUEST_HEADERS& reqHeaders =
	    http_context->GetRequest()->GetRawHttpRequest()->Headers;
	for (USHORT i = 0; i < reqHeaders.UnknownHeaderCount; i++) {
		HTTP_UNKNOWN_HEADER& hdr = reqHeaders.pUnknownHeaders[i];
		StringPiece name(hdr.pName, hdr.NameLength);
		if (name.starts_with("PageSpeed") || name.starts_with("ModPagespeed") ||
		    StringCaseEqual(name, HttpAttributes::kXRequestedWith)) {
			request_headers->Add(name, StringPiece(hdr.pRawValue, hdr.RawValueLength));
		}
	}

	/*PCSTR val = http_context->GetRequest()->GetHeader( "Cookie" );
	if (val != NULL)
	{
		request_headers->Add("Cookie", val);
	}*/
}

void IisModuleBaseFetch::PopulateResponseHeaders(IHttpContext* http_context, ResponseHeaders* response_headers) 
{	
	HTTP_RESPONSE *resp  = http_context->GetResponse()->GetRawHttpResponse();
	if ( http_context->GetResponse()->GetRawHttpResponse()->StatusCode == 200) {
		response_headers->set_reason_phrase("OK");
	}
	response_headers->set_major_version(	resp->Version.MajorVersion);
	response_headers->set_minor_version(	resp->Version.MinorVersion);

	int i;

	for (i = 0; i < HttpHeaderResponseMaximum; i++)
	{
		HTTP_KNOWN_HEADER *hdr = &resp->Headers.KnownHeaders[i];
		/*if (i==HttpHeaderTransferEncoding) continue;
		if (i==HttpHeaderContentEncoding && hdr->RawValueLength>0)
		{
			continue;
		}*/
		if ( hdr->RawValueLength > 0 )
		{
			response_headers->Add(HTTP_HEADER_ID_STRINGS_RESPONSE[i], hdr->pRawValue );
			if (HTTP_HEADER_ID_STRINGS_RESPONSE_CACHE[i]!=NULL) // for safe keeping, we might need the cache headers
				response_headers->Add(HTTP_HEADER_ID_STRINGS_RESPONSE_CACHE[i], hdr->pRawValue);
		}
	}

	for (i = 0; i < resp->Headers.UnknownHeaderCount; i++)
	{
		HTTP_UNKNOWN_HEADER *hdr = &resp->Headers.pUnknownHeaders[i];
		if (
			_stricmp(hdr->pName, "x-powered-by") == 0
			|| _stricmp(hdr->pName, "server") == 0
			) {
			continue;
		}

			//Add known cache headers here
		if (_stricmp(hdr->pName, "expires")==0 ||
			_stricmp(hdr->pName, "e-tag") == 0 ||
			_stricmp(hdr->pName, "content-md5") == 0 ||
			_stricmp(hdr->pName, "last-modified") == 0 ||
			_stricmp(hdr->pName, "accept_ranges") == 0
			) {
			response_headers->Add(std::string("__x_") + hdr->pName, hdr->pRawValue);
		} 
		response_headers->Add(hdr->pName, hdr->pRawValue);

	}
	response_headers->set_status_code( http_context->GetResponse()->GetRawHttpResponse()->StatusCode);
	response_headers->ComputeCaching();
	//OutputDebugStringA(response_headers->ToString().c_str());
}

bool IisModuleBaseFetch::HasChunk(HTTP_DATA_CHUNK* chunk) { 
	for (size_t i = 0; i < chunks_.size(); i++)
	{
		if (chunks_[i] == chunk) {
			return true;
		}
	}
	return false;
}
void IisModuleBaseFetch::AddChunk(char *data,int len,bool lock)
{
	if (lock) Lock();
	HTTP_DATA_CHUNK* dataChunk = new HTTP_DATA_CHUNK();
	dataChunk->DataChunkType = HttpDataChunkFromMemory;
	dataChunk->FromMemory.pBuffer = currentblock;
	dataChunk->FromMemory.BufferLength =len;
	
	chunks_.push_back(dataChunk);
	if (lock) Unlock();
}

bool IisModuleBaseFetch::HandleWrite(const StringPiece& sp,
                               MessageHandler* handler) 
{
	Lock();
	const char *data=sp.data();
	auto size=sp.size();
	const char *end=data+size;
	while (data!=end)
	{
		if (currentblockpos==currentblocksize) // first time 0==0
		{
			if (currentblock) // we have a block so add an entity chunk
			{
				AddChunk(currentblock,currentblockpos, false);				
			}
			// no optimalisation yet for vlb (very large blocks)
			// PSA normally writes small chunks 
			int sizetowrite=end-data;
			
			currentblocksize=sizetowrite>4096 ? sizetowrite  : 4096; 
			if (currentblocksize>65536) currentblocksize=65536;
			currentblockpos=0;
			currentblock=(char *) http_context_->AllocateRequestMemory(currentblocksize);
			if (currentblocksize<=sizetowrite)
			{
				memcpy(currentblock,data,currentblocksize);
				currentblockpos=currentblocksize;
				data+=currentblocksize;
				continue;
			}
			
		}
		currentblock[currentblockpos++]=*data++;
		
		
	}
	Unlock();

	/*while(data!=end)
	{		
		if (!currentblock)
		{
			currentblocksize=(todo+1024)<4096 ? 4096 : todo;
			currentblockpos=0;
			currentblock=(char *) http_context_->AllocateRequestMemory(currentblocksize);
		}

		
	}
	auto size=sp.size();
	if(currentblock)
	{

	}

	HTTP_DATA_CHUNK* dataChunk = new HTTP_DATA_CHUNK();
	dataChunk->DataChunkType = HttpDataChunkFromMemory;
	PVOID buf = http_context_->AllocateRequestMemory(sp.size());
	memcpy(buf, sp.data(), sp.size());
	dataChunk->FromMemory.pBuffer = buf;
	dataChunk->FromMemory.BufferLength =(ULONG) sp.size();
	//http_context_->GetResponse()->WriteEntityChunkByReference(dataChunk);
	*/
	return true;
}

int IisModuleBaseFetch::CollectHeaders() 
{
	Lock();
	log("collect headers!");
	if (headers_collected_) 
	{
		log("collect headers -> already called, bail");
		Unlock();
		return -1;
	}
	headers_collected_=true;
	if (http_context_->GetResponse()->GetHeadersSuppressed())
	{
		log("collect headers -> suppressed, bail");
		Unlock();
		return -1;
	}

	const ResponseHeaders* pagespeed_headers = response_headers();
	Unlock();
	if (fetch_type_ == FetchType::kInPlace) {
		auto server_context = ctx_->server_context();
		bool status_ok = (pagespeed_headers->status_code() != 0) 
			&& (pagespeed_headers->status_code() < 400);
		if (status_ok) { 
			//ctx->in_place = false;
			server_context->rewrite_stats()->ipro_served()->Add(1);
			server_context->message_handler()->Message(
				kInfo, "Serving rewritten resource in-place: %s",
				ctx_->url());
		} else
		if (pagespeed_headers->status_code() == CacheUrlAsyncFetcher::kNotInCacheStatus)
		{
			server_context->rewrite_stats()->ipro_not_in_cache()->Add(1);
			server_context->message_handler()->Message(
					kInfo,
					"Could not rewrite resource in-place "
					"because URL is not in cache: %s",
					ctx_->gurl()->spec_c_str());
			const SystemRewriteOptions* options = (const SystemRewriteOptions*) ctx_->driver()->options();
			//this->request_context()->set_options(options->ComputeHttpOptions());
			this->ctx_->set_recorder(
				new InPlaceResourceRecorder(
				(const RequestContextPtr) this->request_context(),
					ctx_->gurl()->spec_c_str(),
					ctx_->driver()->CacheFragment(),
					request_headers()->GetProperties(),
					options->ipro_max_response_bytes(),
					options->ipro_max_concurrent_recordings(),
					server_context->http_cache(),
					server_context->statistics(),
					server_context->message_handler())
				);
			return 0;
		} else { 
			log("Cache indicates that we should not rewrite in place.");
			return 0;
		}
	}

	if (http_context_->GetResponseHeadersSent())
	{
		//CHECK(false);
		//log("CollectHeaders->PANIC: response headers already sent before we got a chance to set them");
		//return -1;
	}
	IHttpResponse* r = http_context_->GetResponse();

	if (pagespeed_headers->status_code()==200)
	{
		r->SetStatus(pagespeed_headers->status_code(), "OK");
	}
	else
	{
		//os: hotfix
		r->SetStatus(pagespeed_headers->status_code(), pagespeed_headers->reason_phrase());
	}
	

	log("set status %d", pagespeed_headers->status_code());

	int i;
	// Fix for multiple instances of headers
	std::map<std::string,bool> headersSent;

	auto modify_cache_headers = true;
	if (this->ctx_ && this->ctx_->driver() && this->ctx_->driver()->options()) {
		modify_cache_headers = this->ctx_->driver()->options()->modify_caching_headers();
	}

	for (i = 0 ; i < pagespeed_headers->NumAttributes() ; i++) 
	{
		const GoogleString& name_gs = pagespeed_headers->Name(i);
		std::string lowerHeader(name_gs);
		std::transform(lowerHeader.begin(), lowerHeader.end(), lowerHeader.begin(), ::tolower); // Eval@@!!!

		// oschaaf: copy the value, as we possibly override it for application/javascript
		// const GoogleString& value_gs = pagespeed_headers->Value(i);
		GoogleString value_gs(pagespeed_headers->Value(i));

		// IIS's compression won't compress that by default on my box at least. 
		// But text/* should be compressed by default, so that is what we will use in this case.
		if (lowerHeader == "content-type" && value_gs == "application/javascript")
		{
			value_gs = "text/javascript";
		}
		if (!modify_cache_headers
			&& 
			fetch_type_ == FetchType::kHtml
			&&
			(lowerHeader == "cache-control" ||
			lowerHeader == "expires" ||
			lowerHeader == "e-tag" ||
			lowerHeader == "last-modified" 
			))
			continue;
		
		
		if (fetch_type_ == FetchType::kResource && pagespeed_headers->status_code()==200 && lowerHeader == "cache-control")
		{
			if (!strstr(value_gs.c_str(),"private"))
			{
				if (strstr(value_gs.c_str(),"max-age=31536000"))
				{
					auto cp=r->GetCachePolicy();
					auto kcp=cp->GetKernelCachePolicy();
					auto ucp=cp->GetUserCachePolicy();

					if (kcp)
					{
						kcp->Policy=HttpCachePolicyTimeToLive;
						kcp->SecondsToLive=60;
					}
					if (ucp && cp->IsUserCacheEnabled())
					{
						ucp->Policy=HttpCachePolicyTimeToLive;
						kcp->SecondsToLive=120;
					}
				}
			}
		}
		const char *ptr = lowerHeader.c_str();
		int headerOffset = 0;
		// check pos 3 first, smallest chance of underscore being there
		if (lowerHeader.length() > 3 && ptr[3] == '_' && ptr[0] == '_' && ptr[1] == '_' && ptr[2] == 'x') {
			if (modify_cache_headers) // skip header if we are modifying cache headers
				continue;
			lowerHeader = lowerHeader.substr(4, lowerHeader.length() - 4);
			headerOffset = 4; // we don't want the __x_ prefix 
		}
		if (headersSent[lowerHeader])
		{			
			r->SetHeader(name_gs.c_str() + headerOffset, value_gs.c_str(), value_gs.length(), false);
		}
		else
		{
			headersSent[lowerHeader]=1;
			r->SetHeader(name_gs.c_str() + headerOffset, value_gs.c_str(), value_gs.length(), true);
		}
		log("replace header [%s:%s]", name_gs.c_str() + headerOffset, value_gs.c_str());
	}
	// the design record (D2): soft enforcement — on the optimized HTML path, when running
	// unlicensed (live signal), add the soft warn header. Read at serve time
	// from the LIVE license atomic (ShouldOptimize), keyed on the same atomic
	// that encodes the grace window (R6); suppressed until the first license
	// check completes (R5, LicenseCheckedOnce). Added to the IIS transport
	// header map (IHttpResponse::SetHeader) only — never to a cached artifact,
	// Vary, or cache-key.
	if (fetch_type_ == FetchType::kHtml &&
		ctx_->server_context()->LicenseCheckedOnce() &&
		!ctx_->server_context()->ShouldOptimize())
	{
		r->SetHeader("x-pagespeed-warn", "unlicensed",
			(USHORT)strlen("unlicensed"), TRUE);
	}
	// the design record: over-cap detection on the optimized HTML path, keyed on the
	// request Host. Runs for ANY license state — an active scope=site license
	// can be over-cap even while fully licensed, so this is independent of the
	// unlicensed gate above. Self-guards (a no-op unless a site policy is armed)
	// and never gates optimization.
	if (fetch_type_ == FetchType::kHtml)
	{
		USHORT host_len = 0;
		PCSTR host = http_context_->GetRequest()->GetHeader("Host", &host_len);
		if (host != nullptr && host_len > 0)
		{
			ctx_->server_context()->MaybeFlagOverCap(
				StringPiece(host, host_len));
		}
		// the design record: emit the soft over-cap warn header (a sibling
		// x-pagespeed-warn value, appended not replaced; mutually exclusive in
		// practice with "unlicensed" above, since over-cap requires an active
		// site license). Display/telemetry only.
		if (ctx_->server_context()->IsOverCap())
		{
			r->SetHeader("x-pagespeed-warn", "over-cap",
				(USHORT)strlen("over-cap"), FALSE);
		}
	}
	if (fetch_type_ == FetchType::kInPlace || fetch_type_ == FetchType::kResource)
	{
		r->DeleteHeader("Content-MD5");
	} else if (fetch_type_ == FetchType::kHtml) 
	{
		// TODO(oschaaF): using header_id should be faster, but doesn't seem to work :(
		//r->DeleteHeader(_HTTP_HEADER_ID::HttpHeaderLastModified);
		//r->DeleteHeader(_HTTP_HEADER_ID::HttpHeaderContentMd5);
		//r->DeleteHeader(_HTTP_HEADER_ID::HttpHeaderExpires);
		//r->DeleteHeader(_HTTP_HEADER_ID::HttpHeaderEtag);
		
		

		if (modify_cache_headers) {
			r->SetHeader(_HTTP_HEADER_ID::HttpHeaderCacheControl, HttpAttributes::kNoCache, strlen(HttpAttributes::kNoCache), true);
			r->DeleteHeader("E-Tag");
			r->DeleteHeader("Expires");
			r->DeleteHeader("Content-MD5");
			r->DeleteHeader("Last-Modified");
			r->DeleteHeader("Accept-Ranges");

			auto cp = r->GetCachePolicy();
			auto kcp = cp->GetKernelCachePolicy();
			auto ucp = cp->GetUserCachePolicy();

			if (kcp) {
				kcp->Policy = HttpCachePolicyNocache;
				kcp->SecondsToLive = 0;
			}
			if (ucp) {
				ucp->Policy = HttpCachePolicyNocache;
				kcp->SecondsToLive = 0;
			}
		} 
	}
	
  return 0; 
}


void IisModuleBaseFetch::HandleHeadersComplete() 
{	
	CollectHeaders();
}

void IisModuleBaseFetch::SendData(MessageHandler* handler, bool flush) { 
	Lock();
	DWORD bytessent=0;
	log("SendData()");
	if (currentblock && currentblockpos) // last block
	{
		AddChunk(currentblock,currentblockpos,false);
	}
	currentblock=0;
	currentblocksize=0;
	currentblockpos=0;
	DWORD totalsize=0;
	for (size_t i = 0; i < chunks_.size(); i++)
	{
		totalsize+=chunks_[i]->FromMemory.BufferLength;
	}

	for (; chunk_send_index_ < chunks_.size(); chunk_send_index_++)
	{
		bytessent += chunks_[chunk_send_index_]->FromMemory.BufferLength;
		http_context_->GetResponse()->WriteEntityChunkByReference(chunks_[chunk_send_index_]);
	}
	if (flush) {
		//http_context_->GetResponse()->Flush(false,true,&bytessent);
		//need_flush_=false;
	}
	
	log("Send [%d] bytes (totalling [%d])", (int)bytessent, (int)totalsize) ;
	Unlock();
}



	// Create the custom notification class.
	
net_instaweb::MyCustomProvider cp;

bool IisModuleBaseFetch::HandleFlush(MessageHandler* handler) {
#ifndef IISPEEDHASFLUSH
	return true;
#else
	if (alive!=0x12345678)
	{
		CHECK(false);
	}
	Lock();
	log("HandleFlush()");
	need_flush_ = true;
	if (pending_) { 
		DWORD bytessent=0;
		log("Call SenData from HandleFlush (while pending)") ;
		SendData(handler, false);
	}
	else
	{
		if (flush_html_ && !donetime) { 
			log("Write empty chunk from HandleFlush() to wake up IIS") ;
			DWORD bytessent=0;
			DWORD pcbSent = 0;
			BOOL completion_expected = false;
			if (true || (http_context_->GetResponse()->GetRawHttpResponse()->Flags&HTTP_SEND_RESPONSE_FLAG_MORE_DATA))
			{
				
				//http_context_->NotifyCustomNotification(&cp,&completion_expected);
				//http_context_->GetResponse()->WriteEntityChunks(&emptydatachunk,1,true,true, &pcbSent, &completion_expected);
			}
			else
				log("No send more") ;
			CHECK(!completion_expected) << "Completion expected!";
		}
	}
	Unlock();
	return true;
#endif
}

void IisModuleBaseFetch::HandleDone(bool success) 
{
	alive=0;
	donetime=GetTickCount();	
	log("handle done [%s]!", success? "true" : "false");
	if (success) {
		SendData(NULL, false);
	}
	DWORD bytessent=0;
	//http_context_->GetResponse()->Flush(false,false,&bytessent);

	REQUEST_NOTIFICATION_STATUS next_status = fetch_type_ == FetchType::kResource || (fetch_type_ == FetchType::kInPlace && success)  ? RQ_NOTIFICATION_FINISH_REQUEST : RQ_NOTIFICATION_CONTINUE;
	if (next_status == RQ_NOTIFICATION_FINISH_REQUEST)
	{
		http_context_->SetRequestHandled();
	}

	log("base fetch -> indicate completion");
	
	if ( 0 && success)
	{
		IisModuleRequestContext *iisCtx = IisModuleRequestContext::GetRequestContext(http_context_);
		iisCtx->set_nextsendresponsestatus(next_status);
		http_context_->PostCompletion(0);
	}
	else
		http_context_->IndicateCompletion(next_status);
	http_context_=NULL;
	//ctx_->set_base_fetch(NULL);
	ReleaseRef(); 
}

void IisModuleBaseFetch::log(const char * msg, ...)
{
	if (REDUCE_LOG)
		return;
	// start at level one, so this indents one further
	int lvl = 1;
	IHttpContext * p = this->http_context_;
	if (!p) 
		return ;
	while (p != p->GetRootContext())
	{
		lvl++;
		p = p->GetParentContext();
	}

	std::string tabs;
	while  ( lvl > 0) {
		tabs.append("    "); 
		--lvl;
	}
	char *m=new char[1024*32];
	va_list vl;
	va_start(vl, msg);
	vsnprintf(m, 1024*32, msg, vl);
	va_end(vl);
	
	handler_->Message(net_instaweb::kInfo, "%s[%p] - iis_base_fetch: %s", tabs.c_str(), p, m);	
	delete[] m;
}
}	


