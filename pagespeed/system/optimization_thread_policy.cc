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

#include "pagespeed/system/optimization_thread_policy.h"

#include <algorithm>
#include <cstdio>
#include <thread>  // NOLINT(build/c++11) for hardware_concurrency

#ifdef __linux__
#include <sched.h>
#endif
#ifndef _WIN32
#include <unistd.h>
#endif

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

namespace {

// Converts a CPU quota expressed as quota_us per period_us into whole cores.
//
// Rounds down, so above one core the result never exceeds what the process is
// permitted to use.  Below one core rounding down would yield 0, which is not
// a budget anything can run on, so the result floors at 1 and in that one case
// deliberately over-states the quota.  Callers must read the result as "whole
// cores available", not as the quota itself; see
// EffectiveCpuBudget::effective_cores.
int QuotaToWholeCores(int64 quota_us, int64 period_us) {
  if (quota_us <= 0 || period_us <= 0) {
    return kNoCpuLimit;  // Unlimited, or nonsense we should not act on.
  }
  int64 cores = quota_us / period_us;
  if (cores < 1) {
    cores = 1;
  }
  return static_cast<int>(std::min<int64>(cores, kint32max));
}

int ReadOnlineCpus() {
#if defined(_SC_NPROCESSORS_ONLN)
  const long from_sysconf =  // NOLINT(runtime/int) -- sysconf's return type.
      sysconf(_SC_NPROCESSORS_ONLN);
  if (from_sysconf > 0) {
    return static_cast<int>(from_sysconf);
  }
#endif
  const unsigned int from_std = std::thread::hardware_concurrency();
  return from_std > 0 ? static_cast<int>(from_std) : 1;
}

// The deepest cgroup nesting we will walk up through.  Real hierarchies are a
// few levels; the bound only exists so a pathological /proc/self/cgroup
// cannot turn path resolution into an unbounded number of stat calls.
const int kMaxCgroupDepth = 32;

// Splits one /proc/self/cgroup line, which has the shape
// "<hierarchy-id>:<controllers>:<path>".  The path may itself contain ':' in
// principle, so the last field is everything after the second separator.
bool SplitProcCgroupLine(StringPiece line, StringPiece* id,
                         StringPiece* controllers, StringPiece* path) {
  const size_t first = line.find(':');
  if (first == StringPiece::npos) {
    return false;
  }
  const size_t second = line.find(':', first + 1);
  if (second == StringPiece::npos) {
    return false;
  }
  *id = line.substr(0, first);
  *controllers = line.substr(first + 1, second - first - 1);
  *path = line.substr(second + 1);
  return true;
}

#ifdef __linux__

// Longest cgroup file we will read.  The limit files hold two small integers;
// /proc/self/cgroup holds a handful of short lines.
const int kMaxCgroupFileBytes = 4096;

const char kProcSelfCgroupPath[] = "/proc/self/cgroup";
const char kCgroupV2MountRoot[] = "/sys/fs/cgroup";
// The v1 cpu controller is mounted under one of these two names, depending on
// whether the distribution co-mounts cpuacct with it.
const char kCgroupV1CpuMountRoot[] = "/sys/fs/cgroup/cpu";
const char kCgroupV1CpuAcctMountRoot[] = "/sys/fs/cgroup/cpu,cpuacct";

// Reads a small file whole.  Deliberately not going through FileSystem: this
// runs during factory construction, before a message handler is necessarily
// usable, and a missing file is an expected, non-newsworthy outcome.
bool ReadSmallFile(StringPiece path, GoogleString* out) {
  GoogleString path_str;
  path.CopyToString(&path_str);
  FILE* f = fopen(path_str.c_str(), "r");
  if (f == nullptr) {
    return false;
  }
  char buf[kMaxCgroupFileBytes];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = '\0';
  out->assign(buf, n);
  return n > 0;
}

// The CPU affinity mask narrows the usable set without changing the online
// count, so taskset(1) and sched_setaffinity(2) are invisible to
// _SC_NPROCESSORS_ONLN.
int ReadAffinityCpus() {
  cpu_set_t mask;
  CPU_ZERO(&mask);
  if (sched_getaffinity(0, sizeof(mask), &mask) == 0) {
    int count = CPU_COUNT(&mask);
    if (count > 0) {
      return count;
    }
  }
  return kNoCpuLimit;
}

// A container's -- or a systemd unit's -- CPU quota, read from the process's
// own cgroup and its ancestors rather than from the hierarchy root.
int ReadCgroupCpus() {
  StringVector v1_roots;
  v1_roots.push_back(kCgroupV1CpuMountRoot);
  v1_roots.push_back(kCgroupV1CpuAcctMountRoot);
  return ReadCgroupCpusWith(kProcSelfCgroupPath, kCgroupV2MountRoot, v1_roots,
                            &ReadSmallFile);
}

#else  // !__linux__

// Neither an affinity mask nor a cgroup quota is readable here, so the online
// CPU count is the whole story.
int ReadAffinityCpus() { return kNoCpuLimit; }
int ReadCgroupCpus() { return kNoCpuLimit; }

#endif  // __linux__

}  // namespace

const char* CpuBudgetSourceName(CpuBudgetSource source) {
  switch (source) {
    case kCpuBudgetFromOnlineCpus:
      return "online CPUs";
    case kCpuBudgetFromAffinityMask:
      return "CPU affinity mask";
    case kCpuBudgetFromCgroupQuota:
      return "cgroup CPU quota";
  }
  return "online CPUs";
}

int ParseCgroupV2CpuMax(StringPiece contents) {
  StringPieceVector fields;
  SplitStringPieceToVector(contents, " \t\r\n", &fields,
                           true /* omit_empty_strings */);
  if (fields.size() < 2) {
    return kNoCpuLimit;
  }
  if (StringCaseEqual(fields[0], "max")) {
    return kNoCpuLimit;  // Explicitly unlimited.
  }
  int64 quota_us = 0;
  int64 period_us = 0;
  if (!StringToInt64(fields[0], &quota_us) ||
      !StringToInt64(fields[1], &period_us)) {
    return kNoCpuLimit;
  }
  return QuotaToWholeCores(quota_us, period_us);
}

int ParseCgroupV1CpuQuota(StringPiece quota_us_text,
                          StringPiece period_us_text) {
  GoogleString quota_trimmed, period_trimmed;
  TrimWhitespace(quota_us_text, &quota_trimmed);
  TrimWhitespace(period_us_text, &period_trimmed);
  int64 quota_us = 0;
  int64 period_us = 0;
  if (!StringToInt64(quota_trimmed, &quota_us) ||
      !StringToInt64(period_trimmed, &period_us)) {
    return kNoCpuLimit;
  }
  // A quota of -1 is how cgroup v1 spells "unlimited".
  return QuotaToWholeCores(quota_us, period_us);
}

GoogleString ParseProcSelfCgroupV2Path(StringPiece contents) {
  StringPieceVector lines;
  SplitStringPieceToVector(contents, "\r\n", &lines,
                           true /* omit_empty_strings */);
  for (StringPiece line : lines) {
    StringPiece id, controllers, path;
    if (!SplitProcCgroupLine(line, &id, &controllers, &path)) {
      continue;
    }
    // The unified hierarchy is always id 0 with an empty controller list.
    if (id == "0" && controllers.empty()) {
      GoogleString result;
      TrimWhitespace(path, &result);
      return result;
    }
  }
  return GoogleString();
}

GoogleString ParseProcSelfCgroupV1CpuPath(StringPiece contents) {
  StringPieceVector lines;
  SplitStringPieceToVector(contents, "\r\n", &lines,
                           true /* omit_empty_strings */);
  for (StringPiece line : lines) {
    StringPiece id, controllers, path;
    if (!SplitProcCgroupLine(line, &id, &controllers, &path)) {
      continue;
    }
    StringPieceVector names;
    SplitStringPieceToVector(controllers, ",", &names,
                             true /* omit_empty_strings */);
    for (StringPiece name : names) {
      // Exact match only: "cpuset" and "cpuacct" are different controllers
      // and neither carries the quota.
      if (name == "cpu") {
        GoogleString result;
        TrimWhitespace(path, &result);
        return result;
      }
    }
  }
  return GoogleString();
}

StringVector CgroupCandidateDirs(StringPiece mount_root,
                                 StringPiece relative_path) {
  GoogleString root;
  mount_root.CopyToString(&root);
  while (root.size() > 1 && root[root.size() - 1] == '/') {
    root.erase(root.size() - 1);
  }

  StringVector dirs;

  StringPieceVector components;
  SplitStringPieceToVector(relative_path, "/", &components,
                           true /* omit_empty_strings */);

  // A ".." cannot be resolved without the filesystem, and a cgroup path that
  // walks out of its own mount root is not something to act on.  Fall back to
  // the root alone.
  bool usable = static_cast<int>(components.size()) <= kMaxCgroupDepth;
  for (StringPiece component : components) {
    if (component == ".." || component == ".") {
      usable = false;
      break;
    }
  }

  if (usable) {
    GoogleString dir = root;
    StringVector nested;  // Root-first; reversed below.
    for (StringPiece component : components) {
      StrAppend(&dir, "/", component);
      nested.push_back(dir);
    }
    for (int i = static_cast<int>(nested.size()) - 1; i >= 0; --i) {
      dirs.push_back(nested[i]);
    }
  }

  dirs.push_back(root);
  return dirs;
}

int ReadCgroupCpusWith(StringPiece proc_self_cgroup_path,
                       StringPiece v2_mount_root,
                       const StringVector& v1_mount_roots,
                       CgroupFileReader reader) {
  GoogleString proc_contents;
  if (!reader(proc_self_cgroup_path, &proc_contents)) {
    // No /proc mounted, or it is unreadable.  Fall back to treating the mount
    // roots as the process's own cgroup, which is what a container started in
    // the default private cgroup namespace looks like anyway.
    proc_contents.clear();
  }

  // cgroup v2.  A readable cpu.max anywhere on the chain settles the question,
  // including when every one of them says "max".
  bool saw_v2_file = false;
  int tightest = kNoCpuLimit;
  const StringVector v2_dirs = CgroupCandidateDirs(
      v2_mount_root, ParseProcSelfCgroupV2Path(proc_contents));
  for (const GoogleString& dir : v2_dirs) {
    GoogleString contents;
    if (!reader(StrCat(dir, "/cpu.max"), &contents)) {
      continue;
    }
    saw_v2_file = true;
    const int cores = ParseCgroupV2CpuMax(contents);
    if (cores > 0 && (tightest == kNoCpuLimit || cores < tightest)) {
      tightest = cores;
    }
  }
  if (saw_v2_file) {
    return tightest;
  }

  // cgroup v1.
  const GoogleString v1_relative = ParseProcSelfCgroupV1CpuPath(proc_contents);
  for (const GoogleString& v1_root : v1_mount_roots) {
    const StringVector v1_dirs = CgroupCandidateDirs(v1_root, v1_relative);
    for (const GoogleString& dir : v1_dirs) {
      GoogleString quota, period;
      if (!reader(StrCat(dir, "/cpu.cfs_quota_us"), &quota) ||
          !reader(StrCat(dir, "/cpu.cfs_period_us"), &period)) {
        continue;
      }
      const int cores = ParseCgroupV1CpuQuota(quota, period);
      if (cores > 0 && (tightest == kNoCpuLimit || cores < tightest)) {
        tightest = cores;
      }
    }
    if (tightest != kNoCpuLimit) {
      return tightest;
    }
  }
  return tightest;
}

EffectiveCpuBudget CombineCpuBudget(int online_cpus, int affinity_cpus,
                                    int cgroup_cpus) {
  EffectiveCpuBudget budget;
  budget.online_cpus = online_cpus > 0 ? online_cpus : kNoCpuLimit;
  budget.affinity_cpus = affinity_cpus > 0 ? affinity_cpus : kNoCpuLimit;
  budget.cgroup_cpus = cgroup_cpus > 0 ? cgroup_cpus : kNoCpuLimit;

  // Start from the online count, or from 1 if even that is unavailable: with
  // nothing to go on, assume the smallest machine rather than the largest.
  budget.effective_cores = budget.online_cpus > 0 ? budget.online_cpus : 1;
  budget.source = kCpuBudgetFromOnlineCpus;

  // Ties keep the earlier (less specific) source, so the log names a narrower
  // source only when that source is genuinely the binding one.
  if (budget.affinity_cpus > 0 &&
      budget.affinity_cpus < budget.effective_cores) {
    budget.effective_cores = budget.affinity_cpus;
    budget.source = kCpuBudgetFromAffinityMask;
  }
  if (budget.cgroup_cpus > 0 && budget.cgroup_cpus < budget.effective_cores) {
    budget.effective_cores = budget.cgroup_cpus;
    budget.source = kCpuBudgetFromCgroupQuota;
  }
  if (budget.effective_cores < 1) {
    budget.effective_cores = 1;
  }
  return budget;
}

OptimizationThreadCounts ComputeOptimizationThreadCounts(
    int effective_cores, int concurrent_processes) {
  OptimizationThreadCounts counts;
  if (effective_cores < 1) {
    effective_cores = 1;
  }

  if (concurrent_processes <= 0) {
    // the design record D1: the divisor is unknown, so we take the floor rather than
    // guess.  One thread in each pool.
    counts.budget = 1;
  } else {
    // Integer division is the floor the policy asks for.
    int64 numerator =
        static_cast<int64>(effective_cores) * kOptimizationCpuShareNumerator;
    int64 denominator = static_cast<int64>(kOptimizationCpuShareDenominator) *
                        concurrent_processes;
    int64 budget = numerator / denominator;
    counts.budget = static_cast<int>(std::max<int64>(1, budget));
  }

  // the design record A5: the share is per pool, so both pools get the whole budget.
  // No further arithmetic, and in particular no rounding: the budget is
  // already floored at 1, so neither pool can come out empty.
  counts.rewrite = counts.budget;
  counts.expensive = counts.budget;
  return counts;
}

EffectiveCpuBudget DetectEffectiveCpuBudget() {
  return CombineCpuBudget(ReadOnlineCpus(), ReadAffinityCpus(),
                          ReadCgroupCpus());
}

}  // namespace net_instaweb
