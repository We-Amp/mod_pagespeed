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
#include "pagespeed/iis/iis_daemon_check.h"
#include "pagespeed/iis/iis_registration.h"
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
		fetch_factory_(nullptr)
	{ 
	}

	IisRewriteOptions* IisServerContext::config() {
		return (IisRewriteOptions*)global_options();
	}

	DaemonHealth IisServerContext::daemon_health() const {
		if (daemon_adapter_ == nullptr) {
			return DaemonHealth::kNotConfigured;
		}
		const DaemonHealth verdict = daemon_adapter_->ServingHealth();
		// The one transition an operator must hear about: a serving
		// recheck reattached a service this worker started without.  The
		// registry writes ONE informational event-log entry, however many
		// sites share the adapter or noticed the flip; every other change
		// of verdict is bookkeeping only.
		const DaemonHealth previous =
			last_daemon_health_.load(std::memory_order_relaxed);
		last_daemon_health_.store(verdict, std::memory_order_relaxed);
		if (previous == DaemonHealth::kUnavailable &&
			verdict == DaemonHealth::kReady) {
			IisDaemonCheckRegistry::Get()->NoteDaemonRecovered(
				daemon_adapter_);
		}
		return verdict;
	}

	DaemonHtmlEngine* IisServerContext::daemon_html_engine() {
		std::call_once(daemon_html_engine_once_, [this] {
			const IisRewriteOptions* options = config();
			daemon_html_engine_ = NewDaemonHtmlEngineForPort(
				options->daemon_html_cache(), options->daemon_html_max_bytes(),
				options->log_dir(),
				daemon_adapter_ == nullptr ? nullptr : daemon_adapter_->abi(),
				statistics(), timer(), message_handler());
		});
		return daemon_html_engine_.get();
	}

	void IisServerContext::NoteDaemonHtmlFinalViewMissing() {
		if (daemon_html_final_view_missing_logged_.exchange(true)) {
			return;
		}
		message_handler()->Message(
			kWarning,
			"DaemonHtmlCache: the %s module entry did not run for this site, "
			"so the report reads each HTML response's headers as the module "
			"received them, without the site's customHeaders or later "
			"changes. Add the entry at the top of the site's modules list "
			"(the installer does).",
			kIisFinalViewModuleNameAscii);
	}

	IisServerContext::~IisServerContext() {
	}
}  // namespace net_instaweb