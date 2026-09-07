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

#ifndef PAGESPEED_SYSTEM_DAEMON_ADAPTER_H_
#define PAGESPEED_SYSTEM_DAEMON_ADAPTER_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/system/daemon_abi.h"
#include "pagespeed/system/daemon_health.h"

namespace net_instaweb {

class MessageHandler;

// Whether the server may start at all.  Reserved, deliberately, for exactly
// one class of condition: see StartupCheck.
enum class DaemonStartupStatus {
  kOk,
  kRefuseToStart,
};

// ---------------------------------------------------------------------------
// The adapter's startup half: configuration, the volume-sizing mirror, and
// the health verdict the serving seam reads.
//
// THE PROBLEM THE MIRROR SOLVES.  The daemon derives its cache volume's
// on-disk FILENAME from the volume's geometry, and the size is the geometry
// input an operator sets.  A process that opens the same directory with a
// different size does not collide with the daemon and does not fail: it
// silently CREATES AND USES A DIFFERENT FILE, shares nothing, and runs a
// permanently cold cache, with no error on either side.  Every request works.
// Nothing is ever optimized.  That silence is why this one disagreement is
// worth refusing to start over while every other daemon problem degrades.
//
// WHAT THE MIRROR IS.  The module does not carry a size of its own and does
// not compare against one — a compiled-in constant could only ever agree with
// the daemon's compiled-in default, which is not what the daemon runs on.  The
// module INHERITS the size the daemon publishes, and the mirror is the check
// that inheriting it actually landed on the daemon's file.
//
// WHAT IT NEVER DOES.  While the size is unknown it neither opens the volume
// nor creates one — creating a volume is the failure being guarded against,
// and a guard that authors it while checking is worse than no guard.  Once
// the size IS known the volume is opened, and opening can still create a file
// if the published size disagrees with what is on disk; that case is detected
// after the fact and refuses the start (last row below).
//
// THE STATE MACHINE, in the order Resolve() walks it:
//
//   neither path configured        -> kNotConfigured, start.  Classic path.
//   only one path configured       -> kUnavailable,   start.  Config mistake,
//                                     and a loud one already.
//   library absent / wrong ABI     -> kUnavailable,   start.
//   daemon does not publish a size -> kUnavailable,   start.  An older daemon
//                                     package; the mirror cannot be evaluated,
//                                     so the volume is not touched.
//   generation published and NOT
//     this build's                 -> kUnavailable,   start.  The versioned
//                                     cold-start cache directories (vN) share
//                                     nothing across N, so a skew is a loud
//                                     handshake failure, never a silent
//                                     split-brain; nothing is opened.
//   generation unknown (0)         -> proceed, and NOTE once on the healthy
//                                     path.  A pre-H1 daemon publishes no
//                                     cache_dir_generation; that is the legacy
//                                     layout, tolerated with the configured
//                                     paths as-is.
//   published size is 0 (unknown)  -> kUnavailable,   start.  No shared config
//                                     (the daemon has never run), unreadable,
//                                     or a schema this build refuses.
//   size known, no volume on disk  -> kUnavailable,   start.  The daemon has
//                                     not created its volume; we must not.
//   size known, >1 volume on disk  -> proceed, and WARN.  Left-over files from
//                                     an earlier cache size are the expected
//                                     steady state after a resize; the daemon
//                                     does not remove the one it stopped
//                                     using.  The published size names exactly
//                                     one of them and the attach check below
//                                     proves we landed on it, so this is
//                                     wasted disk, not a split.
//   size known, opening created
//     a NEW volume file            -> REFUSE TO START.  The published size
//                                     disagrees with what is on disk; we just
//                                     authored the split and say so, naming
//                                     the file to remove.
//   size known, opening attached
//     to an existing file          -> kReady, start.
//
// The single refusal is the volume-sizing mirror and nothing else, which is
// what keeps refuse-to-start narrow: it is the one state that is silent AND
// wrong.  Everything else is loud by construction, and a second way to brick a
// start buys nothing for a failure an operator can already see — least of all
// for one a routine resize produces.
//
// THE RAM TIER is not part of the mirror.  It does not participate in the
// filename derivation, so it cannot split the cache in two.  The adapter
// forces it to zero unconditionally: this module must never opt in to a
// per-process RAM tier over a volume another process writes, because such a
// tier is keyed without any validation against the on-disk entry and would
// keep serving bytes the daemon has already replaced.  Relying on the peer's
// default is not enough — the peer documents that callers may opt in, so "we
// did not opt in" has to be something this module enforces.
// ---------------------------------------------------------------------------
class DaemonAdapter {
 public:
  // Produces a bound client library or nullptr with *error set.  Injectable
  // so the startup logic can be exercised against a peer that is absent,
  // older, mis-sized, or well-behaved, none of which a unit test can install.
  using AbiLoader =
      std::function<DaemonAbi*(StringPiece path, GoogleString* error)>;

  // The RAM tier this module opens the shared volume with.  Not a mirror: a
  // fixed floor.  MUST stay zero.
  static constexpr size_t kMirroredRamCacheSizeBytes = 0;

  // The record-cache open's retry schedule.
  //
  // WHY THERE IS ONE AT ALL.  The open used to be a permanent one-shot: the
  // first failure in a request-serving process turned in-place optimization
  // off for that process's LIFE, and nothing said so again.  The most likely
  // reason to fail is also the most likely to pass a moment later -- a worker
  // that came up while the daemon was still starting, or during a daemon
  // restart -- so the one-shot converted a race lasting seconds into a worker
  // that never records again, silently, while the module keeps serving.  A
  // bounded retry costs one open attempt on the request that crosses each
  // deadline and nothing at all in between.
  //
  // WHY IT IS BOUNDED.  A durable failure (a mis-permissioned volume, a
  // daemon that is simply not installed) must not become one open syscall per
  // request forever, and must not become one log line per request either.
  // Past the bound the condition is not transient and the answer is an
  // operator's, so the arm gives up loudly and stays off.
  //
  // THE BOUND IS TWELVE ATTEMPTS, WHICH IS 363 SECONDS -- a little over six
  // minutes.  Worth spelling out because the total is not the attempt count
  // times anything: eleven waits separate twelve attempts (the last failure
  // gives up rather than scheduling a thirteenth), and the doubling stops at
  // the cap after the seventh, so the series is
  //
  //     1 + 2 + 4 + 8 + 16 + 32 + 60 + 60 + 60 + 60 + 60 = 363s
  //
  // The cap is what makes the count cheap past that point: each further
  // attempt buys a whole minute of tolerance for one open syscall.  Twelve
  // rather than ten because ten stops at 243s, and a daemon restart that
  // takes longer than four minutes -- a slow host, a large volume, a unit
  // that waits on something else first -- would strand the worker for good,
  // which is the exact failure this schedule exists to prevent.  Any change
  // here must update the arithmetic above with it.
  static constexpr int kRecordCacheMaxAttempts = 12;
  static constexpr int64_t kRecordCacheFirstBackoffMs = 1000;
  static constexpr int64_t kRecordCacheMaxBackoffMs = 60000;

  // A source of monotonic milliseconds.  Injectable ONLY so the schedule
  // above can be tested by advancing a number instead of by sleeping out its
  // 363 seconds; production always uses the steady clock.
  using MonotonicClock = std::function<int64_t()>;

  // The cache-directory generation this module is built for: the N in the
  // daemon's versioned cold-start cache directory
  // (/var/cache/pagespeed-optimizer/vN), introduced with the daemon's
  // privilege drop (H1-H3).  Stamped identically into the daemon, which
  // publishes it in its shared config as `cache_dir_generation`.  A daemon
  // publishing a DIFFERENT non-zero generation is a loud handshake failure
  // (substrate down, fail open to plain serving) -- the two layouts share
  // nothing, and a quiet attach would be a split-brain.  A daemon publishing
  // none (a pre-H1 package) is tolerated as the legacy layout; see Resolve.
  //
  // WHEN N MOVES, and why it did NOT move for the cache format major going
  // 6 -> 7 in this train.  The daemon side owns this rule and states it in
  // full; the short form, kept here so the two records agree, is that N
  // counts SHIPPED generations.  The daemon's privilege drop introduced v1
  // and the format major bumped in the SAME unreleased train, so both
  // triggers collapse into the single 0 -> 1 move: no released binary ever
  // resolved a v1 directory, so there is no peer to skew against.  A format
  // major that bumps after this train ships takes N to 2, in both products
  // at once.
  //
  // What that leaves uncovered here, and what covers it instead.  Across a
  // format-major skew at the same N the handshake above passes (1 == 1) and
  // cannot be the thing that catches it.  On THIS side it does not have to
  // be: Resolve counts the volume files around its probe open, and a module
  // that lands on a different format major creates a file rather than
  // attaching to one -- which is a refusal to start, not a quiet split.  So
  // the skew is loud here by a different mechanism.  Do not read that as
  // permission to let N drift from the daemon's: a generation mismatch and a
  // format mismatch are different failures and only one of them is caught
  // twice.
  static constexpr uint32_t kCacheDirGeneration = 1;

  // What stands between this process and the directory the daemon's volume
  // lives in.  After the privilege drop the volume, socket and shared config
  // are group-rw (0660/0640 pagespeed:pagespeed), which makes "the
  // web-server user is not in the `pagespeed` group" the most likely field
  // failure -- and EACCES reads exactly like "the daemon is not there" to a
  // scan that swallows errors, so the two must be told apart explicitly.
  enum class DirAccess { kReadable, kAbsent, kDenied };

  // Whether the directory holding the daemon's volume for `volume_path` can
  // be looked inside: the path itself when it names a directory, its parent
  // otherwise.  kDenied means permission denied (EACCES/EPERM), kAbsent any
  // other failure.  On Windows there is no distinction and this always
  // reports kAbsent.
  static DirAccess VolumeDirAccess(StringPiece volume_path);

  // `socket_path` and `volume_path` come straight from configuration; either
  // or both may be empty, which is the unconfigured state.  Does not take
  // ownership of `handler`.
  DaemonAdapter(StringPiece socket_path, StringPiece volume_path,
                MessageHandler* handler);
  ~DaemonAdapter();

  DaemonAdapter(const DaemonAdapter&) = delete;
  DaemonAdapter& operator=(const DaemonAdapter&) = delete;

  // Resolves health once and returns whether the server may start.
  //
  // Announces at most one message, and announces each DISTINCT condition at
  // most once PER PROCESS — not per adapter.  A server re-reads its
  // configuration during startup and rebuilds its contexts, so a per-object
  // latch would say "exactly one line" and deliver two; the operator
  // instruction in the deployment doc ("read the error log once") depends on
  // the stronger property.  Distinct rather than global so a second virtual
  // host failing for a different reason is still heard.
  DaemonStartupStatus StartupCheck();

  DaemonHealth health() const { return health_; }

  // The bound client library, or nullptr when there is none.  Borrowed.
  const DaemonAbi* abi() const { return abi_.get(); }

  // The shared cache handle this process records through, opening it on first
  // use.  Returns nullptr unless health() is kReady.
  //
  // OPENED HERE, NOT AT STARTUP, and the difference is the fork.  The startup
  // check runs in the server's parent process and closes the volume again
  // precisely so that no child inherits a handle it never asked for.  A
  // request-serving process opens its own, once, and keeps it: the handle is
  // per-process state, and there is no correct way to have made it before the
  // process existed.
  //
  // Thread-safe; at most one open is in flight per adapter however many
  // threads race for it, and a successful handle is opened exactly once.
  //
  // RETRIES ON A BOUNDED BACKOFF rather than latching on the first failure --
  // see kRecordCacheMaxAttempts.  Returns nullptr while the arm is between
  // attempts and after it has given up, so every caller still sees the same
  // two outcomes it always did; nothing blocks and nothing sleeps on a
  // request thread.
  void* RecordCache();

  bool configured() const {
    return !socket_path_.empty() && !volume_path_.empty();
  }

  const GoogleString& socket_path() const { return socket_path_; }
  const GoogleString& volume_path() const { return volume_path_; }

  // Applies this module's fixed RAM-tier floor and the INHERITED volume size
  // to a config the daemon has just initialised.  `inherited_size` must be a
  // size the daemon published; passing 0 is a programming error and is
  // rejected rather than defaulted.
  static bool ApplyInheritedSizing(PsCacheConfig* config,
                                   uint64_t inherited_size);

  // The daemon volume files present for `volume_path`.
  //
  // The daemon names its volume `<stem>-<format>-<geohash>`, so a
  // geometry-sensitive rename shows up as a SECOND file beside the configured
  // stem rather than as an error.  Counting them before and after an open is
  // what turns "did we land on the daemon's file" into an observation instead
  // of an assumption.  Both the stem-prefix and inside-the-directory shapes
  // are scanned because the configured path may be either.
  static std::vector<GoogleString> VolumeFiles(StringPiece volume_path);

  // Overrides the library binder and the library path.  Test seam only.
  void set_abi_loader(AbiLoader loader) { abi_loader_ = std::move(loader); }
  void set_library_path(StringPiece path) { path.CopyToString(&library_path_); }

  // Overrides the retry schedule's clock.  Test seam only.
  void set_monotonic_clock(MonotonicClock clock) {
    monotonic_clock_ = std::move(clock);
  }

  // Forget every condition announced so far.  Tests only: the latch is
  // process-wide by design, and a test binary is one process.
  static void ResetAnnouncementsForTesting();

 private:
  static bool SocketAnswers(StringPiece path, GoogleString* error);

  DaemonStartupStatus Resolve(GoogleString* error);

  // Milliseconds on the process's steady clock.  The production clock.
  static int64_t MonotonicNowMs();

  // How long to wait after `attempts` consecutive failures: the first
  // backoff doubled each time and then capped.
  static int64_t BackoffMsAfter(int attempts);

  GoogleString socket_path_;
  GoogleString volume_path_;
  GoogleString library_path_;
  MessageHandler* handler_;
  AbiLoader abi_loader_;
  std::unique_ptr<DaemonAbi> abi_;
  DaemonHealth health_ = DaemonHealth::kNotConfigured;
  // The size the daemon published, kept from the startup check so the
  // per-process open uses the same one rather than asking again -- a second
  // read could see a different answer and open a different file.
  uint64_t inherited_volume_size_ = 0;
  MonotonicClock monotonic_clock_;
  std::mutex record_cache_mutex_;
  void* record_cache_ = nullptr;
  // The retry schedule's state, all of it guarded by record_cache_mutex_.
  // Per-adapter and therefore per-process, which is the right scope: the
  // handle being opened is per-process state (see RecordCache), so a
  // process that is still racing the daemon's start must not be held off by
  // another one's history.
  int record_cache_attempts_ = 0;
  int64_t record_cache_next_attempt_ms_ = 0;
  bool record_cache_gave_up_ = false;
  // Set when the volume directory holds left-over files from an earlier cache
  // size.  Reported on the healthy path, where nothing else would mention it.
  GoogleString extra_volume_warning_;
  // Set when the daemon publishes no cache_dir_generation: a pre-H1 daemon on
  // the legacy layout.  Reported once on the healthy path, where the
  // tolerance would otherwise be invisible.
  bool legacy_generation_layout_ = false;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_SYSTEM_DAEMON_ADAPTER_H_
