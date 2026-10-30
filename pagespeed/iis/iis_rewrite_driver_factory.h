// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef IIS_REWRITE_DRIVER_FACTORY_H_
#define IIS_REWRITE_DRIVER_FACTORY_H_

#include "pagespeed/kernel/base/md5_hasher.h"
#include "pagespeed/system/system_rewrite_driver_factory.h"
#include <cstdint>
#include <vector>

namespace net_instaweb {

class AbstractSharedMem;
class AsyncCache;
class CacheInterface;
class AbstractSharedMem;
class IisAsyncUrlFetcher;
class IisProcessContext;
class IisRewriteOptions;
class IisServerContext;
class IisMessageHandler;
class SharedCircularBuffer;
class SharedMemStatistics;
class SharedMemRefererStatistics;
class Statistics;
class StaticAssetManager;

class IisRewriteDriverFactory : public SystemRewriteDriverFactory {
 public:

  IisRewriteDriverFactory(IisProcessContext* process_context, std::wstring app_pool_name, SystemThreadSystem* thread_system, AbstractSharedMem* shm_runtime);
  ~IisRewriteDriverFactory() override;

  // IIS-specific override of the cross-port
  // prefix-scope predicate. Canonicalizes |path| via GetFullPathNameW
  // and case-insensitively requires it to begin with one of the
  // hardcoded prefixes:
  //   C:\ProgramData\We-Amp\PageSpeed\cache\   (canonical 1.1 path)
  //   C:\ProgramData\We-Amp\IISWebSpeed\cache\ (legacy upgrade path)
  // POSIX ports inherit the base "always true" (their directive parser
  // mkdirs the entire FileCachePath unconditionally — no prefix scope
  // to enforce).
  bool IsPathInAutoCreatePrefix(const GoogleString& path) override;

  // IIS-specific prefix-scope
  // predicate for the LogDir auto-create flow. Same canonicalize +
  // case-insensitive comparison shape as IsPathInAutoCreatePrefix
  // above, but the hardcoded prefixes target the logs tree:
  //   C:\ProgramData\We-Amp\PageSpeed\logs\    (canonical 1.1 path)
  //   C:\ProgramData\We-Amp\IISWebSpeed\logs\  (legacy upgrade path)
  // Out-of-prefix LogDir values fall through unchanged (no auto-create,
  // no diagnostic page — the behaviour from before LogDir auto-create
  // existed).
  bool IsLogDirInAutoCreatePrefix(const GoogleString& path) override;

  // IIS-specific implementation of the cross-port
  // server-context init-time filesystem-prep hook. Sequence:
  //   (c) cheap belt: mkdir  — RecursivelyMakeDir (same primitive Apache
  //                            uses at directive-parse time)
  //   (d) reparse-point check on the resulting path, handle retained
  //   (e) writability probe  — OpenTempFile, success short-circuits
  //   (f) conditional ACL    — explicit DACL via SetSecurityInfo on the
  //                            retained handle (PROTECTED_DACL); access
  //                            mask = |acl_mask| if non-zero, else
  //                            CachePathAclMask() (Modify).
  // POSIX ports inherit the base no-op (their directive parser already
  // covers the FileCachePath). Out-of-prefix gating is enforced by the
  // caller via IsPathInAutoCreatePrefix() / IsLogDirInAutoCreatePrefix()
  // above — this hook may assume the path is in scope. Failures return
  // false with a populated |error_message|.
  bool EnsureDirectoryWritable(const GoogleString& path,
                               GoogleString* error_message,
                               uint32_t acl_mask = 0) override;

  // Cache-path ACL mask: Modify
  // (FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE |
  // DELETE), mirroring Product.wxs GrantCacheAcl. Returned as uint32_t
  // to keep the header platform-neutral; implementation in the .cpp
  // casts to DWORD where needed.
  uint32_t CachePathAclMask() const;

  // LogDir ACL mask: RX+W
  // (FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE,
  // NO DELETE), mirroring Product.wxs GrantLogAcl. Narrower than
  // CachePathAclMask() because workers append to logs but admin owns
  // rotation. Caller in iis_process_context.cpp passes this mask to
  // EnsureDirectoryWritable for the LogDir flow.
  uint32_t LogDirAclMask() const;

  Hasher* NewHasher() override;
  UrlAsyncFetcher* AllocateFetcher(SystemRewriteOptions* config) override;
  MessageHandler* DefaultHtmlParseMessageHandler() override;
  MessageHandler* DefaultMessageHandler() override;
  FileSystem* DefaultFileSystem() override;
  Timer* DefaultTimer() override;
  void StartThreads();
  //virtual QueuedWorkerPool* CreateWorkerPool(WorkerPoolCategory name);
  NamedLockManager* DefaultLockManager() override;
  NonceGenerator* DefaultNonceGenerator() override;
  RewriteOptions* NewRewriteOptions() override;

  ServerContext* NewServerContext() override;
  void ShutDown() override;

  // We use a beacon handler to collect data for critical images,
  // css, etc., so filters should be configured accordingly.
  bool UseBeaconResultsInFilters() const override {
    return true;
  }

  void NonStaticInitStats(Statistics* statistics) override {
    InitStats(statistics);
  }
  // Provides an optional hook for customizing the RewriteDriver object
  // using the options set on it. This is called before
  // RewriteDriver::AddFilters() and AddPlatformSpecificRewritePasses().
  void ApplyPlatformSpecificConfiguration(RewriteDriver* driver) override {;}
  // Provides an optional hook for adding rewrite passes to the HTML filter
  // chain.  This should be used for filters that are specific to a particular
  // RewriteDriverFactory implementation.
  void AddPlatformSpecificRewritePasses(RewriteDriver* driver) override;
  static void InitStats(Statistics* statistics);
  IisServerContext* MakeIisServerContext(const GoogleString& site_app_id, unsigned int port);
  void ShutDownMessageHandlers() override;
  void SetCircularBuffer(SharedCircularBuffer* buffer) override;

  ServerContext* NewDecodingServerContext() override;
	  

  void SetServerContextMessageHandler(ServerContext* server_context);
	bool use_per_vhost_statistics() const {
		return use_per_vhost_statistics_;
	}
	void set_use_per_vhost_statistics(bool x) {
		use_per_vhost_statistics_ = x;
	}
	bool use_native_fetcher() {
		return use_native_fetcher_;
	}
	void set_use_native_fetcher(bool x) {
		use_native_fetcher_ = x;
	} 

	// The built-in size of a rewrite pool, used when NumRewriteThreads /
	// NumExpensiveRewriteThreads is unset or "auto": half the CPUs, and at
	// least 4.  A positive |configured| count always wins.
	static int PoolThreadCount(int configured, unsigned int num_cpus) {
		if (configured > 0) {
			return configured;
		}
		const unsigned int half = num_cpus >> 1;
		return static_cast<int>(half > 4 ? half : 4);
	}

	int expires();
	void reset_expired();
protected:
  // Reports the pool sizes this worker process builds, and where they came
  // from, instead of the base class's process-count based wording.
  void LogThreadCountResolution() override;

public:
  // Logs the same line again. The line logged while the factory initialises
  // is written before the message history and the event-log setting exist, so
  // the process context repeats it once they do.
  void ReportPoolSizes() { LogThreadCountResolution(); }

protected:

  QueuedWorkerPool* CreateWorkerPool(WorkerPoolCategory pool,
                                     StringPiece name) override;

  void ShutDownFetchers() override;
private:
  // Non-owning back-pointer to the process context that constructed us.
  // Used by EnsureDirectoryWritable to read the cached worker SID
  // captured at IisProcessContext ctor (before the worker token handle
  // was closed). PSID buffer lives in the process context — the factory
  // just reads through. No copy, no LocalFree.
  IisProcessContext* process_context_;
  std::wstring app_pool_name_;
  bool use_per_vhost_statistics_;
  bool use_native_fetcher_;
  bool rewrite_threads_configured_ = false;
  bool expensive_rewrite_threads_configured_ = false;
  GoogleString site_app_id_;

  // Owned by the superclass.
  // TODO(jefftk): merge the nginx and apache ways of doing this.
  SharedCircularBuffer* iis_shared_circular_buffer_;
  IisMessageHandler* iis_message_handler_;
  IisMessageHandler* iis_html_parse_message_handler_;
  using IisMessageHandlerSet = std::set<IisMessageHandler*>;
  IisMessageHandlerSet server_context_message_handlers_;
  std::vector<IisAsyncUrlFetcher*> native_fetchers_;
  bool shut_down_;
  time_t created_at_;
  IisRewriteDriverFactory(const IisRewriteDriverFactory&) = delete;
  IisRewriteDriverFactory& operator=(const IisRewriteDriverFactory&) = delete;
};

} // namespace net_instaweb

#endif  // IIS_REWRITE_DRIVER_FACTORY_H_