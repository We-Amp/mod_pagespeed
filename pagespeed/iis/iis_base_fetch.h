// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef IIS_BASE_FETCH_H_
#define IIS_BASE_FETCH_H_

#define _WINSOCKAPI_
#include <windows.h>
#include <sal.h>


extern "C" {

}

#include "net/instaweb/http/public/async_fetch.h"
#include "pagespeed/kernel/http/headers.h"
#include "pagespeed/kernel/base/string.h"
#include <atomic>
#include <vector>

namespace net_instaweb {

	class IisBaseFetch : public AsyncFetch {
		public:

		IisBaseFetch(const RequestContextPtr &ptr);
		virtual ~IisBaseFetch();
  
		void ReleaseRef();
  
		protected:
  
  
		void Lock();
		void Unlock();
		virtual void log(const char * msg, ...)=0;
		std::atomic<unsigned int> reference_count_;

		CRITICAL_SECTION critical_section_; 
  
		IisBaseFetch(const IisBaseFetch&) = delete;
		IisBaseFetch& operator=(const IisBaseFetch&) = delete;
	}; 

} // namespace net_instaweb

#endif  // IIS_BASE_FETCH_H_