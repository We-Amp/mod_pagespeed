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
	pagespeedRequestPointer.reset(NULL);
}


void IisInnerRequestContext::CleanupStoredContext()
{
	if (proxy_fetch_ != NULL)
	{
		//CHECK(false);
		//TODO (oschaaf): ?!!!
		//proxy_fetch_->Done(true);
		proxy_fetch_ = NULL;
	}
	if (driver_ != NULL) {
		driver_->Cleanup();
		driver_ = NULL;
	}

	if (recorder_ != NULL) {
		//log("IPRO: recorder CLEANUP");
		recorder_->Fail();
		recorder_->DoneAndSetHeaders(NULL, false);  // Deletes recorder.
		recorder_ = NULL;
	}

	if (base_fetch_ != NULL)
	{
		base_fetch_->ReleaseRef();
		base_fetch_ = NULL;
	}
	if (this->process_context_)
	{
		this->process_context_->ReleaseRef();
	}
	delete gurl_;
	gurl_ = NULL;
	delete this;
}


} 