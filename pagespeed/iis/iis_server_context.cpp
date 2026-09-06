// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/iis/iis_server_context.h"

#include "pagespeed/kernel/cache/purge_context.h"

#include "net/instaweb/rewriter/public/server_context.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_driver_pool.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/rewrite_stats.h"
#include "pagespeed/kernel/base/split_statistics.h"
#include "pagespeed/kernel/sharedmem/shared_mem_statistics.h"
#include "pagespeed/kernel/base/google_message_handler.h"
#include "pagespeed/system/add_headers_fetcher.h"
#include "pagespeed/system/system_cache_path.h"
#include "pagespeed/system/loopback_route_fetcher.h"

#include "pagespeed/iis/iis_request_context.h"
#include "pagespeed/iis/iis_rewrite_driver_factory.h"
#include "pagespeed/iis/iis_rewrite_options.h"
namespace net_instaweb {

	class purgeshizzle {
	public:
		void ExpectSuccessHelper(bool x, StringPiece reason) {
			delete this;
		}
		PurgeContext::PurgeCallback* ExpectSuccess() {
			return NewCallback(this, &purgeshizzle::ExpectSuccessHelper);
		}
	};

	// TODO: fix port!
	IisServerContext::IisServerContext(IisRewriteDriverFactory* factory, const GoogleString& site_app_id, unsigned int port)
		: SystemServerContext(factory, "site." + site_app_id, port),
		iis_factory_(factory),
		site_app_id_(site_app_id),
		fetch_factory_(NULL)
	{ 
	}

	IisRewriteOptions* IisServerContext::config() {
		return (IisRewriteOptions*)global_options();
	}
	IisServerContext::~IisServerContext() {
	}
}  // namespace net_instaweb