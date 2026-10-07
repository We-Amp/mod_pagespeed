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

#include <atomic>
#include <cstddef>
#include <cstdint>

#ifndef _WIN32
#include <sys/types.h>  // uid_t
#endif
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
  //
  // N = 2: the cache format major went 7 -> 8 (CRC-32C) after generation 1
  // shipped, so the daemon moved to /var/cache/pagespeed-optimizer/v2 and
  // publishes cache_dir_generation=2.  This moves with it, in lockstep.
  static constexpr uint32_t kCacheDirGeneration = 2;

  // What stands between this process and the directory the daemon's volume
  // lives in.  After the privilege drop the volume, socket and shared config
  // are group-rw (0660/0640 pagespeed:pagespeed), which makes "the
  // web-server user is not in the `pagespeed` group" the most likely field
  // failure -- and EACCES reads exactly like "the daemon is not there" to a
  // scan that swallows errors, so the two must be told apart explicitly.
  enum class DirAccess { kReadable, kAbsent, kDenied };

  // Whether the directory holding the daemon's volume for `volume_path` can
  // be looked inside: the path itself when it names a directory, its parent
  // otherwise.  kDenied means permission denied (EACCES/EPERM on POSIX, a
  // listing refused with access denied on Windows), kAbsent any other
  // failure.
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

  // Logs the start-up refusal once more, as a warning, through the same
  // handler; nothing when the start-up check left the arm ready or found no
  // daemon configured.  For a serving child process, once its message
  // history is attached: the web server runs StartupCheck() in its parent,
  // before that buffer exists, so the first line never reaches the admin
  // console.  At most once per distinct text per process.
  void ReannounceStartupRefusal();

  // The bound client library, or nullptr when there is none.  Borrowed.
  const DaemonAbi* abi() const { return abi_.get(); }

  // The shared cache handle this process records through, opening it on first
  // use.  Returns nullptr unless health() is kReady.
  //
  // OPENED HERE, NOT AT STARTUP, and the difference is the fork.  The startup
  // check runs in the server's parent process and closes the volume again
  // precisely so that no child inherits a handle it never asked for.  A
  // request-serving process opens its own and keeps it -- until the daemon
  // replaces its volume, when it opens the new one (see "a volume the daemon
  // replaced" below): the handle is per-process state, and there is no
  // correct way to have made it before the process existed.
  //
  // Thread-safe; at most one open is in flight per adapter however many
  // threads race for it, and a successful handle is opened exactly once
  // per volume the daemon has published.
  //
  // RETRIES ON A BOUNDED BACKOFF rather than latching on the first failure --
  // see kRecordCacheMaxAttempts.  Returns nullptr while the arm is between
  // attempts and after it has given up, so every caller still sees the same
  // two outcomes it always did; nothing blocks and nothing sleeps on a
  // request thread.
  void* RecordCache();

  // Closes this process's volume handle if one is open, and clears it.  For
  // a port that tears workers down and wants the close -- and the storage
  // layer's background-thread joins that come with it -- to happen inside
  // the worker's own shutdown sequence rather than at adapter destruction.
  //
  // IDEMPOTENT: a second call is a no-op, and it is safe when no handle was
  // ever opened.  FINAL: afterwards RecordCache() returns nullptr and never
  // reopens.  Finality has its own flag rather than reusing the retry
  // schedule's "gave up" latch: giving up is an operator-visible failure, a
  // clean close is not, and nothing that later reports the one may mistake
  // it for the other.  The attempt counter and next-attempt time are left
  // alone because they cannot fire once the handle is closed.  THREAD-SAFE the same
  // way RecordCache() is: it takes record_cache_mutex_, the lock RecordCache
  // holds across the whole open, so a concurrent RecordCache() and
  // CloseRecordCache() cannot race on the handle -- one of them completes
  // first, and the loser either reopens nothing or has its fresh handle
  // closed.
  //
  // ~DaemonAdapter keeps closing a handle that is still open (ports that
  // never call this rely on the destructor), and never closes a second time
  // after this method has run.
  void CloseRecordCache();

  // The volume handle this process already holds, or nullptr when there is
  // none.  For ports whose caller must not wait: on nginx the caller is the
  // event-loop thread, and nothing on it may block.
  //
  // NEVER OPENS THE VOLUME: performs no volume open and consumes nothing from
  // the retry schedule -- the attempt counter, the next-attempt time and the gave-up
  // latch are untouched, and nothing is logged, so a following RecordCache()
  // behaves exactly as if the accessor had not been called.  NEVER WAITS: it
  // reads the handle off an atomic that RecordCache() publishes under its
  // mutex after a successful open, so it returns promptly even while another
  // thread sits inside a slow open with record_cache_mutex_ held.  After
  // CloseRecordCache() it returns nullptr, and so it does from the moment a
  // replaced volume is noticed until RecordCache() has opened the new one.
  // To notice, it reads the daemon's generation file -- one small file beside
  // the volume -- at most once per kVolumeGenerationCheckIntervalMs across
  // all callers; on a port whose caller is an event loop that read runs on
  // the loop, so a cache directory on a file system that hangs would stall
  // it for that read.  Callers that need the open to
  // happen call RecordCache() instead; this accessor only reports what
  // already is.
  //
  // Unlike RecordCache() it does not consult health(): it reports what was
  // published, and a caller that must honour the health verdict checks it
  // first, as both recorder factories do.
  //
  // The handle is BORROWED, and this accessor is a report, not a lease:
  // nothing here keeps it open.  CloseRecordCache() and ~DaemonAdapter
  // invalidate it, and a holder that outlives either -- a recorder keeps the
  // handle until DoneAndSetHeaders -- uses a closed handle.  A port that
  // closes at worker exit must have finished or joined every holder before
  // it closes.
  void* RecordCacheIfOpen() const;

  // ---- a volume the daemon replaced ---------------------------------------
  //
  // A full cache purge makes the daemon delete its volume file and create a
  // new one.  A process that keeps the handle it opened before keeps the
  // DELETED file: it records originals nobody reads, never sees what the
  // daemon writes into the new file, and can go on serving copies the purge
  // was meant to drop.  The daemon publishes every replacement by rewriting
  // "<volume path>.gen" with a new number, and this adapter reads that file
  // at most once per kVolumeGenerationCheckIntervalMs per process.
  //
  // THE RULE IS TO FAIL CLOSED.  Whenever it is not certain that a handle is
  // on the daemon's current volume, no handle is handed out and requests are
  // answered by the origin, which is always safe.
  //
  // A HANDLE IS HELD AGAINST A NUMBER.  The number is read just before the
  // open and again just after it; the handle is handed out only when both
  // reads answered and agree.  While the number cannot be read nothing is
  // opened, and a handle whose number moved during its open was never
  // handed out, so it is closed at once and the open is tried again.
  //
  // WHEN THE NUMBER MOVED, in this order:
  //   1. RecordCacheIfOpen() returns nullptr from that moment;
  //   2. the next RecordCache() decides again under its lock.  A number that
  //      was read and differs is a replacement; a number that was read and
  //      is the handle's own puts the handle back in use; a read that gave
  //      no answer changes nothing -- the handle stays out of use and the
  //      question is asked again one interval later;
  //   3. on a replacement the old handle is set aside and the volume is
  //      opened again on the ordinary retry schedule.  Set-aside handles are
  //      closed only by CloseRecordCache() and the destructor: a recorder
  //      borrows the handle until its response is complete, so closing one
  //      earlier could pull a mapping out from under a request.  A failed
  //      reopen stays closed; it never falls back to the old handle.
  //
  // WHEN THE NUMBER CANNOT BE READ and nothing has been detected -- the file
  // is there but could not be opened, or does not hold a complete number --
  // nothing is decided: the handle in use stays in use and the next look
  // asks again.  Only a number that was read and differs from the handle's
  // own is a replacement.
  //
  // EVERY OPEN LOOKS BEFORE AND AFTER, AND THIS MODULE DELETES NOTHING.  The
  // daemon's library has one open call, and it creates the volume file it
  // does not find.  A volume file this module's open created is one the
  // daemon never reads, so every open in a serving process -- a process's
  // first as much as a reopen -- asks the daemon for the size it publishes
  // NOW (not the one this server read when it started) and looks at the
  // disk first.  Only volume files of the CURRENT cache format count: the
  // daemon's purge removes its own format's files and deliberately keeps one
  // written in an earlier format, which is therefore never a candidate and
  // never in the way.  No published size, or no current-format file:
  // nothing is opened, and the retry schedule asks again.  A reopen also
  // needs that file to be the only one of its format -- a purge leaves
  // exactly one, so a second one was created by some process by mistake and
  // nothing here can tell the two apart; a first open tolerates several, as
  // the start-up check does after a resize.
  //
  // After the open it looks once more.  If a volume file has appeared or
  // gone, or a file is no longer the same file, the open may itself have
  // created a volume file: the handle is closed without ever having been
  // handed out, every file is LEFT WHERE IT IS, in-place optimization is off
  // in this process until it ends, and one error line names the files.  A
  // file left that way then keeps the other processes of the server from
  // reopening too (they find two), which is the intent: an operator has to
  // look.  So this module CAN create a volume file in that one way; what it
  // guarantees is that it notices, does not use it, says so, and removes
  // nothing.
  //
  // When the configured path is a directory the volume file cannot be told
  // from the other files in it.  No supported configuration has that shape.
  // There a first open is made unchecked, as it always was, and there is no
  // reopen: the process stops using the cache at the first replacement,
  // with one error line.
  //
  // A SECOND PURGE WHILE A PROCESS IS REOPENING AFTER THE FIRST.  Four
  // outcomes, by where this process's open falls; each window is a few
  // milliseconds wide, and each ends safe and logged:
  //   1. the open and the look after it come before the daemon deletes the
  //      file: the handle is handed out, and the next check sees the newer
  //      number and sets it aside like any replaced volume (one info line);
  //   2. the look after the open finds no file, or another file under the
  //      name: the handle is not used and the process stops using the cache
  //      (one error line);
  //   3. the look before the open already saw the second purge's file but
  //      the first purge's number: the handle is handed out on the CURRENT
  //      file, and the next check sets it aside and opens the same file
  //      again.  Harmless, but it spends one of the kMaxVolumeReplacements
  //      and leaves a second mapping of the live file until the process
  //      ends (one info line);
  //   4. the open falls between the daemon's delete and its create: the
  //      look before it saw the old file, the open creates the file, the
  //      look after it sees another file under the name -- outcome 2 for
  //      this process.  The daemon then attaches to the file this process
  //      made, so the cache works for everyone else; the file is owned by
  //      the web server's user.
  // A daemon that replaced its volume without publishing a new number is
  // not noticed at all.
  //
  // THE BOUND, and what it costs.  Each set-aside handle keeps one deleted
  // volume file open and mapped, so its disk space and this process's
  // address space for it are held until the process ends.  The bound is per
  // adapter, and a server has one adapter per virtual host that uses the
  // daemon: each of them follows the daemon through kMaxVolumeReplacements
  // replacements; the next one sets the handle aside like the others and
  // then stops: nothing is handed out any more, in-place optimization is
  // off for that virtual host in this process until it ends, and one error
  // line says so.  At that point it holds kMaxVolumeReplacements + 1
  // replaced volumes.  Disk: up to five times the configured cache size on
  // the host, whatever the number of processes and virtual hosts (they all
  // hold the same deleted files).  Address space: up to five times the
  // cache size per virtual host, in each worker process.
  static constexpr int64_t kVolumeGenerationCheckIntervalMs = 1000;
  static constexpr int kMaxVolumeReplacements = 4;

  // Reads the generation the daemon has published for the volume at
  // `volume_path`: the decimal number in "<volume_path>.gen".  Returns true
  // when the answer is KNOWN, with it in *generation: the number in the
  // file, or 0 when the file does not exist (a daemon that has never
  // replaced its volume has not written one).  Returns false, with
  // *generation 0, when the answer is UNKNOWN: the file exists but could not
  // be opened or read, or does not hold a number followed by a newline (a
  // carriage return before the newline is accepted).
  static bool ReadVolumeGeneration(StringPiece volume_path,
                                   uint64_t* generation);

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

#ifndef _WIN32
  // Replaces the user id the refused-start owner guard compares against
  // (nullptr restores the kernel's answer).  Tests only: an ordinary-user
  // run cannot chown a file to another user, so this is the one way a
  // non-root test can stage a file it owns as another user's.
  using EffectiveUidFn = uid_t (*)();
  static void SetEffectiveUidForTesting(EffectiveUidFn uid_fn);
#endif

  // Forget every latched probe verdict, and restore the latch interval to
  // its default.  Tests only: the latch is process-wide by design, and a
  // test binary is one process.
  static void ResetSocketVerdictsForTesting();

  // Overrides the probe verdict latch interval.  Tests only.
  static void SetSocketVerdictLatchMsForTesting(int64_t latch_ms);

 private:
  static bool SocketAnswers(StringPiece path, GoogleString* error);

  DaemonStartupStatus Resolve(GoogleString* error);

  // Milliseconds on the process's steady clock.  The production clock.
  static int64_t MonotonicNowMs();

  // How long to wait after `attempts` consecutive failures: the first
  // backoff doubled each time and then capped.
  static int64_t BackoffMsAfter(int attempts);

  // Whether the daemon has replaced its volume since this process opened
  // the handle it holds.  Reads the generation file at most once per
  // kVolumeGenerationCheckIntervalMs across all threads; in between it
  // answers from the last read.  Takes no lock and never opens the VOLUME,
  // so the accessor that must not wait can call it -- but the read itself
  // is one open, one small read and one close of a file beside the volume,
  // on the calling thread.  Once it has answered yes it keeps answering
  // yes until RecordCache() has decided under its lock.
  bool VolumeWasReplaced() const;

  GoogleString socket_path_;
  GoogleString volume_path_;
  GoogleString library_path_;
  MessageHandler* handler_;
  AbiLoader abi_loader_;
  std::unique_ptr<DaemonAbi> abi_;
  DaemonHealth health_ = DaemonHealth::kNotConfigured;
  // The size the daemon published when the start-up check ran.  A serving
  // process's open asks the daemon again and looks at the disk before and
  // after (see "a volume the daemon replaced"); this one is used only where
  // that look is not possible, a configured path that is a directory.
  uint64_t inherited_volume_size_ = 0;
  MonotonicClock monotonic_clock_;
  std::mutex record_cache_mutex_;
  void* record_cache_ = nullptr;
  // What RecordCacheIfOpen() reads: the handle, published under
  // record_cache_mutex_ after a successful open and cleared by
  // CloseRecordCache() and the destructor, so the accessor never has to take
  // the mutex to learn whether there is one.  The destructor's clear is
  // unsynchronized: destruction already requires every user to have
  // quiesced.
  std::atomic<void*> record_cache_published_{nullptr};
  // The retry schedule's state, all of it guarded by record_cache_mutex_.
  // Per-adapter and therefore per-process, which is the right scope: the
  // handle being opened is per-process state (see RecordCache), so a
  // process that is still racing the daemon's start must not be held off by
  // another one's history.
  int record_cache_attempts_ = 0;
  int64_t record_cache_next_attempt_ms_ = 0;
  bool record_cache_gave_up_ = false;
  // Set by CloseRecordCache(), guarded by the same mutex.  Final: a closed
  // adapter never reopens.  Deliberately not the gave-up latch above.
  bool record_cache_closed_ = false;
  // The daemon's volume generation record_cache_ is held against: the number
  // read just before the open that produced it and read again, unchanged,
  // just after.  A handle is never published without one.
  mutable std::atomic<uint64_t> record_cache_generation_{0};
  // The next moment a read of the generation file is due.  Claimed by
  // compare-and-swap, so one thread reads per interval.
  mutable std::atomic<int64_t> generation_next_check_ms_{0};
  // Set by a read that found another generation.  While set,
  // RecordCacheIfOpen() hands out nothing.  Cleared only by RecordCache(),
  // under its lock, and only on a KNOWN answer: the handle was set aside,
  // or the number read is the handle's own after all.
  mutable std::atomic<bool> record_cache_replaced_{false};
  // When RecordCache() may next ask again after its own read of the
  // generation gave no answer.  Guarded by record_cache_mutex_, like the
  // three members below.
  int64_t replaced_recheck_ms_ = 0;
  // Handles to volumes the daemon has since replaced.  Kept open until
  // CloseRecordCache() or destruction, because a recorder may still hold
  // one.  Never more than kMaxVolumeReplacements + 1.
  std::vector<void*> retired_record_caches_;
  // How many replacements this process has seen.
  int volume_replacements_ = 0;
  // Set once a replacement has been seen: every open from then on is a
  // REOPEN, which needs the current-format volume file to be the only one.
  bool record_cache_reopening_ = false;
  // Set when the volume directory holds left-over files from an earlier cache
  // size.  Reported on the healthy path, where nothing else would mention it.
  GoogleString extra_volume_warning_;
  // Set when the daemon publishes no cache_dir_generation: a pre-H1 daemon on
  // the legacy layout.  Reported once on the healthy path, where the
  // tolerance would otherwise be invisible.
  bool legacy_generation_layout_ = false;

  // The text StartupCheck() logged when its verdict left in-place
  // optimization off; empty otherwise (including the healthy-path notes).
  // Repeated by ReannounceStartupRefusal().
  GoogleString startup_refusal_;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_SYSTEM_DAEMON_ADAPTER_H_
