// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/iis/iis_request_context.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "pagespeed/system/in_place_resource_recorder.h"
#include "pagespeed/kernel/base/message_handler.h"

namespace net_instaweb
{

IisInnerRequestContext::~IisInnerRequestContext()
{
	pagespeedRequestPointer.reset(nullptr);
}


void IisInnerRequestContext::CleanupStoredContext()
{
	// IIS is done with the request, but the rewriter may still be producing
	// output for it on its own threads: cut the fetch off from the request
	// first, so nothing it does from here on can reach IIS.
	if (base_fetch_ != nullptr)
	{
		base_fetch_->DetachFromRequest();
	}
	if (proxy_fetch_ != nullptr)
	{
		//CHECK(false);
		//TODO (oschaaf): ?!!!
		//proxy_fetch_->Done(true);
		proxy_fetch_ = nullptr;
	}
	if (driver_ != nullptr) {
		driver_->Cleanup();
		driver_ = nullptr;
	}

	if (recorder_ != nullptr) {
		//log("IPRO: recorder CLEANUP");
		recorder_->Fail();
		recorder_->DoneAndSetHeaders(nullptr, false);  // Deletes recorder.
		recorder_ = nullptr;
	}

	if (base_fetch_ != nullptr)
	{
		base_fetch_->ReleaseRef();
		base_fetch_ = nullptr;
	}
	if (this->process_context_)
	{
		this->process_context_->ReleaseRef();
	}
	delete gurl_;
	gurl_ = nullptr;
	delete this;
}


} 