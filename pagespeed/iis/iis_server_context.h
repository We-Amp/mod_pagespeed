// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef IIS_SERVER_CONTEXT_H_
#define IIS_SERVER_CONTEXT_H_

#include "pagespeed/system/system_server_context.h"

#include <atomic>
#include <memory>

#include "pagespeed/system/daemon_adapter.h"
#include "pagespeed/system/daemon_serve_arm.h"
#include "pagespeed/system/daemon_health.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

	class IisRewriteDriverFactory;
	class IisRewriteOptions;
	class ProxyFetchFactory;
	class SharedMemStatistics;

	class IisServerContext : public SystemServerContext {
	public:
		IisServerContext(IisRewriteDriverFactory* factory, const GoogleString& site_app_id, unsigned int port);
		virtual ~IisServerContext();
		virtual bool ProxiesHtml() const { return true; }

		const GoogleString& site_app_id() { return site_app_id_; }
		ProxyFetchFactory* fetch_factory() { return fetch_factory_; }
		void set_fetch_factory(ProxyFetchFactory* x) { fetch_factory_ = x; }
		IisRewriteOptions* config();

		// The daemon verdict for this site, when the site
		// configured a daemon option.  BORROWED: the process-wide
		// registry owns the adapter for the life of the process, so a
		// configuration rebuild that drops this context leaves the
		// adapter valid.  The adapter is SHARED by every site and every
		// context with the same (library, socket, volume) triple: no
		// caller may CloseRecordCache() it, because one context's close
		// would close the handle for all of them.  kNotConfigured while
		// no adapter is attached.  Asked through ServingHealth(): a
		// daemon that was not running when the check ran is looked for
		// again here, at most once per retry interval, so a worker
		// process picks it up without a pool recycle.  The one transition
		// an operator must hear about -- a recheck reattached a service
		// this worker started without -- becomes one informational
		// event-log entry; the registry de-duplicates it across every
		// site sharing the adapter.
		DaemonHealth daemon_health() const;
		DaemonAdapter* daemon_adapter() { return daemon_adapter_; }
		void set_daemon_adapter(DaemonAdapter* adapter) {
			daemon_adapter_ = adapter;
			// Seed the transition memory with the start-up verdict: a
			// context attached to an adapter that has already recovered
			// must not announce a recovery it never saw the start of.
			last_daemon_health_.store(
				adapter == nullptr ? DaemonHealth::kNotConfigured
				                   : adapter->health(),
				std::memory_order_relaxed);
			// The peer's serve-stats mapping, opened and never created: the
			// serve arm records exactly one class per response through it,
			// and without it "the daemon was not usable" and "this serve was
			// not counted" would be the same absence in the peer's numbers.
			// Built whatever the verdict, and holding no volume open.
			if (adapter != nullptr && adapter->abi() != nullptr) {
				daemon_serve_stats_.reset(new DaemonServeStats(
					adapter->abi(), adapter->volume_path()));
			} else {
				daemon_serve_stats_.reset();
			}
		}
		DaemonServeStats* daemon_serve_stats() {
			return daemon_serve_stats_.get();
		}

	private:

		GoogleString site_app_id_;
		ProxyFetchFactory* fetch_factory_;
		IisRewriteDriverFactory* iis_factory_;
		DaemonAdapter* daemon_adapter_ = nullptr;
		// The verdict this context last handed out, so the one transition
		// that matters (unavailable to ready, a serving recheck
		// reattaching the service) is noticed exactly once per context
		// however many threads read it.  Mutable: daemon_health() is
		// const, and what it records is process state, not context state.
		mutable std::atomic<DaemonHealth> last_daemon_health_{
			DaemonHealth::kNotConfigured};
		std::unique_ptr<DaemonServeStats> daemon_serve_stats_;
 		IisServerContext(const IisServerContext&) = delete;
 		IisServerContext& operator=(const IisServerContext&) = delete;
	};

}  // namespace net_instaweb

#endif  // IIS_SERVER_CONTEXT_H_