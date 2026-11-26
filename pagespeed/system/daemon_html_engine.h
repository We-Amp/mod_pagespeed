// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The HTML cache engine: one port-neutral implementation of storing a page
// this module served, serving its Markdown copy, and fencing both against
// purges, for every server port.
//
// WHAT IS DECIDED WHERE.  Every policy verdict -- may this response be
// stored, may this request be answered with the Markdown copy, which entity
// tag does the copy carry, does an If-None-Match match it -- comes from the
// optimizer library through the HTML serve set of the C API (see
// daemon_abi.h).  The engine decides nothing about HTTP caching itself.  What
// it owns is everything the library cannot see:
//
//   * the request's standing: whether the server and this module read the
//     URL the same way (url_agreed), whether the host is one of the site's
//     exact configured names (HostExactlyConfigured), whether PageSpeed is on
//     for the URL;
//   * the two header views a store is classified on, with this module's own
//     header lines taken out of both;
//   * the order of the writes, the purge fence around them and the per-site
//     purge epoch;
//   * the re-store skip, the refusal removal, the notification limiter and
//     the process-wide byte budget;
//   * the exact header set a Markdown answer carries.
//
// WHEN IT ACTS AT ALL.  Only in the serve-variants mode with AgentOptimize
// on, and only while four conditions hold, read at creation and on every
// purge-poll tick (never per request): the library binds the whole HTML
// serve set (DaemonAbi::HasHtmlServeSet), the worker's HTML mode is one of
// the two markdown modes, and the worker reports agent-optimize on.  Any
// other state is report-only: nothing is written or served, the
// daemon_html_serve_unavailable counter moves for each HTML response, and
// one line is logged for each change of state, naming the condition.
//
// THE PURGE FENCE.  An entry is purged when its insert time (the request's
// start, floored to the second and moved back by the inbound Age) is at or
// before the newest purge that covers it: a URL purge, the global purge this
// process observed (clamped to the time it was observed, so a future-dated
// cache.flush cannot fence what is stored after it), or the per-site purge
// epoch persisted beside the optimizer's volume.  A store is re-checked
// against the fence after its write and removed when a purge covers it; a
// serve passes the fence to the library as the PURGED fact.  An epoch read
// that fails fences every entry of the site until a read succeeds.
//
// The engine is thread-safe.  Blocking volume I/O on the store path is
// handed to the port (DaemonHtmlPort::RunBlocking) so an event-driven server
// can run it off its event thread.  The engine must outlive every commit it
// handed to a port or runner, and an engine registered as a purge observer
// (SystemCachePath::AddPurgeObserver) must be removed before it is
// destroyed.

#ifndef PAGESPEED_SYSTEM_DAEMON_HTML_ENGINE_H_
#define PAGESPEED_SYSTEM_DAEMON_HTML_ENGINE_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/system/daemon_html_purge_observer.h"
#include "pagespeed/system/serve_host_names.h"

namespace net_instaweb {

class DaemonAbi;
class DaemonHealNotifyLimiter;
class MessageHandler;
class Statistics;
class Timer;
class Variable;

enum class DaemonHtmlMode {
  kOff,
  kReportOnly,
  kServeVariants,
  kServeCachedHtml,  // Not built; treated as kServeVariants for Markdown.
};

using DaemonHtmlHeaderLines =
    std::vector<std::pair<GoogleString, GoogleString>>;

// One request, as the port reads it.  Owned copies, filled once.
struct DaemonHtmlRequestFacts {
  GoogleString method;
  GoogleString scheme;  // "http" or "https"
  GoogleString host;    // as received, port included
  // The request target's path, with its query string when there is one.
  GoogleString path;
  // The site of the server block, virtual host or IIS site that admitted the
  // request (a site id, see IsSiteId).
  GoogleString site;
  // The port's in-place URL agreement: the server and this module name the
  // same path on the same host.  Main requests only.
  bool url_agreed = false;
  bool has_authorization = false;  // Authorization or Proxy-Authorization
  bool authenticated_user = false;
  bool has_cookie = false;  // Cookie or Cookie2
  // Every Accept line, in order; joined with ", " for the library.
  std::vector<GoogleString> accept_lines;
  // Every Cache-Control and Pragma line, joined with ", "; `has_*` tells an
  // absent field from an empty one.
  GoogleString cache_control;
  bool has_cache_control = false;
  GoogleString pragma;
  bool has_pragma = false;
  // The client's conditionals AS THEY ARRIVED, read from the per-request
  // stash the port fills before it removes this module's own entity tags
  // from the request to the origin.
  GoogleString if_none_match;
  bool has_if_none_match = false;
  GoogleString if_modified_since;
  bool has_if_modified_since = false;
  bool has_if_match = false;
  bool has_if_unmodified_since = false;
  bool has_range = false;
  bool has_if_range = false;
  bool has_origin = false;
  // The options context, passed through to a notification.
  GoogleString option_context;
  GoogleString option_signature;
  // When the request was sent to the origin (or entered the handler), Unix
  // milliseconds; never a later moment.
  int64 capture_start_ms = 0;
  // options->enabled() and options->IsAllowed(url) for this request.
  bool options_enabled_and_allowed = false;
  GoogleString agent_class;  // Not used yet; empty.
};

// What the engine asks the port to do with one request.
struct DaemonHtmlServeResult {
  enum Kind { kFallThrough, kFinal, kAsOrigin /* not produced */ };
  Kind kind = kFallThrough;
  // A closed-set name: the counter suffix of a fall-through.
  GoogleString reason;
  int status = 0;  // 200 or 304 with kFinal
  // Exactly the lines to send, in order.  The port clears every line of
  // these names and removes every other header from all of its tables.
  DaemonHtmlHeaderLines headers;
  GoogleString body;                // Empty for a HEAD and a 304.
  bool suppress_recording = false;  // Not used yet.
};

// A unit of blocking volume I/O.  Runs at most once.
class DaemonHtmlCommit {
 public:
  DaemonHtmlCommit() = default;
  explicit DaemonHtmlCommit(std::function<void()> work)
      : work_(std::move(work)) {}
  DaemonHtmlCommit(DaemonHtmlCommit&&) = default;
  DaemonHtmlCommit& operator=(DaemonHtmlCommit&&) = default;
  DaemonHtmlCommit(const DaemonHtmlCommit&) = delete;
  DaemonHtmlCommit& operator=(const DaemonHtmlCommit&) = delete;

  void Run() {
    std::function<void()> work = std::move(work_);
    work_ = nullptr;
    if (work) work();
  }
  bool empty() const { return !work_; }

 private:
  std::function<void()> work_;
};

// Something that can run blocking work: inline on a thread that may block,
// or on a pool for an event-driven server.
class DaemonHtmlBlockingRunner {
 public:
  virtual ~DaemonHtmlBlockingRunner() = default;
  virtual void RunBlocking(DaemonHtmlCommit commit) = 0;
};

// What a server port provides for one request.
class DaemonHtmlPort : public DaemonHtmlBlockingRunner {
 public:
  ~DaemonHtmlPort() override = default;
  // Clear every header the response carries in every table the server
  // writes from, then emit exactly result.headers and, unless `head`,
  // result.body.  False when the port could not; the request then falls
  // through.
  virtual bool ClearAndEmitAllowlisted(const DaemonHtmlServeResult& result,
                                       bool head) = 0;
  // No compression for this response, whatever the server configures.
  virtual void MarkNoCompress() = 0;
  virtual bool EmitAsOrigin(const DaemonHtmlServeResult& /*result*/) {
    return false;
  }
  // Remove the client's conditionals from the request to the origin, after
  // stashing them (the stash is what DaemonHtmlRequestFacts reads).
  virtual void RemoveClientConditionals() = 0;
};

// The optimizer's shared volume, through the run-time binding.
struct DaemonHtmlSubstrate {
  const DaemonAbi* abi = nullptr;
  void* cache = nullptr;
  // The directory the worker publishes its shared configuration under.
  GoogleString cache_path;
  // The worker's notification socket.
  GoogleString notify_socket;
};

// Where the purge state of this process comes from.
class DaemonHtmlPurgeSource {
 public:
  virtual ~DaemonHtmlPurgeSource() = default;
  // False when `url` was purged by URL at or after `time_ms`.  The global
  // invalidation timestamp is NOT consulted here.
  virtual bool IsUrlValidIgnoringGlobal(const GoogleString& url,
                                        int64 time_ms) const = 0;
  // The global invalidation timestamp this process holds, or a negative
  // value for none.
  virtual int64 GlobalInvalidationTimestampMs() const = 0;
  // The cache.flush file's modification time, when it exists.
  virtual bool FlushFileMtimeMs(int64* mtime_ms) const = 0;
};

// The purge fence: URL purges, the global purge as observed and clamped,
// and the site's persisted epoch.  Thread-safe.
class DaemonHtmlPurgeOracle {
 public:
  DaemonHtmlPurgeOracle(const DaemonHtmlPurgeSource* source, Timer* timer);

  // Takes in the source's global timestamp and the flush file's time.  A
  // value seen for the first time counts as min(value, now): a future
  // value cannot fence what is stored after the moment it was observed.
  // Returns the clamped global purge time, or a negative value for none.
  int64 Observe();
  // A global purge made by this process, at `purge_ms`.
  void NoteGlobalPurge(int64 purge_ms);
  // The result of reading the site's epoch file: a failed read fences the
  // whole site until a read succeeds.
  void SetSiteEpoch(bool read_ok, uint64 epoch_s);
  // An epoch this process wrote for the site.
  void RaiseSiteEpoch(uint64 epoch_s);

  // Whether an entry inserted at `stored_at_s` under `url` (the cache key,
  // normalised) is covered by a purge.  Refreshes the global first.
  bool Purged(const GoogleString& url, uint32 stored_at_s);

  // For a re-check that must not see more than this process observed.
  void set_recheck_disabled_for_testing(bool v) { recheck_disabled_ = v; }
  bool recheck_disabled() const { return recheck_disabled_; }

 private:
  int64 ObserveLocked();

  const DaemonHtmlPurgeSource* source_;
  Timer* timer_;
  std::mutex mutex_;
  int64 last_raw_global_ms_ = -1;
  int64 last_raw_flush_ms_ = -1;
  int64 clamped_global_ms_ = -1;
  bool site_epoch_ok_ = true;
  uint64 site_epoch_s_ = 0;
  std::atomic<bool> recheck_disabled_{false};
};

// The process-wide budget of bytes the engine may hold for bodies being
// recorded.  Thread-safe.
class DaemonHtmlByteBudget {
 public:
  static constexpr int64 kDefaultBytes = 64 << 20;

  explicit DaemonHtmlByteBudget(int64 capacity = kDefaultBytes)
      : capacity_(capacity) {}
  bool TryReserve(int64 bytes);
  void Release(int64 bytes);
  int64 in_use() const { return in_use_.load(std::memory_order_relaxed); }
  void set_capacity(int64 bytes) {
    capacity_.store(bytes, std::memory_order_relaxed);
  }

 private:
  std::atomic<int64> capacity_;
  std::atomic<int64> in_use_{0};
};

class DaemonHtmlEngine;

// The capture of one response: created by StartRecording, fed by the port,
// finished once.  Not thread-safe; one request.
class DaemonHtmlRecording {
 public:
  ~DaemonHtmlRecording();
  DaemonHtmlRecording(const DaemonHtmlRecording&) = delete;
  DaemonHtmlRecording& operator=(const DaemonHtmlRecording&) = delete;

  void set_status(int status) { status_ = status; }
  // The origin's headers at the rewriter's input.
  void SetOriginHeaders(DaemonHtmlHeaderLines lines);
  // The header set the rewriter started from; the origin's when not set.
  void SetBaselineHeaders(DaemonHtmlHeaderLines lines);
  // The lines this module itself added (X-Mod-Pagespeed, its Vary tokens).
  // They are taken out of both views.
  void SetModuleHeaders(DaemonHtmlHeaderLines lines);
  // The header set at the store snapshot (the store view is the origin plus
  // the change from the baseline to this set).
  void NoteStoreHeaders(DaemonHtmlHeaderLines lines);
  // The header set at the final view (the origin plus the change from the
  // baseline to this set, transport changes ignored).
  void NoteFinalHeaders(DaemonHtmlHeaderLines lines);

  // Bounded by the engine's max bytes and the process-wide budget.
  void AddBody(const char* data, size_t size);

  // Classifies both views and stores, or removes a refused entry, as the
  // library and the rules allow.  `aborted` is the server's word that the
  // client went away; nothing is stored then.  Blocking work goes through
  // the port given to StartRecording.
  void Finish(bool aborted);

  bool budget_refused() const { return budget_refused_; }

 private:
  friend class DaemonHtmlEngine;
  DaemonHtmlRecording(DaemonHtmlEngine* engine,
                      const DaemonHtmlRequestFacts& facts, DaemonHtmlPort* port,
                      bool may_store);
  void ReleaseBudget();

  DaemonHtmlEngine* engine_;
  DaemonHtmlRequestFacts facts_;
  DaemonHtmlPort* port_;
  bool may_store_;
  int status_ = 0;
  DaemonHtmlHeaderLines origin_;
  DaemonHtmlHeaderLines baseline_;
  bool has_baseline_ = false;
  DaemonHtmlHeaderLines module_;
  DaemonHtmlHeaderLines store_lines_;
  bool has_store_lines_ = false;
  DaemonHtmlHeaderLines final_lines_;
  bool has_final_lines_ = false;
  GoogleString body_;
  uint64 body_bytes_ = 0;
  int64 reserved_ = 0;
  bool budget_refused_ = false;
  bool over_size_ = false;
  bool finished_ = false;
};

class DaemonHtmlEngine : public DaemonHtmlPurgeObserver {
 public:
  struct Config {
    DaemonHtmlMode mode = DaemonHtmlMode::kOff;
    // DaemonHtmlMaxBytes: the largest body recorded.
    int64 max_bytes = 2 << 20;
    bool agent_optimize = false;
    int serve_scope = 0;  // Not used yet.
    // The names this vhost's configuration lists.
    ConfiguredHostNames host_names;
    // This vhost's site id.
    GoogleString site;
    // Every site configured on this engine's cache path, from the
    // configuration (not from the engines created so far): the global purge
    // epoch is written for each of them.  Empty means `site` alone.
    std::vector<GoogleString> epoch_sites;
  };

  struct Deps {
    DaemonHtmlSubstrate substrate;
    const DaemonHtmlPurgeSource* purge_source = nullptr;
    Statistics* statistics = nullptr;
    Timer* timer = nullptr;
    MessageHandler* handler = nullptr;
    // One per process (ProcessDaemonHealNotifyLimiter); keyed here on the
    // URL and whether the request asked for the Markdown copy.
    DaemonHealNotifyLimiter* limiter = nullptr;
    DaemonHtmlByteBudget* budget = nullptr;
    // Runs the admin path's and the ticks' volume I/O; inline when null.
    DaemonHtmlBlockingRunner* runner = nullptr;
  };

  // Points inside a commit, for tests that interleave a purge.
  class TestHooks {
   public:
    virtual ~TestHooks() = default;
    virtual void AtPoint(const char* point) = 0;  // after_commit, after_recheck
  };

  // The engine's counters are registered by RewriteStats::InitStats
  // (kDaemonHtmlEngineCounters, kDaemonMarkdownFallthroughReasons).

  // Reads the four serving conditions, the site's purge epoch and the purge
  // state, and writes the epoch when that state is newer than what this
  // process wrote.  Logs nothing: the first HTML response or tick does.
  DaemonHtmlEngine(Config config, Deps deps);
  ~DaemonHtmlEngine() override;

  DaemonHtmlEngine(const DaemonHtmlEngine&) = delete;
  DaemonHtmlEngine& operator=(const DaemonHtmlEngine&) = delete;

  // A recording for an HTML response, or nullptr when there is nothing the
  // engine may do with it (mode off, URL disagreement, a host the site does
  // not list, no site).  `port` must outlive the recording.
  std::unique_ptr<DaemonHtmlRecording> StartRecording(
      const DaemonHtmlRequestFacts& facts, DaemonHtmlPort* port);

  // The Markdown serve: a fall-through, or a final answer the port has been
  // asked to emit.  Reads the volume only past the serve gate.
  DaemonHtmlServeResult Serve(const DaemonHtmlRequestFacts& facts,
                              DaemonHtmlPort* port);

  // DaemonHtmlPurgeObserver: removes the URL's entry, or writes the epoch
  // for a global purge, before `done` is called.
  void OnAdminPurge(StringPiece url, bool global, int64 now_ms,
                    Done done) override;

  // The purge-poll tick: re-reads the serving conditions (logging a
  // change) and writes the epoch when the observed purge state advanced.
  void OnPurgePollTick();
  // The statistics tick: as above for the purge state, re-reads the site's
  // epoch, and counts keys whose header block stayed unpaired for longer
  // than their lifetime.
  void OnStatisticsTick();

  // For the port: enforcing the Markdown header set found a
  // Content-Encoding or Content-Range line it had to keep so the body stays
  // parseable (a compressor that ignored the no-compress mark).  Counted as
  // daemon_markdown_transport_conflict, which must stay 0.
  void NoteTransportConflict();

  // Whether the engine stores and serves now.
  bool serving() const;
  const Config& config() const { return config_; }

  void set_test_hooks(TestHooks* hooks) { test_hooks_ = hooks; }
  DaemonHtmlPurgeOracle* oracle() { return &oracle_; }

 private:
  friend class DaemonHtmlRecording;
  struct Counters;
  struct StoreVerdict;

  // The cache key of a request: scheme://host[:port]path, the host
  // lowercased and a default port dropped; and that host.
  static void CacheKey(const DaemonHtmlRequestFacts& facts, GoogleString* url,
                       GoogleString* hostname);

  // §3 state.
  void ReadConditions(bool log_change);
  // Logs the not-serving reason once per state.
  void NoteUnavailable();
  void ObservePurgeState();
  void WriteEpoch(uint64 epoch_s, bool* all_ok);
  void ReadSiteEpoch();

  // The two views and the store path.
  void FinishRecording(DaemonHtmlRecording* recording, bool aborted);
  bool Classify(const DaemonHtmlRecording& r, const DaemonHtmlHeaderLines& view,
                int mode, StoreVerdict* out);
  void Commit(const DaemonHtmlRequestFacts& facts, const GoogleString& url,
              const GoogleString& hostname, const StoreVerdict& verdict,
              const DaemonHtmlHeaderLines& store_view, const GoogleString& body,
              bool agent_request);
  void MaybeRemoveOnRefusal(const DaemonHtmlRequestFacts& facts,
                            const GoogleString& url,
                            const GoogleString& hostname, int status,
                            const StoreVerdict& refusal, DaemonHtmlPort* port);
  bool RecentlyRemoved(const GoogleString& key, int64 now_s);
  void NoteRemoved(const GoogleString& key, int64 now_s);
  void ForgetRemoved(const GoogleString& key);
  void TrackUnpaired(const GoogleString& url, const GoogleString& hostname,
                     const GoogleString& scheme);
  void Notify(const DaemonHtmlRequestFacts& facts, const GoogleString& url,
              const GoogleString& hostname, bool agent_request);
  void CountFallThrough(const GoogleString& reason);
  void Hook(const char* point);

  Config config_;
  Deps deps_;
  DaemonHtmlPurgeOracle oracle_;
  std::unique_ptr<Counters> counters_;
  TestHooks* test_hooks_ = nullptr;

  mutable std::mutex mutex_;
  // §3: the serve set and the worker's state.
  bool set_bound_ = false;
  bool worker_markdown_ = false;
  bool worker_agent_entitled_ = false;
  GoogleString unavailable_reason_;
  bool unavailable_logged_ = false;
  // The newest global purge (seconds) this process wrote as an epoch.
  int64 epoch_written_s_ = -1;
  // Keys this process removed (or found absent) on a refusal, with the
  // second; bounded.
  std::map<GoogleString, int64> recently_removed_;
  // Keys whose header block was found unpaired, with their entry's insert
  // time and lifetime; bounded.
  struct Unpaired {
    GoogleString url, hostname, scheme;
  };
  std::map<GoogleString, Unpaired> unpaired_;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_SYSTEM_DAEMON_HTML_ENGINE_H_
