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
#include "pagespeed/system/daemon_html_purge_source.h"
#include "pagespeed/system/site_cache_fragment.h"
#include "pagespeed/system/system_cache_path.h"
#include "pagespeed/system/loopback_route_fetcher.h"

#include "pagespeed/iis/iis_request_context.h"
#include "pagespeed/iis/iis_daemon_check.h"
#include "pagespeed/iis/iis_registration.h"
#include "pagespeed/iis/iis_rewrite_driver_factory.h"
#include "pagespeed/iis/iis_rewrite_options.h"
#include "pagespeed/iis/iis_misc.h"
#include "pagespeed/iis/iis_site_bindings.h"

#include <algorithm>
#include <utility>
#include <vector>

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

	namespace {

	IHttpServer* iis_http_server = nullptr;

	// A string property of an admin-manager element, as UTF-8; empty when
	// it is absent.
	GoogleString AdminProperty(IAppHostElement* element, const wchar_t* name) {
		GoogleString out;
		BSTR bname = SysAllocString(name);
		IAppHostProperty* property = nullptr;
		if (bname != nullptr &&
			SUCCEEDED(element->GetPropertyByName(bname, &property)) &&
			property != nullptr) {
			VARIANT value;
			VariantInit(&value);
			if (SUCCEEDED(property->get_Value(&value))) {
				if (value.vt == VT_BSTR && value.bstrVal != nullptr) {
					out = ws2s(std::wstring(value.bstrVal,
						SysStringLen(value.bstrVal)));
				} else if (value.vt == VT_UI4 || value.vt == VT_I4) {
					out = IntegerToString(static_cast<int>(value.ulVal));
				}
			}
			VariantClear(&value);
			property->Release();
		}
		SysFreeString(bname);
		return out;
	}

	// The (protocol, bindingInformation) pairs of IIS site `site_id`, read
	// through the admin manager.  A worker process sees only its own
	// application pool's sites, which is the one this request came in on.
	// False when the site could not be read.
	bool ReadIisSiteBindings(
			uint32_t site_id,
			std::vector<std::pair<GoogleString, GoogleString>>* bindings) {
		IHttpServer* server = GetIisHttpServer();
		IAppHostAdminManager* admin =
			server == nullptr ? nullptr : server->GetAdminManager();
		if (admin == nullptr || site_id == 0) return false;
		BSTR section = SysAllocString(L"system.applicationHost/sites");
		BSTR path = SysAllocString(L"MACHINE/WEBROOT/APPHOST");
		IAppHostElement* sites = nullptr;
		HRESULT hr = admin->GetAdminSection(section, path, &sites);
		SysFreeString(section);
		SysFreeString(path);
		if (FAILED(hr) || sites == nullptr) return false;
		bool found = false;
		IAppHostElementCollection* collection = nullptr;
		DWORD count = 0;
		if (SUCCEEDED(sites->get_Collection(&collection)) &&
			collection != nullptr) {
			collection->get_Count(&count);
			for (DWORD i = 0; i < count && !found; ++i) {
				VARIANT index;
				VariantInit(&index);
				index.vt = VT_UI4;
				index.ulVal = i;
				IAppHostElement* site = nullptr;
				if (FAILED(collection->get_Item(index, &site)) || site == nullptr) {
					continue;
				}
				if (AdminProperty(site, L"id") == IntegerToString(static_cast<int>(site_id))) {
					found = true;
					BSTR name = SysAllocString(L"bindings");
					IAppHostElement* list = nullptr;
					if (SUCCEEDED(site->GetElementByName(name, &list)) && list != nullptr) {
						IAppHostElementCollection* items = nullptr;
						DWORD n = 0;
						if (SUCCEEDED(list->get_Collection(&items)) && items != nullptr) {
							items->get_Count(&n);
							for (DWORD j = 0; j < n; ++j) {
								VARIANT at;
								VariantInit(&at);
								at.vt = VT_UI4;
								at.ulVal = j;
								IAppHostElement* binding = nullptr;
								if (SUCCEEDED(items->get_Item(at, &binding)) && binding != nullptr) {
									bindings->emplace_back(
										AdminProperty(binding, L"protocol"),
										AdminProperty(binding, L"bindingInformation"));
									binding->Release();
								}
							}
							items->Release();
						}
						list->Release();
					}
					SysFreeString(name);
				}
				site->Release();
			}
			collection->Release();
		}
		sites->Release();
		return found;
	}

	DaemonHtmlMode DaemonHtmlModeFor(RewriteOptions::DaemonHtmlCacheMode mode) {
		switch (mode) {
		case RewriteOptions::kDaemonHtmlCacheOff:
			return DaemonHtmlMode::kOff;
		case RewriteOptions::kDaemonHtmlCacheReportOnly:
			return DaemonHtmlMode::kReportOnly;
		case RewriteOptions::kDaemonHtmlCacheServeVariants:
			return DaemonHtmlMode::kServeVariants;
		}
		return DaemonHtmlMode::kOff;
	}

	}  // namespace

	void SetIisHttpServer(IHttpServer* server) { iis_http_server = server; }
	IHttpServer* GetIisHttpServer() { return iis_http_server; }

	const char IisServerContext::kDaemonHtmlCommitsStarted[] =
		"daemon_html_iis_commits_started";
	const char IisServerContext::kDaemonHtmlCommitsFinished[] =
		"daemon_html_iis_commits_finished";

	void IisServerContext::InitDaemonHtmlStats(Statistics* statistics) {
		statistics->AddVariable(kDaemonHtmlCommitsStarted);
		statistics->AddVariable(kDaemonHtmlCommitsFinished);
	}

	void IisServerContext::NoteDaemonHtmlCommitStarted() {
		statistics()->GetVariable(kDaemonHtmlCommitsStarted)->Add(1);
	}

	void IisServerContext::NoteDaemonHtmlCommitFinished() {
		statistics()->GetVariable(kDaemonHtmlCommitsFinished)->Add(1);
	}

	bool IisServerContext::daemon_html_serves_variants() const {
		const RewriteOptions* options = global_options();
		return options->daemon_html_cache() ==
			RewriteOptions::kDaemonHtmlCacheServeVariants &&
			options->agent_optimize();
	}

	DaemonHtmlEngine* IisServerContext::daemon_html_engine(uint32_t iis_site_id) {
		std::call_once(daemon_html_engine_once_, [this, iis_site_id] {
			const IisRewriteOptions* options = config();
			if (options->daemon_html_cache() !=
				RewriteOptions::kDaemonHtmlCacheServeVariants) {
				daemon_html_engine_ = NewDaemonHtmlEngineForPort(
					options->daemon_html_cache(), options->daemon_html_max_bytes(),
					options->log_dir(),
					daemon_adapter_ == nullptr ? nullptr : daemon_adapter_->abi(),
					statistics(), timer(), message_handler());
				daemon_html_engine_built_.store(daemon_html_engine_.get(),
					std::memory_order_release);
				return;
			}
			if (options->log_dir().empty()) {
				message_handler()->Message(
					kWarning, "DaemonHtmlCache decision log is off: LogDir is not set");
			}
			// The names the site's bindings vouch for.  A site that cannot be
			// read vouches for none: nothing is stored or served for it.
			std::vector<std::pair<GoogleString, GoogleString>> bindings;
			if (!ReadIisSiteBindings(iis_site_id, &bindings)) {
				message_handler()->Message(
					kWarning,
					"DaemonHtmlCache: the bindings of IIS site %u could not be "
					"read, so no host name is vouched for and no page is stored "
					"or served from the HTML cache for it",
					static_cast<unsigned>(iis_site_id));
			}
			daemon_html_purge_source_.reset(new SystemDaemonHtmlPurgeSource(
				options, file_system(),
				DaemonHtmlCacheFlushPath(options->cache_flush_filename(),
					options->file_cache_path())));
			DaemonHtmlEngine::Config config;
			config.mode = DaemonHtmlModeFor(options->daemon_html_cache());
			config.max_bytes = options->daemon_html_max_bytes();
			config.agent_optimize = options->agent_optimize();
			config.host_names = IisSiteHostNames(bindings);
			config.site = IisRequestSiteId(iis_site_id, iis_site_id,
				options->cache_fragment());
			DaemonHtmlEngine::Deps deps;
			DaemonAdapter* adapter = daemon_adapter_;
			if (adapter != nullptr) {
				deps.substrate.abi = adapter->abi();
				// The handle of the moment: opened on this request thread when
				// none is open yet, the new one once the worker replaced its
				// volume, none while the optimizer is not usable.
				deps.substrate.cache_source = [adapter]() -> void* {
					return adapter->ServingHealth() == DaemonHealth::kReady
						? adapter->RecordCacheForRequest()
						: nullptr;
				};
				deps.substrate.cache_path = adapter->volume_path();
				deps.substrate.notify_socket = adapter->socket_path();
			}
			deps.purge_source = daemon_html_purge_source_.get();
			deps.statistics = statistics();
			deps.timer = timer();
			deps.handler = message_handler();
			deps.limiter = ProcessDaemonHealNotifyLimiter();
			deps.budget = ProcessDaemonHtmlByteBudget();
			// The admin path's and the ticks' work run inline, on the IIS
			// thread that asked.
			deps.runner = nullptr;
			deps.log_dir = options->log_dir();
			daemon_html_engine_.reset(
				new DaemonHtmlEngine(std::move(config), std::move(deps)));
			if (cache_path() != nullptr) {
				cache_path()->AddPurgeObserver(daemon_html_engine_.get());
				daemon_html_observing_ = true;
			}
			daemon_html_engine_built_.store(daemon_html_engine_.get(),
				std::memory_order_release);
		});
		return daemon_html_engine_.get();
	}

	void IisServerContext::MaybeTickDaemonHtmlEngine() {
		DaemonHtmlEngine* engine = daemon_html_engine_if_built();
		if (engine == nullptr ||
			engine->config().mode < DaemonHtmlMode::kServeVariants) {
			return;
		}
		const int64 now_ms = timer()->NowMs();
		const int64 poll_ms =
			std::max<int64>(1, config()->cache_flush_poll_interval_sec()) *
			Timer::kSecondMs;
		int64 last = daemon_html_last_poll_tick_ms_.load();
		if (now_ms - last >= poll_ms &&
			daemon_html_last_poll_tick_ms_.compare_exchange_strong(last, now_ms)) {
			engine->OnPurgePollTick();
		}
		last = daemon_html_last_stats_tick_ms_.load();
		if (now_ms - last >= kDaemonHtmlStatisticsTickMs &&
			daemon_html_last_stats_tick_ms_.compare_exchange_strong(last, now_ms)) {
			engine->OnStatisticsTick();
		}
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

	void IisServerContext::NoteDaemonHtmlMarkdownNotFinalized() {
		if (daemon_html_markdown_not_finalized_.exchange(true)) {
			return;
		}
		message_handler()->Message(
			kWarning,
			"DaemonHtmlCache: the %s module entry did not run for this site, so "
			"a Markdown answer could not be sent (the client got an error) and "
			"no Markdown copy is served here until the application pool "
			"restarts. Add the entry at the top of the site's modules list "
			"(the installer does).",
			kIisFinalViewModuleNameAscii);
	}

	IisServerContext::~IisServerContext() {
		// Every request that recorded or served through the engine held this
		// context, and its commit ran inline before the request let go, so
		// nothing of the engine's is in flight here.
		if (daemon_html_observing_ && cache_path() != nullptr) {
			cache_path()->RemovePurgeObserver(daemon_html_engine_.get());
		}
		daemon_html_observing_ = false;
		daemon_html_engine_built_.store(nullptr, std::memory_order_release);
		daemon_html_engine_.reset();
		daemon_html_purge_source_.reset();
	}
}  // namespace net_instaweb