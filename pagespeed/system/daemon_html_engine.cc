// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/system/daemon_html_engine.h"

#include <algorithm>
#include <cstring>

#include "net/instaweb/rewriter/public/rewrite_stats.h"
#include "openssl/sha.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/system/daemon_abi.h"
#include "pagespeed/system/daemon_html_decision_log.h"
#include "pagespeed/system/daemon_notify_params.h"
#include "pagespeed/system/daemon_serve_arm.h"
#include "pagespeed/system/site_cache_fragment.h"

namespace net_instaweb {

namespace {

// ps_html_store_classify's request_flags (PS_API 1.12), hand-copied for the
// reason daemon_abi.h gives.
constexpr uint32_t kStoreRequestHasAuthorization = 1u;
constexpr uint32_t kStoreRequestAuthenticatedUser = 2u;
constexpr uint32_t kStoreRequestHasCookie = 4u;
constexpr uint32_t kStoreRequestNoStore = 8u;

// ps_html_store_classify's out_flags bits carried into the stored entry:
// needs-validation from either view, explicit-shared only from both.
constexpr uint32_t kStoreFlagNeedsValidation = 2u;
constexpr uint32_t kStoreFlagExplicitShared = 8u;
// ps_html_store_classify's sidecar-evaluated out flag, and its sidecar
// verdicts, for the decision line.
constexpr uint32_t kStoreFlagSidecarEvaluated = 16u;
constexpr int kSidecarEligible = 0;
constexpr int kSidecarFallThrough = 1;
constexpr int kSidecarNever = 2;

// The library that formats vhost_added (and the serve-side lines).
constexpr int kVhostAddedMinor = 14;

// The Cache-Control bits that widen what a cache may do with an entry: kept
// only when both views carry them.  Every other bit restricts (or only
// records that a line was present) and is kept when either view carries it.
constexpr uint16_t kPermissiveCcFlags =
    kPsCcOriginPublic | kPsCcOriginImmutable | kPsCcOriginSMaxagePresent;

uint16_t MergeCcFlags(uint16_t a, uint16_t b) {
  return static_cast<uint16_t>(((a | b) & ~kPermissiveCcFlags) |
                               (a & b & kPermissiveCcFlags));
}

uint32_t MergeStoreFlags(uint32_t store, uint32_t final_flags) {
  uint32_t out =
      store & ~(kStoreFlagNeedsValidation | kStoreFlagExplicitShared);
  out |= (store | final_flags) & kStoreFlagNeedsValidation;
  out |= store & final_flags & kStoreFlagExplicitShared;
  return out;
}

// What the body classifier scans; the rest is counted, not kept twice.
constexpr size_t kBodyPrefixBytes = size_t{64} * 1024;

// The refusal-removal set: keys, and how long a key stays in it.
constexpr size_t kRecentlyRemovedMax = 1024;
constexpr int64 kRecentlyRemovedSeconds = 60;

// The keys watched for a header block that stays unpaired.
constexpr size_t kUnpairedMax = 1024;

constexpr char kAfterCommit[] = "after_commit";
constexpr char kAfterRecheck[] = "after_recheck";

// The response-side refusals that remove a stored entry (a 200 the origin
// now marks as not storable).  Everything else -- a status other than 404 or
// 410, a request-side reason, a lifetime or content rule -- leaves the entry.
const char* const kRemovingReasons[] = {"private", "no_store", "set_cookie",
                                        "vary", "headers"};

// The response lines whose value a stored entry represents itself, or that
// describe this transfer only: never part of the header block compared by
// the re-store skip.  Date, Age and Expires are recomputed per response by
// common server configurations (mod_expires), so an otherwise identical
// response differs in them.  Server and Accept-Ranges are stamped per
// response by the server and never kept in a block.  Cache-Control and
// Content-Type are compared through the stored entry's own fields instead.
const char* const kNotInHeaderBlock[] = {
    "Content-Type",  "Content-Length", "Content-Encoding", "Transfer-Encoding",
    "Cache-Control", "ETag",           "Last-Modified",    "Date",
    "Age",           "Expires",        "Connection",       "Keep-Alive",
    "Server",        "Accept-Ranges"};

// Lines a compressor or the server's framing changes on the way out: not a
// change of the response for the final view.
const char* const kTransportLines[] = {"Content-Encoding", "Content-Length",
                                       "Transfer-Encoding"};

// The lines the server's own configuration can add after the store view and
// still refuse the store with: what decides whether, and for how long, a
// shared cache may keep the page, and what makes the page per-client.  Any
// other line the server adds there is not stored (the header block comes
// from the store view) and is not part of a Markdown answer either, so it
// is left out of the final view the store is classified on.  (Report-only
// classifies the whole final view, as it always has.)
const char* const kFinalViewPolicyLines[] = {
    "Cache-Control",       "Pragma",
    "Expires",             "Date",
    "Age",                 "Set-Cookie",
    "Set-Cookie2",         "Vary",
    "Content-Type",        "Content-Encoding",
    "Content-Range",       "Content-Location",
    "Content-Security-Policy", "Link",
    "WWW-Authenticate",    "Proxy-Authenticate",
    "Authentication-Info", "Proxy-Authentication-Info",
    "Clear-Site-Data",     "Surrogate-Control",
    "CDN-Cache-Control",   "X-Accel-Expires"};

// A policy line by name: one of the above, or any name ending in
// "Cache-Control" (a targeted cache control such as CDN-Cache-Control).
bool IsFinalViewPolicyLine(StringPiece name) {
  const StringPiece kSuffix("Cache-Control");
  if (name.size() >= kSuffix.size() &&
      StringCaseEqual(name.substr(name.size() - kSuffix.size()), kSuffix)) {
    return true;
  }
  for (const char* n : kFinalViewPolicyLines) {
    if (StringCaseEqual(name, n)) return true;
  }
  return false;
}

template <size_t N>
bool NameIn(StringPiece name, const char* const (&names)[N]) {
  for (const char* n : names) {
    if (StringCaseEqual(name, n)) return true;
  }
  return false;
}

template <size_t N>
bool ValueIn(StringPiece value, const char* const (&values)[N]) {
  for (const char* v : values) {
    if (value == v) return true;
  }
  return false;
}

bool SameLine(const std::pair<GoogleString, GoogleString>& a,
              const std::pair<GoogleString, GoogleString>& b) {
  return StringCaseEqual(a.first, b.first) && a.second == b.second;
}

bool IsVary(const std::pair<GoogleString, GoogleString>& line) {
  return StringCaseEqual(line.first, "Vary");
}

// `a` minus `b`, one line for one line, in a's order.
DaemonHtmlHeaderLines Minus(const DaemonHtmlHeaderLines& a,
                            const DaemonHtmlHeaderLines& b) {
  std::vector<bool> used(b.size(), false);
  DaemonHtmlHeaderLines out;
  for (const auto& line : a) {
    bool matched = false;
    for (size_t i = 0; i < b.size(); ++i) {
      if (!used[i] && SameLine(line, b[i])) {
        used[i] = true;
        matched = true;
        break;
      }
    }
    if (!matched) out.push_back(line);
  }
  return out;
}

DaemonHtmlHeaderLines WithoutVary(const DaemonHtmlHeaderLines& lines) {
  DaemonHtmlHeaderLines out;
  for (const auto& line : lines) {
    if (!IsVary(line)) out.push_back(line);
  }
  return out;
}

// The Vary tokens of every Vary line, in order, each once (case-insensitive).
std::vector<GoogleString> VaryTokens(const DaemonHtmlHeaderLines& lines) {
  std::vector<GoogleString> tokens;
  for (const auto& line : lines) {
    if (!IsVary(line)) continue;
    StringPieceVector parts;
    SplitStringPieceToVector(line.second, ",", &parts, true);
    for (StringPiece part : parts) {
      TrimWhitespace(&part);
      if (part.empty()) continue;
      bool seen = false;
      for (const GoogleString& t : tokens) {
        if (StringCaseEqual(t, part)) {
          seen = true;
          break;
        }
      }
      if (!seen) tokens.emplace_back(part.data(), part.size());
    }
  }
  return tokens;
}

bool HasToken(const std::vector<GoogleString>& tokens, StringPiece token) {
  for (const GoogleString& t : tokens) {
    if (StringCaseEqual(t, token)) return true;
  }
  return false;
}

std::vector<GoogleString> TokensMinus(const std::vector<GoogleString>& a,
                                      const std::vector<GoogleString>& b) {
  std::vector<GoogleString> out;
  for (const GoogleString& t : a) {
    if (!HasToken(b, t)) out.push_back(t);
  }
  return out;
}

// An entity tag with a compressor's "-gzip" suffix taken off.
GoogleString WithoutGzipSuffix(const GoogleString& etag) {
  static const char kSuffix[] = "-gzip\"";
  const size_t n = sizeof(kSuffix) - 1;
  if (etag.size() > n && etag.compare(etag.size() - n, n, kSuffix) == 0) {
    return StrCat(etag.substr(0, etag.size() - n), "\"");
  }
  return etag;
}

// One header view: the origin's lines plus the change from the baseline to
// `at_point`, without the module's own lines; for the final view also
// without transport changes.
DaemonHtmlHeaderLines BuildView(const DaemonHtmlHeaderLines& origin,
                                const DaemonHtmlHeaderLines& baseline,
                                const DaemonHtmlHeaderLines& at_point,
                                const DaemonHtmlHeaderLines& module,
                                bool final_view) {
  // Nothing changed after the baseline: the origin's lines as they came, in
  // their order.
  if (at_point.size() == baseline.size() &&
      std::equal(at_point.begin(), at_point.end(), baseline.begin(),
                 SameLine)) {
    return origin;
  }
  DaemonHtmlHeaderLines added = Minus(
      Minus(WithoutVary(at_point), WithoutVary(baseline)), WithoutVary(module));
  DaemonHtmlHeaderLines removed =
      Minus(WithoutVary(baseline), WithoutVary(at_point));
  if (final_view) {
    auto transport = [](const std::pair<GoogleString, GoogleString>& l) {
      return NameIn(l.first, kTransportLines);
    };
    added.erase(std::remove_if(added.begin(), added.end(), transport),
                added.end());
    removed.erase(std::remove_if(removed.begin(), removed.end(), transport),
                  removed.end());
    // A compressor's "-gzip" entity tag stands for the origin's own.
    DaemonHtmlHeaderLines kept;
    for (const auto& line : added) {
      if (StringCaseEqual(line.first, "ETag")) {
        const GoogleString plain = WithoutGzipSuffix(line.second);
        bool origin_tag = false;
        for (const auto& o : origin) {
          if (StringCaseEqual(o.first, "ETag") && o.second == plain) {
            origin_tag = true;
          }
        }
        if (origin_tag) {
          removed = Minus(removed, {{"ETag", plain}});
          continue;
        }
      }
      kept.push_back(line);
    }
    added.swap(kept);
  }

  DaemonHtmlHeaderLines view = Minus(WithoutVary(origin), removed);
  for (const auto& line : added) view.push_back(line);

  // Vary, compared as a set of tokens.
  const std::vector<GoogleString> origin_tokens = VaryTokens(origin);
  const std::vector<GoogleString> base_tokens = VaryTokens(baseline);
  const std::vector<GoogleString> point_tokens = VaryTokens(at_point);
  const std::vector<GoogleString> module_tokens =
      TokensMinus(VaryTokens(module), origin_tokens);
  std::vector<GoogleString> add_tokens =
      TokensMinus(TokensMinus(point_tokens, base_tokens), module_tokens);
  if (final_view) {
    add_tokens = TokensMinus(add_tokens, {"Accept-Encoding"});
  }
  const std::vector<GoogleString> removed_tokens =
      TokensMinus(base_tokens, point_tokens);
  std::vector<GoogleString> tokens = TokensMinus(origin_tokens, removed_tokens);
  for (const GoogleString& t : add_tokens) {
    if (!HasToken(tokens, t)) tokens.push_back(t);
  }
  if (removed_tokens.empty() && add_tokens.empty()) {
    for (const auto& line : origin) {
      if (IsVary(line)) view.push_back(line);
    }
  } else if (!tokens.empty()) {
    view.emplace_back("Vary", JoinCollection(tokens, ", "));
  }
  return view;
}

// The final view as a store is classified on: without the lines the server
// added after the store view whose names are not policy lines (see
// kFinalViewPolicyLines).
DaemonHtmlHeaderLines FinalViewForStore(
    const DaemonHtmlHeaderLines& final_view,
    const DaemonHtmlHeaderLines& store_view) {
  DaemonHtmlHeaderLines out;
  for (const auto& line : final_view) {
    bool in_store = false;
    for (const auto& s : store_view) {
      if (StringCaseEqual(s.first, line.first)) {
        in_store = true;
        break;
      }
    }
    if (in_store || IsFinalViewPolicyLine(line.first)) {
      out.push_back(line);
    }
  }
  return out;
}

// Whether a request path can name a volume entry: a path that starts with
// "/".  Anything else is neither stored, served nor removed, so a cache key
// always splits back into its parts exactly (VolumeUrl).
bool IsVolumePath(StringPiece path) { return !path.empty() && path[0] == '/'; }

// The URL argument of the volume calls for a cache key
// ("scheme://host[:port]/path?query", built by CacheKey from a path that
// IsVolumePath accepts, and a host with no "/"): its path and query.  The
// volume keys an entry by that, the host name, the scheme and the site, as
// the optimizer worker and this module's other volume readers and writers
// do; the full URL is what the purge fence is checked under.
GoogleString VolumeUrl(const GoogleString& url) {
  const size_t sep = url.find("://");
  if (sep == GoogleString::npos) return url;
  const size_t path = url.find('/', sep + 3);
  return path == GoogleString::npos ? GoogleString() : url.substr(path);
}

// The key of the engine's own per-entry sets: the entry's volume identity,
// so one entry reached through two ports is one key.
GoogleString VolumeIdentity(const GoogleString& url,
                            const GoogleString& hostname,
                            const GoogleString& scheme) {
  return StrCat(scheme, "\n", hostname, "\n", VolumeUrl(url));
}

// The header block the re-store skip compares: what a stored entry does not
// already represent.
DaemonHtmlHeaderLines HeaderBlockOf(const DaemonHtmlHeaderLines& lines) {
  DaemonHtmlHeaderLines out;
  for (const auto& line : lines) {
    if (!NameIn(line.first, kNotInHeaderBlock)) out.push_back(line);
  }
  return out;
}

bool SameMultiset(const DaemonHtmlHeaderLines& a,
                  const DaemonHtmlHeaderLines& b) {
  return a.size() == b.size() && Minus(a, b).empty();
}

// Whether a Cache-Control value carries `directive` as a directive name.
bool HasDirective(StringPiece value, StringPiece directive) {
  StringPieceVector parts;
  SplitStringPieceToVector(value, ",", &parts, true);
  for (StringPiece part : parts) {
    const size_t eq = part.find('=');
    if (eq != StringPiece::npos) part = part.substr(0, eq);
    TrimWhitespace(&part);
    if (StringCaseEqual(part, directive)) return true;
  }
  return false;
}

GoogleString FirstValue(const DaemonHtmlHeaderLines& lines, StringPiece name) {
  for (const auto& line : lines) {
    if (StringCaseEqual(line.first, name)) return line.second;
  }
  return GoogleString();
}

void Sha256(const GoogleString& body, uint8_t out[kPsHtmlHashBytes]) {
  SHA256(reinterpret_cast<const uint8_t*>(body.data()), body.size(), out);
}

bool IsZeroHash(const uint8_t* hash) {
  if (hash == nullptr) return true;
  for (size_t i = 0; i < kPsHtmlHashBytes; ++i) {
    if (hash[i] != 0) return false;
  }
  return true;
}

const char* CStrOrNull(bool present, const GoogleString& value) {
  return present ? value.c_str() : nullptr;
}

void RunOn(DaemonHtmlBlockingRunner* runner, std::function<void()> work) {
  if (runner == nullptr) {
    work();
  } else {
    runner->RunBlocking(DaemonHtmlCommit(std::move(work)));
  }
}

// A byte-budget reservation, released when its last owner goes.
class BudgetHold {
 public:
  BudgetHold(DaemonHtmlByteBudget* budget, int64 bytes)
      : budget_(budget), bytes_(bytes) {}
  ~BudgetHold() {
    if (budget_ != nullptr && bytes_ > 0) budget_->Release(bytes_);
  }
  BudgetHold(const BudgetHold&) = delete;
  BudgetHold& operator=(const BudgetHold&) = delete;

 private:
  DaemonHtmlByteBudget* budget_;
  int64 bytes_;
};

// Frees the read results and the header block of one serve.
class ServeReads {
 public:
  explicit ServeReads(const DaemonAbi* abi) : abi_(abi) {}
  ~ServeReads() {
    if (block != nullptr) abi_->HeaderBlockFree(block);
    for (void* r : {original, markdown, withheld}) {
      if (r != nullptr) abi_->ReadFree(r);
    }
  }
  ServeReads(const ServeReads&) = delete;
  ServeReads& operator=(const ServeReads&) = delete;

  void* original = nullptr;
  void* markdown = nullptr;
  void* withheld = nullptr;
  void* block = nullptr;

 private:
  const DaemonAbi* abi_;
};

}  // namespace

// ---------------------------------------------------------------------------
// DaemonHtmlPurgeOracle

DaemonHtmlPurgeOracle::DaemonHtmlPurgeOracle(
    const DaemonHtmlPurgeSource* source, Timer* timer)
    : source_(source), timer_(timer) {}

int64 DaemonHtmlPurgeOracle::ObserveLocked() {
  if (source_ == nullptr) return clamped_global_ms_;
  const int64 raw = source_->GlobalInvalidationTimestampMs();
  if (raw != last_raw_global_ms_) {
    last_raw_global_ms_ = raw;
    if (raw >= 0) {
      clamped_global_ms_ =
          std::max(clamped_global_ms_, std::min(raw, timer_->NowMs()));
    }
  }
  return clamped_global_ms_;
}

int64 DaemonHtmlPurgeOracle::Observe() {
  std::lock_guard<std::mutex> lock(mutex_);
  ObserveLocked();
  int64 mtime_ms = 0;
  if (source_ != nullptr && source_->FlushFileMtimeMs(&mtime_ms) &&
      mtime_ms != last_raw_flush_ms_) {
    last_raw_flush_ms_ = mtime_ms;
    clamped_global_ms_ =
        std::max(clamped_global_ms_, std::min(mtime_ms, timer_->NowMs()));
  }
  return clamped_global_ms_;
}

void DaemonHtmlPurgeOracle::NoteGlobalPurge(int64 purge_ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  clamped_global_ms_ =
      std::max(clamped_global_ms_, std::min(purge_ms, timer_->NowMs()));
}

void DaemonHtmlPurgeOracle::SetSiteEpoch(bool read_ok, uint64 epoch_s) {
  std::lock_guard<std::mutex> lock(mutex_);
  site_epoch_ok_ = read_ok;
  if (read_ok) site_epoch_s_ = std::max(site_epoch_s_, epoch_s);
}

void DaemonHtmlPurgeOracle::RaiseSiteEpoch(uint64 epoch_s) {
  std::lock_guard<std::mutex> lock(mutex_);
  site_epoch_s_ = std::max(site_epoch_s_, epoch_s);
}

void DaemonHtmlPurgeOracle::NoteUrlPurge(const GoogleString& url,
                                         int64 purge_ms) {
  // A purge time is never later than now (as for the global purge).
  if (timer_ != nullptr) purge_ms = std::min(purge_ms, timer_->NowMs());
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = local_url_purges_.find(url);
  if (it != local_url_purges_.end()) {
    it->second = std::max(it->second, purge_ms);
    return;
  }
  if (local_url_purges_.size() >= kLocalUrlPurgesMax) {
    auto oldest = local_url_purges_.begin();
    for (auto i = local_url_purges_.begin(); i != local_url_purges_.end();
         ++i) {
      if (i->second < oldest->second) oldest = i;
    }
    // The purge set may not have polled the evicted purge in yet: every
    // entry stored at or before it stays fenced, whatever its URL.  The
    // evicted purge is the oldest remembered, so this covers nothing newer
    // than a purge still in the map.
    local_url_floor_ms_ = std::max(local_url_floor_ms_, oldest->second);
    local_url_purges_.erase(oldest);
  }
  local_url_purges_[url] = purge_ms;
}

bool DaemonHtmlPurgeOracle::Purged(const GoogleString& url,
                                   uint32 stored_at_s) {
  std::lock_guard<std::mutex> lock(mutex_);
  const int64 global_ms = ObserveLocked();
  if (!site_epoch_ok_) return true;
  if (stored_at_s <= site_epoch_s_) return true;
  const int64 stored_ms = static_cast<int64>(stored_at_s) * Timer::kSecondMs;
  if (global_ms >= 0 && stored_ms <= global_ms) return true;
  if (stored_ms <= local_url_floor_ms_) return true;
  const auto local = local_url_purges_.find(url);
  if (local != local_url_purges_.end() && stored_ms <= local->second) {
    return true;
  }
  return source_ != nullptr &&
         !source_->IsUrlValidIgnoringGlobal(url, stored_ms);
}

// ---------------------------------------------------------------------------
// DaemonHtmlByteBudget

bool DaemonHtmlByteBudget::TryReserve(int64 bytes) {
  const int64 capacity = capacity_.load(std::memory_order_relaxed);
  int64 used = in_use_.load(std::memory_order_relaxed);
  do {
    if (used + bytes > capacity) return false;
  } while (!in_use_.compare_exchange_weak(used, used + bytes,
                                          std::memory_order_relaxed));
  return true;
}

DaemonHtmlByteBudget* ProcessDaemonHtmlByteBudget() {
  static DaemonHtmlByteBudget* const budget = new DaemonHtmlByteBudget;
  return budget;
}

void DaemonHtmlByteBudget::Release(int64 bytes) {
  in_use_.fetch_sub(bytes, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Counters

struct DaemonHtmlEngine::Counters {
  explicit Counters(Statistics* s)
      : stored(s->GetVariable("daemon_html_stored")),
        refused_budget(s->GetVariable("daemon_html_refused_budget")),
        refused_url_disagree(
            s->GetVariable("daemon_html_refused_url_disagree")),
        sidecar_write_failed(
            s->GetVariable("daemon_html_sidecar_write_failed")),
        sidecar_unpaired_stale(
            s->GetVariable("daemon_html_sidecar_unpaired_stale")),
        purge_race_refused(s->GetVariable("daemon_html_purge_race_refused")),
        removed(s->GetVariable("daemon_html_removed")),
        removed_on_refusal(s->GetVariable("daemon_html_removed_on_refusal")),
        restore_skipped(s->GetVariable("daemon_html_restore_skipped")),
        epoch_write_failed(s->GetVariable("daemon_html_epoch_write_failed")),
        purge_remove_failed(s->GetVariable("daemon_html_purge_remove_failed")),
        serve_unavailable(s->GetVariable("daemon_html_serve_unavailable")),
        notify_sent(s->GetVariable("daemon_html_notify_sent")),
        notify_limited(s->GetVariable("daemon_html_notify_limited")),
        markdown_served(s->GetVariable("daemon_markdown_served")),
        markdown_not_modified(s->GetVariable("daemon_markdown_not_modified")),
        served_without_preference(
            s->GetVariable("daemon_markdown_served_without_preference")),
        transport_conflict(
            s->GetVariable("daemon_markdown_transport_conflict")),
        evaluated(s->GetVariable("daemon_html_evaluated")),
        would_store(s->GetVariable("daemon_html_would_store")),
        would_store_no_notify(
            s->GetVariable("daemon_html_would_store_no_notify")),
        would_store_bytes(s->GetVariable("daemon_html_would_store_bytes")),
        refused(s->GetVariable("daemon_html_refused")),
        refused_other(s->GetVariable("daemon_html_refused_other")),
        unavailable(s->GetVariable("daemon_html_unavailable")),
        classify_failed(s->GetVariable("daemon_html_classify_failed")),
        log_dropped(s->GetVariable("daemon_html_log_dropped")) {
    for (const char* const* reason =
             RewriteStats::kDaemonMarkdownFallthroughReasons;
         *reason != nullptr; ++reason) {
      fallthrough[*reason] =
          s->GetVariable(StrCat("daemon_markdown_fallthrough_", *reason));
    }
    for (const char* const* reason = RewriteStats::kDaemonHtmlRefusedReasons;
         *reason != nullptr; ++reason) {
      refused_by_reason[*reason] =
          s->GetVariable(StrCat("daemon_html_refused_", *reason));
    }
  }

  Variable* stored;
  Variable* refused_budget;
  Variable* refused_url_disagree;
  Variable* sidecar_write_failed;
  Variable* sidecar_unpaired_stale;
  Variable* purge_race_refused;
  Variable* removed;
  Variable* removed_on_refusal;
  Variable* restore_skipped;
  Variable* epoch_write_failed;
  Variable* purge_remove_failed;
  Variable* serve_unavailable;
  Variable* notify_sent;
  Variable* notify_limited;
  Variable* markdown_served;
  Variable* markdown_not_modified;
  Variable* served_without_preference;
  Variable* transport_conflict;
  std::map<GoogleString, Variable*> fallthrough;
  // The report's, in every mode.
  Variable* evaluated;
  Variable* would_store;
  Variable* would_store_no_notify;
  Variable* would_store_bytes;
  Variable* refused;
  Variable* refused_other;
  Variable* unavailable;
  Variable* classify_failed;
  Variable* log_dropped;
  std::map<GoogleString, Variable*> refused_by_reason;
};

// One view's classification.
struct DaemonHtmlEngine::StoreVerdict {
  int action = kPsHtmlStoreActionRefuse;
  GoogleString reason;
  GoogleString detail;
  uint32_t lifetime_s = 0;
  uint32_t flags = 0;
  uint16_t cc_flags = 0;
  int sidecar = 0;
  GoogleString content_type;

  bool stores() const {
    return action == kPsHtmlStoreActionStore ||
           action == kPsHtmlStoreActionStoreNoNotify;
  }
  // A store or, in report-only, a would-store.
  bool would_store() const {
    return stores() || action == kPsHtmlStoreActionWouldStore ||
           action == kPsHtmlStoreActionWouldStoreNoNotify;
  }
  bool no_notify() const {
    return action == kPsHtmlStoreActionStoreNoNotify ||
           action == kPsHtmlStoreActionWouldStoreNoNotify;
  }
};

// ---------------------------------------------------------------------------
// DaemonHtmlRecording

DaemonHtmlRecording::DaemonHtmlRecording(DaemonHtmlEngine* engine,
                                         const DaemonHtmlRequestFacts& facts,
                                         DaemonHtmlPort* port, bool may_store)
    : engine_(engine), facts_(facts), port_(port), may_store_(may_store) {}

DaemonHtmlRecording::~DaemonHtmlRecording() { ReleaseBudget(); }

void DaemonHtmlRecording::ReleaseBudget() {
  if (reserved_ > 0 && engine_->deps_.budget != nullptr) {
    engine_->deps_.budget->Release(reserved_);
  }
  reserved_ = 0;
}

void DaemonHtmlRecording::SetOriginHeaders(DaemonHtmlHeaderLines lines) {
  origin_ = std::move(lines);
}

void DaemonHtmlRecording::SetBaselineHeaders(DaemonHtmlHeaderLines lines) {
  baseline_ = std::move(lines);
  has_baseline_ = true;
}

void DaemonHtmlRecording::SetModuleHeaders(DaemonHtmlHeaderLines lines) {
  module_ = std::move(lines);
}

void DaemonHtmlRecording::NoteStoreHeaders(DaemonHtmlHeaderLines lines) {
  store_lines_ = std::move(lines);
  has_store_lines_ = true;
}

void DaemonHtmlRecording::NoteFinalHeaders(DaemonHtmlHeaderLines lines) {
  if (has_final_lines_) return;  // Only the first counts.
  final_lines_ = std::move(lines);
  has_final_lines_ = true;
}

void DaemonHtmlRecording::AddBody(const char* data, size_t size) {
  if (finished_ || size == 0) return;
  body_bytes_ += size;
  if (!may_store_) {
    // Only classified, never stored: the prefix the body scan reads is all
    // that is kept, and it is not held against the budget.
    if (body_.size() < kBodyPrefixBytes) {
      body_.append(data, std::min(size, kBodyPrefixBytes - body_.size()));
    }
    return;
  }
  if (budget_refused_ || over_size_) return;
  const int64 max_bytes = engine_->config_.max_bytes;
  size_t take = size;
  if (static_cast<int64>(body_.size() + size) > max_bytes) {
    // Over the size cap: the library refuses it by the byte count; keep
    // only what the body scan needs.
    over_size_ = true;
    take = body_.size() < kBodyPrefixBytes
               ? std::min(size, kBodyPrefixBytes - body_.size())
               : 0;
  }
  if (take == 0) return;
  DaemonHtmlByteBudget* budget = engine_->deps_.budget;
  if (budget != nullptr && !budget->TryReserve(static_cast<int64>(take))) {
    budget_refused_ = true;
    ReleaseBudget();
    GoogleString().swap(body_);
    return;
  }
  reserved_ += static_cast<int64>(take);
  body_.append(data, take);
}

void DaemonHtmlRecording::Finish(bool aborted) {
  if (finished_) return;
  finished_ = true;
  engine_->FinishRecording(this, aborted);
  ReleaseBudget();
}

// ---------------------------------------------------------------------------
// DaemonHtmlEngine

DaemonHtmlEngine::DaemonHtmlEngine(Config config, Deps deps)
    : config_(std::move(config)),
      deps_(std::move(deps)),
      oracle_(deps_.purge_source, deps_.timer),
      counters_(new Counters(deps_.statistics)) {
  if (!deps_.log_dir.empty()) {
    log_ = DaemonHtmlDecisionLog::ForLogDir(deps_.log_dir);
  }
  if (config_.epoch_sites.empty()) {
    config_.epoch_sites.push_back(config_.site);
  }
  ReadConditions(/*log_change=*/false);
  ReadSiteEpoch();
  ObservePurgeState();
}

DaemonHtmlEngine::~DaemonHtmlEngine() = default;

void* DaemonHtmlEngine::Cache() const {
  if (deps_.substrate.cache_source) return deps_.substrate.cache_source();
  return deps_.substrate.cache;
}

bool DaemonHtmlEngine::serving() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return set_bound_ && worker_markdown_ && worker_agent_entitled_;
}

void DaemonHtmlEngine::CacheKey(const DaemonHtmlRequestFacts& facts,
                                GoogleString* url, GoogleString* hostname) {
  GoogleString host;
  for (const char c : facts.host) host.push_back(LowerChar(c));
  GoogleString port;
  if (!host.empty() && host[0] != '[') {
    const size_t colon = host.rfind(':');
    if (colon != GoogleString::npos) {
      port = host.substr(colon);
      host.resize(colon);
    }
  } else {
    const size_t close = host.find(']');
    if (close != GoogleString::npos && close + 1 < host.size()) {
      port = host.substr(close + 1);
      host.resize(close + 1);
    }
  }
  if ((facts.scheme == "https" && port == ":443") ||
      (facts.scheme == "http" && port == ":80")) {
    port.clear();
  }
  *hostname = host;
  *url = StrCat(facts.scheme, "://", host, port, facts.path);
}

void DaemonHtmlEngine::ReadConditions(bool log_change) {
  const DaemonAbi* abi = deps_.substrate.abi;
  const bool set = abi != nullptr && abi->HasHtmlServeSet();
  bool markdown = false;
  bool entitled = false;
  GoogleString reason;
  if (!set) {
    reason = abi == nullptr ? GoogleString("no optimizer library is loaded")
                            : abi->HtmlServeSetUnavailableReason();
  } else if (config_.mode < DaemonHtmlMode::kServeVariants ||
             !config_.agent_optimize) {
    // Nothing to serve here: the worker's state is not read at all.
    reason = "this configuration does not serve";
  } else {
    const char* path = deps_.substrate.cache_path.c_str();
    const int mode = abi->ReadSharedConfigHtmlMode(path);
    markdown = mode == kPsHtmlModeMarkdownStatic ||
               mode == kPsHtmlModeMarkdownRendered;
    entitled = abi->ReadSharedConfigAgentEntitled(path) == 1;
    if (!markdown) {
      reason =
          "the optimizer worker's html_mode is not markdown-static or "
          "markdown-rendered";
    } else if (!entitled) {
      reason = "the optimizer worker reports agent-optimize off";
    }
  }
  bool log = false;
  bool now_serving = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool was_serving =
        set_bound_ && worker_markdown_ && worker_agent_entitled_;
    now_serving = set && markdown && entitled;
    const bool changed =
        was_serving != now_serving || reason != unavailable_reason_;
    set_bound_ = set;
    worker_markdown_ = markdown;
    worker_agent_entitled_ = entitled;
    unavailable_reason_ = reason;
    if (changed) {
      unavailable_logged_ = false;
      if (log_change) {
        log = true;
        unavailable_logged_ = !now_serving;
      }
    }
  }
  if (log && deps_.handler != nullptr) {
    if (now_serving) {
      deps_.handler->Message(
          kInfo,
          "DaemonHtmlCache: the optimizer library and worker allow serving; "
          "storing pages and serving their Markdown copies");
    } else {
      deps_.handler->Message(
          kWarning,
          "DaemonHtmlCache: serving is unavailable (%s); running "
          "report-only",
          reason.c_str());
    }
  }
}

void DaemonHtmlEngine::NoteUnavailable() {
  GoogleString reason;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (unavailable_logged_) return;
    unavailable_logged_ = true;
    reason = unavailable_reason_;
  }
  if (deps_.handler != nullptr) {
    deps_.handler->Message(
        kWarning,
        "DaemonHtmlCache: serving is unavailable (%s); running report-only",
        reason.c_str());
  }
}

void DaemonHtmlEngine::ReadSiteEpoch() {
  const DaemonAbi* abi = deps_.substrate.abi;
  bool bound = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    bound = set_bound_;
  }
  if (!bound) return;
  void* cache = Cache();
  if (cache == nullptr) return;
  uint64_t epoch_s = 0;
  const int rc =
      abi->CacheReadSitePurgeEpoch(cache, config_.site.c_str(), &epoch_s);
  oracle_.SetSiteEpoch(rc == kPsOk, rc == kPsOk ? epoch_s : 0);
}

void DaemonHtmlEngine::WriteEpoch(void* cache, uint64 epoch_s, bool* all_ok) {
  const DaemonAbi* abi = deps_.substrate.abi;
  for (const GoogleString& site : config_.epoch_sites) {
    const int rc = abi->CacheWriteSitePurgeEpoch(cache, site.c_str(), epoch_s);
    if (rc != kPsOk) {
      *all_ok = false;
      counters_->epoch_write_failed->Add(1);
    }
  }
}

void DaemonHtmlEngine::ObservePurgeState() {
  const int64 global_ms = oracle_.Observe();
  if (global_ms < 0) return;
  bool bound = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    bound = set_bound_;
  }
  if (!bound || config_.mode < DaemonHtmlMode::kServeVariants) {
    return;
  }
  void* cache = Cache();
  if (cache == nullptr) return;
  {
    // A replaced volume has none of the epochs written to the one before.
    std::lock_guard<std::mutex> lock(mutex_);
    if (cache != epoch_cache_) {
      epoch_cache_ = cache;
      epoch_written_s_ = -1;
    }
    if (global_ms / Timer::kSecondMs <= epoch_written_s_) return;
  }
  const uint64 epoch_s = static_cast<uint64>(global_ms / Timer::kSecondMs);
  RunOn(deps_.runner, [this, cache, epoch_s] {
    bool all_ok = true;
    WriteEpoch(cache, epoch_s, &all_ok);
    if (all_ok) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        epoch_written_s_ =
            std::max(epoch_written_s_, static_cast<int64>(epoch_s));
      }
      oracle_.RaiseSiteEpoch(epoch_s);
    }
  });
}

void DaemonHtmlEngine::OnPurgePollTick() {
  ReadConditions(/*log_change=*/true);
  ObservePurgeState();
}

void DaemonHtmlEngine::OnStatisticsTick() {
  ObservePurgeState();
  ReadSiteEpoch();
  std::map<GoogleString, Unpaired> watched;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!set_bound_) return;
    watched = unpaired_;
  }
  if (watched.empty()) return;
  void* cache = Cache();
  if (cache == nullptr) return;
  RunOn(deps_.runner, [this, cache, watched] {
    const DaemonAbi* abi = deps_.substrate.abi;
    const int64 now_s = deps_.timer->NowMs() / Timer::kSecondMs;
    for (const auto& [identity, key] : watched) {
      const GoogleString& url = key.url;
      ServeReads reads(abi);
      bool done = true;
      if (abi->CacheReadAlternateSite(cache, VolumeUrl(url).c_str(),
                                      key.hostname.c_str(), key.scheme.c_str(),
                                      config_.site.c_str(), kPsSentinelOriginal,
                                      &reads.original) != kPsOk) {
        reads.original = nullptr;
      }
      if (reads.original != nullptr &&
          abi->HeadersSidecarRead(cache, VolumeUrl(url).c_str(),
                                  key.hostname.c_str(), key.scheme.c_str(),
                                  config_.site.c_str(),
                                  &reads.block) == kPsOk &&
          reads.block != nullptr &&
          abi->HeaderBlockPairing(reads.block,
                                  abi->ReadOriginHtmlHash(reads.original)) ==
              kPsSidecarUnpaired) {
        const int64 stored_at = abi->ReadCacheInsertedAt(reads.original);
        const int64 lifetime = abi->ReadOriginMaxAge(reads.original);
        if (now_s - stored_at > lifetime) {
          counters_->sidecar_unpaired_stale->Add(1);
        } else {
          done = false;  // Still inside its lifetime: watch it further.
        }
      }
      if (done) {
        std::lock_guard<std::mutex> lock(mutex_);
        unpaired_.erase(identity);
      }
    }
  });
}

void DaemonHtmlEngine::TrackUnpaired(const GoogleString& url,
                                     const GoogleString& hostname,
                                     const GoogleString& scheme) {
  const GoogleString key = VolumeIdentity(url, hostname, scheme);
  std::lock_guard<std::mutex> lock(mutex_);
  if (unpaired_.size() >= kUnpairedMax && unpaired_.count(key) == 0) return;
  unpaired_[key] = Unpaired{url, hostname, scheme};
}

bool DaemonHtmlEngine::RecentlyRemoved(const GoogleString& key, int64 now_s) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = recently_removed_.find(key);
  if (it == recently_removed_.end()) return false;
  if (now_s - it->second >= kRecentlyRemovedSeconds || now_s < it->second) {
    recently_removed_.erase(it);
    return false;
  }
  return true;
}

void DaemonHtmlEngine::NoteRemoved(const GoogleString& key, int64 now_s) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (recently_removed_.size() >= kRecentlyRemovedMax) {
    // Drop the expired keys first, then the oldest.
    for (auto it = recently_removed_.begin(); it != recently_removed_.end();) {
      if (now_s - it->second >= kRecentlyRemovedSeconds) {
        it = recently_removed_.erase(it);
      } else {
        ++it;
      }
    }
    if (recently_removed_.size() >= kRecentlyRemovedMax) {
      auto oldest = recently_removed_.begin();
      for (auto it = recently_removed_.begin(); it != recently_removed_.end();
           ++it) {
        if (it->second < oldest->second) oldest = it;
      }
      recently_removed_.erase(oldest);
    }
  }
  recently_removed_[key] = now_s;
}

void DaemonHtmlEngine::ForgetRemoved(const GoogleString& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  recently_removed_.erase(key);
}

void DaemonHtmlEngine::Hook(const char* point) {
  if (test_hooks_ != nullptr) test_hooks_->AtPoint(point);
}

void DaemonHtmlEngine::CountFallThrough(const GoogleString& reason) {
  auto it = counters_->fallthrough.find(reason);
  if (it == counters_->fallthrough.end()) {
    it = counters_->fallthrough.find("other");
  }
  it->second->Add(1);
}

void DaemonHtmlEngine::CountUnavailable() { counters_->unavailable->Add(1); }

std::unique_ptr<DaemonHtmlRecording> DaemonHtmlEngine::StartRecording(
    const DaemonHtmlRequestFacts& facts, DaemonHtmlPort* port,
    bool* unavailable) {
  if (unavailable != nullptr) *unavailable = false;
  if (config_.mode == DaemonHtmlMode::kOff) return nullptr;
  if (deps_.substrate.abi == nullptr) {
    if (unavailable != nullptr) *unavailable = true;
    return nullptr;
  }
  const bool serve_mode =
      config_.mode >= DaemonHtmlMode::kServeVariants && config_.agent_optimize;
  bool may_store = false;
  if (serve_mode) {
    if (serving() && Cache() != nullptr) {
      // Without a port there is nowhere to run the store's blocking work:
      // such a recording only reports.
      may_store = port != nullptr;
    } else {
      counters_->serve_unavailable->Add(1);
      NoteUnavailable();
    }
  }
  if (may_store) {
    // What the engine may store: this site's own requests, on one of its
    // exact names, read the same way by the server and this module.
    if (!IsSiteId(config_.site) || facts.site != config_.site) return nullptr;
    if (!facts.url_agreed) {
      counters_->refused_url_disagree->Add(1);
      LogServeSide("refuse", "url_disagree", facts.host, facts.path, 0, 0, 0,
                   nullptr, -1);
      return nullptr;
    }
    if (!IsVolumePath(facts.path) ||
        !HostExactlyConfigured(facts.host, config_.host_names)) {
      return nullptr;
    }
  } else if (!deps_.substrate.abi->PublishesHtmlStorePolicy()) {
    // A library older than the policy: nothing can be classified.
    if (unavailable != nullptr) *unavailable = true;
    return nullptr;
  }
  return std::unique_ptr<DaemonHtmlRecording>(
      new DaemonHtmlRecording(this, facts, port, may_store));
}

bool DaemonHtmlEngine::Classify(const DaemonHtmlRecording& r,
                                const DaemonHtmlHeaderLines& view, int mode,
                                bool body_complete, StoreVerdict* out) {
  const DaemonAbi* abi = deps_.substrate.abi;
  std::vector<const char*> names;
  std::vector<const char*> values;
  names.reserve(view.size());
  values.reserve(view.size());
  for (const auto& [name, value] : view) {
    names.push_back(name.c_str());
    values.push_back(value.c_str());
  }
  const DaemonHtmlRequestFacts& f = r.facts_;
  uint32_t request_flags = 0;
  if (f.has_authorization) request_flags |= kStoreRequestHasAuthorization;
  if (f.authenticated_user) request_flags |= kStoreRequestAuthenticatedUser;
  if (f.has_cookie) request_flags |= kStoreRequestHasCookie;
  if (f.has_cache_control && HasDirective(f.cache_control, "no-store")) {
    request_flags |= kStoreRequestNoStore;
  }
  int reason = 0;
  char detail[160] = {0};
  size_t detail_len = 0;
  const size_t prefix = std::min(r.body_.size(), kBodyPrefixBytes);
  const int action = abi->HtmlStoreClassify(
      mode, f.method.c_str(), f.path.c_str(), request_flags, r.status_,
      names.data(), values.data(), names.size(), r.body_bytes_,
      body_complete ? 1 : 0, r.body_.data(), prefix,
      static_cast<uint64_t>(std::max<int64>(config_.max_bytes, 0)),
      static_cast<uint32_t>(deps_.timer->NowMs() / Timer::kSecondMs), &reason,
      &out->lifetime_s, &out->flags, &out->cc_flags, &out->sidecar, detail,
      sizeof(detail), &detail_len);
  if (action < 0) return false;
  out->action = action;
  const char* name = abi->HtmlStoreReasonName(reason);
  out->reason = name == nullptr ? "" : name;
  out->detail.assign(detail, std::min(detail_len, sizeof(detail) - 1));
  out->content_type = FirstValue(view, "Content-Type");
  return true;
}

void DaemonHtmlEngine::QueueLine(
    const std::vector<std::pair<const char*, GoogleString>>& fields,
    const GoogleString& dedup_key, int64 now_ms, bool request_path) {
  if (log_ == nullptr) return;
  if (request_path) {
    // What the writer dropped and had to say since the last line.
    const int64 dropped = log_->TakeDropped();
    if (dropped > 0) counters_->log_dropped->Add(dropped);
    if (deps_.handler != nullptr) {
      for (const GoogleString& message : log_->TakeMessages()) {
        deps_.handler->Message(kWarning, "%s", message.c_str());
      }
    }
  }
  if (!log_->Queue(*deps_.substrate.abi, fields, dedup_key, now_ms)) {
    counters_->log_dropped->Add(1);
  }
}

void DaemonHtmlEngine::Report(const DaemonHtmlRecording& r,
                              const StoreVerdict& v, bool classified,
                              const DaemonHtmlHeaderLines& store_view,
                              const DaemonHtmlHeaderLines& final_view,
                              bool serve_line) {
  counters_->evaluated->Add(1);
  if (!classified) {
    counters_->classify_failed->Add(1);
    return;
  }
  const bool would_store = v.would_store();
  if (would_store) {
    counters_->would_store->Add(1);
    counters_->would_store_bytes->Add(static_cast<int64>(r.body_bytes_));
    if (v.no_notify()) counters_->would_store_no_notify->Add(1);
  } else {
    counters_->refused->Add(1);
    auto found = counters_->refused_by_reason.find(v.reason);
    (found == counters_->refused_by_reason.end() ? counters_->refused_other
                                                 : found->second)
        ->Add(1);
  }
  // A store is logged when it is written (Commit).
  if (log_ == nullptr || (serve_line && would_store)) return;

  const DaemonAbi* abi = deps_.substrate.abi;
  const DaemonHtmlRequestFacts& f = r.facts_;
  const GoogleString& bare_reason = v.reason;
  const bool has_detail = !v.detail.empty();
  GoogleString reason_code;
  if (!would_store) {
    reason_code = bare_reason;
    if ((bare_reason == "headers" || bare_reason == "vary") && has_detail) {
      reason_code = StrCat(bare_reason, ":", v.detail);
    }
  }
  GoogleString sidecar_code;
  if ((v.flags & kStoreFlagSidecarEvaluated) != 0) {
    if (v.sidecar == kSidecarEligible) {
      sidecar_code = "eligible_block";
    } else if (v.sidecar == kSidecarFallThrough) {
      sidecar_code = bare_reason == "headers" && has_detail
                         ? StrCat("fall_through:", v.detail)
                         : GoogleString("fall_through");
    } else if (v.sidecar == kSidecarNever) {
      if (bare_reason == "nonce_csp") sidecar_code = "never:nonce_csp";
      if (bare_reason == "nonce_link") sidecar_code = "never:nonce_link";
    }
  }
  GoogleString request;
  if (f.has_authorization) request = "authorization";
  if (f.has_cookie) {
    if (!request.empty()) request.push_back(',');
    request.append("cookie");
  }
  if (request.empty()) request = "none";

  const int64 now_ms = deps_.timer->NowMs();
  const GoogleString path = DaemonHtmlLogPath(f.path);
  const GoogleString action = serve_line ? "refuse"
                              : v.action == kPsHtmlStoreActionWouldStoreNoNotify
                                  ? "would_store_no_notify"
                              : would_store ? "would_store"
                                            : "refuse";
  const bool vhost_added_known = abi->VersionMinor() >= kVhostAddedMinor;
  const DaemonHtmlDecisionLog::Fields fields = {
      {"ts_ms", Integer64ToString(now_ms)},
      {"host", DaemonHtmlIsLogHost(f.host) ? f.host : GoogleString()},
      {"path", path},
      {"mode", serve_line ? "serve-variants" : "report-only"},
      {"action", action},
      {"reason", reason_code},
      {"status", IntegerToString(r.status_)},
      {"size", Integer64ToString(static_cast<int64>(r.body_bytes_))},
      {"lifetime_s", Integer64ToString(v.lifetime_s)},
      {"validators", DaemonHtmlLogValidators(final_view)},
      {"cc", DaemonHtmlLogCc(v.cc_flags, v.flags)},
      {"vary", DaemonHtmlLogVary(final_view)},
      {"sidecar", sidecar_code},
      {"request", request},
      {"personalisation",
       bare_reason == "personalisation_signal" ? v.detail : GoogleString()},
      {"vhost_added", vhost_added_known
                          ? DaemonHtmlVhostAdded(store_view, final_view)
                          : GoogleString()},
  };
  QueueLine(fields,
            StrCat(f.host, "\x1f", path, "\x1f", action, "\x1f", reason_code,
                   "\x1f", sidecar_code),
            now_ms, /*request_path=*/true);
}

void DaemonHtmlEngine::LogServeSide(const char* action,
                                    const GoogleString& reason,
                                    const GoogleString& host,
                                    const GoogleString& path, int status,
                                    uint64 size, uint32 lifetime_s,
                                    const char* variant, int64 age_s) {
  if (log_ == nullptr) return;
  const int64 now_ms = deps_.timer->NowMs();
  const GoogleString log_path = DaemonHtmlLogPath(path);
  const DaemonHtmlDecisionLog::Fields fields = {
      {"ts_ms", Integer64ToString(now_ms)},
      {"host", DaemonHtmlIsLogHost(host) ? host : GoogleString()},
      {"path", log_path},
      {"mode", "serve-variants"},
      {"action", action},
      {"reason", reason},
      {"status", status > 0 ? IntegerToString(status) : GoogleString()},
      {"size",
       size > 0 ? Integer64ToString(static_cast<int64>(size)) : GoogleString()},
      {"lifetime_s",
       lifetime_s > 0 ? Integer64ToString(lifetime_s) : GoogleString()},
      {"variant", variant == nullptr ? GoogleString() : GoogleString(variant)},
      {"age_s", age_s >= 0 ? Integer64ToString(age_s) : GoogleString()},
  };
  // Only the request path takes what the writer has to say; a commit may
  // run on a pool thread.
  QueueLine(fields,
            StrCat(host, "\x1f", log_path, "\x1f", action, "\x1f", reason),
            now_ms, /*request_path=*/false);
}

void DaemonHtmlEngine::FinishRecording(DaemonHtmlRecording* r, bool aborted) {
  const DaemonAbi* abi = deps_.substrate.abi;
  if (abi == nullptr) return;
  if (r->may_store_) {
    if (aborted) return;
    if (r->budget_refused_) {
      counters_->refused_budget->Add(1);
      LogServeSide("refuse", "budget", r->facts_.host, r->facts_.path,
                   r->status_, r->body_bytes_, 0, nullptr, -1);
      return;
    }
  }
  const DaemonHtmlHeaderLines& baseline =
      r->has_baseline_ ? r->baseline_ : r->origin_;
  const DaemonHtmlHeaderLines& store_point =
      r->has_store_lines_ ? r->store_lines_ : baseline;
  const DaemonHtmlHeaderLines& final_point =
      r->has_final_lines_ ? r->final_lines_ : store_point;
  DaemonHtmlHeaderLines store_view =
      BuildView(r->origin_, baseline, store_point, r->module_, false);
  const DaemonHtmlHeaderLines final_view =
      BuildView(r->origin_, baseline, final_point, r->module_, true);

  const int mode =
      r->may_store_ ? kPsHtmlCacheServeVariants : kPsHtmlCacheReportOnly;
  // A report reads whether the body ended; a store needs it whole (an
  // aborted response was never classified for one).
  const bool body_complete = r->may_store_ || (r->body_ended_ && !aborted);
  StoreVerdict store;
  StoreVerdict fin;
  const bool store_ok = Classify(*r, store_view, mode, body_complete, &store);
  const bool final_ok = Classify(
      *r,
      r->may_store_ ? FinalViewForStore(final_view, store_view) : final_view,
      mode, body_complete, &fin);
  if (!r->may_store_) {
    // Report-only: the final view's verdict, the one the report has always
    // read.
    Report(*r, fin, final_ok, store_view, final_view, /*serve_line=*/false);
    return;
  }
  // A body over the cap was not kept whole: never stored, whatever a
  // verdict says (the library refuses it by its byte count), so it is
  // reported as refused for its size.
  if (store_ok && final_ok && r->over_size_ &&
      (store.stores() || fin.stores())) {
    StoreVerdict too_large = fin;
    too_large.action = kPsHtmlStoreActionRefuse;
    too_large.reason = "size";
    too_large.detail.clear();
    Report(*r, too_large, true, store_view, final_view, /*serve_line=*/true);
    return;
  }
  // A store needs both views; the refusal, when there is one, is the view
  // that refused (the store view first).
  Report(*r, store_ok && !store.stores() ? store : fin, store_ok && final_ok,
         store_view, final_view, /*serve_line=*/true);
  if (!store_ok || !final_ok) return;

  GoogleString url;
  GoogleString hostname;
  CacheKey(r->facts_, &url, &hostname);
  if (store.stores() && fin.stores()) {
    StoreVerdict verdict = store;
    if (fin.action == kPsHtmlStoreActionStoreNoNotify) {
      verdict.action = kPsHtmlStoreActionStoreNoNotify;
    }
    verdict.lifetime_s = std::min(store.lifetime_s, fin.lifetime_s);
    // The stored entry takes the more restrictive reading of the two
    // views' cache directives.
    verdict.flags = MergeStoreFlags(store.flags, fin.flags);
    verdict.cc_flags = MergeCcFlags(store.cc_flags, fin.cc_flags);
    const bool agent_request =
        deps_.substrate.abi->WantsAgentMarkdown(
            JoinCollection(r->facts_.accept_lines, ", ").c_str()) == 1;
    // The inbound Age as the origin sent it: a server stage that drops the
    // line does not make the entry younger.
    std::vector<GoogleString> ages;
    for (const auto& [name, value] : r->origin_) {
      if (StringCaseEqual(name, "Age")) ages.push_back(value);
    }
    // The body and its budget move to the commit.  The reservation is owned,
    // so it is released when the commit has run or is dropped unrun.
    std::shared_ptr<BudgetHold> hold =
        std::make_shared<BudgetHold>(deps_.budget, r->reserved_);
    r->reserved_ = 0;
    DaemonHtmlRequestFacts facts = r->facts_;
    GoogleString body;
    body.swap(r->body_);
    r->port_->RunBlocking(DaemonHtmlCommit(
        [this, facts = std::move(facts), url, hostname, verdict,
         store_view = std::move(store_view), ages = std::move(ages),
         body = std::move(body), hold, agent_request] {
          Commit(facts, url, hostname, verdict, store_view, ages, body,
                 agent_request);
        }));
    return;
  }
  MaybeRemoveOnRefusal(r->facts_, url, hostname, r->status_, store, fin,
                       r->port_);
}

void DaemonHtmlEngine::Commit(const DaemonHtmlRequestFacts& facts,
                              const GoogleString& url,
                              const GoogleString& hostname,
                              const StoreVerdict& verdict,
                              const DaemonHtmlHeaderLines& store_view,
                              const std::vector<GoogleString>& ages,
                              const GoogleString& body, bool agent_request) {
  const DaemonAbi* abi = deps_.substrate.abi;
  void* cache = Cache();
  if (cache == nullptr) return;
  const char* site = config_.site.c_str();
  uint8_t hash[kPsHtmlHashBytes];
  Sha256(body, hash);

  // The insert time: computed by the library alone, from the request's
  // start and the inbound Age.  A refused Age stores nothing.
  std::vector<const char*> age_lines;
  age_lines.reserve(ages.size());
  for (const GoogleString& a : ages) age_lines.push_back(a.c_str());
  const GoogleString& content_type = verdict.content_type;
  PsWriteParams params;
  abi->WriteParamsInit(&params);
  if (abi->HtmlStoreWriteParams(
          verdict.action, verdict.lifetime_s, verdict.flags, verdict.cc_flags,
          /*sidecar_stored=*/1, facts.capture_start_ms, age_lines.data(),
          age_lines.size(), body.size(), content_type.c_str(), &params) != 0) {
    return;
  }
  const uint32 inserted_at = params.cache_inserted_at;

  // The re-store skip: only an unchanged, paired, fresh entry with more than
  // half its lifetime left, or a newer one, is kept as it is.
  bool hash_changed = true;
  {
    ServeReads existing(abi);
    if (abi->CacheReadAlternateSite(cache, VolumeUrl(url).c_str(),
                                    hostname.c_str(), facts.scheme.c_str(),
                                    site, kPsSentinelOriginal,
                                    &existing.original) != kPsOk) {
      existing.original = nullptr;
    }
    if (existing.original != nullptr) {
      const uint32 stored_at = abi->ReadCacheInsertedAt(existing.original);
      if (!oracle_.Purged(url, stored_at)) {
        const uint8_t* old_hash = abi->ReadOriginHtmlHash(existing.original);
        const bool same_body = old_hash != nullptr && !IsZeroHash(old_hash) &&
                               memcmp(old_hash, hash, kPsHtmlHashBytes) == 0;
        hash_changed = !same_body;
        // The key is present again (written here or by a peer): a later
        // refusal must read and remove it, whatever this process removed.
        ForgetRemoved(VolumeIdentity(url, hostname, facts.scheme));
        const char* stored_ct = abi->ReadOriginContentType(existing.original);
        bool skip = stored_at > inserted_at;
        if (!skip && same_body &&
            abi->ReadOriginMaxAge(existing.original) == verdict.lifetime_s &&
            abi->ReadOriginCcFlags(existing.original) == verdict.cc_flags &&
            stored_ct != nullptr && verdict.content_type == stored_ct) {
          const int64 capture_s = facts.capture_start_ms / Timer::kSecondMs;
          const int64 age = capture_s - static_cast<int64>(stored_at);
          if (age >= 0 && age * 2 < static_cast<int64>(verdict.lifetime_s) &&
              abi->HeadersSidecarRead(cache, VolumeUrl(url).c_str(),
                                      hostname.c_str(), facts.scheme.c_str(),
                                      site, &existing.block) == kPsOk &&
              existing.block != nullptr &&
              abi->HeaderBlockPairing(existing.block, old_hash) ==
                  kPsSidecarPaired) {
            DaemonHtmlHeaderLines stored_block;
            const size_t n = abi->HeaderBlockCount(existing.block);
            for (size_t i = 0; i < n; ++i) {
              const char* name = nullptr;
              const char* value = nullptr;
              size_t name_len = 0;
              size_t value_len = 0;
              if (abi->HeaderBlockGet(existing.block, i, &name, &name_len,
                                      &value, &value_len) == kPsOk) {
                stored_block.emplace_back(GoogleString(name, name_len),
                                          GoogleString(value, value_len));
              }
            }
            skip = SameMultiset(HeaderBlockOf(stored_block),
                                HeaderBlockOf(store_view));
          }
        }
        if (skip) {
          counters_->restore_skipped->Add(1);
          return;
        }
      }
    }
  }

  // The header block first (a failure stores nothing), then the entry.
  std::vector<const char*> names;
  std::vector<const char*> values;
  names.reserve(store_view.size());
  values.reserve(store_view.size());
  for (const auto& [name, value] : store_view) {
    names.push_back(name.c_str());
    values.push_back(value.c_str());
  }
  PsSidecarWriteParams sidecar = {};
  sidecar.struct_size = sizeof(sidecar);
  const GoogleString volume_url = VolumeUrl(url);
  sidecar.url = volume_url.c_str();
  sidecar.hostname = hostname.c_str();
  sidecar.scheme = facts.scheme.c_str();
  sidecar.site = site;
  sidecar.origin_html_hash = hash;
  int sidecar_verdict = 0;
  int sidecar_stored = 0;
  // A paired write keeps a block even with nothing to carry, so a block
  // that was not kept is a refusal: nothing is stored.
  if (abi->CacheWriteHeadersSidecarSite(
          cache, &sidecar, names.data(), values.data(), names.size(),
          &sidecar_verdict, &sidecar_stored) != kPsOk ||
      sidecar_stored == 0) {
    counters_->sidecar_write_failed->Add(1);
    return;
  }
  params.origin_html_hash = hash;
  void* handle = nullptr;
  if (abi->CacheWriteOriginalSite(cache, VolumeUrl(url).c_str(),
                                  hostname.c_str(), facts.scheme.c_str(), site,
                                  &params, &handle) != kPsOk ||
      handle == nullptr) {
    TrackUnpaired(url, hostname, facts.scheme);
    return;
  }
  if (abi->WriteData(handle, body.data(), body.size()) != kPsOk) {
    abi->WriteAbort(handle);
    TrackUnpaired(url, hostname, facts.scheme);
    return;
  }
  if (abi->WriteClose(handle) != kPsOk) {
    TrackUnpaired(url, hostname, facts.scheme);
    return;
  }
  ForgetRemoved(VolumeIdentity(url, hostname, facts.scheme));
  Hook(kAfterCommit);

  // The fence: a purge that covers the insert time and became visible while
  // the response was on its way removes what was just written.
  if (!oracle_.recheck_disabled() && oracle_.Purged(url, inserted_at)) {
    const int rc =
        abi->CacheRemoveSite(cache, VolumeUrl(url).c_str(), hostname.c_str(),
                             facts.scheme.c_str(), site);
    if (rc == kPsOk) counters_->removed->Add(1);
    counters_->purge_race_refused->Add(1);
    LogServeSide("purged", "purge_race", facts.host, facts.path, 200,
                 body.size(), verdict.lifetime_s, nullptr, -1);
    return;
  }
  Hook(kAfterRecheck);
  counters_->stored->Add(1);
  LogServeSide("stored", GoogleString(), facts.host, facts.path, 200,
               body.size(), verdict.lifetime_s, nullptr, -1);
  if (verdict.action == kPsHtmlStoreActionStore && hash_changed) {
    Notify(facts, url, hostname, agent_request);
  }
}

void DaemonHtmlEngine::MaybeRemoveOnRefusal(
    const DaemonHtmlRequestFacts& facts, const GoogleString& url,
    const GoogleString& hostname, int status, const StoreVerdict& store,
    const StoreVerdict& final_view, DaemonHtmlPort* port) {
  const bool clean_request =
      facts.method == "GET" && facts.url_agreed && !facts.has_authorization &&
      !facts.authenticated_user && !facts.has_cookie &&
      !facts.has_if_none_match && !facts.has_if_modified_since &&
      !facts.has_if_match && !facts.has_if_unmodified_since &&
      !facts.has_if_range && !facts.has_range &&
      !(facts.has_cache_control &&
        HasDirective(facts.cache_control, "no-store"));
  if (!clean_request) return;
  // A response-side refusal in either view removes.
  auto removing = [](const StoreVerdict& v) {
    return !v.stores() && ValueIn(v.reason, kRemovingReasons) &&
           v.detail != "request";
  };
  const bool removing_response =
      (status == 200 && (removing(store) || removing(final_view))) ||
      status == 404 || status == 410;
  if (!removing_response) return;
  const int64 now_s = deps_.timer->NowMs() / Timer::kSecondMs;
  const GoogleString scheme = facts.scheme;
  const GoogleString identity = VolumeIdentity(url, hostname, scheme);
  if (RecentlyRemoved(identity, now_s)) return;
  port->RunBlocking(DaemonHtmlCommit([this, url, hostname, scheme, identity,
                                      now_s] {
    const DaemonAbi* abi = deps_.substrate.abi;
    void* cache = Cache();
    if (cache == nullptr) return;
    const char* site = config_.site.c_str();
    ServeReads existing(abi);
    if (abi->CacheReadAlternateSite(
            cache, VolumeUrl(url).c_str(), hostname.c_str(), scheme.c_str(),
            site, kPsSentinelOriginal, &existing.original) != kPsOk) {
      existing.original = nullptr;
    }
    if (existing.original != nullptr) {
      const int rc =
          abi->CacheRemoveSite(cache, VolumeUrl(url).c_str(), hostname.c_str(),
                               scheme.c_str(), site);
      if (rc != kPsOk && rc != kPsErrNotFound) return;
      if (rc == kPsOk) counters_->removed->Add(1);
      counters_->removed_on_refusal->Add(1);
    }
    NoteRemoved(identity, now_s);
  }));
}

void DaemonHtmlEngine::Notify(const DaemonHtmlRequestFacts& facts,
                              const GoogleString& url,
                              const GoogleString& hostname,
                              bool agent_request) {
  const int64 now_s = deps_.timer->NowMs() / Timer::kSecondMs;
  if (deps_.limiter != nullptr &&
      !deps_.limiter->AdmitAsk(StrCat("html\n", config_.site, "\n", url,
                                      agent_request ? "\nagent" : "\nplain"),
                               now_s)) {
    counters_->notify_limited->Add(1);
    return;
  }
  ModuleNotifyInput input;
  input.url = url;
  input.hostname = hostname;
  input.scheme = facts.scheme;
  input.site = config_.site;
  input.content_type = kPsContentHtml;
  input.option_context = facts.option_context;
  input.option_signature = facts.option_signature;
  input.html_stored = true;
  input.agent_request = agent_request;
  ModuleNotifyParams built;
  if (!BuildModuleNotifyParams(*deps_.substrate.abi, input, &built)) return;
  if (deps_.substrate.abi->NotifyWorker(deps_.substrate.notify_socket.c_str(),
                                        &built.params()) == kPsOk) {
    counters_->notify_sent->Add(1);
  }
}

DaemonHtmlServeResult DaemonHtmlEngine::Serve(
    const DaemonHtmlRequestFacts& facts, DaemonHtmlPort* port) {
  DaemonHtmlServeResult result;
  result.reason = "mode";
  // The serve gate: nothing is read before every part of it holds.
  if (config_.mode < DaemonHtmlMode::kServeVariants ||
      !config_.agent_optimize || !IsSiteId(config_.site) ||
      facts.site != config_.site) {
    return result;
  }
  if (!serving()) {
    NoteUnavailable();
    return result;
  }
  result.reason = "other";
  if (!facts.options_enabled_and_allowed || !IsVolumePath(facts.path) ||
      !HostExactlyConfigured(facts.host, config_.host_names)) {
    return result;
  }
  const DaemonAbi* abi = deps_.substrate.abi;
  const GoogleString accept = JoinCollection(facts.accept_lines, ", ");
  const bool has_accept = !facts.accept_lines.empty();
  if (abi->WantsAgentMarkdown(accept.c_str()) != 1) {
    result.reason = "";  // Not a Markdown request at all.
    return result;
  }
  // Counted only for a Markdown request, so the counter means one.
  if (!facts.url_agreed) {
    result.reason = "url_disagree";
    CountFallThrough(result.reason);
    return result;
  }
  void* cache = Cache();
  if (cache == nullptr) return result;

  GoogleString url;
  GoogleString hostname;
  CacheKey(facts, &url, &hostname);
  const char* site = config_.site.c_str();
  const char* scheme = facts.scheme.c_str();
  ServeReads reads(abi);
  auto read = [&](uint8_t id, void** out) {
    if (abi->CacheReadAlternateSite(cache, VolumeUrl(url).c_str(),
                                    hostname.c_str(), scheme, site, id,
                                    out) != kPsOk) {
      *out = nullptr;
    }
  };
  read(kPsSentinelOriginal, &reads.original);
  read(kPsSentinelAgentMarkdown, &reads.markdown);
  read(kPsSentinelMarkdownWithheld, &reads.withheld);
  bool sidecar_present = false;
  const uint8_t* stamp = reads.original == nullptr
                             ? nullptr
                             : abi->ReadOriginHtmlHash(reads.original);
  if (reads.original != nullptr &&
      abi->HeadersSidecarRead(cache, VolumeUrl(url).c_str(), hostname.c_str(),
                              scheme, site, &reads.block) == kPsOk &&
      reads.block != nullptr) {
    if (abi->HeaderBlockPairing(reads.block, stamp) != kPsSidecarPaired) {
      TrackUnpaired(url, hostname, facts.scheme);
      result.reason = "sidecar_unpaired";
      CountFallThrough(result.reason);
      return result;
    }
    sidecar_present = true;
  }
  PsHtmlStoredFacts stored = {};
  stored.struct_size = sizeof(stored);
  if (abi->HtmlStoredFactsFromReads(reads.original, reads.markdown,
                                    reads.withheld, sidecar_present ? 1 : 0,
                                    &stored) != 0) {
    CountFallThrough(result.reason);
    return result;
  }
  if ((stored.flags & kPsHtmlFactsPresent) != 0 &&
      oracle_.Purged(url, stored.stored_at)) {
    stored.flags |= kPsHtmlFactsPurged;
  }

  const int64 now_s = deps_.timer->NowMs() / Timer::kSecondMs;
  uint32_t request_flags = 0;
  if (facts.has_authorization) {
    request_flags |= kPsHtmlServeRequestHasAuthorization;
  }
  if (facts.authenticated_user) {
    request_flags |= kPsHtmlServeRequestAuthenticatedUser;
  }
  if (facts.has_cookie) request_flags |= kPsHtmlServeRequestHasCookie;
  if (facts.has_range) request_flags |= kPsHtmlServeRequestHasRange;
  if (facts.has_if_range) request_flags |= kPsHtmlServeRequestHasIfRange;
  if (facts.has_origin) request_flags |= kPsHtmlServeRequestHasOrigin;
  PsHtmlServeRequest request = {};
  request.struct_size = sizeof(request);
  request.mode = kPsHtmlCacheServeVariants;
  request.method = facts.method.c_str();
  request.accept = CStrOrNull(has_accept, accept);
  request.cache_control =
      CStrOrNull(facts.has_cache_control, facts.cache_control);
  request.pragma = CStrOrNull(facts.has_pragma, facts.pragma);
  request.if_none_match =
      CStrOrNull(facts.has_if_none_match, facts.if_none_match);
  request.request_flags = request_flags;
  request.now_seconds = static_cast<uint32_t>(now_s);
  PsHtmlServeVerdict verdict = {};
  verdict.struct_size = sizeof(verdict);
  if (abi->HtmlServeClassify(&request, &stored, &verdict) != 0) {
    CountFallThrough(result.reason);
    return result;
  }

  if (verdict.action != kPsHtmlServeActionServeMarkdown) {
    const char* name = abi->HtmlServeReasonName(verdict.reason);
    result.reason = name == nullptr ? "other" : name;
    CountFallThrough(result.reason);
    // The lost copy: a present, fresh page this module's store wrote, with
    // no copy bound to it, asks the worker once per limiter window.
    if ((result.reason == "absent" || result.reason == "binding_stale") &&
        verdict.wants_markdown != 0 && reads.original != nullptr &&
        (stored.flags & kPsHtmlFactsPresent) != 0 &&
        (stored.flags & (kPsHtmlFactsPurged | kPsHtmlFactsWithheld)) == 0 &&
        (abi->ReadFlags(reads.original) & kPsFlagHeadersClassified) != 0 &&
        (abi->ReadOriginCcFlags(reads.original) & kPsCcOriginNoTransform) ==
            0 &&
        abi->ReadContentType(reads.original) == kPsContentHtml &&
        !IsZeroHash(stamp) &&
        now_s - static_cast<int64>(stored.stored_at) <
            static_cast<int64>(stored.lifetime_s)) {
      Notify(facts, url, hostname, /*agent_request=*/true);
    }
    return result;
  }
  if (verdict.wants_markdown == 0) {
    // The library said serve without a preference: a defect, never sent.
    counters_->served_without_preference->Add(1);
    result.reason = "other";
    CountFallThrough(result.reason);
    return result;
  }
  const uint8_t* markdown = nullptr;
  size_t markdown_len = 0;
  if (reads.markdown == nullptr ||
      abi->ReadContent(reads.markdown, &markdown, &markdown_len) != kPsOk ||
      markdown_len == 0) {
    // Never an empty answer for a copy that cannot be read.
    result.reason = "absent";
    CountFallThrough(result.reason);
    return result;
  }

  char etag[kPsHtmlMarkdownEtagChars + 8] = {0};
  size_t etag_len = 0;
  const bool has_etag =
      abi->HtmlMarkdownEtag(stored.binding_hash, stored.markdown_hash, etag,
                            sizeof(etag), &etag_len) == 0 &&
      etag_len > 0;
  const bool not_modified =
      has_etag && facts.has_if_none_match &&
      abi->IfNoneMatchMatchesWeak(facts.if_none_match.c_str(), etag) == 1;

  // One Vary line: the origin's own tokens (from the header block) and
  // Accept.
  std::vector<GoogleString> vary;
  if (reads.block != nullptr) {
    DaemonHtmlHeaderLines block_vary;
    const size_t n = abi->HeaderBlockCount(reads.block);
    for (size_t i = 0; i < n; ++i) {
      const char* name = nullptr;
      const char* value = nullptr;
      size_t name_len = 0;
      size_t value_len = 0;
      if (abi->HeaderBlockGet(reads.block, i, &name, &name_len, &value,
                              &value_len) == kPsOk &&
          StringCaseEqual(StringPiece(name, name_len), "Vary")) {
        block_vary.emplace_back("Vary", GoogleString(value, value_len));
      }
    }
    vary = TokensMinus(VaryTokens(block_vary), {"Accept"});
  }
  vary.push_back("Accept");
  const GoogleString vary_line = JoinCollection(vary, ", ");

  const bool head = facts.method == "HEAD";
  result.kind = DaemonHtmlServeResult::kFinal;
  result.reason = "fresh";
  if (not_modified) {
    result.status = 304;
    result.headers.emplace_back("ETag", etag);
    result.headers.emplace_back("Cache-Control", "private");
    result.headers.emplace_back("Vary", vary_line);
  } else {
    result.status = 200;
    result.headers.emplace_back("Content-Type", "text/markdown; charset=utf-8");
    result.headers.emplace_back("Content-Length",
                                Integer64ToString(markdown_len));
    result.headers.emplace_back("Cache-Control", "private");
    result.headers.emplace_back("Vary", vary_line);
    result.headers.emplace_back("X-Robots-Tag", "noindex");
    result.headers.emplace_back("Age", Integer64ToString(verdict.age_s));
    if (has_etag) result.headers.emplace_back("ETag", etag);
    if (!head) {
      result.body.assign(reinterpret_cast<const char*>(markdown), markdown_len);
    }
  }
  port->MarkNoCompress();
  if (!port->ClearAndEmitAllowlisted(result, head)) {
    DaemonHtmlServeResult fall_through;
    fall_through.reason = "other";
    CountFallThrough(fall_through.reason);
    return fall_through;
  }
  if (not_modified) {
    counters_->markdown_not_modified->Add(1);
  } else {
    counters_->markdown_served->Add(1);
  }
  LogServeSide("served_markdown", "fresh", facts.host, facts.path,
               result.status, not_modified || head ? 0 : markdown_len, 0,
               "markdown", verdict.age_s);
  return result;
}

void DaemonHtmlEngine::NoteTransportConflict() {
  counters_->transport_conflict->Add(1);
}

void DaemonHtmlEngine::OnAdminPurge(StringPiece url, bool global, int64 now_ms,
                                    Done done) {
  bool bound = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    bound = set_bound_;
  }
  if (!bound || config_.mode < DaemonHtmlMode::kServeVariants) {
    done(true, GoogleString());
    return;
  }
  // The key a URL purge names, and the fence for it in this process: noted
  // at once, before anything can fail or wait, so a capture already under
  // way fails its re-check; peer processes see the purge set's record at
  // their next poll.  The fence is per URL as spelled after normalisation
  // (its port included), as the purge set's is; the removal below takes the
  // entry every port of the site shares.
  GoogleString hostname;
  GoogleString cache_url;
  DaemonHtmlRequestFacts parts;
  if (global) {
    oracle_.NoteGlobalPurge(now_ms);
  } else {
    const GoogleString key = DaemonHtmlNormalizePurgeUrl(url);
    const size_t sep = key.find("://");
    if (sep == GoogleString::npos) {
      done(true, GoogleString());
      return;
    }
    parts.scheme = key.substr(0, sep);
    const size_t path_start = key.find_first_of("/?#", sep + 3);
    parts.host = key.substr(sep + 3, path_start == GoogleString::npos
                                         ? GoogleString::npos
                                         : path_start - sep - 3);
    // An empty path is the root ("https://host" names "https://host/").
    parts.path = path_start == GoogleString::npos ? GoogleString("/")
                                                  : key.substr(path_start);
    CacheKey(parts, &cache_url, &hostname);
    oracle_.NoteUrlPurge(cache_url, now_ms);
  }
  void* cache = Cache();
  if (cache == nullptr) {
    // No volume this process can vouch for: nothing is served from one
    // either, and the purge is fenced above.  A process with a handle
    // writes the global epoch on its purge-poll tick.
    done(true, GoogleString());
    return;
  }
  if (global) {
    const uint64 epoch_s = static_cast<uint64>(
        std::min(now_ms, deps_.timer->NowMs()) / Timer::kSecondMs);
    RunOn(deps_.runner, [this, cache, epoch_s, done] {
      bool all_ok = true;
      WriteEpoch(cache, epoch_s, &all_ok);
      if (!all_ok) {
        done(false, "the HTML cache's purge epoch could not be written");
        return;
      }
      {
        std::lock_guard<std::mutex> lock(mutex_);
        epoch_written_s_ =
            std::max(epoch_written_s_, static_cast<int64>(epoch_s));
      }
      oracle_.RaiseSiteEpoch(epoch_s);
      done(true, GoogleString());
    });
    return;
  }
  if (!IsVolumePath(parts.path)) {
    // Names no volume entry; the fence above still holds.
    done(true, GoogleString());
    return;
  }
  RunOn(deps_.runner, [this, cache, cache_url, hostname, host = parts.host,
                       path = parts.path, scheme = parts.scheme, done] {
    const int rc = deps_.substrate.abi->CacheRemoveSite(
        cache, VolumeUrl(cache_url).c_str(), hostname.c_str(), scheme.c_str(),
        config_.site.c_str());
    if (rc == kPsOk || rc == kPsErrNotFound) {
      if (rc == kPsOk) {
        counters_->removed->Add(1);
        LogServeSide("purged", GoogleString(), host, path, 0, 0, 0, nullptr,
                     -1);
      }
      done(true, GoogleString());
      return;
    }
    counters_->purge_remove_failed->Add(1);
    done(false, "the HTML cache's stored copy could not be removed");
  });
}

std::unique_ptr<DaemonHtmlEngine> NewDaemonHtmlEngineForPort(
    RewriteOptions::DaemonHtmlCacheMode mode, int64 max_bytes,
    const GoogleString& log_dir, const DaemonAbi* abi, Statistics* statistics,
    Timer* timer, MessageHandler* handler) {
  if (mode == RewriteOptions::kDaemonHtmlCacheOff) return nullptr;
  if (log_dir.empty()) {
    handler->Message(kWarning,
                     "DaemonHtmlCache decision log is off: LogDir is not set");
  }
  DaemonHtmlEngine::Config config;
  // No port stores or serves yet: serve-variants reports, as report-only.
  config.mode = DaemonHtmlMode::kReportOnly;
  config.max_bytes = max_bytes;
  DaemonHtmlEngine::Deps deps;
  deps.substrate.abi = abi;
  deps.statistics = statistics;
  deps.timer = timer;
  deps.handler = handler;
  deps.log_dir = log_dir;
  return std::make_unique<DaemonHtmlEngine>(std::move(config), std::move(deps));
}

}  // namespace net_instaweb
