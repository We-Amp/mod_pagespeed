// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef IIS_PROCESS_CONTEXT_H_                                        
#define IIS_PROCESS_CONTEXT_H_                                        


#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#ifndef _WINSOCKAPI_
#define _WINSOCKAPI_
#endif
#include <Windows.h>

#include <httpserv.h>

#include "net/instaweb/rewriter/public/process_context.h"
#include "pagespeed/automatic/proxy_fetch.h"
#include "pagespeed/iis/iis_configuration.h"
#include "pagespeed/iis/FileCheck.h"

namespace net_instaweb 
{
	class IisRewriteDriverFactory;
	class IisRewriteOptions;
	class IisServerContext;
	struct global_settings;

	class IisProcessContext
	{
	public:
		IisProcessContext(
			const GoogleString& site_app_id,
			HTTP_MODULE_ID module_id, 
			DWORD page_size, 
			IisRewriteOptions* root_options, /*we take ownership*/
			std::wstring app_pool_name,
			global_settings settings);
		virtual ~IisProcessContext();

		HTTP_MODULE_ID module_id() { return module_id_; };
		IisRewriteDriverFactory* driver_factory() { return driver_factory_; };
		MessageHandler* handler() { return message_handler_; };
		IisRewriteOptions* root_options() { return root_options_; };
		DWORD page_size() { return page_size_; }
		//RequestContext *CreatePagespeedRequestContext() {return new RequestContext(thread_system_->NewMutex());}
		IisServerContext* GetServerContext(const GoogleString& site_app_id, const char * hostname, unsigned int port, const char * config_path);
		void Shutdown();
		global_settings *  settings() { return &settings_; }
		void AddRef() { reference_count_.fetch_add(1); }
		void ReleaseRef() { if ( reference_count_.fetch_sub(1) == 1 )	{this->Shutdown();delete this;} }
		bool ok() { return ok_; };

		// Diagnostic surface for OnBeginRequest's local-only error page.
		// When ok() returns false, these report WHY initialization failed,
		// so the response can give the operator actionable detail (e.g. the
		// resolved cache path and the AppPool identity that couldn't write
		// to it) instead of the legacy generic "missing configuration" page.
		// All getters are valid to call after construction; values are set
		// by GetServerContext() as failures are detected.
		enum class InitFailureKind {
			kOk = 0,
			kCachePathEmpty,
			kCachePathMissing,
			kCachePathUnwritable,
			kCachePathCreateFailed,
			// LogDir auto-create failure. Parallel to
			// kCachePathCreateFailed but for the logs tree (RX+W grant
			// on the conditional ACL leg, mirroring Product.wxs
			// GrantLogAcl). The failed_init_path() accessor returns
			// the LogDir path in this case (not a cache path).
			kLogDirCreateFailed,
			kPostConfigFailed,
			kUnknown,
		};
		InitFailureKind init_failure_kind() const { return init_failure_kind_; }
		const GoogleString& init_error_message() const { return init_error_message_; }
		// The rename: this accessor previously named
		// failed_cache_path() returned the cache path on every failure
		// mode. Now that LogDir failures also populate it, the name was
		// misleading — it is "the directory whose init failed,"
		// whichever directory that was. The cache-path failure kinds
		// still populate it with the cache path; kLogDirCreateFailed
		// populates it with the LogDir path. Consumers (the diagnostic
		// page in iis_http_module.cpp) switch on init_failure_kind() to
		// know which.
		const GoogleString& failed_init_path() const { return failed_init_path_; }
		const GoogleString& app_pool_identity() const { return app_pool_identity_; }

		// Returns a PSID pointing into the worker SID bytes captured at
		// construction time (before the OpenProcessToken handle was closed).
		// Stable for the lifetime of this IisProcessContext. Used by the
		// IIS factory's EnsureDirectoryWritable override to build the
		// conditional Modify ACE on probe failure.
		// Returns nullptr if SID capture failed at construction (worker
		// token was unavailable); callers MUST null-check and fall back
		// to inherited ACLs / diagnostic page in that case.
		PSID worker_sid() {
			return worker_sid_buf_.empty()
				? nullptr
				: reinterpret_cast<PSID>(worker_sid_buf_.data());
		}

		ULONGLONG lastchecktime;
		struct FileCheck FileID1;
		struct FileCheck FileID2;
		GoogleString site_app_id() { return site_app_id_; }

		static ProcessContext* PSOL_PROCESS_CONTEXT;

	private:
		// Serializes the one-time initialization in GetServerContext:
		// IIS dispatches requests to a fresh worker on many threadpool
		// threads at once, and the old unsynchronized driver_factory_
		// check-then-act let two threads run the init branch concurrently,
		// clobbering each other's factory/mutex members mid-init and
		// publishing a ServerContext whose decoding_driver_ was never set
		// (AV in IsPagespeedResource on a ~6s-old w3wp).
		std::mutex init_mutex_;
		const GoogleString site_app_id_;
		std::atomic<DWORD> reference_count_;
		HTTP_MODULE_ID module_id_;
		IisRewriteDriverFactory* driver_factory_; 
		MessageHandler * message_handler_;
		IisRewriteOptions* root_options_;
		ProxyFetchFactory* fetch_factory_;
		IisServerContext* server_context_;
		DWORD page_size_;
		// KS: needed for request context
		AbstractMutex *loggingMutex_;
		std::wstring app_pool_name_;
		AtomicInt32 stopped_;
		global_settings settings_;
		AbstractMutex* server_context_mutex_;
		bool ok_;

		// Init-failure diagnostics. Populated by GetServerContext() on the
		// non-ok() exit paths; consumed by IisHttpModule::OnBeginRequest to
		// render a per-failure-mode local-only error page.
		InitFailureKind init_failure_kind_ = InitFailureKind::kOk;
		GoogleString init_error_message_;
		// Path that the init-failure references. For cache-path failure
		// kinds, the cache path; for kLogDirCreateFailed, the
		// LogDir path. Renamed from failed_cache_path_ to reflect the
		// generalization; same lifecycle (populated only on failure
		// paths).
		GoogleString failed_init_path_;
		GoogleString app_pool_identity_;  // e.g. "IIS APPPOOL\\DefaultAppPool"

		// Captured at ctor (before CloseHandle on the worker token) so the
		// IIS factory's EnsureDirectoryWritable can build an ACE for the
		// worker without re-resolving via the locale-fragile
		// LookupAccountNameW path under restricted AppPool tokens.
		// Empty if SID capture failed; worker_sid() returns nullptr
		// in that case.
		std::vector<BYTE> worker_sid_buf_;
	};
}                                                                     

#endif // IIS_PROCESS_CONTEXT_H_    