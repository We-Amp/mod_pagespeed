// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/iis/iis_base_fetch.h"
#include "pagespeed/iis/iis_global_constants.h"

namespace net_instaweb {

	IisBaseFetch::IisBaseFetch(const RequestContextPtr &ptr) : AsyncFetch(ptr) 
	 {
		 InitializeCriticalSection(&critical_section_);
		 reference_count_=2;
	 }
	IisBaseFetch::~IisBaseFetch()
	{
		DeleteCriticalSection(&critical_section_);
	}

	void IisBaseFetch::Lock() 
	{
		EnterCriticalSection(&critical_section_);
	}

	void IisBaseFetch::Unlock() 
	{
		LeaveCriticalSection(&critical_section_);
	}
	void IisBaseFetch::ReleaseRef()
	{
		if ( reference_count_.fetch_sub(1) == 1 )
			delete this;
	}

}