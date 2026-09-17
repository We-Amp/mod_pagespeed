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

// How many optimization threads a single server process runs.
//
// Optimization work -- image transcode, CSS and JavaScript parse and minify --
// is CPU-bound, so the budget is cores.  But every supported server runs
// several processes and each one builds its own worker pools, so a per-process
// count that ignores the process count oversubscribes the machine by exactly
// that factor.  The policy is therefore
//
//     budget    = max(1, floor(effective_cores * share / concurrent_processes))
//     rewrite   = budget
//     expensive = budget
//
// The share is *per pool*, not a budget the two pools divide between them.
// That is a deliberate choice: the two pools do different work at
// different times -- a request is parsed and minified, or an image is
// transcoded -- so splitting a single budget between them leaves whichever
// pool is busy under-provisioned while the other sits idle.  Sizing each pool
// to the same budget costs nothing when only one is active, and pools grow
// lazily, so an unused worker is never created in the first place.
//
// The aggregate over all processes is therefore
//
//     sum over processes of (rewrite + expensive) = 2 * budget * processes
//
// which is bounded by
//
//     effective_cores * 2 * share       when effective_cores * share >= processes
//     2 * concurrent_processes          otherwise (the budget's floor)
//
// At share = 1/2 the first branch is at most effective_cores: the two pools
// together may occupy the machine once, and neither can occupy it alone.  It
// is a ceiling, not an equality -- integer flooring means it is attained only
// when 2 * processes divides effective_cores, and never for an odd core count.
// The tight integer form is 2 * floor(effective_cores * share), which is what
// the tests assert.
//
// The floor in the second branch is deliberate -- a process that cannot
// optimize at all is worse than a slightly over-subscribed one -- and it is
// the only way the aggregate exceeds the share.  It is a real exception and
// not a rounding detail: one core with 4096 processes yields 4096 threads in
// each pool.  What keeps that hypothetical is that nothing runs thousands of
// processes on one core, not anything in this policy.
//
// It is also where every forking server lands, Apache included.  A stock
// event configuration has MaxRequestWorkers 400 and ThreadsPerChild 25, so
// AP_MPMQ_MAX_DAEMONS is 16 and the budget is floor(cores / 32): 1 + 1 up to
// 32 cores.  That is the intended outcome and not a regression.
// What a source-built Apache ran before was 4 + 4 *per child* -- measured with
// `ps -T` against a prelinked event MPM, not inferred -- which on a default
// event server is up to 128 optimization threads on 8 cores.  A distribution
// package ran 1 + 1, because its MPM is a DSO and the static carrying the
// answer does not survive httpd's configuration-pool clear.
//
// The shapes this policy *will* raise are the single-process ones -- one nginx
// worker, or one IIS application-pool worker, on a many-core host.  It raises
// none of them yet: neither port reports a divisor, so both take the
// unknown-divisor floor.  Wiring those divisors is #608.
//
// This header is split out from SystemRewriteDriverFactory so the arithmetic,
// the cgroup parsing and the cgroup *path resolution* are unit-testable
// without a container, a server, or a particular machine.

#ifndef PAGESPEED_SYSTEM_OPTIMIZATION_THREAD_POLICY_H_
#define PAGESPEED_SYSTEM_OPTIMIZATION_THREAD_POLICY_H_

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

// The fraction of the machine that *one* optimization worker pool may occupy
// under sustained load, across all server processes.  Half the machine leaves
// the other half for serving requests, which is the concern behind both the
// historical "cap it at 4" and the IIS module's halving of its optimization
// pools relative to its HTML pool.
//
// Per pool, not shared between the two: see the aggregate bound at the top of
// this file, and the exception the per-process floor makes to it.  The
// rationale for sizing per pool rather than splitting one budget is that the
// two pools do different work at different times, so the two rarely saturate
// together.  That is a judgement, not a measurement -- it is the one part of
// this policy that has no evidence behind it, and if optimization is found to
// be starving request serving on a single-process many-core host, this is the
// constant to revisit first.
//
// This is deliberately a constant and not a directive: NumRewriteThreads and
// NumExpensiveRewriteThreads already override the result entirely, and a
// second knob governing the same quantity invites the two to disagree.
const int kOptimizationCpuShareNumerator = 1;
const int kOptimizationCpuShareDenominator = 2;

// The largest count an *explicitly configured* NumRewriteThreads or
// NumExpensiveRewriteThreads will install in a worker pool.  It is applied
// where the directive argument is parsed, so it does not bound the computed
// path, which needs no bound of its own: that path is already a fraction of
// the machine.  The policy proper asks only for a max(1, ...) clamp at pool
// construction as a backstop; this upper bound is an addition made during
// implementation, so its whole rationale is stated here.  A larger
// value is clamped to this and a warning naming both numbers is logged: an
// explicit count that would create thousands of threads per server process is
// a typo far more often than an intention, and "never oversubscribe the
// machine without saying so" is the whole point of the policy.  1024 is far
// above any defensible per-process value on any machine this runs on, so the
// clamp cannot bite a real configuration.
const int kMaxOptimizationThreadsPerPool = 1024;

// Returned by a port that cannot say how many peer processes share the
// machine with it.  Such a port resolves to one thread per pool
// and logs that it did.  It never guesses a larger number -- silently
// oversubscribing a machine is the failure mode this policy exists to end.
const int kUnknownProcessConcurrency = -1;

// Returned by a CPU-budget source that has no answer: the platform does not
// expose it, or it exposes it as "unlimited".
const int kNoCpuLimit = -1;

// Which of the three inputs to effective_cores turned out to be the binding
// one.  Reported in the startup log so an operator can tell a container limit
// from an affinity mask from the raw machine.
enum CpuBudgetSource {
  kCpuBudgetFromOnlineCpus,
  kCpuBudgetFromAffinityMask,
  kCpuBudgetFromCgroupQuota,
};

// effective_cores = min(online CPUs, affinity mask, cgroup
// quota).  Never the host's CPU count alone: a 2-CPU container on a 64-core
// host reports 64, and getting that wrong is how such a container decides it
// may run 32 optimization threads.
struct EffectiveCpuBudget {
  // The binding minimum, always >= 1.  Read it as "whole cores this process
  // may use", not as the quota: a fractional CPU quota is rounded down, and a
  // quota below one whole core still reports 1, because a process that may
  // run no optimization thread at all is not a configuration we can honour.
  // A 0.5-CPU container therefore reads 1 here, one more than its quota
  // permits.  That is the single place this number over-states the limit, it
  // is the same floor the per-process budget already has, and the startup log
  // says "whole cores" for exactly this reason.
  int effective_cores = 1;
  // Which input produced it.
  CpuBudgetSource source = kCpuBudgetFromOnlineCpus;
  // The individual readings, for logging.  kNoCpuLimit where unavailable.
  int online_cpus = kNoCpuLimit;
  int affinity_cpus = kNoCpuLimit;
  int cgroup_cpus = kNoCpuLimit;
};

// Human-readable name of a CpuBudgetSource, for the startup log.
const char* CpuBudgetSourceName(CpuBudgetSource source);

// --------------------------------------------------------------------------
// Testable seams.  These are pure functions of their inputs so the parsing
// and the arithmetic can be pinned without a container or a specific host.
// --------------------------------------------------------------------------

// Parses the contents of a cgroup v2 "cpu.max" file, which holds
// "<quota_us> <period_us>" or "max <period_us>".  Returns the whole cores the
// quota permits, rounded down and then floored at 1, or kNoCpuLimit when the
// quota is unlimited or the contents cannot be understood.
//
// Rounding down matters above one core: a 1.5-CPU container must not size its
// pools as if it had 2, or effective_cores would exceed what the process is
// permitted to use.  Below one core rounding down would yield 0, so the floor
// at 1 takes over and the result deliberately over-states the quota; see
// EffectiveCpuBudget::effective_cores.
int ParseCgroupV2CpuMax(StringPiece contents);

// Parses a cgroup v1 pair of files, "cpu.cfs_quota_us" and "cpu.cfs_period_us".
// A quota of -1 means unlimited.  Same rounding, same floor and same
// kNoCpuLimit convention as ParseCgroupV2CpuMax().
int ParseCgroupV1CpuQuota(StringPiece quota_us, StringPiece period_us);

// --------------------------------------------------------------------------
// cgroup path resolution.
//
// The process's CPU limit is not necessarily at the root of the cgroup
// hierarchy.  It is there for a container started in Docker's default private
// cgroup namespace, which is why reading /sys/fs/cgroup/cpu.max directly
// appears to be enough -- but it is not there for a systemd unit with
// CPUQuota= on a cgroup v2 host (the limit sits on the unit, or on an
// enclosing slice), not for --cgroupns=host, and not for any nested cgroup.
// Getting this wrong fails in the unsafe direction: no limit is found, and the
// pools are sized from the whole host.
//
// So: read the process's own cgroup path from /proc/self/cgroup, then look for
// a limit there and at every ancestor up to the mount root, and take the
// tightest.  cgroup limits are inherited, so the effective limit is the
// minimum down the chain whichever level carries it.
// --------------------------------------------------------------------------

// The process's cgroup path relative to the v2 mount root, read from the
// contents of /proc/self/cgroup: the "0::<path>" line.  Returns the empty
// string when there is no such line, i.e. when the process is under cgroup v1
// only.
GoogleString ParseProcSelfCgroupV2Path(StringPiece contents);

// The process's cgroup path relative to the v1 cpu-controller mount root: the
// "<id>:<controllers>:<path>" line whose comma-separated controller list
// contains "cpu" exactly ("cpuset" and "cpuacct" do not count).  Returns the
// empty string when there is none.
GoogleString ParseProcSelfCgroupV1CpuPath(StringPiece contents);

// The directories to search for a CPU limit, most specific first: the
// process's own cgroup directory under mount_root, then each ancestor, ending
// with mount_root itself.  relative_path may be empty, "/", or any absolute
// cgroup path.  A path containing ".." is refused outright -- it cannot be
// resolved without the filesystem and must never be allowed to walk out of
// the mount root -- and yields just mount_root.
StringVector CgroupCandidateDirs(StringPiece mount_root,
                                 StringPiece relative_path);

// Reads a small file whole.  Returns false when the file does not exist, which
// is the ordinary outcome for most candidate paths.
typedef bool (*CgroupFileReader)(StringPiece path, GoogleString* contents);

// Walks the cgroup hierarchy of the calling process and returns the tightest
// CPU limit found, in whole cores, or kNoCpuLimit when there is none.  Every
// filesystem access goes through `reader` and every root is a parameter, so
// the whole walk -- path resolution included -- can be driven from fixture
// data rather than from the machine the test happens to run on.
//
// v2 wins outright: if any "cpu.max" is readable along the chain, the v1
// controller is not consulted at all, because a v2 file that says "max" is a
// positive statement that there is no quota.
int ReadCgroupCpusWith(StringPiece proc_self_cgroup_path,
                       StringPiece v2_mount_root,
                       const StringVector& v1_mount_roots,
                       CgroupFileReader reader);

// The min-of-three arithmetic, split out from the reading so it can be tested
// directly.  An input of kNoCpuLimit (or anything <= 0) means "this source
// could not answer" and is skipped.  If no source answers, the result is one
// core attributed to the online-CPU source -- the conservative reading.
EffectiveCpuBudget CombineCpuBudget(int online_cpus, int affinity_cpus,
                                    int cgroup_cpus);

// The counts a single server process should run.
struct OptimizationThreadCounts {
  // floor(effective_cores * share / concurrent_processes), floored at 1.
  int budget = 1;
  // Both pools get the whole budget.  Kept as separate fields
  // rather than collapsed into `budget` because the two are independently
  // overridable by NumRewriteThreads and NumExpensiveRewriteThreads, and
  // because a future port may have a reason to size them differently.
  int rewrite = 1;
  int expensive = 1;
};

// Applies the policy formula.  concurrent_processes of
// kUnknownProcessConcurrency (or anything <= 0) yields the minimum, 1 + 1:
// a port that cannot determine its divisor never guesses higher.
OptimizationThreadCounts ComputeOptimizationThreadCounts(
    int effective_cores, int concurrent_processes);

// --------------------------------------------------------------------------
// Live query.
// --------------------------------------------------------------------------

// Reads the three CPU-budget sources for the calling process and combines
// them.  On Linux that is sysconf(_SC_NPROCESSORS_ONLN), sched_getaffinity(),
// and the cgroup v2 or v1 CPU quota.  Elsewhere only the online CPU count is
// available and the other two report kNoCpuLimit.
EffectiveCpuBudget DetectEffectiveCpuBudget();

}  // namespace net_instaweb

#endif  // PAGESPEED_SYSTEM_OPTIMIZATION_THREAD_POLICY_H_
