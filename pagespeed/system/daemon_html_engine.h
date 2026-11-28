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
//   * the exact header set a Markdown answer carries, and with a library
//     that publishes C API 1.16 the coded copy it carries: the coding the
//     library chose from the request's Accept-Encoding, read by exact id and
//     sent as stored, never compressed here.
//
// WHEN IT ACTS AT ALL.  Only in the serve-variants mode or a later one
// (serve-cached-html, which needs the library's 1.15 set and otherwise runs
// as serve-variants: effective_mode) with AgentOptimize on, and only while
// four conditions hold, read at creation and on every
// purge-poll tick (never per request): the library binds the whole HTML
// serve set (DaemonAbi::HasHtmlServeSet), the worker's HTML mode is one of
// the two markdown modes, and the worker reports agent-optimize on.  Any
// other state is report-only: nothing is written or served, the
// daemon_html_serve_unavailable counter moves for each HTML response, and
// one line is logged for each change of state, naming the condition.
//
// REPORT-ONLY.  In report-only (the mode, or the state above) every HTML
// response a port records is classified, counted (daemon_html_evaluated,
// daemon_html_would_store, daemon_html_refused_<reason>, ...) and logged to
// the decision log, whatever its host or URL, and nothing else happens: no
// volume call, no byte of the response changed.  The line carries the
// final view's verdict, the view the report has always read; with a library
// that publishes C API 1.14 it also names, in vhost_added, the header lines
// the server's own configuration added after the store snapshot.  When the
// engine stores and serves, the same log takes a line for each store, each
// Markdown answer and each purge that removed an entry, through the same
// writer thread and under the same per-second limit.
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
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <utility>
#include <vector>

#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/system/daemon_html_agent_class.h"
#include "pagespeed/system/daemon_html_purge_observer.h"
#include "pagespeed/system/serve_host_names.h"

namespace net_instaweb {

class DaemonAbi;
class DaemonHealNotifyLimiter;
class DaemonHtmlDecisionLog;
class MessageHandler;
class Statistics;
class Timer;
class Variable;

enum class DaemonHtmlMode {
  kOff,
  kReportOnly,
  kServeVariants,
  // Serves the Markdown copies exactly as kServeVariants does, and sends the
  // library the serve-cached-html mode with the 1.15 request fields.  Runs
  // as kServeVariants while the library lacks the 1.15 set (see
  // DaemonHtmlEngine::effective_mode).  The stored-HTML answer itself is not
  // built yet: such a verdict falls through.
  kServeCachedHtml,
};

// The engine mode a DaemonHtmlCache setting asks for.
DaemonHtmlMode DaemonHtmlModeFor(RewriteOptions::DaemonHtmlCacheMode mode);

// Whether a DaemonHtmlCache setting stores pages and serves from them
// (serve-variants or any later mode).  Every port asks this, never `==` or
// `!=` against one mode, so a mode added later is accepted wherever
// serve-variants is.
inline bool DaemonHtmlCacheServes(RewriteOptions::DaemonHtmlCacheMode mode) {
  return mode >= RewriteOptions::kDaemonHtmlCacheServeVariants;
}

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
  // Every Accept-Encoding line as received, in order (none: the field was
  // absent).  Read only with the library's 1.16 set, for the coding of a
  // Markdown answer.
  std::vector<GoogleString> accept_encoding_lines;
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
  // The first AJAX marker the request carries (DaemonHtmlAjaxMarker), or
  // empty.  A request with one is never stored and never served.  With a
  // library that publishes C API 1.15 the request guard reads the markers
  // too, from request_headers, and refuses either way.
  GoogleString ajax_marker;
  // Every request header line as received, name and value, unfiltered: what
  // the request guard (ps_html_request_guard) reads for the headers a cache
  // key does not contain.  Read only with C API 1.15.
  DaemonHtmlHeaderLines request_headers;
  // The request target as received, path and query, not normalised: what
  // the request guard reads for the shape of the path.
  GoogleString raw_path;
  // Every Cookie line's value, in order: what ps_html_classify_cookies sorts
  // into none, ignorable-only or blocking.  has_cookie is still set for any
  // Cookie or Cookie2 line; a cookie the lines do not show is blocking.
  std::vector<GoogleString> cookie_lines;
  // The options context, passed through to a notification.
  GoogleString option_context;
  GoogleString option_signature;
  // When the request was sent to the origin (or entered the handler), Unix
  // milliseconds; never a later moment.
  int64 capture_start_ms = 0;
  // options->enabled() and options->IsAllowed(url) for this request.
  bool options_enabled_and_allowed = false;
  // The request's agent class (daemon_html_agent_class.h): 0 unknown, 1 a
  // signed agent, 3 a known agent's User-Agent token; 2 is never produced.
  // The port computes it once per client request.  Sent in the
  // serve-cached-html mode.
  int agent_class = 0;
};

// The name of the AJAX marker one request header line is, or nullptr: an
// X-Requested-With whose value contains XMLHttpRequest (compared
// case-insensitively), or an X-MicrosoftAjax or X-Prototype-Version with any
// non-empty value.  A
// script's request for a page may get a fragment from the origin, so the
// HTML cache neither stores the answer under the page's key nor answers the
// request from the cache.  Each port feeds every request header line here and
// keeps the first match in DaemonHtmlRequestFacts::ajax_marker.
const char* DaemonHtmlAjaxMarker(StringPiece name, StringPiece value);

// The request target as the client sent it, for DaemonHtmlRequestFacts::
// raw_path: path and query, with the scheme and authority of an
// absolute-form target taken off; "/" for an empty one.
GoogleString DaemonHtmlRawRequestPath(StringPiece target);

// The scheme the HTML cache keys a request on: the connection's
// (`connection_scheme`), or, with RespectXForwardedProto on
// (`respect_forwarded_proto`), the first X-Forwarded-Proto line of
// `request_headers` when it names http or https (any case), as the URL this
// module builds for the request reads it.
GoogleString DaemonHtmlKeyedScheme(StringPiece connection_scheme,
                                   const DaemonHtmlHeaderLines& request_headers,
                                   bool respect_forwarded_proto);

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
  // With a coded copy (a Content-Encoding line in `headers`): the plain
  // copy's entity tag and, for a GET, its bytes, for a port that finds, once
  // the server's own header stages ran, that a body filter would rewrite the
  // answer: it sends the plain copy instead (CodedAnswerReplacedByPlain).
  GoogleString plain_etag;
  GoogleString plain_body;
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
  // through, and the port must undo MarkNoCompress first, so the HTML
  // answer is sent exactly as it would have been.
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
  // After ClearAndEmitAllowlisted: the port sent the result's plain copy in
  // place of its coded one (a body filter of the server would have rewritten
  // the coded bytes).
  virtual bool CodedAnswerReplacedByPlain() const { return false; }
  // Whether the port may do so: only then is the plain copy read and carried
  // beside a coded answer (DaemonHtmlServeResult::plain_body, plain_etag).
  virtual bool MaySendPlainInstead() const { return false; }
};

// The optimizer's shared volume, through the run-time binding.
struct DaemonHtmlSubstrate {
  const DaemonAbi* abi = nullptr;
  void* cache = nullptr;
  // When set, asked for the handle at each use instead of `cache`, and
  // `cache` is ignored.  A handle it returned must stay valid until the
  // engine is destroyed (a replaced handle is set aside, not closed).  For a server whose handle can change while the
  // engine lives: a volume the optimizer replaced is reopened, and while no
  // handle can be vouched for the answer is null, so nothing is written,
  // removed or served (DaemonAdapter::RecordCacheForRequest).  Each store,
  // serve, removal and tick asks once and keeps that handle to its end.
  std::function<void*()> cache_source;
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
  // A URL purge this process carried out (the admin path), at `purge_ms`
  // (never later than now), under the cache key `url`: fenced here at once,
  // before the purge set the source reads has polled the purge in.
  // Bounded: when the oldest has to go, everything stored at or before it
  // stays fenced.
  void NoteUrlPurge(const GoogleString& url, int64 purge_ms);
  static constexpr size_t kLocalUrlPurgesMax = 1024;

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
  std::map<GoogleString, int64> local_url_purges_;
  // Every entry stored at or before this is purged: the newest purge the
  // bounded map above had to forget.
  int64 local_url_floor_ms_ = -1;
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

// The budget every engine of this process shares (never destroyed).
DaemonHtmlByteBudget* ProcessDaemonHtmlByteBudget();

// The keys this process asked the optimizer to compress the Markdown copy
// of: each at most once, in a bounded set that forgets its oldest key first.
// A key is kept as its SHA-256 digest, so the set's memory is bounded by its
// capacity whatever the URLs' lengths.  Thread-safe.
class DaemonMarkdownCompressDemands {
 public:
  static constexpr size_t kDefaultCapacity = 4096;

  explicit DaemonMarkdownCompressDemands(size_t capacity = kDefaultCapacity)
      : capacity_(capacity) {}
  // True the first time `key` is seen (or seen again after it was dropped):
  // the caller sends the demand.  False while the key is in the set.
  bool Admit(const GoogleString& key);
  // The demand for `key` was not sent: the next request may ask again.
  void Forget(const GoogleString& key);
  size_t size() const;

 private:
  static GoogleString Digest(const GoogleString& key);

  const size_t capacity_;
  mutable std::mutex mutex_;
  std::set<GoogleString> keys_;
  std::deque<GoogleString> order_;
};

// The set every engine of this process shares (never destroyed).
DaemonMarkdownCompressDemands* ProcessDaemonMarkdownCompressDemands();

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
  // baseline to this set, transport changes ignored).  Only the first call
  // counts.
  void NoteFinalHeaders(DaemonHtmlHeaderLines lines);

  // Bounded by the engine's max bytes and the process-wide budget.  A
  // recording that cannot store keeps only the prefix the body scan reads,
  // outside the budget.
  void AddBody(const char* data, size_t size);
  // The server's word that the body ended: a report of a body that never
  // ended reads as incomplete.
  void set_body_ended() { body_ended_ = true; }
  // Whether a baseline was set and the final view has not been noted yet.
  bool needs_final_headers() const {
    return has_baseline_ && !has_final_lines_;
  }

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
  bool body_ended_ = false;
  bool finished_ = false;
  // With C API 1.15: the request's cookie class decides HAS_COOKIE (only a
  // blocking cookie sets it), and the ignore-listed cookies (name, value)
  // are what the echo check looks for in the page before it is stored.
  bool cookie_classified_ = false;
  bool blocking_cookie_ = false;
  std::vector<std::pair<GoogleString, GoogleString>> ignorable_cookies_;
};

class DaemonHtmlEngine : public DaemonHtmlPurgeObserver {
 public:
  struct Config {
    DaemonHtmlMode mode = DaemonHtmlMode::kOff;
    // DaemonHtmlMaxBytes: the largest body recorded.
    int64 max_bytes = 2 << 20;
    bool agent_optimize = false;
    // The serve-cached-html settings (FillDaemonHtmlServeSettings).
    // DaemonHtmlCacheServeScope: kPsHtmlServeScopeAgents or ...All.
    int serve_scope = 0;
    // DaemonHtmlCacheMaxAge: 0 = no cap; it only shortens the origin's
    // lifetime, and only on the HTML path.
    uint32_t max_age_s = 3600;
    // DaemonHtmlCacheCredentialed: kPsHtmlCredentialedAnonymousOnly or
    // ...ExplicitPublic.
    int credentialed = 0;
    // DaemonHtmlCacheIgnoreCookies (the built-in list of analytics cookies),
    // and the DaemonHtmlCacheIgnoreCookie additions.  Applied with a library
    // that publishes C API 1.15: a request whose every cookie is ignorable
    // counts as anonymous.  Below 1.15 every cookie counts as a cookie.
    bool ignore_cookies = true;
    std::vector<GoogleString> ignore_cookie;
    // The stamp of the credentialed policy (ApplyPolicySettings), written
    // into every stored page's header block and sent as the request's
    // policy_stamp; 0 = none (a library below 1.15).
    uint64_t policy_stamp = 0;
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
    // The compress demands sent; the process's own when null.
    DaemonMarkdownCompressDemands* compress_demands = nullptr;
    // The decision log goes to <log_dir>/daemon-html/; none when empty.
    GoogleString log_dir;
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

  // Configuration load, run by every port before it creates the engine:
  // checks each ignore-list addition with the library's guard
  // (ps_html_ignore_cookie_guard), keeps the additions as a sorted set, and
  // computes config->policy_stamp (ps_html_policy_stamp).  False, with
  // *error naming the addition, when the guard refuses one or the stamp
  // cannot be computed: the port must not store or serve with this
  // configuration.  With a library below 1.15 the settings have no effect:
  // true, and the stamp stays 0.
  static bool ApplyPolicySettings(const DaemonAbi& abi, Config* config,
                                  GoogleString* error);

  DaemonHtmlEngine(const DaemonHtmlEngine&) = delete;
  DaemonHtmlEngine& operator=(const DaemonHtmlEngine&) = delete;

  // A recording for an HTML response, or nullptr when there is nothing the
  // engine may do with it: mode off, or no library that can classify (then
  // `*unavailable` is set, when given; the port counts it with
  // CountUnavailable once it knows it handles the response).  While the
  // engine stores, a request it may not store for gets nullptr too: a URL
  // disagreement, a host the site does not list, no site.  Otherwise
  // (report-only) every response is recorded and reported.  `port` must
  // outlive the recording; with a null port nothing is ever stored, as there
  // is nowhere to run the blocking work, and the recording only reports.
  std::unique_ptr<DaemonHtmlRecording> StartRecording(
      const DaemonHtmlRequestFacts& facts, DaemonHtmlPort* port,
      bool* unavailable = nullptr);

  // An HTML response the report covers that no installed library could
  // classify (daemon_html_unavailable).
  void CountUnavailable();

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
  // The mode the engine runs: the configured one, except that
  // serve-cached-html runs as serve-variants while the library lacks the 1.15
  // set (DaemonAbi::HasHtmlServe15Set), read with the other serving
  // conditions.  The library is never sent the serve-cached-html mode then.
  // The binding is fixed when the library is loaded, so in a server this
  // changes only with a server restart.
  DaemonHtmlMode effective_mode() const;
  const Config& config() const { return config_; }
  // The agent class counters, resolved once for this engine's statistics;
  // the port counts each request's class with them.
  const DaemonHtmlAgentClassCounters& agent_class_counters() const {
    return agent_class_counters_;
  }

  void set_test_hooks(TestHooks* hooks) { test_hooks_ = hooks; }
  // Tests only (no directive, option or environment variable reaches it):
  // the request-credential skip before any read is turned off, so the
  // refusal at emit can be reached.  The credentials are still classified.
  void set_credential_skip_disabled_for_testing(bool v) {
    credential_skip_disabled_ = v;
  }
  DaemonHtmlPurgeOracle* oracle() { return &oracle_; }

 private:
  friend class DaemonHtmlRecording;
  struct Counters;
  struct StoreVerdict;
  struct RequestPolicy;

  // The credentialed policy, the request guard and the cookie classes of one
  // request (C API 1.15 only; see RequestPolicy).
  void EvaluateRequestPolicy(const DaemonHtmlRequestFacts& facts,
                             RequestPolicy* out);
  // Counts and logs a request the policy refused before any read.
  void NoteRequestRefused(const DaemonHtmlRequestFacts& facts,
                          const RequestPolicy& policy,
                          const GoogleString& reason,
                          const GoogleString& detail, const char* mode);
  // Logs once per vhost that a request carried the header IIS URL Rewrite
  // adds to a rewritten request.
  void NoteRewrittenUrlRefused();

  // The cache key of a request: scheme://host[:port]path, the host
  // lowercased and a default port dropped; and that host.
  static void CacheKey(const DaemonHtmlRequestFacts& facts, GoogleString* url,
                       GoogleString* hostname);

  // The volume handle for one operation (Substrate::cache_source).
  void* Cache() const;

  // The serving conditions.
  void ReadConditions(bool log_change);
  // Logs the not-serving reason once per state.
  void NoteUnavailable();
  // The 1.15 set: read with the serving conditions, and its absence logged
  // once per state for a configuration that stores and serves.
  void ReadServe15Condition(bool log_change);
  void NoteServe15Unavailable();
  // Logs once per vhost that a request carried an X-Forwarded-Proto that is
  // not the scheme this module keys on.
  void NoteForwardedProtoMismatch();
  // The mode name a decision line carries for a store or serve.
  const char* ServeModeName() const;
  void ObservePurgeState();
  void WriteEpoch(void* cache, uint64 epoch_s, bool* all_ok);
  void ReadSiteEpoch();

  // The two views and the store path.
  void FinishRecording(DaemonHtmlRecording* recording, bool aborted);
  bool Classify(const DaemonHtmlRecording& r, const DaemonHtmlHeaderLines& view,
                int mode, bool body_complete, StoreVerdict* out);
  // The report of one recording: the counters, and a decision line.
  void Report(const DaemonHtmlRecording& r, const StoreVerdict& verdict,
              bool classified, const DaemonHtmlHeaderLines& store_view,
              const DaemonHtmlHeaderLines& final_view, bool serve_line);
  // Queues one decision line (absent values left out).  `request_path` is a
  // thread a server's message handler may be called from.
  void QueueLine(const std::vector<std::pair<const char*, GoogleString>>& f,
                 const GoogleString& dedup_key, int64 now_ms,
                 bool request_path);
  void LogServeSide(const char* action, const GoogleString& reason,
                    const GoogleString& host, const GoogleString& path,
                    int status, uint64 size, uint32 lifetime_s,
                    const char* variant, int64 age_s,
                    // nullptr: the mode that runs (ServeModeName).
                    const char* mode = nullptr,
                    // The name a refusal is about (a cookie or a request
                    // header, never a value); written from C API 1.15.
                    const GoogleString& detail = GoogleString());
  void Commit(const DaemonHtmlRequestFacts& facts, const GoogleString& url,
              const GoogleString& hostname, const StoreVerdict& verdict,
              const DaemonHtmlHeaderLines& store_view,
              const std::vector<GoogleString>& age_lines,
              const GoogleString& body, bool agent_request,
              const std::vector<std::pair<GoogleString, GoogleString>>&
                  ignorable_cookies);
  void MaybeRemoveOnRefusal(const DaemonHtmlRequestFacts& facts,
                            const GoogleString& url,
                            const GoogleString& hostname, int status,
                            const StoreVerdict& store,
                            const StoreVerdict& final_view,
                            DaemonHtmlPort* port);
  bool RecentlyRemoved(const GoogleString& key, int64 now_s);
  void NoteRemoved(const GoogleString& key, int64 now_s);
  void ForgetRemoved(const GoogleString& key);
  void TrackUnpaired(const GoogleString& url, const GoogleString& hostname,
                     const GoogleString& scheme);
  void Notify(const DaemonHtmlRequestFacts& facts, const GoogleString& url,
              const GoogleString& hostname, bool agent_request);
  // Asks the worker, once per key, to write the coded copies of the page's
  // Markdown copy (1.16).
  void DemandCompress(const DaemonHtmlRequestFacts& facts,
                      const GoogleString& url, const GoogleString& hostname);
  void CountFallThrough(const GoogleString& reason);
  void Hook(const char* point);

  Config config_;
  Deps deps_;
  DaemonHtmlPurgeOracle oracle_;
  std::unique_ptr<Counters> counters_;
  const DaemonHtmlAgentClassCounters agent_class_counters_;
  std::shared_ptr<DaemonHtmlDecisionLog> log_;
  TestHooks* test_hooks_ = nullptr;

  mutable std::mutex mutex_;
  // The serving conditions: the serve set and the worker's state.
  bool set_bound_ = false;
  bool worker_markdown_ = false;
  bool worker_agent_entitled_ = false;
  GoogleString unavailable_reason_;
  bool unavailable_logged_ = false;
  // The 1.15 set, and why it is absent (empty while bound).
  bool set15_bound_ = false;
  GoogleString set15_reason_;
  bool set15_logged_ = false;
  // The 1.16 set, and the codings the worker publishes
  // (ReadSharedConfigMarkdownCodings; -1 = none, or the set is unbound).
  bool set16_bound_ = false;
  int markdown_codings_ = -1;
  bool xfp_logged_ = false;
  bool rewritten_url_logged_ = false;
  std::atomic<bool> credential_skip_disabled_{false};
  // The newest global purge (seconds) this process wrote as an epoch.
  int64 epoch_written_s_ = -1;
  // The handle epoch_written_s_ was written through.
  void* epoch_cache_ = nullptr;
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

// Copies the serve-cached-html settings (scope, cap, credentialed policy,
// ignore list) from `options` into `config`.
void FillDaemonHtmlServeSettings(const RewriteOptions& options,
                                 DaemonHtmlEngine::Config* config);

// A RewriteOptions::DaemonHtmlIgnoreCookieCheck backed by the optimizer
// library under its default name, loaded for each check and released after
// it: ps_html_ignore_cookie_guard refuses an addition that is `*` alone,
// has fewer than 3 characters before its `*`, or would cover a session
// cookie name.  With no library, or one below C API 1.15, the setting has
// no effect and every addition the syntax check passed is accepted.  A port
// installs it with RewriteOptions::SetDaemonHtmlIgnoreCookieCheck before it
// reads a configuration, so a refused addition fails the configuration and
// its test.
bool DaemonHtmlCheckIgnoreCookieWithLibrary(StringPiece addition,
                                            GoogleString* error);

// Configuration load for a port, after FillDaemonHtmlServeSettings:
// DaemonHtmlEngine::ApplyPolicySettings with the library the port loaded
// (none: nothing to apply).  When it refuses, logs the error naming the
// addition and runs the configuration report-only: nothing is stored or
// served under a policy the library refused.  Returns whether it applied.
// When given, *error receives the error on a refusal (a port that must also
// report it elsewhere, such as the IIS event log).
bool ApplyDaemonHtmlPolicySettingsForPort(const DaemonAbi* abi,
                                          DaemonHtmlEngine::Config* config,
                                          MessageHandler* handler,
                                          GoogleString* error_out = nullptr);

// The engine a server port builds from its DaemonHtmlCache settings, or
// nullptr for off.  Until the port stores and serves, serve-variants runs
// exactly as report-only: the response is never changed and nothing is
// written to the volume.  An empty `log_dir` leaves the counters and logs one
// warning.  `abi` may be null (no optimizer library: every HTML response
// counts as daemon_html_unavailable).  All pointers must outlive the engine.
std::unique_ptr<DaemonHtmlEngine> NewDaemonHtmlEngineForPort(
    RewriteOptions::DaemonHtmlCacheMode mode, int64 max_bytes,
    const GoogleString& log_dir, const DaemonAbi* abi, Statistics* statistics,
    Timer* timer, MessageHandler* handler);

}  // namespace net_instaweb

#endif  // PAGESPEED_SYSTEM_DAEMON_HTML_ENGINE_H_
