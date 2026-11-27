// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Default worker thread count that respects the container CPU budget.
//
// std::thread::hardware_concurrency() honours CPU affinity (cpusets) but
// ignores CFS quotas, so a container started with a CPU limit on a large host
// would size its pool for the whole host.  AvailableCpus() takes the smallest
// of the affinity count and the cgroup v2 / v1 quota.

#ifndef SRC_WORKER_CPU_BUDGET_H_
#define SRC_WORKER_CPU_BUDGET_H_

#include <string>

namespace pagespeed {

// Where the CPU count is read from.  The defaults are the real system paths;
// tests point them at fixture files.
struct CpuBudgetSources {
  // cgroup v2: "<quota|max> <period>".
  std::string cgroup_v2_cpu_max = "/sys/fs/cgroup/cpu.max";
  // cgroup v1: quota (-1 = unlimited) and period, in microseconds.
  std::string cgroup_v1_quota = "/sys/fs/cgroup/cpu/cpu.cfs_quota_us";
  std::string cgroup_v1_period = "/sys/fs/cgroup/cpu/cpu.cfs_period_us";
  // CPU count to use instead of querying the system (affinity, falling back
  // to hardware_concurrency); a negative value means "query the system".
  int cpu_count_override = -1;
};

struct CpuBudget {
  int cpus = 1;                     // Never below 1.
  const char* source = "hardware";  // "affinity", "cgroup v2 quota",
                                    // "cgroup v1 quota" or "hardware".
};

// The effective CPU count and the source that set it.  A quota wins only when
// it is strictly smaller than the affinity/hardware count.
CpuBudget AvailableCpus(const CpuBudgetSources& sources = {});

// The default pool size: AvailableCpus() clamped to [2, 128].
int DefaultThreadCount(const CpuBudget& budget);

}  // namespace pagespeed

#endif  // SRC_WORKER_CPU_BUDGET_H_
