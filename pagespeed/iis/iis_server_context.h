// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef IIS_SERVER_CONTEXT_H_
#define IIS_SERVER_CONTEXT_H_

#include "pagespeed/system/system_server_context.h"

#include <atomic>
#include <memory>
#include <mutex>

#include "pagespeed/system/daemon_adapter.h"
#include "pagespeed/system/daemon_html_engine.h"
#include "pagespeed/system/daemon_serve_arm.h"
#include "pagespeed/system/daemon_health.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

class IHttpServer;

namespace net_instaweb {

	// The IIS server interface RegisterModule is given, kept for the admin
	// manager (a site's bindings); nullptr before registration.
	void SetIisHttpServer(IHttpServer* server);
	IHttpServer* GetIisHttpServer();

	class DaemonHtmlPurgeSource;
	class IisRewriteDriverFactory;
	class IisRewriteOptions;
	class ProxyFetchFactory;
	class SharedMemStatistics;

	class IisServerContext : public SystemServerContext {
	public:
		IisServerContext(IisRewriteDriverFactory* factory, const GoogleString& site_app_id, unsigned int port);
		~IisServerContext() override;
		bool ProxiesHtml() const override { return true; }

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

		// The HTML cache engine of this site (DaemonHtmlCache), built on
		// first use so a site that never sets the option has none; nullptr
		// with the option off.  `iis_site_id` is the IIS site this context
		// serves (IisProcessContext::site_id()).  In serve-variants it is
		// built with the host names the site's bindings vouch for, the
		// site's purge state and the volume handle of the moment, and is
		// registered as a purge observer of the cache path; otherwise it
		// only reports.
		DaemonHtmlEngine* daemon_html_engine(uint32_t iis_site_id);
		// The engine once built, or nullptr; never builds it.
		DaemonHtmlEngine* daemon_html_engine_if_built() {
			return daemon_html_engine_built_.load(std::memory_order_acquire);
		}
		// Whether HTML responses of this site can be answered with their
		// Markdown copy: serve-variants with AgentOptimize on.  Such a
		// response must never be left in IIS's response caches, which
		// would answer an agent with the HTML.
		bool daemon_html_serves_variants() const;

		// The engine's purge-poll and statistics ticks, from the request
		// path next to FlushCacheIfNecessary, inline (an IIS request
		// thread may block): at most one purge-poll tick per poll interval
		// and one statistics tick per kDaemonHtmlStatisticsTickMs.
		void MaybeTickDaemonHtmlEngine();
		static constexpr int64 kDaemonHtmlStatisticsTickMs = 10 * 1000;

		// The capture of an HTML response finished its body, so the engine
		// will classify it, and store or remove, when IIS releases the
		// request; and that work ran.  The two counters
		// (kDaemonHtmlCommitsStarted, kDaemonHtmlCommitsFinished) are what
		// a test reads to know every commit of the responses it has is done.
		void NoteDaemonHtmlCommitStarted();
		void NoteDaemonHtmlCommitFinished();
		static const char kDaemonHtmlCommitsStarted[];
		static const char kDaemonHtmlCommitsFinished[];
		static void InitDaemonHtmlStats(Statistics* statistics);

		// A recorded HTML response got no final header view: the
		// final-view module entry did not run for this site.  Logged once
		// per site.
		void NoteDaemonHtmlFinalViewMissing();

		// A Markdown answer the main registration kept was never taken: the
		// final-view module entry did not run for it (it is not in the
		// site's modules list), so the client got the pending error.  From
		// then on this site answers Markdown requests with their HTML.
		// Logged once.
		void NoteDaemonHtmlMarkdownNotFinalized();
		bool daemon_html_final_view_missing() const {
			return daemon_html_markdown_not_finalized_.load(std::memory_order_relaxed);
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
		std::once_flag daemon_html_engine_once_;
		// What the serving engine reads purges from; declared before the
		// engine, which borrows it.
		std::unique_ptr<DaemonHtmlPurgeSource> daemon_html_purge_source_;
		std::unique_ptr<DaemonHtmlEngine> daemon_html_engine_;
		std::atomic<DaemonHtmlEngine*> daemon_html_engine_built_{nullptr};
		bool daemon_html_observing_ = false;
		std::atomic<int64> daemon_html_last_poll_tick_ms_{0};
		std::atomic<int64> daemon_html_last_stats_tick_ms_{0};
		std::atomic<bool> daemon_html_final_view_missing_logged_{false};
		std::atomic<bool> daemon_html_markdown_not_finalized_{false};
 		IisServerContext(const IisServerContext&) = delete;
 		IisServerContext& operator=(const IisServerContext&) = delete;
	};

}  // namespace net_instaweb

#endif  // IIS_SERVER_CONTEXT_H_