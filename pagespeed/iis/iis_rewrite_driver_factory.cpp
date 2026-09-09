// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/iis/iis_rewrite_driver_factory.h"
#include <cstdio>
#include <stdlib.h>

#ifndef _WINSOCKAPI_
#define _WINSOCKAPI_
#endif
#include <Windows.h>
#include <accctrl.h>
#include <aclapi.h>
#include <sddl.h>

#include "pagespeed/kernel/base/file_system.h"
#include "pagespeed/kernel/http/content_type.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_driver_factory.h"
#include "net/instaweb/rewriter/public/static_asset_manager.h"
#include "pagespeed/kernel/base/google_message_handler.h"
#include "pagespeed/kernel/base/std_timer.h"
#include "pagespeed/kernel/cache/lru_cache.h"
#include "pagespeed/kernel/sharedmem/inprocess_shared_mem.h"
#include "pagespeed/kernel/base/md5_hasher.h"
#include "pagespeed/kernel/base/stdio_file_system.h"
#include "pagespeed/kernel/util/simple_stats.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/cache/threadsafe_cache.h"
#include "pagespeed/kernel/thread/scheduler_thread.h"
#include "pagespeed/kernel/sharedmem/shared_circular_buffer.h"
#include "pagespeed/kernel/sharedmem/shared_mem_statistics.h"
#include "pagespeed/kernel/sharedmem/shared_mem_lock_manager.h"
#include "pagespeed/system/system_caches.h"
#include "pagespeed/automatic/proxy_interface.h"

#include "pagespeed/iis/util.h"
#include "pagespeed/iis/iis_async_url_fetcher.h"
#include "pagespeed/iis/iis_message_handler.h"
#include "pagespeed/iis/iis_rewrite_options.h"
#include "pagespeed/iis/iis_server_context.h"
#include "pagespeed/iis/iis_global_constants.h"
#include "pagespeed/iis/iis_process_context.h"

#include "pagespeed/iis/log_message_handler.h"
#include "pagespeed/iis/iis_request_context.h"
#include "pagespeed/kernel/util/hashed_nonce_generator.h"

#include <bcrypt.h>

namespace net_instaweb {

namespace {

// Format a Win32 error code into a human-readable, single-line message.
// Mirrors the helper in iis_process_context.cpp (kept local here to
// avoid a public dependency-direction edge between sibling .cpp files
// in the same cc_library).
GoogleString FactoryWin32ErrorString(DWORD code) {
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

// Prefix scope guardrail. Canonicalize via GetFullPathNameW (resolves
// "." / ".." / relative segments BUT does NOT resolve symlinks /
// junctions — that's why the reparse-point check in
// EnsureDirectoryWritable is also mandatory) and require the result to
// begin (case-insensitive) with one of the supplied hardcoded prefixes.
// Operator cannot widen the prefix via config — by design.
//
// Shared between IsPathInAutoCreatePrefixImpl (cache) and
// IsLogDirInAutoCreatePrefixImpl (logs); the only difference is the
// prefix list passed in.
bool IsPathInPrefixListImpl(const GoogleString& path,
                            const wchar_t* const* prefixes,
                            size_t n_prefixes) {
	std::wstring wpath = s2ws(path);
	if (wpath.empty()) return false;

	// Resolve to a canonical absolute Win32 path. Buffer up to
	// MAX_PATH initially, retry once on overflow (paths above MAX_PATH
	// without the \\?\ prefix can still be passed here — be defensive).
	wchar_t small_buf[MAX_PATH];
	DWORD needed = GetFullPathNameW(wpath.c_str(), MAX_PATH, small_buf, nullptr);
	std::wstring full;
	if (needed == 0) {
		return false;
	} else if (needed < MAX_PATH) {
		full.assign(small_buf);
	} else {
		std::vector<wchar_t> big(needed + 1);
		DWORD again = GetFullPathNameW(wpath.c_str(),
			static_cast<DWORD>(big.size()), big.data(), nullptr);
		if (again == 0 || again >= big.size()) return false;
		full.assign(big.data(), again);
	}

	// Hardcoded prefix scope. The trailing backslash on each prefix
	// is mandatory: we must not accept the bare ...\cache (or ...\logs)
	// directory itself — only paths strictly underneath. Comparison is
	// case-insensitive per Windows filesystem semantics.
	for (size_t i = 0; i < n_prefixes; ++i) {
		const wchar_t* prefix = prefixes[i];
		size_t plen = wcslen(prefix);
		if (full.size() > plen &&
		    _wcsnicmp(full.c_str(), prefix, plen) == 0) {
			return true;
		}
	}
	return false;
}

// Prefix scope guardrail for the cache tree. See
// IsPathInPrefixListImpl above for the canonicalize+compare shape.
//
// thin wrapper IisRewriteDriverFactory::IsPathInAutoCreatePrefix
// exposes this through the cross-port RewriteDriverFactory virtual so
// IisProcessContext::GetServerContext can branch on prefix membership
// without string-matching the EnsureDirectoryWritable error message.
bool IsPathInAutoCreatePrefixImpl(const GoogleString& path) {
	static const wchar_t* kCachePrefixes[] = {
		L"C:\\ProgramData\\We-Amp\\PageSpeed\\cache\\",
		L"C:\\ProgramData\\We-Amp\\IISWebSpeed\\cache\\",
	};
	return IsPathInPrefixListImpl(path, kCachePrefixes,
		sizeof(kCachePrefixes) / sizeof(kCachePrefixes[0]));
}

// prefix scope guardrail for
// the logs tree. Same shape as IsPathInAutoCreatePrefixImpl above but
// with the logs-tree prefixes. Out-of-prefix LogDir values cause the
// caller in IisProcessContext::GetServerContext to skip auto-create
// entirely (no mkdir, no diagnostic page) — preserving legacy
// behaviour for operator-customized LogDir locations.
bool IsLogDirInAutoCreatePrefixImpl(const GoogleString& path) {
	static const wchar_t* kLogPrefixes[] = {
		L"C:\\ProgramData\\We-Amp\\PageSpeed\\logs\\",
		L"C:\\ProgramData\\We-Amp\\IISWebSpeed\\logs\\",
	};
	return IsPathInPrefixListImpl(path, kLogPrefixes,
		sizeof(kLogPrefixes) / sizeof(kLogPrefixes[0]));
}

// RAII wrapper for a Win32 HANDLE so the auto-create probe / ACL
// sequence can early-return without leaking the handle from (d).
class ScopedHandle {
public:
	explicit ScopedHandle(HANDLE h = INVALID_HANDLE_VALUE) : h_(h) {}
	~ScopedHandle() {
		if (h_ != nullptr && h_ != INVALID_HANDLE_VALUE) {
			CloseHandle(h_);
		}
	}
	HANDLE get() const { return h_; }
	bool valid() const {
		return h_ != nullptr && h_ != INVALID_HANDLE_VALUE;
	}
	ScopedHandle(const ScopedHandle&) = delete;
	ScopedHandle& operator=(const ScopedHandle&) = delete;
private:
	HANDLE h_;
};

}  // namespace

	  // namespace

	class CacheInterface;
	class FileSystem;
	class Hasher;
	class MessageHandler;
	class Statistics;
	class Timer;
	class UrlAsyncFetcher;
	class UrlFetcher;
	class Writer;

	IisRewriteDriverFactory::IisRewriteDriverFactory(
		IisProcessContext* process_context,
		std::wstring app_pool_name, 
		SystemThreadSystem* thread_system,
		AbstractSharedMem* shm_runtime)
		: SystemRewriteDriverFactory(*IisProcessContext::PSOL_PROCESS_CONTEXT,
		thread_system, 
		//WEAMPKS: 1.9 NULL is default shared memory
		shm_runtime,
		"foo.com",
		80),
	 process_context_(process_context),
	 app_pool_name_(app_pool_name),
	 use_per_vhost_statistics_(true),
	 use_native_fetcher_(process_context->settings()->use_native_fetcher),
	 //iis_url_async_fetcher_(NULL),
	 iis_shared_circular_buffer_(NULL),
	 iis_message_handler_(new IisMessageHandler(timer(), this->thread_system()->NewMutex())),
	 iis_html_parse_message_handler_(new IisMessageHandler(timer(), this->thread_system()->NewMutex())),
	 site_app_id_(process_context->site_app_id()), shut_down_(false)
	{
		InitializeDefaultOptions();
		// TOD(oschaaf): 1.9 -> def. beacon url? maintain  it?>
		//default_options()->set_beacon_url("/");
		SystemRewriteOptions* system_options = (SystemRewriteOptions*)(
			default_options());
		system_options->set_avoid_renaming_introspective_javascript(true);
		// We tune down the 50 MB default shared mem cache, because on IIS we use one per process.
		system_options->set_default_shared_memory_cache_kb(1024*5);
		system_options->set_lru_cache_byte_limit(16384);
		system_options->set_lru_cache_kb_per_process(1024*10);
		set_message_buffer_size(process_context->settings()->message_buffer_size);
		set_message_handler(iis_message_handler_);
		set_html_parse_message_handler(iis_html_parse_message_handler_);

		// We can't support convert_meta_tags as we don't see the response body before we send out the headers.
		system_options->ForbidFiltersByCommaSeparatedList("convert_meta_tags", NULL);
		// we need to seal the system_options to prevent a debug dcheck
		
		system_options->ComputeSignature();
		created_at_ = time(0);
		GoogleString err;
		// TODO(oschaaf): 1.9!
		// TODO(oschaaf): -> https moet configureerbaar zijn, opties:		

		//TODO(oschaaf): release. name should be prefixed with app-pool id
		// we need to know if we are the one that is creating the segment, instead of attaching
		// it, because the datastructure(s) that live in it need to be initialised
		//std::wstring mutex_name(app_pool_name_);
		//mutex_name.append(L"_iispeed_factory_startup");
		/*
		std::wstring mutex_name(L"_iispeed_factory_startup");

		SECURITY_ATTRIBUTES sa;
		SECURITY_DESCRIPTOR sd;
		InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
		SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE);
		sa.nLength = sizeof(sa);
		sa.lpSecurityDescriptor = &sd;
		HANDLE mutex = CreateMutex(&sa,true,mutex_name.c_str());
		DWORD last_err = GetLastError();
		bool is_root = last_err != ERROR_ALREADY_EXISTS;
		message_handler()->Message(kInfo, ">>>>>>>>>>> iispeed_factory_startup for pool [%s] - [%s]"
			, ws2s(app_pool_name_).c_str(),  is_root ? "true" : "false");
			*/
	}

	void IisRewriteDriverFactory::ShutDownFetchers() {
		message_handler()->Message(net_instaweb::kInfo, "IisRewriteDriverFactory::ShutDownFetchers()");
		for (int i = 0; i < native_fetchers_.size(); i++) {
			native_fetchers_[i]->ShutDown();
		}
		native_fetchers_.clear();
	}

	IisRewriteDriverFactory::~IisRewriteDriverFactory() {
		message_handler()->Message( net_instaweb::kInfo, "IisRewriteDriverFactory::~IisRewriteDriverFactory()");
		ShutDown();
		//Sleep(100);
		if (iis_shared_circular_buffer_ != NULL) {
			//delete iis_shared_circular_buffer_;
			iis_shared_circular_buffer_ = NULL;
		}
	}

	void IisRewriteDriverFactory::ShutDown() {
		if (!shut_down_) {
			shut_down_ = true;
			SystemRewriteDriverFactory::ShutDown();
		}
	}

	// IIS prefix-scope predicate (cache).
	// Thin wrapper over the file-scope helper so the cross-port virtual
	// on RewriteDriverFactory can be overridden without exposing the
	// Win32-specific implementation in the header.
	bool IisRewriteDriverFactory::IsPathInAutoCreatePrefix(
		const GoogleString& path) {
		return IsPathInAutoCreatePrefixImpl(path);
	}

	// the referenced issue IIS prefix-scope predicate
	// (logs). Parallel to IsPathInAutoCreatePrefix but matches the
	// logs-tree prefixes. Caller in iis_process_context.cpp gates on
	// this BEFORE delegating to EnsureDirectoryWritable, so the
	// out-of-prefix case stays silent (no mkdir, no diagnostic page) —
	// legacy LogDir behaviour preserved for operator-customized
	// LogDir locations.
	bool IisRewriteDriverFactory::IsLogDirInAutoCreatePrefix(
		const GoogleString& path) {
		return IsLogDirInAutoCreatePrefixImpl(path);
	}

	// Cache-path ACL mask: Modify, mirroring
	// Product.wxs GrantCacheAcl. Returned as uint32_t (not DWORD) to
	// keep the header platform-neutral; identical underlying values.
	uint32_t IisRewriteDriverFactory::CachePathAclMask() const {
		return FILE_GENERIC_READ | FILE_GENERIC_WRITE |
			FILE_GENERIC_EXECUTE | DELETE;
	}

	// the referenced issue LogDir ACL mask: RX+W
	// (no DELETE), mirroring Product.wxs GrantLogAcl. Workers append
	// to logs but admin owns rotation, so DELETE is intentionally
	// withheld — matches the WiX-side narrower grant.
	uint32_t IisRewriteDriverFactory::LogDirAclMask() const {
		return FILE_GENERIC_READ | FILE_GENERIC_WRITE |
			FILE_GENERIC_EXECUTE;
	}

	// IIS implementation of the cross-port init-time filesystem-prep
	// hook. See header for sequence overview.
	// Caller (IisProcessContext::GetServerContext) is expected to gate
	// on IsPathInAutoCreatePrefix() / IsLogDirInAutoCreatePrefix() FIRST
	// and skip this hook for out-of-prefix paths — removed
	// the prior in-hook prefix re-check / string-matched dispatch.
	// Failure modes that still surface here (return false + populated
	// |error_message|):
	//   - empty path                              (caller bug)
	//   - RecursivelyMakeDir fails                (disk full, EDR, etc.)
	//   - reparse-point planted at the result     (security event)
	//   - writability probe AND ACL grant both    (broken inheritance +
	//     fail                                     EDR blocking DACL write)
	// |acl_mask|: granular access mask applied to the worker SID on the
	// conditional ACL leg (step f). Value 0 means "use the cache
	// default" (Modify), matching legacy cache callers. The LogDir
	// caller passes LogDirAclMask() = RX+W (no DELETE),
	// mirroring Product.wxs GrantLogAcl.
	bool IisRewriteDriverFactory::EnsureDirectoryWritable(
		const GoogleString& path, GoogleString* error_message,
		uint32_t acl_mask) {
		auto set_err = [&](const GoogleString& m) {
			if (error_message != nullptr) *error_message = m;
		};

		if (path.empty()) {
			set_err("EnsureDirectoryWritable called with empty path");
			return false;
		}

		// Resolve |acl_mask|=0 (the sentinel-default for "use this
		// implementation's default") to the cache-path Modify mask —
		// preserves the legacy single-caller behaviour without
		// requiring the cache call site to be touched. The LogDir
		// caller passes LogDirAclMask() (RX+W, no DELETE) explicitly.
		const DWORD effective_acl_mask = (acl_mask != 0)
			? static_cast<DWORD>(acl_mask)
			: static_cast<DWORD>(CachePathAclMask());

		// (c) cheap belt: cross-port mkdir. Apache uses the same
		// primitive at directive-parse time (apache_server_context.cc:80).
		// RecursivelyMakeDir treats ERROR_ALREADY_EXISTS as success, so
		// concurrent worker startups race safely (§3g).
		if (!file_system()->RecursivelyMakeDir(path, message_handler())) {
			DWORD gle = GetLastError();
			set_err(StrCat("RecursivelyMakeDir failed: ",
				FactoryWin32ErrorString(gle)));
			return false;
		}

		// (d) reparse-point check on the resulting path. Open with
		// FILE_FLAG_OPEN_REPARSE_POINT so we don't traverse a
		// junction-plant; FILE_FLAG_BACKUP_SEMANTICS so we can open
		// a directory. WRITE_DAC + READ_CONTROL on the handle so the
		// same handle is reusable for the conditional ACL apply in (f).
		std::wstring wpath = s2ws(path);
		ScopedHandle dir_handle(::CreateFileW(
			wpath.c_str(),
			GENERIC_READ | WRITE_DAC | READ_CONTROL,
			FILE_SHARE_READ | FILE_SHARE_WRITE,
			nullptr,
			OPEN_EXISTING,
			FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
			nullptr));
		if (!dir_handle.valid()) {
			DWORD gle = GetLastError();
			set_err(StrCat("CreateFileW(reparse-check) failed: ",
				FactoryWin32ErrorString(gle)));
			return false;
		}

		FILE_ATTRIBUTE_TAG_INFO tag_info = {0};
		if (::GetFileInformationByHandleEx(dir_handle.get(),
			    FileAttributeTagInfo, &tag_info, sizeof(tag_info))) {
			if (tag_info.ReparseTag != 0) {
				set_err(StrCat(
					"reparse-point detected at ", path,
					" (ReparseTag=0x",
					IntegerToString(static_cast<int>(tag_info.ReparseTag)),
					"); refusing to auto-create"));
				return false;
			}
		} else {
			// Non-fatal: log + continue. The reparse-point attack
			// class we're closing requires NTFS in the first place,
			// and the canonical product tree is always on NTFS, so
			// any failure here is most likely a permissions edge
			// case (which the writability probe in (e) will catch).
			DWORD gle = GetLastError();
			message_handler()->Message(kWarning,
				"GetFileInformationByHandleEx(FileAttributeTagInfo) "
				"failed on %s: %s (continuing)", path.c_str(),
				FactoryWin32ErrorString(gle).c_str());
		}

		// (e) writability probe. Re-uses the same primitive the legacy
		// post-existence-check uses at iis_process_context.cpp
		// OpenTempFile path (returns OutputFile* — has Write/Flush;
		// the base File* does not). Common case: probe succeeds under
		// inherited installer ACLs, no SetSecurityInfo call.
		auto probe_writable = [&](GoogleString* probe_err) -> bool {
			GoogleString tc = path;
			if (!tc.empty() && tc.back() != '\\' && tc.back() != '/') {
				tc += "\\";
			}
			FileSystem::OutputFile* tempfile = file_system()->OpenTempFile(
				tc, message_handler());
			if (tempfile == nullptr) {
				if (probe_err != nullptr) {
					*probe_err = FactoryWin32ErrorString(GetLastError());
				}
				return false;
			}
			GoogleString fn = tempfile->filename();
			bool ok = true;
			GoogleString first_err;
			if (!tempfile->Write("probe", message_handler())) {
				first_err = FactoryWin32ErrorString(GetLastError());
				ok = false;
			} else if (!tempfile->Flush(message_handler())) {
				first_err = FactoryWin32ErrorString(GetLastError());
				ok = false;
			}
			if (!file_system()->Close(tempfile, message_handler())) {
				if (first_err.empty()) {
					first_err = FactoryWin32ErrorString(GetLastError());
				}
				ok = false;
			}
			file_system()->RemoveFile(fn.c_str(), message_handler());
			if (!ok && probe_err != nullptr) *probe_err = first_err;
			return ok;
		};

		GoogleString first_probe_err;
		if (probe_writable(&first_probe_err)) {
			return true;
		}

		// (f) conditional suspenders: ACL grant via SetSecurityInfo on
		// the retained handle from (d). Skip if SID capture failed at
		// ctor (no way to build an ACE) — surface the probe failure
		// instead.
		PSID worker_sid = process_context_ != nullptr
			? process_context_->worker_sid()
			: nullptr;
		if (worker_sid == nullptr) {
			set_err(StrCat(
				"writability probe failed (",
				first_probe_err.empty() ? GoogleString("no error") : first_probe_err,
				") and worker SID is unavailable for conditional ACL grant"));
			return false;
		}

		// Construct an EXPLICIT_ACCESS_W directly. The obvious helper
		// `BuildExplicitAccessWithSidW(...)` does not exist in the
		// Windows SDK aclapi.h (only the *Name* and *Sid* (no W
		// suffix) overloads exist, and the Sid overload takes the
		// PSID via TRUSTEE.ptstrName cast). The structure init is
		// straightforward and avoids any signature ambiguity.
		// Mask is parameterized: |effective_acl_mask| resolves to
		// CachePathAclMask() (Modify = RX+W+DELETE) for cache callers,
		// or LogDirAclMask() (RX+W, no DELETE) for the LogDir caller
		// per the referenced issue. Either way, no WRITE_DAC / WRITE_OWNER,
		// so the worker can never re-ACL the tree.
		EXPLICIT_ACCESS_W ea = {0};
		ea.grfAccessPermissions = effective_acl_mask;
		ea.grfAccessMode = GRANT_ACCESS;
		ea.grfInheritance = NO_INHERITANCE;
		ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
		ea.Trustee.TrusteeType = TRUSTEE_IS_UNKNOWN;
		// TRUSTEE_W.ptstrName is LPWSTR by type but for TRUSTEE_IS_SID
		// it is reinterpreted as PSID per the Win32 ACL APIs contract.
		ea.Trustee.ptstrName = reinterpret_cast<LPWSTR>(worker_sid);

		PACL new_dacl = nullptr;
		DWORD se = SetEntriesInAclW(1, &ea, nullptr, &new_dacl);
		if (se != ERROR_SUCCESS) {
			set_err(StrCat(
				"writability probe failed (",
				first_probe_err.empty() ? GoogleString("no error") : first_probe_err,
				") and SetEntriesInAclW failed: ",
				FactoryWin32ErrorString(se)));
			return false;
		}

		// PROTECTED_DACL is mandatory: without it, inherited ACEs flow
		// through and a broader inherited grant would defeat the
		// Modify-only guarantee on our explicit ACE.
		DWORD si = SetSecurityInfo(
			dir_handle.get(),
			SE_KERNEL_OBJECT,
			DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
			nullptr, nullptr, new_dacl, nullptr);
		if (new_dacl != nullptr) {
			LocalFree(new_dacl);
			new_dacl = nullptr;
		}
		if (si != ERROR_SUCCESS) {
			set_err(StrCat(
				"writability probe failed (",
				first_probe_err.empty() ? GoogleString("no error") : first_probe_err,
				") and SetSecurityInfo failed: ",
				FactoryWin32ErrorString(si)));
			return false;
		}

		// Re-probe. Persistent failure → both errors surface.
		GoogleString second_probe_err;
		if (probe_writable(&second_probe_err)) {
			return true;
		}
		set_err(StrCat(
			"writability probe failed before ACL grant (",
			first_probe_err.empty() ? GoogleString("no error") : first_probe_err,
			"); ACL grant succeeded but post-grant probe still failed (",
			second_probe_err.empty() ? GoogleString("no error") : second_probe_err,
			")"));
		return false;
	}

	ServerContext* IisRewriteDriverFactory::NewDecodingServerContext() {
		ServerContext* sc = new IisServerContext(this, site_app_id_, 9999 /* port doesn't really matter*/);
		InitStubDecodingServerContext(sc);
		return sc;
	}

	Hasher* IisRewriteDriverFactory::NewHasher() {
		return new MD5Hasher;
	}

	NonceGenerator* IisRewriteDriverFactory::DefaultNonceGenerator() {
		// Use Windows BCrypt API instead of /dev/urandom (which doesn't exist).
		const int kKeySize = 64;
		char key[kKeySize];
		NTSTATUS status = BCryptGenRandom(
			nullptr,
			reinterpret_cast<PUCHAR>(key),
			kKeySize,
			BCRYPT_USE_SYSTEM_PREFERRED_RNG);
		if (status != 0) {
			uint64_t seed = static_cast<uint64_t>(timer()->NowUs());
			memcpy(key, &seed, sizeof(seed));
			for (int i = sizeof(seed); i < kKeySize; i++) {
				key[i] = static_cast<char>(seed ^ (seed >> (i % 8)));
			}
		}
		return new HashedNonceGenerator(
			hasher(),
			StringPiece(key, kKeySize),
			thread_system()->NewMutex());
	}

	UrlAsyncFetcher* IisRewriteDriverFactory::AllocateFetcher(
		SystemRewriteOptions* config) {
		// FetchHttps allow_self_signed: the curl-based system fetcher parses
		// the https_options string; the WinHTTP fetcher has no curl, so detect
		// the token here and pass it through to relax TLS validation when set.
		bool allow_self_signed = config != NULL &&
			config->https_options().find("allow_self_signed") != GoogleString::npos;
		if (use_native_fetcher_) {
			message_handler()->Message(kInfo, "Allocating native fetcher");
			IisAsyncUrlFetcher* f = new IisAsyncUrlFetcher();
			f->set_allow_self_signed(allow_self_signed);
			native_fetchers_.push_back(f);
			return f;
		}
		else {
			message_handler()->Message(kInfo, "Allocating default fetcher");
			// WinHTTP is the only fetcher on Windows (no curl)
			IisAsyncUrlFetcher* f = new IisAsyncUrlFetcher();
			f->set_allow_self_signed(allow_self_signed);
			native_fetchers_.push_back(f);
			return f;
		}
	}

	MessageHandler* IisRewriteDriverFactory::DefaultHtmlParseMessageHandler() {
		return iis_html_parse_message_handler_;
	}
	 
	MessageHandler* IisRewriteDriverFactory::DefaultMessageHandler() {
		return iis_message_handler_;
	}

	FileSystem* IisRewriteDriverFactory::DefaultFileSystem() {
		return new StdioFileSystem();
	}

	Timer* IisRewriteDriverFactory::DefaultTimer() {
		return new StdTimer;
	}


	

	void IisRewriteDriverFactory::AddPlatformSpecificRewritePasses(RewriteDriver* driver) 
	{ 
	}


	void IisRewriteDriverFactory::StartThreads() 
	{
	  SchedulerThread* thread = new SchedulerThread(thread_system(), scheduler());
	  bool ok = thread->Start();
	  CHECK(ok) << "Unable to start scheduler thread";
	  defer_cleanup(thread->MakeDeleter());
	}


	NamedLockManager* IisRewriteDriverFactory::DefaultLockManager() {
		CHECK(false);
		return NULL;
	}

	RewriteOptions* IisRewriteDriverFactory::NewRewriteOptions() {
		IisRewriteOptions* options = new IisRewriteOptions(thread_system());
		options->SetRewriteLevel(RewriteOptions::kCoreFilters);
		return options;
	}

	IisServerContext* IisRewriteDriverFactory::MakeIisServerContext(const GoogleString& site_app_id, unsigned int port) {
		IisServerContext* rm = new IisServerContext(this, site_app_id, port);
		this->uninitialized_server_contexts_.insert(rm);
		return rm;
	}

	// oschaaf: this gets called multiple times - however, if it doesn't get
	// called, we shouldn't set up the buffer at all. (message buffer size would be 0)
	void IisRewriteDriverFactory::SetCircularBuffer(
		SharedCircularBuffer* buffer) {

		iis_shared_circular_buffer_ = buffer;
		iis_message_handler_->set_buffer(buffer);
		iis_html_parse_message_handler_->set_buffer(buffer);
		/* old code for when we had real shared mem.
		if (iis_shared_circular_buffer_ == NULL && buffer != NULL) {
			iis_shared_circular_buffer_ = new SharedCircularBuffer(
											shared_mem_runtime(),
											1024 * 128,
											filename_prefix().as_string(),
											hostname_identifier());
			iis_shared_circular_buffer_->InitSegment(true, message_handler());
			iis_shared_circular_buffer_->InitSegment(false, message_handler());
			iis_message_handler_->set_buffer(iis_shared_circular_buffer_);
			iis_html_parse_message_handler_->set_buffer(iis_shared_circular_buffer_);
		}*/
	}


	// oschaaf: note that we completely ignore the underlying sharedcircular buffer
	// from the SystemRewriteDriverFactory, which gets initialized badly due to
	// RootInit/ChildInit methodology
	void IisRewriteDriverFactory::SetServerContextMessageHandler(
		ServerContext* server_context) {
	  IisMessageHandler* handler = new IisMessageHandler(timer(), this->thread_system()->NewMutex());
	  handler->set_buffer(iis_shared_circular_buffer_);
	  server_context_message_handlers_.insert(handler);
	  defer_cleanup(new Deleter<IisMessageHandler>(handler));
	  server_context->set_message_handler(handler);
	}

	ServerContext* IisRewriteDriverFactory::NewServerContext() {
	  LOG(DFATAL) << "MakeIisServerContext should be used instead";
	  return NULL;
	}
	void IisRewriteDriverFactory::ShutDownMessageHandlers() {
	  iis_message_handler_->set_buffer(NULL);
	  iis_html_parse_message_handler_->set_buffer(NULL);
	  for (IisMessageHandlerSet::iterator p =
			   server_context_message_handlers_.begin();
		   p != server_context_message_handlers_.end(); ++p) {
		(*p)->set_buffer(NULL);
	  }
	  server_context_message_handlers_.clear();
	}

	void IisRewriteDriverFactory::InitStats(Statistics* statistics) {
	  SystemRewriteDriverFactory::InitStats(statistics);
	  IisServerContext::InitStats(statistics);
	}
	QueuedWorkerPool* IisRewriteDriverFactory::CreateWorkerPool(
		WorkerPoolCategory pool, StringPiece name) {
		SYSTEM_INFO sysinfo;
		GetSystemInfo( &sysinfo );

		auto numCPU = sysinfo.dwNumberOfProcessors;
	  switch (pool) {
		case kHtmlWorkers:

			return new QueuedWorkerPool(__max(4,numCPU), name, thread_system());
		case kRewriteWorkers:
			return new QueuedWorkerPool(__max(4,numCPU>>1), name, thread_system());
		case kLowPriorityRewriteWorkers:
		  return new QueuedWorkerPool(__max(4,numCPU>>1),
									  name,
									  thread_system());
		default:
		  return RewriteDriverFactory::CreateWorkerPool(pool, name);
	  }
	}


}  // namespace net_instaweb