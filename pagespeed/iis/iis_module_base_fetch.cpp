// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

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
#include "pagespeed/iis/iis_module_request_context.h"
#include "pagespeed/iis/iis_request_context.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/http/content_type.h"
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
	, zc_data_(NULL)
	, zc_size_(0)
	, zc_has_borrowed_(false)
	, zc_aliased_counted_(false)
	, zc_active_(false)
	, zc_aliased_stat_(NULL)
	, zc_copied_out_stat_(NULL)
	, zc_renew_fail_stat_(NULL)
	, zc_aborted_stat_(NULL)
	
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
	// Content-Type is forwarded so JSON-API POSTs reach the admin handlers
	// with the type the browser sent.
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
	// Also pass through X-Requested-With (JSON-API request marker). It is an
	// HTTP Unknown Header in IIS (not in HTTP_HEADER_ID), so it falls into
	// pUnknownHeaders rather than KnownHeaders.
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

// ==== Zero-copy aliased serve (CycloneZeroCopyServe, the design record) =============

// Guards that must disqualify aliasing at write time.  Runs on the PSOL
// delivery thread while the request is parked (single-operator invariant:
// OnBeginRequest returned RQ_NOTIFICATION_PENDING, so nothing else touches
// the IHttpContext), the same contract the existing HandleWrite/SendData/
// CollectHeaders calls rely on.
bool IisModuleBaseFetch::ZeroCopyGuardsAllowAlias()
{
	IHttpRequest* req = http_context_->GetRequest();
	if (req == NULL)
	{
		return false;
	}
	// GET only: HEAD keeps the legacy path (no entity aliasing), anything
	// else never reaches the resource serve anyway.
	PCSTR method = req->GetHttpMethod();
	if (method == NULL || !StringCaseEqual(method, "GET"))
	{
		return false;
	}
	// Range / If-Range: a partial response must never alias (sub-range
	// chunk bookkeeping is not modeled).  Structurally excluded already --
	// OnBeginRequestPageSpeed bails on Range/If-Range before any base fetch
	// exists -- this is belt-and-braces.
	USHORT len = 0;
	PCSTR hdr = req->GetHeader("Range", &len);
	if (hdr != NULL && len > 0)
	{
		return false;
	}
	len = 0;
	hdr = req->GetHeader("If-Range", &len);
	if (hdr != NULL && len > 0)
	{
		return false;
	}
	// IIS dynamic compression: if the client advertises Accept-Encoding,
	// the compression module may read/transform the response body on its
	// own schedule; alias only content types that are absent from IIS's
	// default compressible-type lists (raster images; note image/svg+xml
	// IS compressible and must not alias, hence an explicit allowlist
	// instead of an image/* prefix test).  Everything else -- notably the
	// rewritten CSS/JS that IIS would gzip -- keeps the copying path.
	len = 0;
	hdr = req->GetHeader("Accept-Encoding", &len);
	if (hdr != NULL && len > 0)
	{
		const ContentType* ct = response_headers()->DetermineContentType();
		if (ct == NULL)
		{
			return false;
		}
		switch (ct->type())
		{
		case ContentType::kPng:
		case ContentType::kGif:
		case ContentType::kJpeg:
		case ContentType::kWebp:
			break;
		default:
			return false;
		}
	}
	// A response that already carries Content-Encoding is served verbatim
	// either way; nothing extra to check for it here (the engine's
	// InflatingFetch force-copies whenever inflation is needed).
	//
	// KNOWN LIMITATIONS of this experimental option (document with the
	// option): (a) a third-party SEND_RESPONSE module that RETAINS a
	// pointer into entity chunks past the flush that delivered them reads
	// outside the per-chunk verify window; (b) an administrator who adds a
	// raster-image type to IIS's compressible-type lists re-enables
	// compression on a type this allowlist aliases.  Both sit outside what
	// the serve-side barrier can observe.
	return true;
}

// Every downstream ULONG/DWORD cast (HTTP_DATA_CHUNK BufferLength,
// AllocateRequestMemory) is bounded by declining spans above this size in
// WriteMapped, so a truncation-driven heap overwrite is unreachable.  The
// copying fallback (HandleWrite) chunks with no full-size cast, so it is
// safe for any size.
static const size_t kZcMaxServeBytes = size_t{1} << 30;  // 1GB, ULONG-safe

bool IisModuleBaseFetch::WriteMapped(const StringPiece& mmap_sp,
                                     const MappedSharedString& keepalive,
                                     MessageHandler* handler)
{
	// Only the '.pagespeed.' resource serve is wired for aliasing; the
	// engine calls HeadersComplete() before WriteMapped on that path
	// (rewrite_driver.cc CacheCallback::DeliverDone), so a missing
	// headers-complete means an unexpected caller: decline.
	bool decline = fetch_type_ != FetchType::kResource ||
	               http_context_ == NULL || !headers_complete() ||
	               mmap_sp.size() > kZcMaxServeBytes ||
	               !ZeroCopyGuardsAllowAlias();
	if (!decline)
	{
		Lock();
		if (zc_has_borrowed_)
		{
			// Never expected: a mapped serve is a single WriteMapped.
			// Fall through to the copying path release-safely rather
			// than drop bytes.
			decline = true;
		}
		else
		{
			zc_data_ = mmap_sp.data();
			zc_size_ = mmap_sp.size();
			zc_has_borrowed_ = true;
			zc_pin_ = keepalive;  // refcount bump on the read handle
		}
		Unlock();
		if (!decline)
		{
			log("zero-copy: recorded borrowed span (%d bytes)",
			    (int)mmap_sp.size());
			return true;
		}
	}
	// Declined: the copying fallback memcpys out of the MAPPED region
	// (HandleWrite -> request-pool blocks), so it needs the same
	// copy-then-verify discipline as every other copy that leaves the
	// region -- a ceiling-forced wrap can race the memcpy.  Probe the
	// lease AFTER the copy and fail the fetch on kTorn so a torn copy is
	// never served as a complete 200 (the fetch fails, no body is sent).
	// kCopyNow (region intact) and kLeasesOff (no protection) keep the
	// verified/unprotected copy, matching the aliased path's rules.
	bool ok = Write(mmap_sp, handler);
	if (ok && keepalive.RenewLeaseStrict() == LeaseRenewal::kTorn)
	{
		log("zero-copy: fallback copy torn (epoch moved), failing fetch");
		ok = false;
	}
	return ok;
}

// Kicks off the chunked aliased serve from HandleDone (PSOL thread).  From
// the first async flush on, completion signaling belongs to the serve:
// either an inline drive finishes synchronously here (then this method
// calls IndicateCompletion), or OnZeroCopyCompletion finishes it from the
// module's OnAsyncCompletion.
void IisModuleBaseFetch::StartZeroCopyServe()
{
	log("zero-copy serve: start (%d bytes)", (int)zc_size_);
	// Statistics (same variable names as the nginx sink; registered
	// centrally in RewriteDriverFactory::InitStats).
	IisModuleRequestContext* rc =
	    IisModuleRequestContext::GetRequestContext(http_context_);
	if (rc != NULL && rc->GetInnerContext() != NULL)
	{
		Statistics* stats =
		    rc->GetInnerContext()->server_context()->statistics();
		if (stats != NULL)
		{
			zc_aliased_stat_ = stats->FindVariable("zerocopy_serve_aliased");
			zc_copied_out_stat_ =
			    stats->FindVariable("zerocopy_serve_copied_out");
			zc_renew_fail_stat_ =
			    stats->FindVariable("zerocopy_serve_renew_fail_reset");
			zc_aborted_stat_ = stats->FindVariable("zerocopy_serve_aborted");
		}
	}
	// HTTP.sys must never capture aliased bytes into the shared kernel
	// response cache: a ceiling-forced wrap racing the send would poison a
	// cache entry served to every client for its TTL, and no post-hoc
	// verification can un-cache it.  CollectHeaders arms kernel TTL caching
	// for long-max-age resources; disarm it for this response before the
	// first flush sends the headers.  (Mirrors the HTML-path disarm in
	// OnSendResponse.)
	IHttpResponse* r = http_context_->GetResponse();
	r->DisableKernelCache();
	auto cp = r->GetCachePolicy();
	if (cp != NULL)
	{
		auto kcp = cp->GetKernelCachePolicy();
		if (kcp)
		{
			kcp->Policy = HttpCachePolicyNocache;
			kcp->SecondsToLive = 0;
		}
		auto ucp = cp->GetUserCachePolicy();
		if (ucp)
		{
			ucp->Policy = HttpCachePolicyNocache;
		}
	}
	zc_planner_.reset(new IisZeroCopyServePlanner(zc_size_));
	zc_has_borrowed_ = false;  // ownership moved to the planner/driver
	zc_active_.store(true, std::memory_order_release);
	ZcDrive d = DriveZeroCopyServe();
	if (d == ZcDrive::kPending)
	{
		// An async flush is in flight; OnZeroCopyCompletion resumes on an
		// IIS thread-pool thread.  Do not touch serve state past this
		// point.
		return;
	}
	// Finished or aborted synchronously on this thread.  We are outside a
	// notification handler (OnBeginRequest returned PENDING long ago), so
	// resume the pipeline explicitly, exactly like the legacy HandleDone.
	IHttpContext* http_context = http_context_;
	http_context_ = NULL;
	zc_active_.store(false, std::memory_order_release);
	http_context->IndicateCompletion(RQ_NOTIFICATION_FINISH_REQUEST);
}

// The submit loop.  Runs on exactly one thread at a time (PSOL thread for
// the initial drive, then whichever IIS completion thread resumes it);
// strict submit -> completion alternation means no lock is needed over the
// serve state.
IisModuleBaseFetch::ZcDrive IisModuleBaseFetch::DriveZeroCopyServe()
{
	IHttpResponse* response = http_context_->GetResponse();
	while (true)
	{
		if (zc_planner_->done())
		{
			// Every aliased chunk passed its post-exposure verification and
			// the (always-copied) tail is queued by reference in request-
			// pool memory; IIS sends it while finishing the request.  The
			// pin can be dropped: no aliased byte remains unsent.
			zc_pin_ = MappedSharedString();
			log("zero-copy serve: complete (%d bytes)", (int)zc_size_);
			http_context_->SetRequestHandled();
			return ZcDrive::kFinished;
		}
		// the design record submit-time decision.  RenewLeaseStrict re-stamps the
		// read lease (keeping NORMAL wraps blocked for a fresh T while the
		// client keeps up -- the between-chunks renewal) AND Dekker-
		// revalidates the borrow; NsUntilForcedWrap is the ceiling-forced
		// wrap deadline that ignores leases.  Because a chunk's in-flight
		// window is client-paced (unbounded), ANY finite deadline forces
		// the copy path -- a time margin cannot prove safety here.
		const LeaseRenewal renew = zc_pin_.RenewLeaseStrict();
		const bool pressure = zc_pin_.NsUntilForcedWrap() != UINT64_MAX;
		const IisZeroCopyServePlanner::Next n =
		    zc_planner_->PlanNext(renew, pressure);
		if (n.step == IisZeroCopyServePlanner::Step::kAbort)
		{
			// Epoch moved: the remaining source bytes are gone.  A copy
			// fallback is impossible -- fail closed.
			return AbortZeroCopyServe("lease epoch moved (region overwritten)",
			                           true /* epoch_torn */);
		}
		if (n.step == IisZeroCopyServePlanner::Step::kCopyTail)
		{
			char* owned =
			    (char*)http_context_->AllocateRequestMemory((DWORD)n.length);
			if (owned == NULL)
			{
				return AbortZeroCopyServe("tail copy allocation failed",
			                           false /* epoch_torn */);
			}
			memcpy(owned, zc_data_ + n.offset, n.length);
			// Copy-then-verify, never check-then-copy: a ceiling-forced
			// wrap ignores the fresh lease and can overwrite the region
			// DURING the memcpy above.  Re-verify the epoch AFTER the copy;
			// only kTorn (epoch moved) aborts -- kCopyNow (wrap in flight,
			// region intact) and kLeasesOff (no lease protection) serve the
			// copy, so leases-off never turns serves into resets.
			if (zc_pin_.RenewLeaseStrict() == LeaseRenewal::kTorn)
			{
				return AbortZeroCopyServe("epoch moved during tail copy-out",
				                           true /* epoch_torn */);
			}
			zc_pin_ = MappedSharedString();  // verified owned bytes: unpin
			HTTP_DATA_CHUNK* chunk = new HTTP_DATA_CHUNK();
			chunk->DataChunkType = HttpDataChunkFromMemory;
			chunk->FromMemory.pBuffer = owned;
			chunk->FromMemory.BufferLength = (ULONG)n.length;
			chunks_.push_back(chunk);  // struct freed in ~IisModuleBaseFetch
			HRESULT hr = response->WriteEntityChunkByReference(chunk);
			if (FAILED(hr))
			{
				return AbortZeroCopyServe("queueing the copied tail failed",
				                           false /* epoch_torn */);
			}
			zc_planner_->Advance(n.length);
			if (zc_copied_out_stat_ != NULL)
			{
				zc_copied_out_stat_->Add(1);
			}
			continue;  // next iteration: done() -> kFinished
		}
		// kAliasChunk: point HTTP.sys straight at the mmap bytes and flush
		// asynchronously.  The flush completion is the per-chunk barrier:
		// HTTP.sys guarantees the buffer is not read again after it.
		HTTP_DATA_CHUNK* chunk = new HTTP_DATA_CHUNK();
		chunk->DataChunkType = HttpDataChunkFromMemory;
		chunk->FromMemory.pBuffer =
		    (PVOID)const_cast<char*>(zc_data_ + n.offset);
		chunk->FromMemory.BufferLength = (ULONG)n.length;
		chunks_.push_back(chunk);
		HRESULT hr = response->WriteEntityChunkByReference(chunk);
		if (FAILED(hr))
		{
			// Nothing new in flight; the response chunk queue is wedged, so
			// a copy retry through the same queue cannot help.  Fail
			// closed.
			return AbortZeroCopyServe("queueing an aliased chunk failed",
			                           false /* epoch_torn */);
		}
		zc_planner_->Advance(n.length);
		if (!zc_aliased_counted_)
		{
			zc_aliased_counted_ = true;
			if (zc_aliased_stat_ != NULL)
			{
				zc_aliased_stat_->Add(1);  // once per aliased serve
			}
		}
		// Publish every serve-state write above (planner offset, chunk
		// bookkeeping) before the async submit: the completion thread's
		// dispatch acquire-loads zc_active_ (HasActiveZeroCopyServe) and
		// synchronizes with this release-store, so all writes sequenced
		// before it are visible on that thread.  (Belt-and-braces: the
		// kernel submit/completion round trip is itself a synchronizing
		// event -- the same assumption every OVERLAPPED pattern rests on.)
		zc_active_.store(true, std::memory_order_release);
		// MERGE GATE (M1): this async Flush is initiated from a PSOL
		// thread while the request is parked in RQ_BEGIN_REQUEST, relying
		// on IIS routing its completion to the module's OnAsyncCompletion.
		// That matches the documented IIS async-module pattern, but this
		// module has NO in-tree precedent for it; before any merge it must
		// be validated on IIS with an integration test: slow client +
		// AppVerifier + a canary that rewrites the mapped region after
		// each completion and asserts byte-identity-or-reset.
		DWORD sent = 0;
		BOOL completion_expected = FALSE;
		hr = response->Flush(TRUE /*fAsync*/, TRUE /*fMoreData*/, &sent,
		                     &completion_expected);
		if (FAILED(hr))
		{
			// The aliased chunk may be partially on the wire; state is
			// unknowable.  Fail closed.
			return AbortZeroCopyServe("async flush of an aliased chunk failed",
			                           false /* epoch_torn */);
		}
		if (completion_expected)
		{
			// KNOWN LIMITATION (availability, not correctness): the lease
			// is renewed only between chunks.  A client that drains one
			// chunk slower than the lease term on a stripe with concurrent
			// write churn lapses the lease mid-flight; a normal wrap can
			// then overwrite the in-flight region, and the post-exposure
			// verify converts that serve into a connection reset where the
			// copy path would have served it.  Fix direction if the reset
			// rate ever matters: a <=3T/4 renewal timer armed while a
			// flush is pending (not implemented, to keep this experimental
			// diff bounded).
			return ZcDrive::kPending;  // OnZeroCopyCompletion resumes
		}
		// Completed inline: HTTP.sys already consumed the buffer.  Post-
		// exposure verification (the TOCTOU rule: verify AFTER the window
		// closes): if the epoch moved while the kernel read the region, the
		// bytes on the wire may be torn; the body cannot be complete yet
		// (the copied tail is still pending), so abort truncates it.
		if (!IisZeroCopyServePlanner::SentBytesTrustworthy(
		        zc_pin_.RenewLeaseStrict()))
		{
			return AbortZeroCopyServe("epoch moved during inline aliased send",
			                           true /* epoch_torn */);
		}
	}
}

IisModuleBaseFetch::ZcDrive IisModuleBaseFetch::AbortZeroCopyServe(
    const char* reason, bool epoch_torn)
{
	log("zero-copy serve: ABORT: %s", reason);
	// Stat split: zerocopy_serve_renew_fail_reset keeps its nginx-
	// compatible meaning (epoch moved / torn borrow -> reset);
	// allocation/submit-HRESULT aborts count separately.
	if (epoch_torn)
	{
		if (zc_renew_fail_stat_ != NULL)
		{
			zc_renew_fail_stat_->Add(1);
		}
	}
	else if (zc_aborted_stat_ != NULL)
	{
		zc_aborted_stat_->Add(1);
	}
	zc_pin_ = MappedSharedString();
	if (http_context_ != NULL)
	{
		// The client must never see a COMPLETE body assembled from an
		// overwritten region, and the serve is HTTP/1.1 chunked (no
		// Content-Length), so the truncation is only client-detectable if
		// the terminating 0-length chunk never goes out.  CloseConnection()
		// tears the connection down asynchronously and can lose the race
		// against end-of-request processing, which flushes the terminating
		// chunk first: the abort then reaches the client as a
		// cleanly-terminated short body that neither it nor a downstream
		// cache can tell apart from a complete response (observed on the
		// M1 gate rig).  ResetConnection() aborts the socket immediately
		// (TCP RST), so no terminator is ever emitted for a torn serve.
		IHttpResponse* r = http_context_->GetResponse();
		if (r != NULL)
		{
			r->ResetConnection();
		}
		http_context_->SetRequestHandled();
	}
	return ZcDrive::kAborted;
}

REQUEST_NOTIFICATION_STATUS IisModuleBaseFetch::OnZeroCopyCompletion(
    HRESULT completion_status)
{
	// IIS thread-pool thread.  Strict alternation with the submit path (the
	// previous submit returned kPending and nothing touches serve state
	// until its completion arrives), so no lock is taken here.
	ZcDrive d;
	if (FAILED(completion_status))
	{
		// Client disconnect / transport error mid-serve.  The pin MUST
		// still be released on this path; there is nobody left to serve.
		log("zero-copy serve: completion error 0x%08x, finishing",
		    (unsigned)completion_status);
		zc_pin_ = MappedSharedString();
		if (http_context_ != NULL)
		{
			http_context_->SetRequestHandled();
		}
		d = ZcDrive::kAborted;
	}
	else if (!IisZeroCopyServePlanner::SentBytesTrustworthy(
	             zc_pin_.RenewLeaseStrict()))
	{
		// Post-exposure verification of the chunk HTTP.sys just finished
		// reading (verify AFTER the exposure window, the copy-then-verify
		// rule).  kTorn: the sent bytes may be torn; the body is not
		// complete (tail still pending), so the reset truncates it.
		d = AbortZeroCopyServe("epoch moved during aliased send",
		                       true /* epoch_torn */);
	}
	else
	{
		d = DriveZeroCopyServe();
	}
	if (d == ZcDrive::kPending)
	{
		return RQ_NOTIFICATION_PENDING;
	}
	zc_active_.store(false, std::memory_order_release);
	http_context_ = NULL;
	// We are inside OnAsyncCompletion for the parked RQ_BEGIN_REQUEST:
	// returning FINISH here resumes and finishes the request without an
	// IndicateCompletion call.
	return RQ_NOTIFICATION_FINISH_REQUEST;
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
	if (zc_has_borrowed_)
	{
		// Zero-copy aliased serve (CycloneZeroCopyServe): WriteMapped
		// recorded the borrowed mmap span instead of copying it.
		if (success && http_context_ != NULL &&
		    fetch_type_ == FetchType::kResource && chunks_.empty() &&
		    currentblockpos == 0)
		{
			// The expected shape: a single WriteMapped, no other body
			// bytes.  Hand the serve to the async chunked driver; it owns
			// completion signaling (IndicateCompletion / notification
			// return) from here on.  This fetch object stays alive through
			// the whole drain: the inner request context holds the last
			// reference until CleanupStoredContext at request teardown,
			// which is also the backstop release point for the pin.
			StartZeroCopyServe();
			ReleaseRef();
			return;
		}
		// Never expected: mixed Write()+WriteMapped output, a failed
		// fetch, or no context.  Degrade release-safely to the classic
		// copying serve (copy-then-verify; on a torn copy the body is
		// dropped rather than serving wrong bytes).  An empty mapped span
		// has nothing to copy and must not fail a producible response.
		zc_has_borrowed_ = false;
		bool copy_ok = (zc_size_ == 0);
		if (success && http_context_ != NULL && zc_size_ > 0)
		{
			// zc_size_ <= kZcMaxServeBytes (WriteMapped bound), so the
			// DWORD/ULONG casts below cannot truncate.
			char* owned =
			    (char*)http_context_->AllocateRequestMemory((DWORD)zc_size_);
			if (owned != NULL)
			{
				memcpy(owned, zc_data_, zc_size_);
				// Verify AFTER the memcpy (copy-then-verify): only kTorn
				// (epoch moved) invalidates the copy.
				copy_ok =
				    zc_pin_.RenewLeaseStrict() != LeaseRenewal::kTorn;
				if (copy_ok)
				{
					Lock();
					// Body order: any unflushed Write() bytes precede the
					// mapped bytes chronologically, but SendData appends
					// currentblock AFTER chunks_ -- flush it into chunks_
					// FIRST so the mapped copy cannot be emitted out of
					// order on this (unexpected) mixed path.
					if (currentblock && currentblockpos)
					{
						AddChunk(currentblock, currentblockpos, false);
						currentblock = 0;
						currentblocksize = 0;
						currentblockpos = 0;
					}
					HTTP_DATA_CHUNK* chunk = new HTTP_DATA_CHUNK();
					chunk->DataChunkType = HttpDataChunkFromMemory;
					chunk->FromMemory.pBuffer = owned;
					chunk->FromMemory.BufferLength = (ULONG)zc_size_;
					chunks_.push_back(chunk);
					Unlock();
				}
			}
		}
		zc_pin_ = MappedSharedString();
		if (success && !copy_ok)
		{
			log("zero-copy: degrade copy failed, failing the fetch");
			success = false;
		}
	}
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


