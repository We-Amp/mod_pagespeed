/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 * 
 *   http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#ifndef PAGESPEED_SYSTEM_SYSTEM_REWRITE_DRIVER_FACTORY_H_
#define PAGESPEED_SYSTEM_SYSTEM_REWRITE_DRIVER_FACTORY_H_

#include <map>
#include <memory>
#include <set>
#include <vector>

#include "net/instaweb/rewriter/public/rewrite_driver_factory.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/hasher.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/thread/queued_worker_pool.h"
#include "pagespeed/system/optimization_thread_policy.h"

namespace net_instaweb {

class AbstractSharedMem;
class FileSystem;
class MessageHandler;
class NamedLockManager;
class NonceGenerator;
class ProcessContext;
class ServerContext;
class SharedCircularBuffer;
class SharedMemStatistics;
class StaticAssetManager;
class SystemCaches;
class SystemRewriteOptions;
class SystemServerContext;
class SystemThreadSystem;
class Timer;
class UrlAsyncFetcher;

// A server context with features specific to a psol port on a unix system.
class SystemRewriteDriverFactory : public RewriteDriverFactory {
 public:
  // Takes ownership of thread_system.
  //
  // On Posix systems implementers should leave shared_mem_runtime NULL,
  // otherwise they should implement AbstractSharedMem for their platform and
  // pass in an instance here.  The factory takes ownership of the shared memory
  // runtime if one is passed in.  Implementers who don't want to support shared
  // memory at all should set PAGESPEED_SUPPORT_POSIX_SHARED_MEM to false and
  // pass in NULL, and the factory will use a NullSharedMem.
  //
  // After construction, you must call Init() to finish the initialization.
  SystemRewriteDriverFactory(const ProcessContext& process_context,
                             SystemThreadSystem* thread_system,
                             AbstractSharedMem* shared_mem_runtime,
                             StringPiece hostname, int port);
  ~SystemRewriteDriverFactory() override;
  void Init();

  AbstractSharedMem* shared_mem_runtime() const {
    return shared_mem_runtime_.get();
  }

  // Creates and ::Initializes a shared memory statistics object.
  SharedMemStatistics* AllocateAndInitSharedMemStatistics(
      bool local, const StringPiece& name, const SystemRewriteOptions& options);

  // Hook for subclasses to init their stats and call
  // SystemRewriteDriverFactory::InitStats().
  virtual void NonStaticInitStats(Statistics* statistics) = 0;

  // Creates a HashedNonceGenerator initialized with data from /dev/random.
  NonceGenerator* DefaultNonceGenerator() override;

  // For shared memory resources the general setup we follow is to have the
  // first running process (aka the root) create the necessary segments and
  // fill in their shared data structures, while processes created to actually
  // handle requests attach to already existing shared data structures.
  //
  // During normal server startup[1], RootInit() must be called from the
  // root process and ChildInit() in every child process.
  //
  // Keep in mind, however, that when fork() is involved a process may
  // effectively see both calls, in which case the 'ChildInit' call would
  // come second and override the previous root status. Both calls are also
  // invoked in the debug single-process mode (In Apache, "httpd -X".).
  //
  // Note that these are not static methods -- they are invoked on every
  // SystemRewriteDriverFactory instance, which exist for the global
  // configuration as well as all the virtual hosts.
  //
  // Implementations should override RootInit and ChildInit for their setup.
  // See ApacheRewriteDriverFactory for an example.
  //
  // [1] Besides normal startup, Apache also uses a temporary process to
  // syntax check the config file. That basically looks like a complete
  // normal startup and shutdown to the code.
  bool is_root_process() const { return is_root_process_; }
  virtual void RootInit();
  virtual void ChildInit();

  // This helper method contains init procedures invoked by both RootInit()
  // and ChildInit()
  virtual void ParentOrChildInit();

  // After the whole configuration has been read, we need to do additional
  // configuration that requires a global view.
  // - server_contexts should be all server contexts on the system
  // - error_message and error_index are set if there's an error, and
  //   error_index is the index in server_contexts of the one with the issue.
  // - global_statistics is lazily initialized to a shared memory statistics
  //   owned by this factory if any of the server contexts require it.
  void PostConfig(const std::vector<SystemServerContext*>& server_contexts,
                  GoogleString* error_message, int* error_index,
                  Statistics** global_statistics);

  // Initialize SharedCircularBuffer and pass it to SystemMessageHandler and
  // SystemHtmlParseMessageHandler. is_root is true if this is invoked from
  // root (ie. parent) process.
  void SharedCircularBufferInit(bool is_root);

  // Most options are parsed by and applied to the RewriteOptions via
  // ParseAndSetOptionFromNameN, but process-scope options need to be set on the
  // rewrite driver factory.
  //
  // ParseAndSetOptionN will only apply changes to the rewrite driver factory if
  // process_scope is true, but it should be called regardless in order to give
  // more helpful error messages ("wrong scope" vs "no such option").  If an
  // option is used out of scope an appropriate message is put in msg and either
  // kOptionValueInvalid or kOptionOk is returned: invalid if the parser should
  // abort with an error, ok if parsing should continue past the error.
  virtual RewriteOptions::OptionSettingResult ParseAndSetOption1(
      StringPiece option, StringPiece arg, bool process_scope,
      GoogleString* msg, MessageHandler* handler);
  virtual RewriteOptions::OptionSettingResult ParseAndSetOption2(
      StringPiece option, StringPiece arg1, StringPiece arg2,
      bool process_scope, GoogleString* msg, MessageHandler* handler);

  Hasher* NewHasher() override;
  Timer* DefaultTimer() override;
  ServerContext* NewServerContext() override;

  // Hook so implementations may disable the property cache.
  virtual bool enable_property_cache() const { return true; }

  GoogleString hostname_identifier() { return hostname_identifier_; }

  // Release all the resources. It also calls the base class ShutDown to release
  // the base class resources.
  void ShutDown() override;

  void StopCacheActivity() override;

  SystemCaches* caches() { return caches_.get(); }

  virtual void set_message_buffer_size(int x) { message_buffer_size_ = x; }

  // Finds a fetcher for the settings in this config, sharing with
  // existing fetchers if possible, otherwise making a new one (and
  // its required thread).
  UrlAsyncFetcher* GetFetcher(SystemRewriteOptions* config);

  // Tracks the size of resources fetched from origin and populates the
  // X-Original-Content-Length header for resources derived from them.
  void set_track_original_content_length(bool x) {
    track_original_content_length_ = x;
  }
  bool track_original_content_length() const {
    return track_original_content_length_;
  }

  // When the fetcher gets a system error during polling, to avoid spamming
  // the log we just print the number of outstanding fetch URLs.  To
  // debug this it's useful to print the complete set of URLs, in
  // which case this should be turned on.
  bool list_outstanding_urls_on_error() const {
    return list_outstanding_urls_on_error_;
  }
  void set_list_outstanding_urls_on_error(bool x) {
    list_outstanding_urls_on_error_ = x;
  }

  // When RateLimitBackgroundFetches is enabled the fetcher needs to apply some
  // limits.  An implementation may need to tune these based on conditions only
  // observable at startup, in which case they can override these.
  virtual int max_queue_size() { return 500 * requests_per_host(); }
  virtual int queued_per_host() { return 500 * requests_per_host(); }
  virtual int requests_per_host();  // Normally 4, or #threads if that's more.

  void set_static_asset_prefix(StringPiece s) {
    s.CopyToString(&static_asset_prefix_);
  }
  const GoogleString& static_asset_prefix() { return static_asset_prefix_; }

  // The resolved counts: what the worker pools will actually be built with.
  // Only meaningful once thread_counts_finalized(); before that they read as
  // kAutoThreadCount.
  int num_rewrite_threads() const { return num_rewrite_threads_; }
  int num_expensive_rewrite_threads() const {
    return num_expensive_rewrite_threads_;
  }

  // The *configured* counts, as NumRewriteThreads /
  // NumExpensiveRewriteThreads set them.  kAutoThreadCount (which is what
  // both `auto` and `0` parse to, and the initial value) means "compute it
  // from the thread-count policy".  A positive value always wins over the
  // computed one, whichever order parsing and detection happen in: setting one
  // after the counts have been finalized re-resolves them.
  //
  // constexpr, not `static const int`, so it has no out-of-class definition to
  // forget: an in-class-initialized `static const int` still needs one for any
  // ODR use, and an EXPECT_EQ() or a std::max() against it would fail to link.
  static constexpr int kAutoThreadCount = 0;
  void set_num_rewrite_threads(int x) {
    configured_rewrite_threads_ = x;
    ReresolveIfFinalized();
  }
  void set_num_expensive_rewrite_threads(int x) {
    configured_expensive_rewrite_threads_ = x;
    ReresolveIfFinalized();
  }
  bool use_per_vhost_statistics() const { return use_per_vhost_statistics_; }
  void set_use_per_vhost_statistics(bool x) { use_per_vhost_statistics_ = x; }
  bool install_crash_handler() const { return install_crash_handler_; }
  void set_install_crash_handler(bool x) { install_crash_handler_ = x; }

  // mod_pagespeed uses a beacon handler to collect data for critical images,
  // css, etc., so filters should be configured accordingly.
  bool UseBeaconResultsInFilters() const override { return true; }

  // Check whether the server is threaded.  For example, Nginx uses an event
  // loop and can keep with the default of false, while Apache with a threaded
  // multiprocessing module (MPM) overrides this method to return true.
  //
  // This no longer feeds the thread-count policy -- request
  // concurrency is not the constraint on CPU-bound optimization work, and the
  // divisor that matters is ConcurrentProcessCount().  It is still reported in
  // the startup log, because it describes the deployment shape an operator is
  // looking at.
  virtual bool IsServerThreaded() {
    return false;  // Most new servers are non-threaded nowadays.
  }

  // Threaded implementing servers should return the maximum number of threads
  // that might be used for handling user requests.
  virtual int LookupThreadLimit() { return 1; }

  // How many server processes on this machine each build their
  // own optimization worker pools -- Apache's configured child count, nginx's
  // worker_processes, the IIS application pool's worker-process count, Envoy's
  // concurrency.  It is the divisor that keeps the aggregate bounded: without
  // it a per-process count oversubscribes the machine by exactly the process
  // count.
  //
  // The default is kUnknownProcessConcurrency, and a port that leaves it there
  // resolves to one thread in each pool and logs that it did.  It deliberately
  // does not guess a larger number: silently oversubscribing a machine is the
  // failure mode the thread-count policy exists to end.
  virtual int ConcurrentProcessCount() { return kUnknownProcessConcurrency; }

  // Returns false on platforms that cannot answer IsServerThreaded() /
  // ConcurrentProcessCount() / LookupThreadLimit() correctly until after
  // configuration has been processed; such platforms must call
  // FinalizeThreadCounts() themselves once the answers are available, and
  // update the cache thread limit with caches()->set_thread_limit().
  virtual bool ThreadCountsKnownAtInit() { return true; }

  // Runs thread-count resolution if it hasn't run yet.  Platforms that return
  // false from ThreadCountsKnownAtInit() must call this as soon as
  // ConcurrentProcessCount() can be answered, and before anything reads the
  // thread counts.  Idempotent.
  void FinalizeThreadCounts() { AutoDetectThreadCounts(); }

  // The CPU budget and process-concurrency divisor the resolved counts were
  // computed from.  Only meaningful once thread_counts_finalized().  Exposed
  // so ports can report them; the admin/statistics surface is a planned
  // follow-up.
  const EffectiveCpuBudget& cpu_budget() const { return cpu_budget_; }
  int concurrent_process_count() const { return concurrent_processes_; }

 protected:
  // Initializes all the statistics objects created transitively by
  // SystemRewriteDriverFactory.  Only subclasses should call this.
  static void InitStats(Statistics* statistics);

  // Initializes the StaticAssetManager.
  void InitStaticAssetManager(
      StaticAssetManager* static_asset_manager) override;

  void SetupCaches(ServerContext* server_context) override;
  QueuedWorkerPool* CreateWorkerPool(WorkerPoolCategory pool,
                                     StringPiece name) override;

  // TODO(jefftk): create SystemMessageHandler and get rid of these hooks.
  virtual void SetupMessageHandlers() {}
  virtual void ShutDownMessageHandlers() {}
  virtual void SetCircularBuffer(SharedCircularBuffer* buffer) {}

  // Can be overridden by subclasses to shutdown any fetchers we don't
  // know about.
  virtual void ShutDownFetchers() {}

  // Once ServerContexts are initialized via
  // RewriteDriverFactory::InitServerContext, they will be
  // managed by the RewriteDriverFactory.  But in the root process
  // the ServerContexts will never be initialized.  We track these here
  // so that SystemRewriteDriverFactory::ChildInit can iterate over all
  // the server contexts that need to be ChildInit'd, and so that we can free
  // them in the Root process that does not run ChildInit.
  using SystemServerContextSet = std::set<SystemServerContext*>;
  SystemServerContextSet uninitialized_server_contexts_;

  // Allocates a fetcher for the given config.  Implementations must override
  // this method to supply an appropriate fetcher.  For example, the Apache
  // module returns a Curl fetcher, while the Envoy filter returns an
  // Envoy-native fetcher.
  virtual UrlAsyncFetcher* AllocateFetcher(SystemRewriteOptions* config) = 0;

  FileSystem* DefaultFileSystem() override;
  NamedLockManager* DefaultLockManager() override;

  // Reads the effective CPU budget and the process-concurrency divisor, then
  // resolves num_rewrite_threads_ and num_expensive_rewrite_threads_ from the
  // thread-count policy for any count the operator did not set explicitly.
  // Idempotent; the first call is the one that counts.
  virtual void AutoDetectThreadCounts();

  // Logs the resolved counts and everything they were derived from.
  //
  // This is emitted at kWarning, not kInfo, on purpose: Apache's default
  // LogLevel is warn, and a resolution reported below the default log level is
  // indistinguishable from one that never happened -- which is a large part of
  // why the old behaviour went unnoticed for years.  Ports that
  // can describe their deployment shape in more detail override this.
  virtual void LogThreadCountResolution();

  bool thread_counts_finalized() { return thread_counts_finalized_; }

 private:
  // Applies the policy formula to cpu_budget_ / concurrent_processes_ and
  // writes the result into num_rewrite_threads_ and
  // num_expensive_rewrite_threads_, except where the operator configured a
  // positive count.
  void ResolveThreadCounts();

  // Called when a directive is set.  If the counts were already resolved --
  // which happens on the ports that read configuration after Init() -- redo
  // the resolution so the explicitly configured count wins regardless of
  // ordering, and report the new answer.
  void ReresolveIfFinalized();

  // Calls LogThreadCountResolution() unless the resolved pair is what was
  // logged last.
  void LogThreadCountResolutionIfChanged();

  // Complains, in every build and not only in debug ones, if a worker pool is
  // built before thread-count resolution has run.  See the definition.
  void WarnIfThreadCountsNotFinalized();

  // Build global shared-memory statistics, taking ownership.  This is invoked
  // if at least one server context (global or VirtualHost) enables statistics.
  Statistics* SetUpGlobalSharedMemStatistics(
      const SystemRewriteOptions& options);

  // Generates a cache-key incorporating all the parameters from config that
  // might be relevant to fetching.  When include_slurping_config, then
  // slurping-related options are ignored for the fetch-key.
  GoogleString GetFetcherKey(bool include_slurping_config,
                             const SystemRewriteOptions* config);

  // GetFetcher returns fetchers wrapped in various kinds of filtering (rate
  // limiting and slurping).  Because the underlying fetchers are expensive, and
  // you can wrap the same base fetcher in multiple ways, we provide a helper
  // method for GetFetcher that reuses fetchers as much as possible.
  UrlAsyncFetcher* GetBaseFetcher(SystemRewriteOptions* config);

  UrlAsyncFetcher* DefaultAsyncUrlFetcher() override;

  std::unique_ptr<SharedMemStatistics> shared_mem_statistics_;
  // While split statistics in the ServerContext cleans up the actual objects,
  // we do the segment cleanup for local stats here.
  StringVector local_shm_stats_segment_names_;
  std::unique_ptr<AbstractSharedMem> shared_mem_runtime_;
  std::unique_ptr<SharedCircularBuffer> shared_circular_buffer_;

  bool statistics_frozen_;
  bool is_root_process_;

  // hostname_identifier_ equals to "server_hostname:port" of the webserver.
  // It's used to distinguish the name of shared memory, so that each virtual
  // host has its own SharedCircularBuffer.
  const GoogleString hostname_identifier_;

  // Size of shared circular buffer for displaying Info messages in
  // /pagespeed_messages (or /mod_pagespeed_messages, /ngx_pagespeed_messages)
  int message_buffer_size_;

  // Manages all our caches & lock managers.
  std::unique_ptr<SystemCaches> caches_;

  bool track_original_content_length_;
  bool list_outstanding_urls_on_error_;

  // Fetchers are expensive--they each cost a thread.  Instead of allocating one
  // for every server context we keep a cache of defined fetchers with various
  // configurations.  There are two caches depending on whether the underlying
  // fetcher (the thing that takes a thread) needs to know about various
  // options.  The inner cache is base_fetcher_map_ which GetBaseFetcher() uses
  // to keep track of what fetchers it has requested from AllocateFetcher().
  // Base fetchers are all curl fetchers with various options unless an
  // implementation overrides AllocateFetcher() to return other kinds of
  // fetchers.  The outer cache is fetcher_map_, used by GetFetcher(), and is
  // fragmented on every option that affects fetching.  All of these fetchers
  // are either exactly as returned by GetBaseFetcher() or first wrapped in
  // slurping or rate-limiting.
  using FetcherMap = std::map<GoogleString, UrlAsyncFetcher*>;
  FetcherMap base_fetcher_map_;
  FetcherMap fetcher_map_;

  // URL prefix for support files required by pagespeed.
  GoogleString static_asset_prefix_;

  // The same as our parent's thread_system_, but without casting.
  SystemThreadSystem* system_thread_system_;

  // If true, we'll have a separate statistics object for each vhost
  // (along with a global aggregate), rather than just a single object
  // aggregating all of them.
  bool use_per_vhost_statistics_;

  // If true, we'll install a signal handler that prints backtraces.
  bool install_crash_handler_;

  // true iff we ran through AutoDetectThreadCounts().
  bool thread_counts_finalized_;

  // What the operator configured.  kAutoThreadCount means "apply the
  // policy"; that is both the initial value and what `auto` and `0` parse to.
  int configured_rewrite_threads_;
  int configured_expensive_rewrite_threads_;

  // What the pools will be built with.  kAutoThreadCount until resolution has
  // run.
  int num_rewrite_threads_;
  int num_expensive_rewrite_threads_;

  // The inputs the resolved counts came from, kept for logging and for
  // re-resolution when a directive arrives after finalization.
  EffectiveCpuBudget cpu_budget_;
  int concurrent_processes_;

  // The last pair LogThreadCountResolution() was called with, so a
  // re-resolution that changes nothing does not repeat itself.
  int logged_rewrite_threads_;
  int logged_expensive_rewrite_threads_;

  SystemRewriteDriverFactory(const SystemRewriteDriverFactory&) = delete;
  SystemRewriteDriverFactory& operator=(const SystemRewriteDriverFactory&) =
      delete;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_SYSTEM_SYSTEM_REWRITE_DRIVER_FACTORY_H_
