// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_SRC_NGINX_CACHE_GENERATION_CHECK_H_
#define PAGESPEED_SRC_NGINX_CACHE_GENERATION_CHECK_H_

// Cache-directory generation check between the nginx module and the
// optimizer it shares a cache volume with.  Pure C++ (no nginx headers) so
// the decision and the log-once bookkeeping are unit-testable without an
// nginx build (same pattern as authz_cache_gate / etag_util).
//
// WHAT IT GUARDS.  The optimizer publishes the cache-directory generation it
// was compiled with (kCacheDirGeneration, src/worker/shared_config.h) as
// `cache_dir_generation=` in pagespeed-shared.conf — the same file, and the
// same key, the mod_pagespeed 1.1 daemon adapter checks through
// ps_read_shared_config_generation.  A generation changes when the on-disk
// cache format does, and Cyclone carries the format in the volume FILENAME,
// so a module and an optimizer of different generations pointed at the same
// directory open two different volume files: everything works, nothing is
// shared, and nothing says so.
//
// WHAT A MISMATCH DOES: the module runs with the shared cache DISABLED —
// requests pass through to the origin, and nothing is read from the cache,
// recorded to it, or announced to the optimizer.  It does NOT refuse to
// start (a transient skew during a rolling upgrade is normal, and serving is
// the module's job), and it does NOT keep using a volume of its own:
//   * its own volume is one the optimizer never writes and never purges, so
//     a purge issued to the optimizer would not reach entries the module
//     keeps serving from it — pass-through cannot serve stale content;
//   * it would create a second, full-size volume in the optimizer's cache
//     directory, and notify the optimizer about URLs the optimizer then
//     looks up in its OWN volume and does not find;
//   * pass-through touches no file in that directory, so it cannot damage
//     the other side's cache either.
// The verdict is re-evaluated whenever the shared config changes (the
// module's existing ~1s mtime poll), so a completed rolling upgrade
// re-enables the cache without an nginx reload.
//
// WHAT "UNKNOWN" DOES: an optimizer that states no generation (it predates
// the field, the shared config is missing or unreadable, or its schema
// version is one this module cannot read) keeps today's behaviour — the
// cache is used exactly as before.  Mixed deployments that worked must keep
// working; the module logs a warning once and says it could not verify.

#include <cstdint>
#include <string>
#include <string_view>

namespace pagespeed {

// The cache-directory generation this module was compiled for.  The SAME
// constant the optimizer publishes (kCacheDirGeneration), not a copy.
int ModuleCacheDirGeneration();

enum class CacheGenerationState : std::uint8_t {
  kUnknown,   // The optimizer stated no generation.
  kMatch,     // Same generation: the cache is shared.
  kMismatch,  // Different generations: the shared cache is disabled.
};

// "unknown", "match" or "mismatch" — the value of the
// $pagespeed_cache_generation variable.
const char* CacheGenerationStateName(CacheGenerationState state);

struct CacheGenerationVerdict {
  CacheGenerationState state = CacheGenerationState::kUnknown;
  int module_generation = 0;
  // What the optimizer published; 0 = not stated.
  int optimizer_generation = 0;

  // Whether the module may use the shared cache: read, record, notify.
  // Only a proven mismatch turns it off; unknown keeps today's behaviour.
  [[nodiscard]] bool cache_enabled() const {
    return state != CacheGenerationState::kMismatch;
  }

  bool operator==(const CacheGenerationVerdict&) const = default;
};

// optimizer_generation <= 0 means "not stated" (the parser leaves 0 for an
// absent or unparseable key), never a generation to compare against.
CacheGenerationVerdict EvaluateCacheGeneration(int optimizer_generation,
                                               int module_generation);

// The one log line for a verdict.  `shared_config_path` names the file the
// optimizer's generation was read from; `previous` is the state before this
// verdict (a mismatch -> match move is reported as the cache coming back).
// Every line starts with "pagespeed: " like the module's other log lines.
std::string DescribeCacheGenerationVerdict(const CacheGenerationVerdict& v,
                                           CacheGenerationState previous,
                                           std::string_view shared_config_path);

// Tracks the verdict across shared-config re-reads and decides WHEN it is
// worth a log line: once per change of verdict, and once more for each new
// nginx configuration cycle (Rearm), so a reload repeats an unresolved
// mismatch instead of going quiet.  An unchanged re-read — including the one
// every nginx worker process does after fork — logs nothing.
class CacheGenerationMonitor {
 public:
  explicit CacheGenerationMonitor(int module_generation)
      : module_generation_(module_generation) {
    verdict_ = EvaluateCacheGeneration(0, module_generation_);
  }

  // Records the generation from a fresh read of the shared config.  Returns
  // true iff the verdict changed (the first read always counts as a change).
  bool Update(int optimizer_generation);

  // A new configuration cycle (start or reload): the next verdict is logged
  // even if it equals the current one.
  void Rearm() { log_pending_ = true; }

  // Whether the current verdict still has to be logged; MarkLogged() clears
  // it.  Nothing is pending before the first Update().
  [[nodiscard]] bool log_pending() const {
    return has_verdict_ && log_pending_;
  }
  void MarkLogged() {
    log_pending_ = false;
    logged_state_ = verdict_.state;
  }

  // The state last logged — the `previous` to describe a transition from.
  [[nodiscard]] CacheGenerationState logged_state() const {
    return logged_state_;
  }

  [[nodiscard]] bool has_verdict() const { return has_verdict_; }
  [[nodiscard]] const CacheGenerationVerdict& verdict() const {
    return verdict_;
  }
  [[nodiscard]] bool cache_enabled() const { return verdict_.cache_enabled(); }

 private:
  int module_generation_;
  CacheGenerationVerdict verdict_;
  bool has_verdict_ = false;
  bool log_pending_ = true;
  CacheGenerationState logged_state_ = CacheGenerationState::kUnknown;
};

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_NGINX_CACHE_GENERATION_CHECK_H_
