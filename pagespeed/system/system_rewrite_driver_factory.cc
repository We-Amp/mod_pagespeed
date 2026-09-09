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

#include "pagespeed/system/system_rewrite_driver_factory.h"

#include <algorithm>  // for min, max
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <utility>  // for pair

#include "base/logging.h"
#include "net/instaweb/http/public/http_dump_url_async_writer.h"
#include "net/instaweb/http/public/http_dump_url_fetcher.h"
#include "net/instaweb/http/public/rate_controller.h"
#include "net/instaweb/http/public/rate_controlling_url_async_fetcher.h"
#include "net/instaweb/http/public/url_async_fetcher.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_driver_factory.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "net/instaweb/rewriter/public/static_asset_manager.h"
#include "net/instaweb/util/public/property_cache.h"
#include "pagespeed/kernel/base/abstract_shared_mem.h"
#include "pagespeed/kernel/base/file_system.h"
#include "pagespeed/kernel/base/google_message_handler.h"
#include "pagespeed/kernel/base/md5_hasher.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/null_shared_mem.h"
#ifdef _WIN32
#include "pagespeed/kernel/base/std_timer.h"
#else
#include "pagespeed/kernel/base/posix_timer.h"
#endif
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/stdio_file_system.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/kernel/sharedmem/shared_circular_buffer.h"
#include "pagespeed/kernel/sharedmem/shared_mem_statistics.h"
#if PAGESPEED_SUPPORT_POSIX_SHARED_MEM
#include "pagespeed/kernel/thread/pthread_shared_mem.h"
#endif
#include "pagespeed/kernel/thread/queued_worker_pool.h"
#include "pagespeed/kernel/util/input_file_nonce_generator.h"
#include "pagespeed/kernel/util/nonce_generator.h"
#include "pagespeed/system/in_place_resource_recorder.h"
#include "pagespeed/system/optimization_thread_policy.h"
#include "pagespeed/system/system_caches.h"
#include "pagespeed/system/system_rewrite_options.h"
#include "pagespeed/system/system_server_context.h"
#include "pagespeed/system/system_thread_system.h"

namespace net_instaweb {

class ProcessContext;

namespace {

const char kShutdownCount[] = "child_shutdown_count";

const char kStaticAssetPrefix[] = "StaticAssetPrefix";
const char kUsePerVHostStatistics[] = "UsePerVHostStatistics";
const char kInstallCrashHandler[] = "InstallCrashHandler";
const char kNumRewriteThreads[] = "NumRewriteThreads";
const char kNumExpensiveRewriteThreads[] = "NumExpensiveRewriteThreads";
const char kForceCaching[] = "ForceCaching";
const char kListOutstandingUrlsOnError[] = "ListOutstandingUrlsOnError";
const char kMessageBufferSize[] = "MessageBufferSize";
const char kTrackOriginalContentLength[] = "TrackOriginalContentLength";
const char kCreateSharedMemoryMetadataCache[] =
    "CreateSharedMemoryMetadataCache";

// The literal that asks for the computed policy by name.
const char kAutoThreadCountLiteral[] = "auto";

// Parses a NumRewriteThreads / NumExpensiveRewriteThreads argument.  Accepts
// `auto` and its alias `0` (both yielding kAutoThreadCount) and any positive
// integer; rejects everything else with a message naming the directive, which
// is what the operator sees at startup.
//
// A positive value above kMaxOptimizationThreadsPerPool is clamped rather than
// rejected, with a warning naming both the requested and the resolved count.
// An explicit count is an override, so it is not the policy's business to
// refuse it -- but installing thousands of threads per server process because
// a digit was mistyped, silently, is exactly what this policy exists to stop.
bool ParseThreadCountArgument(StringPiece option, StringPiece arg, int* count,
                              GoogleString* msg, MessageHandler* handler) {
  if (StringCaseEqual(arg, kAutoThreadCountLiteral)) {
    *count = SystemRewriteDriverFactory::kAutoThreadCount;
    return true;
  }
  int value = 0;
  if (!StringToInt(arg, &value)) {
    *msg = StrCat("'", option, "' must be a positive integer or '",
                  kAutoThreadCountLiteral, "'; got '", arg, "'.");
    return false;
  }
  if (value < 0) {
    *msg = StrCat("'", option, "' must not be negative; got '", arg, "'. Use '",
                  kAutoThreadCountLiteral,
                  "' (or 0) to size the pool automatically.");
    return false;
  }
  if (value > kMaxOptimizationThreadsPerPool) {
    GoogleString option_str;
    option.CopyToString(&option_str);
    handler->Message(kWarning,
                     "'%s' was set to %d, which is far more threads than any "
                     "server process can use; using %d instead. Leave it "
                     "unset, or set it to '%s', to size the pool from the CPU "
                     "budget.",
                     option_str.c_str(), value, kMaxOptimizationThreadsPerPool,
                     kAutoThreadCountLiteral);
    value = kMaxOptimizationThreadsPerPool;
  }
  *count = value;  // 0 is the alias for `auto`.
  return true;
}

}  // namespace

SystemRewriteDriverFactory::SystemRewriteDriverFactory(
    const ProcessContext& process_context, SystemThreadSystem* thread_system,
    AbstractSharedMem* shared_mem_runtime, /* may be null */
    StringPiece hostname, int port)
    : RewriteDriverFactory(process_context, thread_system),
      statistics_frozen_(false),
      is_root_process_(true),
      hostname_identifier_(StrCat(hostname, ":", IntegerToString(port))),
      message_buffer_size_(0),
      track_original_content_length_(false),
      list_outstanding_urls_on_error_(false),
      static_asset_prefix_("/pagespeed_static/"),
      system_thread_system_(thread_system),
      use_per_vhost_statistics_(true),
      install_crash_handler_(false),
      thread_counts_finalized_(false),
      configured_rewrite_threads_(kAutoThreadCount),
      configured_expensive_rewrite_threads_(kAutoThreadCount),
      num_rewrite_threads_(kAutoThreadCount),
      num_expensive_rewrite_threads_(kAutoThreadCount),
      concurrent_processes_(kUnknownProcessConcurrency),
      logged_rewrite_threads_(-1),
      logged_expensive_rewrite_threads_(-1) {
  // cpu_budget_ stays at its conservative default (one core, nothing read)
  // until AutoDetectThreadCounts() reads the machine.
  if (shared_mem_runtime == nullptr) {
#if PAGESPEED_SUPPORT_POSIX_SHARED_MEM
    shared_mem_runtime = new PthreadSharedMem();
#else
    shared_mem_runtime = new NullSharedMem();
#endif
  }
  shared_mem_runtime_.reset(shared_mem_runtime);
}

// We need an Init() method to finish construction because we want to call
// virtual methods that subclasses can override.
void SystemRewriteDriverFactory::Init() {
  // Some servers can't answer questions about their threading model this
  // early: they only know once the configuration has been processed.  Those
  // return false here and call FinalizeThreadCounts() themselves later,
  // updating caches()->set_thread_limit() at the same time.
  if (ThreadCountsKnownAtInit()) {
    AutoDetectThreadCounts();
  }

  int thread_limit = LookupThreadLimit();
  if (thread_counts_finalized()) {
    thread_limit += num_rewrite_threads() + num_expensive_rewrite_threads();
  }
  // SystemCaches must exist before configuration is parsed: ParseAndSetOption2
  // calls caches()->CreateShmMetadataCache() while reading the config.
  caches_ = std::make_unique<SystemCaches>(this, shared_mem_runtime_.get(),
                                           thread_limit);
}

SystemRewriteDriverFactory::~SystemRewriteDriverFactory() {
  shared_mem_statistics_.reset(nullptr);
}

// Initializes global statistics object if needed, using factory to
// help with the settings if needed.
// Note: does not call set_statistics() on the factory.
Statistics* SystemRewriteDriverFactory::SetUpGlobalSharedMemStatistics(
    const SystemRewriteOptions& options) {
  if (shared_mem_statistics_.get() == nullptr) {
    shared_mem_statistics_.reset(AllocateAndInitSharedMemStatistics(
        false /* not local */, "global", options));
  }
  DCHECK(!statistics_frozen_);
  statistics_frozen_ = true;
  SetStatistics(shared_mem_statistics_.get());
  return shared_mem_statistics_.get();
}

SharedMemStatistics*
SystemRewriteDriverFactory::AllocateAndInitSharedMemStatistics(
    bool local, const StringPiece& name, const SystemRewriteOptions& options) {
  // Note that we create the statistics object in the parent process, and
  // it stays around in the kids but gets reinitialized for them
  // inside ChildInit(), called from pagespeed_child_init.
  GoogleString log_filename;
  bool logging_enabled = false;
  if (!options.log_dir().empty()) {
    // Only enable statistics logging if a log_dir() is actually specified.
    log_filename = StrCat(options.log_dir(), "/stats_log_", name);
    logging_enabled = options.statistics_logging_enabled();
  }
  SharedMemStatistics* stats = new SharedMemStatistics(
      options.statistics_logging_interval_ms(),
      options.statistics_logging_max_file_size_kb(), log_filename,
      logging_enabled,
      // TODO(jmarantz): it appears that filename_prefix() is not actually
      // established at the time of this construction, calling into question
      // whether we are naming our shared-memory segments correctly.
      StrCat(filename_prefix(), name), shared_mem_runtime(), message_handler(),
      file_system(), timer());
  NonStaticInitStats(stats);
  bool init_ok = stats->Init(true, message_handler());
  if (local && init_ok) {
    local_shm_stats_segment_names_.push_back(stats->SegmentName());
  }
  return stats;
}

void SystemRewriteDriverFactory::InitStats(Statistics* statistics) {
  // Init standard PSOL stats.
  RewriteDriverFactory::InitStats(statistics);

  // Init System-specific stats.
  StdioFileSystem::InitStats(statistics);
  SystemCaches::InitStats(statistics);
  PropertyCache::InitCohortStats(RewriteDriver::kBeaconCohort, statistics);
  PropertyCache::InitCohortStats(RewriteDriver::kDomCohort, statistics);
  PropertyCache::InitCohortStats(RewriteDriver::kDependenciesCohort,
                                 statistics);
  InPlaceResourceRecorder::InitStats(statistics);
  RateController::InitStats(statistics);

  statistics->AddVariable(kShutdownCount);
}

NonceGenerator* SystemRewriteDriverFactory::DefaultNonceGenerator() {
  MessageHandler* handler = message_handler();
  FileSystem::InputFile* random_file =
      file_system()->OpenInputFile("/dev/urandom", handler);
  CHECK(random_file != nullptr) << "Couldn't open /dev/urandom";
  // Now use the key to construct an InputFileNonceGenerator.  Passing in a NULL
  // random_file here will create a generator that will fail on first access.
  return new InputFileNonceGenerator(random_file, file_system(),
                                     thread_system()->NewMutex(), handler);
}

void SystemRewriteDriverFactory::SetupCaches(ServerContext* server_context) {
  if (caches_ == nullptr) {
    return;
  }
  caches_->SetupCaches(server_context, enable_property_cache());
}

void SystemRewriteDriverFactory::InitStaticAssetManager(
    StaticAssetManager* static_asset_manager) {
  static_asset_manager->set_library_url_prefix(static_asset_prefix_);
}

void SystemRewriteDriverFactory::WarnIfThreadCountsNotFinalized() {
  if (thread_counts_finalized_) {
    return;
  }
  // A DCHECK alone is not enough here.  A port whose FinalizeThreadCounts()
  // call site is deleted -- the Apache one lives in a hand-written post-config
  // hook -- falls back to one worker in each pool and, because the resolution
  // never ran, never logs the line that would have said so.  That is the
  // silent failure the policy's "always report the resolved counts" rule
  // exists to prevent, so say it out loud in every build, not only in debug
  // ones.
  DCHECK(thread_counts_finalized_)
      << "Rewrite worker pool created before thread counts were finalized";
  message_handler()->Message(
      kWarning,
      "PageSpeed optimization worker pool created before the thread counts "
      "were resolved: falling back to one thread per pool. This is a bug -- "
      "this port must call FinalizeThreadCounts() once it can answer "
      "ConcurrentProcessCount(), and before any worker pool is built.");
}

QueuedWorkerPool* SystemRewriteDriverFactory::CreateWorkerPool(
    WorkerPoolCategory pool, StringPiece name) {
  switch (pool) {
    case kHtmlWorkers:
      // In Apache this will effectively be 0, as it doesn't use HTML threads.
      return new QueuedWorkerPool(1, name, thread_system());
    case kRewriteWorkers:
      // Thread counts must have been finalized by now; clamp defensively so a
      // regression in that ordering can't create a pool with <= 0 workers.
      WarnIfThreadCountsNotFinalized();
      return new QueuedWorkerPool(std::max(1, num_rewrite_threads_), name,
                                  thread_system());
    case kLowPriorityRewriteWorkers:
      WarnIfThreadCountsNotFinalized();
      return new QueuedWorkerPool(std::max(1, num_expensive_rewrite_threads_),
                                  name, thread_system());
    default:
      return RewriteDriverFactory::CreateWorkerPool(pool, name);
  }
}

void SystemRewriteDriverFactory::ParentOrChildInit() {
  SharedCircularBufferInit(is_root_process_);
}

void SystemRewriteDriverFactory::RootInit() {
  ParentOrChildInit();

  // Let SystemCaches know about the various paths we have in configuration
  // first, as well as external cache instances.
  for (SystemServerContextSet::iterator
           p = uninitialized_server_contexts_.begin(),
           e = uninitialized_server_contexts_.end();
       p != e; ++p) {
    SystemServerContext* server_context = *p;
    caches_->RegisterConfig(server_context->global_system_rewrite_options());
  }

  caches_->RootInit();
}

void SystemRewriteDriverFactory::ChildInit() {
  const SystemRewriteOptions* conf =
      SystemRewriteOptions::DynamicCast(default_options());
  CHECK(conf != nullptr);

  StdioFileSystem* fs = dynamic_cast<StdioFileSystem*>(file_system());
  DCHECK(fs != nullptr)
      << "Expected StdioFileSystem so we can call TrackTiming";
  if (fs != nullptr) {
    fs->TrackTiming(conf->slow_file_latency_threshold_us(), timer(),
                    statistics(), message_handler());
  }

  is_root_process_ = false;
  system_thread_system_->PermitThreadStarting();

  ParentOrChildInit();

  SetupMessageHandlers();

  if (shared_mem_statistics_.get() != nullptr) {
    shared_mem_statistics_->Init(false, message_handler());
  }

  caches_->ChildInit();

  // Static asset config is process-global.
  if (conf->has_static_assets_to_cdn()) {
    StaticAssetConfig out_conf;
    conf->FillInStaticAssetCDNConf(&out_conf);
    static_asset_manager()->ServeAssetsFromGStatic(
        conf->static_assets_cdn_base());
    static_asset_manager()->ApplyGStaticConfiguration(
        out_conf, StaticAssetManager::kInitialConfiguration);
  }

  for (SystemServerContextSet::iterator
           p = uninitialized_server_contexts_.begin(),
           e = uninitialized_server_contexts_.end();
       p != e; ++p) {
    SystemServerContext* server_context = *p;
    server_context->ChildInit(this);
  }
  uninitialized_server_contexts_.clear();
}

// TODO(jmarantz): make this per-vhost.
void SystemRewriteDriverFactory::SharedCircularBufferInit(bool is_root) {
  // Set buffer size to 0 means turning it off
  if (shared_mem_runtime() != nullptr && (message_buffer_size_ != 0)) {
    // TODO(jmarantz): it appears that filename_prefix() is not actually
    // established at the time of this construction, calling into question
    // whether we are naming our shared-memory segments correctly.
    shared_circular_buffer_ = std::make_unique<SharedCircularBuffer>(
        shared_mem_runtime(), message_buffer_size_,
        filename_prefix().as_string(), hostname_identifier());
    if (shared_circular_buffer_->InitSegment(is_root, message_handler())) {
      SetCircularBuffer(shared_circular_buffer_.get());
    }
  }
}

RewriteOptions::OptionSettingResult
SystemRewriteDriverFactory::ParseAndSetOption1(StringPiece option,
                                               StringPiece arg,
                                               bool process_scope,
                                               GoogleString* msg,
                                               MessageHandler* handler) {
  // First check the scope.
  if (StringCaseEqual(option, kStaticAssetPrefix) ||
      StringCaseEqual(option, kUsePerVHostStatistics) ||
      StringCaseEqual(option, kInstallCrashHandler) ||
      StringCaseEqual(option, kNumRewriteThreads) ||
      StringCaseEqual(option, kNumExpensiveRewriteThreads)) {
    if (!process_scope) {
      *msg = StrCat("'", option, "' is global and can't be set at this scope.");
      return RewriteOptions::kOptionValueInvalid;
    }
  } else if (StringCaseEqual(option, kForceCaching) ||
             StringCaseEqual(option, kListOutstandingUrlsOnError) ||
             StringCaseEqual(option, kMessageBufferSize) ||
             StringCaseEqual(option, kTrackOriginalContentLength)) {
    if (!process_scope) {
      // msg is only printed to the user on error, so warnings must be logged.
      handler->Message(kWarning, "'%s' is global and is ignored at this scope",
                       option.as_string().c_str());
      // OK here means "move on" not "accepted and applied".
      return RewriteOptions::kOptionOk;
    }
  } else {
    return RewriteOptions::kOptionNameUnknown;
  }

  // Scope is ok and option is known.  Parse and apply.

  if (StringCaseEqual(option, kStaticAssetPrefix)) {
    set_static_asset_prefix(arg);
    return RewriteOptions::kOptionOk;
  }

  // Most of our options take booleans, so just parse once.
  bool is_on = false;
  RewriteOptions::OptionSettingResult parsed_as_bool =
      RewriteOptions::ParseFromString(arg, &is_on)
          ? RewriteOptions::kOptionOk
          : RewriteOptions::kOptionValueInvalid;
  if (StringCaseEqual(option, kUsePerVHostStatistics)) {
    set_use_per_vhost_statistics(is_on);
    return parsed_as_bool;
  } else if (StringCaseEqual(option, kForceCaching)) {
    set_force_caching(is_on);
    return parsed_as_bool;
  } else if (StringCaseEqual(option, kInstallCrashHandler)) {
    set_install_crash_handler(is_on);
    return parsed_as_bool;
  } else if (StringCaseEqual(option, kListOutstandingUrlsOnError)) {
    set_list_outstanding_urls_on_error(is_on);
    return parsed_as_bool;
  } else if (StringCaseEqual(option, kTrackOriginalContentLength)) {
    set_track_original_content_length(is_on);
    return parsed_as_bool;
  }

  // The two thread counts accept `auto` as a literal, with `0` as its alias,
  // and reject anything negative: a negative count used to
  // convert to size_t as SIZE_MAX on its way to the worker pools, which means
  // unbounded thread creation and a CHECK failure at child init.  Nothing
  // validated it, so the only symptom was the crash.
  if (StringCaseEqual(option, kNumRewriteThreads) ||
      StringCaseEqual(option, kNumExpensiveRewriteThreads)) {
    int count = kAutoThreadCount;
    if (!ParseThreadCountArgument(option, arg, &count, msg, handler)) {
      return RewriteOptions::kOptionValueInvalid;
    }
    if (StringCaseEqual(option, kNumRewriteThreads)) {
      set_num_rewrite_threads(count);
    } else {
      set_num_expensive_rewrite_threads(count);
    }
    return RewriteOptions::kOptionOk;
  }

  // Others take an integer >= 0.
  //
  // Values of 0 have special meanings:
  //   MessageBufferSize: disable the message buffer
  int int_value = 0;
  RewriteOptions::OptionSettingResult parsed_as_int =
      RewriteOptions::ParseFromString(arg, &int_value)
          ? RewriteOptions::kOptionOk
          : RewriteOptions::kOptionValueInvalid;
  if (StringCaseEqual(option, kMessageBufferSize)) {
    set_message_buffer_size(int_value);
    return parsed_as_int;
  }

  LOG(ERROR) << "Unknown option '" << option << "' should have been handled "
             << "in scope checking; ignoring";
  return RewriteOptions::kOptionNameUnknown;
}

RewriteOptions::OptionSettingResult
SystemRewriteDriverFactory::ParseAndSetOption2(
    StringPiece option, StringPiece arg1, StringPiece arg2, bool process_scope,
    GoogleString* msg, MessageHandler* handler) {
  if (StringCaseEqual(option, kCreateSharedMemoryMetadataCache)) {
    if (!process_scope) {
      // msg is only printed to the user on error, so warnings must be logged.
      handler->Message(kWarning, "'%s' is global and is ignored at this scope",
                       option.as_string().c_str());
      // OK here means "move on" not "accepted and applied".
      return RewriteOptions::kOptionOk;
    }

    int64 kb = 0;
    if (!StringToInt64(arg2, &kb) || kb < 0) {
      *msg = "size_kb must be a positive 64-bit integer";
      return RewriteOptions::kOptionValueInvalid;
    }
    bool ok = caches()->CreateShmMetadataCache(arg1, kb, msg);
    return ok ? RewriteOptions::kOptionOk : RewriteOptions::kOptionValueInvalid;
  }
  return RewriteOptions::kOptionNameUnknown;
}

void SystemRewriteDriverFactory::PostConfig(
    const std::vector<SystemServerContext*>& server_contexts,
    GoogleString* error_message, int* error_index,
    Statistics** global_statistics) {
  for (int i = 0, n = server_contexts.size(); i < n; ++i) {
    server_contexts[i]->CollapseConfigOverlaysAndComputeSignatures();
    SystemRewriteOptions* options =
        server_contexts[i]->global_system_rewrite_options();
    if (options->unplugged()) {
      continue;
    }

    if (options->enabled()) {
      GoogleString file_cache_path = options->file_cache_path();
      if (file_cache_path.empty()) {
        *error_index = i;
        *error_message = "FileCachePath must not be empty";
        return;
      }
    }

    if (options->statistics_enabled()) {
      // Lazily create shared-memory statistics if enabled in any config, even
      // when PageSpeed is totally disabled.  This allows statistics to work if
      // PageSpeed gets turned on via .htaccess or query param.
      if (*global_statistics == nullptr) {
        *global_statistics = SetUpGlobalSharedMemStatistics(*options);
      }

      // If we have per-vhost statistics on as well, then set it up.
      if (use_per_vhost_statistics()) {
        server_contexts[i]->CreateLocalStatistics(*global_statistics, this);
      }
    }
  }
}

void SystemRewriteDriverFactory::StopCacheActivity() {
  RewriteDriverFactory::StopCacheActivity();
  caches_->StopCacheActivity();
}

void SystemRewriteDriverFactory::ShutDown() {
  if (!is_root_process_) {
    Variable* child_shutdown_count = statistics()->GetVariable(kShutdownCount);
    child_shutdown_count->Add(1);
    message_handler()->MessageS(kInfo, "Shutting down PageSpeed child");
  }

  StopCacheActivity();

  // Next, we shutdown the fetchers before killing the workers in
  // RewriteDriverFactory::ShutDown; this is so any rewrite jobs in progress
  // can quickly wrap up.
  for (FetcherMap::iterator p = fetcher_map_.begin(), e = fetcher_map_.end();
       p != e; ++p) {
    UrlAsyncFetcher* fetcher = p->second;
    fetcher->ShutDown();
    defer_cleanup(new Deleter<UrlAsyncFetcher>(fetcher));
  }
  fetcher_map_.clear();
  ShutDownFetchers();

  RewriteDriverFactory::ShutDown();

  caches_->ShutDown(message_handler());

  ShutDownMessageHandlers();

  if (is_root_process_) {
    // Cleanup statistics.
    // TODO(morlovich): This looks dangerous with async.
    if (shared_mem_statistics_.get() != nullptr) {
      shared_mem_statistics_->GlobalCleanup(message_handler());
    }

    // Likewise for local ones. We no longer have the objects here
    // (since SplitStats destroyed them), but we saved the segment names.
    for (int i = 0, n = local_shm_stats_segment_names_.size(); i < n; ++i) {
      SharedMemStatistics::GlobalCleanup(shared_mem_runtime_.get(),
                                         local_shm_stats_segment_names_[i],
                                         message_handler());
    }

    // Cleanup SharedCircularBuffer.
    // Use GoogleMessageHandler instead of SystemMessageHandler.
    // As we are cleaning SharedCircularBuffer, we do not want to write to its
    // buffer and passing SystemMessageHandler here may cause infinite loop.
    GoogleMessageHandler handler;
    if (shared_circular_buffer_ != nullptr) {
      shared_circular_buffer_->GlobalCleanup(&handler);
    }
  }
}

GoogleString SystemRewriteDriverFactory::GetFetcherKey(
    bool include_slurping_config, const SystemRewriteOptions* config) {
  // Include all the fetcher parameters in the fetcher key, one per line.
  GoogleString key;
  if (config->unplugged()) {
    key = "unplugged";
  } else {
    key = StrCat(
        list_outstanding_urls_on_error_ ? "list_errors\n" : "no_errors\n",
        config->fetcher_proxy(), "\n",
        config->fetch_with_gzip() ? "fetch_with_gzip\n" : "no_gzip\n",
        track_original_content_length_ ? "track_content_length\n"
                                       : "no_track\n"
                                         "timeout: ",
        Integer64ToString(config->blocking_fetch_timeout_ms()), "\n");
    if (config->slurping_enabled() && include_slurping_config) {
      if (config->slurp_read_only()) {
        StrAppend(&key, "R", config->slurp_directory(), "\n");
      } else {
        StrAppend(&key, "W", config->slurp_directory(), "\n");
      }
    }
    StrAppend(&key, "\nhttps: ", config->https_options(),
              "\ncert_dir: ", config->ssl_cert_directory(),
              "\ncert_file: ", config->ssl_cert_file());
  }

  return key;
}

UrlAsyncFetcher* SystemRewriteDriverFactory::GetFetcher(
    SystemRewriteOptions* config) {
  // Include all the fetcher parameters in the fetcher key, one per line.
  GoogleString key = GetFetcherKey(true, config);
  std::pair<FetcherMap::iterator, bool> result = fetcher_map_.insert(
      std::make_pair(key, static_cast<UrlAsyncFetcher*>(nullptr)));
  FetcherMap::iterator iter = result.first;
  if (result.second) {
    UrlAsyncFetcher* fetcher = nullptr;
    if (config->slurping_enabled()) {
      if (config->slurp_read_only()) {
        HttpDumpUrlFetcher* dump_fetcher = new HttpDumpUrlFetcher(
            config->slurp_directory(), file_system(), timer());
        fetcher = dump_fetcher;
      } else {
        UrlAsyncFetcher* base_fetcher = GetBaseFetcher(config);
        HttpDumpUrlAsyncWriter* dump_writer = new HttpDumpUrlAsyncWriter(
            config->slurp_directory(), base_fetcher, file_system(), timer());
        fetcher = dump_writer;
      }
    } else {
      fetcher = GetBaseFetcher(config);
      if (config->rate_limit_background_fetches()) {
        // Unfortunately, we need stats for load-shedding.
        if (config->statistics_enabled()) {
          TakeOwnership(fetcher);
          fetcher = new RateControllingUrlAsyncFetcher(
              fetcher, max_queue_size(), requests_per_host(), queued_per_host(),
              thread_system(), statistics());
        } else {
          message_handler()->Message(
              kError, "Can't enable fetch rate-limiting without statistics");
        }
      }
    }
    iter->second = fetcher;
  }
  return iter->second;
}

UrlAsyncFetcher* SystemRewriteDriverFactory::GetBaseFetcher(
    SystemRewriteOptions* config) {
  GoogleString cache_key = GetFetcherKey(false, config);
  std::pair<FetcherMap::iterator, bool> result = base_fetcher_map_.insert(
      std::make_pair(cache_key, static_cast<UrlAsyncFetcher*>(nullptr)));
  FetcherMap::iterator iter = result.first;
  if (result.second) {
    iter->second = AllocateFetcher(config);
  }
  return iter->second;
}

UrlAsyncFetcher* SystemRewriteDriverFactory::DefaultAsyncUrlFetcher() {
  LOG(DFATAL) << "The fetchers are not global, but kept in a map.";
  return nullptr;
}

FileSystem* SystemRewriteDriverFactory::DefaultFileSystem() {
  return new StdioFileSystem();
}

Hasher* SystemRewriteDriverFactory::NewHasher() { return new MD5Hasher(); }

#ifdef _WIN32
Timer* SystemRewriteDriverFactory::DefaultTimer() { return new StdTimer(); }
#else
Timer* SystemRewriteDriverFactory::DefaultTimer() { return new PosixTimer(); }
#endif

NamedLockManager* SystemRewriteDriverFactory::DefaultLockManager() {
  LOG(DFATAL) << "Locks are owned by SystemCachePath, not the factory";
  return nullptr;
}

ServerContext* SystemRewriteDriverFactory::NewServerContext() {
  LOG(DFATAL) << "Use implementation-specific MakeXServerXContext() instead";
  return nullptr;
}

int SystemRewriteDriverFactory::requests_per_host() {
  CHECK(thread_counts_finalized_);
  return std::min(4, num_rewrite_threads_);
}

void SystemRewriteDriverFactory::AutoDetectThreadCounts() {
  if (thread_counts_finalized_) {
    return;
  }

  cpu_budget_ = DetectEffectiveCpuBudget();
  concurrent_processes_ = ConcurrentProcessCount();
  ResolveThreadCounts();

  thread_counts_finalized_ = true;
  LogThreadCountResolutionIfChanged();
}

void SystemRewriteDriverFactory::ResolveThreadCounts() {
  const OptimizationThreadCounts computed = ComputeOptimizationThreadCounts(
      cpu_budget_.effective_cores, concurrent_processes_);

  // An explicitly configured count always beats the computed one, in either
  // ordering.  kAutoThreadCount -- which is what both `auto` and `0` parse to
  // -- means "apply the policy", so it does not count as explicit.
  num_rewrite_threads_ = configured_rewrite_threads_ > 0
                             ? configured_rewrite_threads_
                             : computed.rewrite;
  num_expensive_rewrite_threads_ = configured_expensive_rewrite_threads_ > 0
                                       ? configured_expensive_rewrite_threads_
                                       : computed.expensive;
}

void SystemRewriteDriverFactory::ReresolveIfFinalized() {
  if (!thread_counts_finalized_) {
    return;  // Resolution hasn't run yet; it will pick the new value up.
  }
  ResolveThreadCounts();
  LogThreadCountResolutionIfChanged();
}

void SystemRewriteDriverFactory::LogThreadCountResolutionIfChanged() {
  if (num_rewrite_threads_ == logged_rewrite_threads_ &&
      num_expensive_rewrite_threads_ == logged_expensive_rewrite_threads_) {
    return;
  }
  logged_rewrite_threads_ = num_rewrite_threads_;
  logged_expensive_rewrite_threads_ = num_expensive_rewrite_threads_;
  LogThreadCountResolution();
}

void SystemRewriteDriverFactory::LogThreadCountResolution() {
  // kWarning, not kInfo: see the declaration.  Apache's default LogLevel is
  // warn, and this line has to reach an operator who has not changed it.
  if (concurrent_processes_ <= 0) {
    message_handler()->Message(
        kWarning,
        "PageSpeed optimization threads: %d rewrite, %d expensive rewrite "
        "(per server process). Could not determine how many server processes "
        "share this machine, so the minimum was used; set NumRewriteThreads "
        "and NumExpensiveRewriteThreads to choose the counts yourself. "
        "Effective CPU budget: %d whole cores (limited by %s).",
        num_rewrite_threads_, num_expensive_rewrite_threads_,
        cpu_budget_.effective_cores, CpuBudgetSourceName(cpu_budget_.source));
    return;
  }
  message_handler()->Message(
      kWarning,
      "PageSpeed optimization threads: %d rewrite, %d expensive rewrite "
      "(per server process). Effective CPU budget: %d whole cores (limited by "
      "%s); server processes: %d; %s. Override with NumRewriteThreads and "
      "NumExpensiveRewriteThreads.",
      num_rewrite_threads_, num_expensive_rewrite_threads_,
      cpu_budget_.effective_cores, CpuBudgetSourceName(cpu_budget_.source),
      concurrent_processes_,
      IsServerThreaded() ? "threaded server" : "non-threaded server");
}

}  // namespace net_instaweb
