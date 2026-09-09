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

// Optimization thread-count policy unit tests: the effective-CPU-budget
// arithmetic and the thread-count formula, both of which are pure functions
// here precisely so they can be pinned without a container, a server, or a
// particular host.
//
// The DeploymentShapes suite is the consequences table -- the counts each
// deployment shape is expected to resolve to.  An earlier table was built
// from the children a server happens to be running rather than the ceiling it
// is configured for, so it was wrong for a stock Apache configuration and
// was withdrawn without a replacement.  So this table is now the
// authoritative one -- if the share or the flooring changes, these cases
// fail, which is the point:
// the numbers are the decision, not an implementation detail.

#include "pagespeed/system/optimization_thread_policy.h"

#include <map>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

// ---------------------------------------------------------------------------
// cgroup v2: /sys/fs/cgroup/cpu.max holds "<quota_us> <period_us>", or
// "max <period_us>" when there is no quota.
// ---------------------------------------------------------------------------

TEST(CgroupV2Test, UnlimitedQuota) {
  EXPECT_EQ(kNoCpuLimit, ParseCgroupV2CpuMax("max 100000\n"));
}

TEST(CgroupV2Test, TwoWholeCpus) {
  EXPECT_EQ(2, ParseCgroupV2CpuMax("200000 100000\n"));
}

// The container the effective-CPU-budget rule exists for: two CPUs granted
// on a much larger host.
TEST(CgroupV2Test, TwoCpusNoTrailingNewline) {
  EXPECT_EQ(2, ParseCgroupV2CpuMax("200000 100000"));
}

// Rounded down, never up: 1.5 CPUs of quota must not be sized as 2, or the
// effective budget would exceed what the process may actually use.
TEST(CgroupV2Test, FractionalQuotaRoundsDown) {
  EXPECT_EQ(1, ParseCgroupV2CpuMax("150000 100000"));
}

// Below one whole CPU the floor takes over -- the process still has to be
// able to run something.
TEST(CgroupV2Test, SubCoreQuotaFloorsAtOne) {
  EXPECT_EQ(1, ParseCgroupV2CpuMax("50000 100000"));
}

TEST(CgroupV2Test, NonStandardPeriod) {
  EXPECT_EQ(4, ParseCgroupV2CpuMax("200000 50000"));
}

TEST(CgroupV2Test, GarbageIsNotALimit) {
  EXPECT_EQ(kNoCpuLimit, ParseCgroupV2CpuMax("nonsense"));
  EXPECT_EQ(kNoCpuLimit, ParseCgroupV2CpuMax(""));
  EXPECT_EQ(kNoCpuLimit, ParseCgroupV2CpuMax("200000"));
  EXPECT_EQ(kNoCpuLimit, ParseCgroupV2CpuMax("abc def"));
}

// ---------------------------------------------------------------------------
// cgroup v1: cpu.cfs_quota_us and cpu.cfs_period_us, quota -1 for unlimited.
// ---------------------------------------------------------------------------

TEST(CgroupV1Test, UnlimitedQuota) {
  EXPECT_EQ(kNoCpuLimit, ParseCgroupV1CpuQuota("-1\n", "100000\n"));
}

TEST(CgroupV1Test, FourWholeCpus) {
  EXPECT_EQ(4, ParseCgroupV1CpuQuota("400000\n", "100000\n"));
}

TEST(CgroupV1Test, FractionalQuotaRoundsDown) {
  EXPECT_EQ(2, ParseCgroupV1CpuQuota("250000", "100000"));
}

TEST(CgroupV1Test, SubCoreQuotaFloorsAtOne) {
  EXPECT_EQ(1, ParseCgroupV1CpuQuota("20000", "100000"));
}

TEST(CgroupV1Test, GarbageIsNotALimit) {
  EXPECT_EQ(kNoCpuLimit, ParseCgroupV1CpuQuota("", "100000"));
  EXPECT_EQ(kNoCpuLimit, ParseCgroupV1CpuQuota("400000", "0"));
  EXPECT_EQ(kNoCpuLimit, ParseCgroupV1CpuQuota("what", "100000"));
}

// ---------------------------------------------------------------------------
// cgroup path resolution.
//
// Reading /sys/fs/cgroup/cpu.max works for a container started in Docker's
// default private cgroup namespace and for nothing else.  These cases are the
// shapes it does not cover, and every one of them fails in the unsafe
// direction -- no limit found, pools sized from the whole host.
// ---------------------------------------------------------------------------

TEST(ProcSelfCgroupTest, V2PathFromUnifiedHierarchy) {
  EXPECT_EQ("/system.slice/httpd.service",
            ParseProcSelfCgroupV2Path("0::/system.slice/httpd.service\n"));
}

// A container in the default private cgroup namespace: its own cgroup *is*
// the root it can see.
TEST(ProcSelfCgroupTest, V2PathInPrivateNamespaceIsRoot) {
  EXPECT_EQ("/", ParseProcSelfCgroupV2Path("0::/\n"));
}

// A hybrid host: v1 controllers alongside the unified hierarchy.
TEST(ProcSelfCgroupTest, V2PathFromHybridHierarchy) {
  const char kContents[] =
      "12:pids:/docker/abc\n"
      "4:cpu,cpuacct:/docker/abc\n"
      "0::/docker/abc\n";
  EXPECT_EQ("/docker/abc", ParseProcSelfCgroupV2Path(kContents));
}

TEST(ProcSelfCgroupTest, V2PathAbsentUnderCgroupV1Only) {
  const char kContents[] =
      "4:cpu,cpuacct:/docker/abc\n"
      "3:cpuset:/docker/abc\n"
      "1:name=systemd:/docker/abc\n";
  EXPECT_EQ("", ParseProcSelfCgroupV2Path(kContents));
}

TEST(ProcSelfCgroupTest, V1CpuPathFromCoMountedController) {
  const char kContents[] =
      "4:cpu,cpuacct:/docker/abc\n"
      "1:name=systemd:/docker/abc\n";
  EXPECT_EQ("/docker/abc", ParseProcSelfCgroupV1CpuPath(kContents));
}

// "cpuset" and "cpuacct" are different controllers and neither carries the
// quota, so a prefix or substring match would resolve the wrong path.
TEST(ProcSelfCgroupTest, V1CpuPathIgnoresOtherCpuControllers) {
  const char kContents[] =
      "3:cpuset:/wrong\n"
      "2:cpuacct:/also-wrong\n"
      "4:cpu:/right\n";
  EXPECT_EQ("/right", ParseProcSelfCgroupV1CpuPath(kContents));
}

TEST(ProcSelfCgroupTest, GarbageResolvesToNothing) {
  EXPECT_EQ("", ParseProcSelfCgroupV2Path("nonsense"));
  EXPECT_EQ("", ParseProcSelfCgroupV1CpuPath("nonsense"));
  EXPECT_EQ("", ParseProcSelfCgroupV2Path(""));
  EXPECT_EQ("", ParseProcSelfCgroupV1CpuPath(""));
}

TEST(CgroupCandidateDirsTest, RootOnlyWhenThePathIsTheRoot) {
  StringVector dirs = CgroupCandidateDirs("/sys/fs/cgroup", "/");
  ASSERT_EQ(1, static_cast<int>(dirs.size()));
  EXPECT_EQ("/sys/fs/cgroup", dirs[0]);

  dirs = CgroupCandidateDirs("/sys/fs/cgroup", "");
  ASSERT_EQ(1, static_cast<int>(dirs.size()));
  EXPECT_EQ("/sys/fs/cgroup", dirs[0]);
}

// Most specific first, then every ancestor: the limit is as likely to sit on
// the enclosing slice as on the unit itself.
TEST(CgroupCandidateDirsTest, WalksUpToTheMountRoot) {
  const StringVector dirs =
      CgroupCandidateDirs("/sys/fs/cgroup", "/system.slice/httpd.service");
  ASSERT_EQ(3, static_cast<int>(dirs.size()));
  EXPECT_EQ("/sys/fs/cgroup/system.slice/httpd.service", dirs[0]);
  EXPECT_EQ("/sys/fs/cgroup/system.slice", dirs[1]);
  EXPECT_EQ("/sys/fs/cgroup", dirs[2]);
}

TEST(CgroupCandidateDirsTest, TrailingSlashesDoNotDoubleUp) {
  const StringVector dirs = CgroupCandidateDirs("/sys/fs/cgroup/", "/docker/");
  ASSERT_EQ(2, static_cast<int>(dirs.size()));
  EXPECT_EQ("/sys/fs/cgroup/docker", dirs[0]);
  EXPECT_EQ("/sys/fs/cgroup", dirs[1]);
}

// A cgroup path is not a filesystem path to normalise; one that tries to walk
// out of its mount root is not something to act on.
TEST(CgroupCandidateDirsTest, DotDotIsRefused) {
  const StringVector dirs =
      CgroupCandidateDirs("/sys/fs/cgroup", "/../../etc");
  ASSERT_EQ(1, static_cast<int>(dirs.size()));
  EXPECT_EQ("/sys/fs/cgroup", dirs[0]);
}

// ---------------------------------------------------------------------------
// The whole cgroup walk, driven from fixture data rather than from whatever
// the machine running the test happens to be inside.
// ---------------------------------------------------------------------------

using CgroupFixture = std::map<GoogleString, GoogleString>;

CgroupFixture* g_cgroup_fixture = nullptr;

// Mirrors the real reader, including that an empty file reads as absent.
bool FixtureReader(StringPiece path, GoogleString* contents) {
  GoogleString key;
  path.CopyToString(&key);
  const CgroupFixture::const_iterator it = g_cgroup_fixture->find(key);
  if (it == g_cgroup_fixture->end() || it->second.empty()) {
    return false;
  }
  *contents = it->second;
  return true;
}

int ReadCgroupCpusFromFixture(CgroupFixture* fixture) {
  g_cgroup_fixture = fixture;
  StringVector v1_roots;
  v1_roots.push_back("/sys/fs/cgroup/cpu");
  v1_roots.push_back("/sys/fs/cgroup/cpu,cpuacct");
  const int result = ReadCgroupCpusWith("/proc/self/cgroup", "/sys/fs/cgroup",
                                        v1_roots, &FixtureReader);
  g_cgroup_fixture = nullptr;
  return result;
}

// Docker's default: a private cgroup namespace, so the container's own limit
// really is at the root it can see.  This is the one shape the old code got
// right, and it must keep working.
TEST(CgroupWalkTest, DockerPrivateNamespace) {
  CgroupFixture fixture;
  fixture["/proc/self/cgroup"] = "0::/\n";
  fixture["/sys/fs/cgroup/cpu.max"] = "200000 100000\n";
  EXPECT_EQ(2, ReadCgroupCpusFromFixture(&fixture));
}

// A systemd unit with CPUQuota= on a cgroup v2 host -- bare metal, not an
// exotic shape.  The root's cpu.max says "max"; the limit is two levels down.
// Reading the root alone finds nothing and sizes the pools from the whole
// host.
TEST(CgroupWalkTest, SystemdUnitQuotaOnAV2Host) {
  CgroupFixture fixture;
  fixture["/proc/self/cgroup"] = "0::/system.slice/httpd.service\n";
  fixture["/sys/fs/cgroup/cpu.max"] = "max 100000\n";
  fixture["/sys/fs/cgroup/system.slice/httpd.service/cpu.max"] =
      "400000 100000\n";
  EXPECT_EQ(4, ReadCgroupCpusFromFixture(&fixture));
}

// --cgroupns=host: the container sees the host hierarchy, and its own cgroup
// is nested under /docker.
TEST(CgroupWalkTest, ContainerInTheHostCgroupNamespace) {
  CgroupFixture fixture;
  fixture["/proc/self/cgroup"] = "0::/docker/abc123\n";
  fixture["/sys/fs/cgroup/cpu.max"] = "max 100000\n";
  fixture["/sys/fs/cgroup/docker/abc123/cpu.max"] = "200000 100000\n";
  EXPECT_EQ(2, ReadCgroupCpusFromFixture(&fixture));
}

// The limit sits on an ancestor -- a Kubernetes pod sandbox, or a systemd
// slice -- and the leaf has none of its own.  cgroup limits are inherited, so
// the ancestor's is the one that binds.
TEST(CgroupWalkTest, LimitOnAnAncestor) {
  CgroupFixture fixture;
  fixture["/proc/self/cgroup"] = "0::/kubepods/podabc/container1\n";
  fixture["/sys/fs/cgroup/kubepods/podabc/container1/cpu.max"] =
      "max 100000\n";
  fixture["/sys/fs/cgroup/kubepods/podabc/cpu.max"] = "300000 100000\n";
  fixture["/sys/fs/cgroup/cpu.max"] = "max 100000\n";
  EXPECT_EQ(3, ReadCgroupCpusFromFixture(&fixture));
}

// Two levels both carry a limit: the tightest one is what the process gets.
TEST(CgroupWalkTest, TightestLimitOnTheChainWins) {
  CgroupFixture fixture;
  fixture["/proc/self/cgroup"] = "0::/kubepods/podabc/container1\n";
  fixture["/sys/fs/cgroup/kubepods/podabc/container1/cpu.max"] =
      "800000 100000\n";
  fixture["/sys/fs/cgroup/kubepods/podabc/cpu.max"] = "200000 100000\n";
  EXPECT_EQ(2, ReadCgroupCpusFromFixture(&fixture));
}

// A cpu.max that says "max" everywhere is a positive statement that there is
// no quota, and must not send us looking at the v1 controller.
TEST(CgroupWalkTest, ExplicitlyUnlimitedV2StopsTheSearch) {
  CgroupFixture fixture;
  fixture["/proc/self/cgroup"] = "0::/system.slice/httpd.service\n";
  fixture["/sys/fs/cgroup/cpu.max"] = "max 100000\n";
  fixture["/sys/fs/cgroup/cpu/cpu.cfs_quota_us"] = "200000\n";
  fixture["/sys/fs/cgroup/cpu/cpu.cfs_period_us"] = "100000\n";
  EXPECT_EQ(kNoCpuLimit, ReadCgroupCpusFromFixture(&fixture));
}

TEST(CgroupWalkTest, CgroupV1NestedPath) {
  CgroupFixture fixture;
  fixture["/proc/self/cgroup"] =
      "4:cpu,cpuacct:/docker/abc123\n"
      "1:name=systemd:/docker/abc123\n";
  fixture["/sys/fs/cgroup/cpu/docker/abc123/cpu.cfs_quota_us"] = "400000\n";
  fixture["/sys/fs/cgroup/cpu/docker/abc123/cpu.cfs_period_us"] = "100000\n";
  EXPECT_EQ(4, ReadCgroupCpusFromFixture(&fixture));
}

// Distributions differ on whether the v1 cpu controller is co-mounted with
// cpuacct; both mount points have to be tried.
TEST(CgroupWalkTest, CgroupV1CoMountedMountPoint) {
  CgroupFixture fixture;
  fixture["/proc/self/cgroup"] = "4:cpu,cpuacct:/\n";
  fixture["/sys/fs/cgroup/cpu,cpuacct/cpu.cfs_quota_us"] = "200000\n";
  fixture["/sys/fs/cgroup/cpu,cpuacct/cpu.cfs_period_us"] = "100000\n";
  EXPECT_EQ(2, ReadCgroupCpusFromFixture(&fixture));
}

TEST(CgroupWalkTest, CgroupV1UnlimitedQuota) {
  CgroupFixture fixture;
  fixture["/proc/self/cgroup"] = "4:cpu,cpuacct:/\n";
  fixture["/sys/fs/cgroup/cpu/cpu.cfs_quota_us"] = "-1\n";
  fixture["/sys/fs/cgroup/cpu/cpu.cfs_period_us"] = "100000\n";
  EXPECT_EQ(kNoCpuLimit, ReadCgroupCpusFromFixture(&fixture));
}

// A machine with no cgroup CPU limit at all: bare metal outside a container
// and outside a quota'd unit.
TEST(CgroupWalkTest, NoLimitAnywhere) {
  CgroupFixture fixture;
  fixture["/proc/self/cgroup"] = "0::/user.slice/user-1000.slice\n";
  EXPECT_EQ(kNoCpuLimit, ReadCgroupCpusFromFixture(&fixture));
}

// No /proc mounted: fall back to treating the mount roots as the process's
// own cgroup, which is the old behaviour and still right for a container in a
// private namespace.
TEST(CgroupWalkTest, UnreadableProcFallsBackToTheMountRoot) {
  CgroupFixture fixture;
  fixture["/sys/fs/cgroup/cpu.max"] = "200000 100000\n";
  EXPECT_EQ(2, ReadCgroupCpusFromFixture(&fixture));
}

// The live reader has to survive whatever the test machine is: a container, a
// systemd user session, or neither.
TEST(CgroupWalkTest, LiveReadIsSane) {
  EffectiveCpuBudget budget = DetectEffectiveCpuBudget();
  EXPECT_TRUE(budget.cgroup_cpus == kNoCpuLimit || budget.cgroup_cpus >= 1);
}

// ---------------------------------------------------------------------------
// effective_cores = min(online CPUs, affinity mask, cgroup quota).
// ---------------------------------------------------------------------------

TEST(EffectiveCpuBudgetTest, NoLimitsMeansOnlineCpus) {
  EffectiveCpuBudget budget = CombineCpuBudget(16, kNoCpuLimit, kNoCpuLimit);
  EXPECT_EQ(16, budget.effective_cores);
  EXPECT_EQ(kCpuBudgetFromOnlineCpus, budget.source);
}

// The case the effective-CPU-budget rule exists for: a 2-CPU container on a
// 64-core host.  The host's 64 must never enter the arithmetic.
TEST(EffectiveCpuBudgetTest, CgroupQuotaBeatsHostCpuCount) {
  EffectiveCpuBudget budget = CombineCpuBudget(64, kNoCpuLimit, 2);
  EXPECT_EQ(2, budget.effective_cores);
  EXPECT_EQ(kCpuBudgetFromCgroupQuota, budget.source);
}

// taskset(1) narrows the usable set without changing the online count.
TEST(EffectiveCpuBudgetTest, AffinityMaskBeatsHostCpuCount) {
  EffectiveCpuBudget budget = CombineCpuBudget(64, 4, kNoCpuLimit);
  EXPECT_EQ(4, budget.effective_cores);
  EXPECT_EQ(kCpuBudgetFromAffinityMask, budget.source);
}

TEST(EffectiveCpuBudgetTest, TightestOfThreeWins) {
  EffectiveCpuBudget budget = CombineCpuBudget(64, 8, 3);
  EXPECT_EQ(3, budget.effective_cores);
  EXPECT_EQ(kCpuBudgetFromCgroupQuota, budget.source);

  budget = CombineCpuBudget(64, 3, 8);
  EXPECT_EQ(3, budget.effective_cores);
  EXPECT_EQ(kCpuBudgetFromAffinityMask, budget.source);
}

// A limit that is not actually binding must not be blamed in the log.
TEST(EffectiveCpuBudgetTest, NonBindingLimitDoesNotClaimTheSource) {
  EffectiveCpuBudget budget = CombineCpuBudget(4, 8, 16);
  EXPECT_EQ(4, budget.effective_cores);
  EXPECT_EQ(kCpuBudgetFromOnlineCpus, budget.source);
}

TEST(EffectiveCpuBudgetTest, TiesKeepTheLessSpecificSource) {
  EffectiveCpuBudget budget = CombineCpuBudget(4, 4, 4);
  EXPECT_EQ(4, budget.effective_cores);
  EXPECT_EQ(kCpuBudgetFromOnlineCpus, budget.source);
}

// With nothing readable, assume the smallest machine rather than the largest.
TEST(EffectiveCpuBudgetTest, NothingReadableFloorsAtOne) {
  EffectiveCpuBudget budget =
      CombineCpuBudget(kNoCpuLimit, kNoCpuLimit, kNoCpuLimit);
  EXPECT_EQ(1, budget.effective_cores);
}

TEST(EffectiveCpuBudgetTest, ReadingsAreRetainedForLogging) {
  EffectiveCpuBudget budget = CombineCpuBudget(64, 8, 3);
  EXPECT_EQ(64, budget.online_cpus);
  EXPECT_EQ(8, budget.affinity_cpus);
  EXPECT_EQ(3, budget.cgroup_cpus);
}

// The live query has to produce something usable on whatever machine runs the
// tests; it must never hand the formula a zero or a negative.
TEST(EffectiveCpuBudgetTest, LiveDetectionIsSane) {
  EffectiveCpuBudget budget = DetectEffectiveCpuBudget();
  EXPECT_GE(budget.effective_cores, 1);
  EXPECT_GE(budget.online_cpus, 1);
}

// ---------------------------------------------------------------------------
// The formula.
// ---------------------------------------------------------------------------

TEST(ThreadCountFormulaTest, UnknownDivisorTakesTheFloor) {
  // A port that cannot determine its process-concurrency divisor resolves to
  // one thread per pool.  It must never guess higher, however large the
  // machine: silently oversubscribing the box is the failure mode the policy
  // exists to end.
  OptimizationThreadCounts counts =
      ComputeOptimizationThreadCounts(96, kUnknownProcessConcurrency);
  EXPECT_EQ(1, counts.rewrite);
  EXPECT_EQ(1, counts.expensive);
  EXPECT_EQ(1, counts.budget);

  counts = ComputeOptimizationThreadCounts(96, 0);
  EXPECT_EQ(1, counts.rewrite);
  EXPECT_EQ(1, counts.expensive);
}

// The share is per pool, so neither pool is sized against the other: both get
// the whole budget.  The earlier 2/3 - 1/3 split was justified by the IIS
// module's halving, which on reading turned out to be
// optimization-versus-HTML rather than expensive-versus-rewrite -- both IIS
// optimization pools are max(4, nCPU >> 1).  The intent behind that halving
// (heavy image compute must not starve request serving) is carried by `share`
// itself, not by a second mechanism aimed at the same goal.
TEST(ThreadCountFormulaTest, BothPoolsGetTheWholeBudget) {
  OptimizationThreadCounts counts = ComputeOptimizationThreadCounts(60, 1);
  EXPECT_EQ(30, counts.budget);
  EXPECT_EQ(30, counts.rewrite);
  EXPECT_EQ(30, counts.expensive);
}

TEST(ThreadCountFormulaTest, AggregateBudgetStaysWithinTheShare) {
  // The property that makes the policy statable in one sentence: summing the
  // per-process *budget* over the processes never exceeds
  // effective_cores * share.  It is an equality only where the process count
  // divides the share evenly, as it does here; the sweep below asserts the
  // inequality, which is the part that always holds.
  const int kCores = 64;
  const int kProcesses = 4;
  OptimizationThreadCounts counts =
      ComputeOptimizationThreadCounts(kCores, kProcesses);
  EXPECT_EQ(kCores * kOptimizationCpuShareNumerator /
                kOptimizationCpuShareDenominator,
            counts.budget * kProcesses);
  // Each pool separately stays within the share; the two together may reach
  // twice it, which is the deliberate loosening A5 records.
  EXPECT_EQ(2 * counts.budget, counts.rewrite + counts.expensive);
}

// The budget's floor is the only way the aggregate exceeds the share, and it
// is where every forking server lands.  A process that cannot optimize at all
// is worse than a slightly over-subscribed one.
TEST(ThreadCountFormulaTest, BudgetFloorsAtOneRatherThanZero) {
  // 8 cores, 16 children -- a stock Apache event configuration.  The exact
  // quotient is 8 / 32 = 0.25, and the floor takes it to 1 + 1.
  OptimizationThreadCounts counts = ComputeOptimizationThreadCounts(8, 16);
  EXPECT_EQ(1, counts.budget);
  EXPECT_EQ(1, counts.rewrite);
  EXPECT_EQ(1, counts.expensive);

  // The floor holds however lopsided the shape gets.
  counts = ComputeOptimizationThreadCounts(1, 4096);
  EXPECT_EQ(1, counts.budget);
  EXPECT_EQ(1, counts.rewrite);
  EXPECT_EQ(1, counts.expensive);
}

// The bound the header states, swept rather than asserted.  Every previous
// statement of this bound was an unverified claim about arithmetic nobody
// had enumerated, and two of them were wrong; this sweep is what makes the
// current one a fact.
//
// The window below is a sample, not the argument range -- the range is int, and
// the next test feeds it kint32max.  The bound is general for the reason the
// two branches make plain: where c >= 2p, 2p * floor(c / 2p) <= c, and the left
// side is even, so it is also <= 2 * floor(c / 2); where c < 2p the budget is
// the floor's 1 and the aggregate is exactly 2p.  The sweep is there to catch
// an implementation that stops matching that argument, not to establish it.
TEST(ThreadCountFormulaTest, AggregateBoundHoldsAcrossEveryShape) {
  for (int cores = 1; cores <= 256; ++cores) {
    for (int processes = 1; processes <= 64; ++processes) {
      const OptimizationThreadCounts counts =
          ComputeOptimizationThreadCounts(cores, processes);
      const int per_process = counts.rewrite + counts.expensive;

      // A5: neither pool is sized against the other.
      EXPECT_EQ(counts.budget, counts.rewrite)
          << cores << " cores, " << processes << " processes";
      EXPECT_EQ(counts.budget, counts.expensive)
          << cores << " cores, " << processes << " processes";
      EXPECT_EQ(2 * counts.budget, per_process)
          << cores << " cores, " << processes << " processes";

      // Two branches, and exactly one applies to any shape.  Either the share
      // binds -- and then each pool's total across all processes stays inside
      // it, so the two together stay inside twice it -- or the budget's floor
      // binds, and the aggregate is 2 per process.
      const int share = cores * kOptimizationCpuShareNumerator /
                        kOptimizationCpuShareDenominator;
      if (share >= processes) {
        EXPECT_LE(counts.budget * processes, share)
            << cores << " cores, " << processes << " processes";
        EXPECT_LE(per_process * processes, 2 * share)
            << cores << " cores, " << processes << " processes";
      } else {
        EXPECT_EQ(1, counts.budget)
            << cores << " cores, " << processes << " processes";
        EXPECT_EQ(2 * processes, per_process * processes)
            << cores << " cores, " << processes << " processes";
      }

      // And the union of the two, which is the form the bound is stated in.
      EXPECT_LE(per_process * processes, 2 * share + 2 * processes)
          << cores << " cores, " << processes << " processes";
    }
  }
}

// ComputeOptimizationThreadCounts() is exported, so it has to survive its
// declared argument range.  effective_cores * kOptimizationCpuShareNumerator
// overflows a signed 32-bit int for a numerator above 1; the budget
// arithmetic is int64 for that reason and must stay so.
TEST(ThreadCountFormulaTest, ExtremeCoreCountDoesNotOverflow) {
  const OptimizationThreadCounts counts =
      ComputeOptimizationThreadCounts(kint32max, 1);
  EXPECT_EQ(kint32max / kOptimizationCpuShareDenominator, counts.budget);
  EXPECT_EQ(counts.budget, counts.rewrite);
  EXPECT_EQ(counts.budget, counts.expensive);
}

TEST(ThreadCountFormulaTest, DegenerateInputsStillYieldAWorkingPool) {
  OptimizationThreadCounts counts = ComputeOptimizationThreadCounts(0, 1);
  EXPECT_EQ(1, counts.rewrite);
  EXPECT_EQ(1, counts.expensive);

  counts = ComputeOptimizationThreadCounts(-4, -4);
  EXPECT_EQ(1, counts.rewrite);
  EXPECT_EQ(1, counts.expensive);
}

// ---------------------------------------------------------------------------
// Every row of the consequences table, at share = 1/2.
// ---------------------------------------------------------------------------

struct Shape {
  const char* name;
  int effective_cores;
  int concurrent_processes;
  int expected_budget;
  int expected_rewrite;
  int expected_expensive;
};

TEST(DeploymentShapesTest, MatchesTheAdrConsequencesTable) {
  // Every divisor here is the *configured* ceiling -- AP_MPMQ_MAX_DAEMONS,
  // worker_processes -- and not the number of children a server happens to be
  // running.  The original table conflated the two and so was wrong for a
  // stock configuration on every Apache row.
  const Shape kShapes[] = {
      // Apache prefork, MaxRequestWorkers 256, 8 cores: the budget floor
      // wins.  prefork is not threaded, so this is also what it ran before.
      {"apache prefork, MaxRequestWorkers 256, 8 cores", 8, 256, 1, 1, 1},
      // Apache event, stock MaxRequestWorkers 400 / ThreadsPerChild 25, so
      // AP_MPMQ_MAX_DAEMONS is 16 and the budget is floor(cores / 32).  This
      // is the shape A6 is about: 1 + 1 up to 32 cores, deliberately.
      {"apache event, stock config, 8 cores", 8, 16, 1, 1, 1},
      // The same server tuned down to two children, where the share starts
      // to bind instead of the floor.
      {"apache event, 2 children configured, 8 cores", 8, 2, 2, 2, 2},
      // The remaining rows are the formula only.  Neither nginx nor IIS
      // reports a divisor yet, so both take the unknown-divisor floor
      // in this release whatever these rows say; naming them for a port would
      // read as a claim about shipped behaviour.  They are kept because they
      // are the shapes the policy exists to serve, and because they are what
      // the deferred work has to reproduce.
      {"8 processes, 8 cores (fully forked out)", 8, 8, 1, 1, 1},
      // The single-process many-core shape.  Note the fixed cap of 4 that the
      // documentation promised was never what such a server actually ran --
      // nginx reported itself non-threaded and took 1 + 1 -- so this is the
      // shape both the old cap and the old detection failed, by different
      // routes.
      {"1 process, 64 cores", 64, 1, 32, 32, 32},
      {"1 process, 16 cores", 16, 1, 8, 8, 8},
      // A 2-CPU container, however large the host it sits on.
      {"2-cpu container, 1 process", 2, 1, 1, 1, 1},
  };

  for (const Shape& shape : kShapes) {
    OptimizationThreadCounts counts = ComputeOptimizationThreadCounts(
        shape.effective_cores, shape.concurrent_processes);
    EXPECT_EQ(shape.expected_budget, counts.budget) << shape.name;
    EXPECT_EQ(shape.expected_rewrite, counts.rewrite) << shape.name;
    EXPECT_EQ(shape.expected_expensive, counts.expensive) << shape.name;
  }
}

// The share is a stated quantity, not an accident of the arithmetic.  If it
// is ever retuned, this fails and the consequences table above has to be
// re-derived rather than quietly drifting.
TEST(DeploymentShapesTest, ShareIsOneHalf) {
  EXPECT_EQ(1, kOptimizationCpuShareNumerator);
  EXPECT_EQ(2, kOptimizationCpuShareDenominator);
}

}  // namespace
}  // namespace net_instaweb
