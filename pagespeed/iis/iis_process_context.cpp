#include "pagespeed/iis/iis_process_context.h"

#include <sddl.h>      // ConvertSidToStringSidW
#include <mutex>       // std::once_flag, std::call_once
#include <vector>

#include "pagespeed/automatic/proxy_fetch.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "pagespeed/kernel/base/google_message_handler.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/iis/iis_rewrite_driver_factory.h"
#include "pagespeed/iis/iis_message_handler.h"
#include "pagespeed/iis/iis_global_constants.h"
#include "pagespeed/iis/iis_rewrite_options.h"
//#include "win_thread/win_thread_system.h"
#include "pagespeed/kernel/base/stdio_file_system.h"

#include "pagespeed/system/system_caches.h"
#include "pagespeed/system/system_thread_system.h"

#include "pagespeed/iis/iis_misc.h"

#include "pagespeed/iis/log_message_handler.h"
#include "pagespeed/iis/iis_configuration.h"
#include "pagespeed/iis/iis_server_context.h"

#include "pagespeed/kernel/base/atomic_int32.h"
#include "pagespeed/kernel/sharedmem/inprocess_shared_mem.h"

namespace net_instaweb {

extern std::string modulePath;

namespace {

// Format a Win32 GetLastError() code into a human-readable message,
// stripping trailing CRLF. Falls back to "win32 error <N>" on failure
// so callers always get a non-empty string suitable for an error page.
GoogleString Win32ErrorString(DWORD code) {
	LPSTR buf = nullptr;
	DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER
		| FORMAT_MESSAGE_FROM_SYSTEM
		| FORMAT_MESSAGE_IGNORE_INSERTS;
	DWORD len = FormatMessageA(flags, nullptr, code,
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		reinterpret_cast<LPSTR>(&buf), 0, nullptr);
	if (len == 0 || buf == nullptr) {
		return StrCat("win32 error ", IntegerToString(static_cast<int>(code)));
	}
	GoogleString out(buf, len);
	LocalFree(buf);
	// Strip trailing whitespace / CRLF that FormatMessage appends.
	while (!out.empty() &&
		   (out.back() == '\r' || out.back() == '\n' ||
		    out.back() == ' '  || out.back() == '.')) {
		out.pop_back();
	}
	if (out.empty()) {
		return StrCat("win32 error ", IntegerToString(static_cast<int>(code)));
	}
	return out;
}

// Resolve the current process's identity in DOMAIN\Name form (e.g.
// "IIS APPPOOL\DefaultAppPool", "NT AUTHORITY\NETWORK SERVICE") for
// inclusion in the diagnostic error page, AND capture the worker
// SID bytes for later ACL construction. Falls back to
// S-1-5-... SID string form if LookupAccountSid fails (rare; e.g.
// the SID is well-known but the local SAM can't translate it under
// restricted app-pool tokens), and to "(unknown)" if even the SID
// conversion fails. On total failure |sid_out| is left empty and
// downstream auto-create code must skip the conditional ACL grant
// and fall through to the diagnostic page.
//
// Uses OpenProcessToken+CloseHandle rather than the Win8+
// GetCurrentProcessToken pseudo-handle so this compiles cleanly
// against the project's _WIN32_WINNT=0x0601 (Windows 7) baseline
// declared in .bazelrc.
GoogleString ResolveWorkerIdentity(std::vector<BYTE>* sid_out) {
	if (sid_out != nullptr) sid_out->clear();
	HANDLE token = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
		return "(unknown)";
	}
	DWORD needed = 0;
	GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
	if (needed == 0) { CloseHandle(token); return "(unknown)"; }
	std::vector<BYTE> token_buf(needed);
	if (!GetTokenInformation(token, TokenUser, token_buf.data(), needed, &needed)) {
		CloseHandle(token);
		return "(unknown)";
	}
	PSID sid = reinterpret_cast<TOKEN_USER*>(token_buf.data())->User.Sid;

	// Copy out the SID bytes BEFORE CloseHandle. The SID lives inside
	// token_buf; we want a self-contained buffer where PSID = &buf[0]
	// is a valid SID and survives token_buf going out of scope. The
	// caller owns sid_out — we just copy in.
	if (sid_out != nullptr && IsValidSid(sid)) {
		DWORD sid_len = GetLengthSid(sid);
		if (sid_len > 0) {
			sid_out->resize(sid_len);
			if (!CopySid(sid_len, sid_out->data(), sid)) {
				sid_out->clear();
			}
		}
	}
	CloseHandle(token);

	WCHAR name[256];
	WCHAR domain[256];
	DWORD nlen = 256;
	DWORD dlen = 256;
	SID_NAME_USE use;
	if (LookupAccountSidW(nullptr, sid, name, &nlen, domain, &dlen, &use)) {
		std::wstring full;
		if (dlen > 0 && domain[0] != L'\0') {
			full.assign(domain);
			full.push_back(L'\\');
		}
		full.append(name);
		return ws2s(full);
	}

	LPWSTR sid_str = nullptr;
	if (ConvertSidToStringSidW(sid, &sid_str) && sid_str != nullptr) {
		GoogleString out = ws2s(sid_str);
		LocalFree(sid_str);
		return out;
	}
	return "(unknown)";
}

}  // namespace


// the design record §5: gate the "AutoCreateCachePath: on|off" INFO line so it
// fires once per process, not once per site. IisProcessContext is
// per-site (constructed in iis_module_factory.cpp per site_app_id), but
// the AutoCreateCachePath directive is process-scope (kProcessScopeStrict
// in the property registration) and the diagnostic line is operational
// noise on multi-site hosts otherwise.
static std::once_flag g_auto_create_log_once;

ProcessContext* IisProcessContext::PSOL_PROCESS_CONTEXT = NULL;

IisProcessContext::IisProcessContext(
	const GoogleString& site_app_id,
	HTTP_MODULE_ID module_id, 
	DWORD page_size, 
	IisRewriteOptions* root_options,
	std::wstring app_pool_name,
	global_settings settings
	)  
	: site_app_id_(site_app_id),
	module_id_(module_id)
	, message_handler_( new GoogleMessageHandler() ) // TODO(oschaaf): want IisMessageHandler here.
	, page_size_(page_size)
	, root_options_(root_options)
	, app_pool_name_(app_pool_name)
	, settings_(settings)
	, server_context_mutex_(NULL)
	, ok_(true)
	, server_context_(NULL)

{
	stopped_.set_value(0);
	reference_count_=1;
	driver_factory_ = NULL;
	// Resolve the IIS worker identity once at construction. It does not
	// change for the lifetime of the process, and we want it available
	// even on early init-failure paths that bail before GetServerContext.
	// We also capture the worker SID bytes here (before any CloseHandle
	// on the worker token in ResolveWorkerIdentity) so the factory's
	// EnsureDirectoryWritable can build an ACE without re-resolving via
	// the locale-fragile LookupAccountNameW path under restricted AppPool
	// tokens. See the design record §3a.
	app_pool_identity_ = ResolveWorkerIdentity(&worker_sid_buf_);
}

IisServerContext* IisProcessContext::GetServerContext(const GoogleString& site_app_id, const char * hostname,
	unsigned int port, const char * config_path)
{
	if (!ok_) { 
		CHECK(false) << "Forbidden code path - the process context is not OK. Check it before requesting a server context.";
	}

	if (driver_factory_ == NULL) {
		auto ts = new SystemThreadSystem();
		server_context_mutex_ = ts->NewMutex();
		driver_factory_ = new IisRewriteDriverFactory(this, app_pool_name_, ts, new InProcessSharedMem(ts));
		driver_factory_->Init();
	} else {
		// Might be NULL from a previous attempt.
		return server_context_;
	}
	//DebugBreak();
	ScopedMutex lock(server_context_mutex_);

	if (server_context_ == NULL)
	{
		IisServerContext* server_context;
		std::list<std::string> paths;
		paths.push_back(config_path);
		std::map<std::string,std::string> input;

		std::string foopath("/");
		input["path"]=std::string(foopath.data(),foopath.length());
		input["fullpath"]=std::string(foopath.data(),foopath.length());
		input["hostname"]=std::string(hostname);
		input["config"]=std::string("base");		 
		
		ConfigFactory *cf = new ConfigFactory();
		IisRewriteOptions * options = new IisRewriteOptions(driver_factory_->thread_system());
		cf->GetConfig(paths, input, *options, driver_factory_->message_handler(), settings());
		delete cf;
		if (!logToEventLogSet) 
		logToEventLog=false; // the options parser will have set logToEventLogSet

		message_handler_->Message( kInfo, "create server context");

		server_context = driver_factory_->MakeIisServerContext(site_app_id, port);
		IisRewriteOptions* server_options = root_options_->Clone();
		
		// If we have not file cache path, we postfix it with site-id here to make it unique accross sites
		// to ensure we don't overwrite shmem snapshats as different sites/processes dump them.
		if (options->file_cache_path().empty() && !root_options_->file_cache_path().empty()) {
			GoogleString server_cache_path = StrCat(root_options_->file_cache_path(), "/", site_app_id);
			options->set_file_cache_path(server_cache_path);
		}

		server_options->Merge(*options);
		delete options;
		
		server_context->global_options()->Merge(*server_options);
		delete server_options;	

		// oschaaf: beware: do not remove the line below - global_options() needs to be called!
		server_context->global_options();
		std::vector<SystemServerContext*> server_contexts;
		server_contexts.push_back(server_context);
		GoogleString error_message;
		int error_index = -1;
		Statistics* global_statistics = NULL;
		driver_factory_->PostConfig(
			server_contexts, &error_message, &error_index, &global_statistics);
		if (error_index != -1) {
			driver_factory_->message_handler()->Message(
				kError, "IIS WebSpeed is not enabled. %s", error_message.c_str());
			// Capture the PostConfig failure for the diagnostic error page.
			// Note: ok_ stays true on this path because the legacy code did
			// not mark the process context as failed here — it just returns
			// NULL and lets the caller (CreateRequestContext → http_module)
			// fall through to the local-only error page. We only annotate.
			init_failure_kind_ = InitFailureKind::kPostConfigFailed;
			init_error_message_ = error_message;
			return NULL;
		}
		if (global_statistics == NULL) {
			IisRewriteDriverFactory::InitStats(driver_factory_->statistics());
		}
		driver_factory_->SetServerContextMessageHandler(server_context);
		driver_factory_->RootInit();
		driver_factory_->ChildInit();

		// Verify the cache path is configured, exists, and is writable by the
		// worker identity. Each failure mode populates init_failure_kind_ +
		// init_error_message_ + failed_init_path_ (set on failure only per
		// the design record §4 last paragraph; renamed from failed_cache_path_ per
		// since LogDir failures now use the same member) so the
		// local-only error page in
		// IisHttpModule::OnBeginRequest can render an actionable diagnostic
		// (which path, which identity, which OS error) and a stable
		// X-Pagespeed-Init-Status response header for CI / test fixtures
		// to match on without scraping the page body.
		auto cache_path = server_context->global_system_rewrite_options()->file_cache_path();
		IisRewriteOptions* effective_options =
			dynamic_cast<IisRewriteOptions*>(server_context->global_options());

		// Case 1: no FileCachePath at all. This is the classic "module
		// loaded but pagespeed.config missing or empty" symptom.
		if (cache_path.empty()) {
			init_failure_kind_ = InitFailureKind::kCachePathEmpty;
			failed_init_path_ = cache_path;
			driver_factory_->message_handler()->Message(kError,
				"FileCachePath is empty in pagespeed.config");
			ok_ = false;
			driver_factory_->ShutDown();
			return NULL;
		}

		// the design record §5: emit the AutoCreateCachePath status once per
		// process (this IisProcessContext is per-site; the directive is
		// process-scope). The std::call_once gate ensures multi-site
		// hosts get one line not N.
		std::call_once(g_auto_create_log_once, [&]() {
			const bool on = effective_options != nullptr
				? effective_options->auto_create_cache_path()
				: true;
			driver_factory_->message_handler()->Message(kInfo,
				"AutoCreateCachePath: %s", on ? "on" : "off");
		});

		// the design record §3 +: when AutoCreateCachePath is on
		// (default) AND the path passes the hardcoded prefix guardrail
		// (PageSpeed\cache\ OR IISWebSpeed\cache\), delegate to the
		// factory's mkdir+probe+conditional-ACL sequence. On success,
		// the directory exists and is writable — skip the legacy
		// existence check and proceed to the post-create write probe.
		// On failure, surface kCachePathCreateFailed with the
		// underlying Win32 error. If AutoCreateCachePath is off OR
		// the path is out-of-prefix, fall through to the legacy
		// existence check (which sets kCachePathMissing; its
		// diagnostic copy surfaces the prefix rationale).
		//
		// Prefix gating runs HERE via IsPathInAutoCreatePrefix() — not
		// inside EnsureDirectoryWritable — so the dispatch can no
		// longer be silently re-routed by a future edit to a
		// stringly-typed error message.
		const bool auto_create_on = effective_options != nullptr
			? effective_options->auto_create_cache_path()
			: true;
		bool autocreate_handled_existence = false;
		if (auto_create_on &&
		    driver_factory_->IsPathInAutoCreatePrefix(cache_path)) {
			GoogleString ec_error;
			if (driver_factory_->EnsureDirectoryWritable(cache_path, &ec_error)) {
				autocreate_handled_existence = true;
			} else {
				init_failure_kind_ = InitFailureKind::kCachePathCreateFailed;
				init_error_message_ = ec_error;
				failed_init_path_ = cache_path;
				driver_factory_->message_handler()->Message(kError,
					"Auto-create of FileCachePath failed: %s (identity: %s) - %s",
					cache_path.c_str(), app_pool_identity_.c_str(),
					ec_error.c_str());
				ok_ = false;
				driver_factory_->ShutDown();
				return NULL;
			}
		}

		// Case 2: path is set but does not resolve to a directory. Distinct
		// from "not writable" because the operator remedy differs: create
		// the directory (and grant ACLs) rather than just grant ACLs.
		// Skipped when auto-create above already created+probed the dir.
		if (!autocreate_handled_existence) {
			DWORD attrs = GetFileAttributesA(cache_path.c_str());
			if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
				init_failure_kind_ = InitFailureKind::kCachePathMissing;
				DWORD gle = GetLastError();
				init_error_message_ = Win32ErrorString(gle);
				failed_init_path_ = cache_path;
				driver_factory_->message_handler()->Message(kError,
					"FileCachePath does not exist or is not a directory: %s (%s)",
					cache_path.c_str(), init_error_message_.c_str());
				ok_ = false;
				driver_factory_->ShutDown();
				return NULL;
			}
		}

		// Case 3: directory exists but is not writable by the worker. Use
		// the existing OpenTempFile probe (real-world I/O including any
		// AV/EDR filter-driver interference, not just ACL inspection).
		// Note: when auto-create succeeded above, the factory already
		// ran an OpenTempFile probe; this second probe is a
		// belt-and-braces re-check and costs microseconds in the
		// common case.
		bool cache_path_ok = false;
		GoogleString last_io_error;
		{
			GoogleString tc = cache_path;
			if (tc[0] == '/' && tc[tc.size() - 1] != '/') {
				tc += "/";
			} else if (tc[tc.size() - 1] != '\\') {
				tc += "\\";
			}
			auto tempfile = driver_factory_->file_system()->OpenTempFile(
				tc, driver_factory_->message_handler());
			if (tempfile) {
				cache_path_ok = true;
				GoogleString fn = tempfile->filename();
				if (tempfile->Write("test", driver_factory_->message_handler())) {
					if (!tempfile->Flush(driver_factory_->message_handler())) {
						// Capture Win32 error code BEFORE any subsequent
						// I/O (Close, RemoveFile, even the next branch's
						// Close-failed assignment) clobbers GetLastError().
						last_io_error = Win32ErrorString(GetLastError());
						cache_path_ok = false;
					}
				} else {
					last_io_error = Win32ErrorString(GetLastError());
					cache_path_ok = false;
				}
				if (!driver_factory_->file_system()->Close(
					    tempfile, driver_factory_->message_handler())) {
					// Only overwrite last_io_error if we don't already have
					// one from Write/Flush above; the earliest failure is
					// the most diagnostic.
					if (last_io_error.empty()) {
						last_io_error = Win32ErrorString(GetLastError());
					}
					cache_path_ok = false;
				}
				driver_factory_->file_system()->RemoveFile(
					fn.c_str(), driver_factory_->message_handler());
			} else {
				// Best-effort: OpenTempFile didn't go through GetLastError(),
				// but it's the most likely source if anything was logged.
				last_io_error = Win32ErrorString(GetLastError());
			}
		}

		if (!cache_path_ok) {
			init_failure_kind_ = InitFailureKind::kCachePathUnwritable;
			init_error_message_ = last_io_error.empty()
				? GoogleString("write probe failed")
				: last_io_error;
			failed_init_path_ = cache_path;
			driver_factory_->message_handler()->Message(kError,
				"Server context cache path not writeable: %s (identity: %s)",
				cache_path.c_str(), app_pool_identity_.c_str());
			ok_ = false;
			driver_factory_->ShutDown();
			return NULL;
		}

		// LogDir auto-create — the referenced issue, the design record §Operational.
		// Parallel structure to the cache path block above; deliberately
		// placed AFTER cache succeeds so cache failures (the louder,
		// trial-customer-visible class) render their dedicated
		// diagnostic page first. Gated on:
		//   - LogDir non-empty (pre-existing module behaviour leaves
		//     LogDir empty in some configurations — nothing to create)
		//   - AutoCreateLogDir = on (default; opt-out per directive)
		//   - LogDir in one of the canonical logs prefixes
		//     (PageSpeed\logs\ or IISWebSpeed\logs\) — out-of-prefix
		//     LogDir values are operator-customized and we leave them
		//     untouched, preserving legacy behaviour.
		// ACL grant on the conditional suspenders leg is RX+W (no
		// DELETE), mirroring Product.wxs GrantLogAcl — workers append
		// to logs but admin owns rotation.
		auto log_dir = server_context->global_system_rewrite_options()->log_dir();
		const bool auto_create_log_on = effective_options != nullptr
			? effective_options->auto_create_log_dir()
			: true;
		if (!log_dir.empty() && auto_create_log_on &&
		    driver_factory_->IsLogDirInAutoCreatePrefix(log_dir)) {
			GoogleString log_err;
			if (!driver_factory_->EnsureDirectoryWritable(
				    log_dir, &log_err,
				    driver_factory_->LogDirAclMask())) {
				init_failure_kind_ = InitFailureKind::kLogDirCreateFailed;
				init_error_message_ = log_err;
				failed_init_path_ = log_dir;
				driver_factory_->message_handler()->Message(kError,
					"Auto-create of LogDir failed: %s (identity: %s) - %s",
					log_dir.c_str(), app_pool_identity_.c_str(),
					log_err.c_str());
				ok_ = false;
				driver_factory_->ShutDown();
				return NULL;
			}
		}

		fetch_factory_ = new ProxyFetchFactory(server_context);
		server_context->set_fetch_factory(fetch_factory_);
		driver_factory_->StartThreads();
		server_context_ = server_context;
	}
	return server_context_;
}

void IisProcessContext::Shutdown()
{
	// TODO: looks like barrierincrement has become unnessecary
	if (stopped_.BarrierIncrement(1) == 1) 
	{
		server_context_mutex_->Lock();
		message_handler_->Message( net_instaweb::kInfo, "delete driver factory");
	
		if (driver_factory_ != NULL) 
		{
			delete driver_factory_;
			driver_factory_ = NULL;
		}
		if ( fetch_factory_ != NULL ) {
			message_handler_->Message( net_instaweb::kInfo, "delete fetch factory");
			delete fetch_factory_;
			fetch_factory_ = NULL;
		}
		if (root_options_ != NULL)
		{
			delete root_options_;
			root_options_ = NULL;
		}


		message_handler_->Message(kInfo, "Iis Process Context terminated");
		delete message_handler_;
		message_handler_ = NULL;


		server_context_mutex_->Unlock();
		delete server_context_mutex_;
		server_context_mutex_= NULL;
	}
}

IisProcessContext::~IisProcessContext() 
{
}	

}                                                                     

