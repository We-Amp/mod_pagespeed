// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "src/worker/cpu_budget.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <thread>

#ifdef __linux__
#include <sched.h>
#endif

namespace pagespeed {
namespace {

constexpr int64_t kUnlimited = std::numeric_limits<int64_t>::max();

bool ReadFirstLine(const std::string& path, std::string* line) {
  std::ifstream in(path);
  return in.is_open() && static_cast<bool>(std::getline(in, *line));
}

bool ParseInt64(const std::string& text, int64_t* out) {
  try {
    size_t used = 0;
    long long v = std::stoll(text, &used);
    while (used < text.size() && (text[used] == ' ' || text[used] == '\t' ||
                                  text[used] == '\r' || text[used] == '\n')) {
      ++used;
    }
    if (used != text.size()) return false;
    *out = v;
    return true;
  } catch (...) {
    return false;
  }
}

// ceil(quota / period) CPUs, or kUnlimited when there is no usable quota.
int64_t CeilCpus(int64_t quota, int64_t period) {
  if (quota <= 0 || period <= 0) return kUnlimited;
  return quota / period + (quota % period != 0);
}

int64_t CgroupV2Cpus(const std::string& path) {
  std::string line;
  if (!ReadFirstLine(path, &line)) return kUnlimited;
  size_t space = line.find(' ');
  if (space == std::string::npos) return kUnlimited;
  int64_t quota = 0;
  int64_t period = 0;
  if (!ParseInt64(line.substr(0, space), &quota)) return kUnlimited;  // "max"
  if (!ParseInt64(line.substr(space + 1), &period)) return kUnlimited;
  return CeilCpus(quota, period);
}

int64_t CgroupV1Cpus(const std::string& quota_path,
                     const std::string& period_path) {
  std::string quota_text;
  std::string period_text;
  if (!ReadFirstLine(quota_path, &quota_text) ||
      !ReadFirstLine(period_path, &period_text)) {
    return kUnlimited;
  }
  int64_t quota = 0;
  int64_t period = 0;
  if (!ParseInt64(quota_text, &quota) || !ParseInt64(period_text, &period)) {
    return kUnlimited;
  }
  return CeilCpus(quota, period);  // quota -1 -> unlimited
}

CpuBudget QuerySystem() {
  CpuBudget base;
#ifdef __linux__
  cpu_set_t set;
  CPU_ZERO(&set);
  if (sched_getaffinity(0, sizeof(set), &set) == 0 && CPU_COUNT(&set) > 0) {
    base.cpus = CPU_COUNT(&set);
    base.source = "affinity";
    return base;
  }
#endif
  base.cpus = static_cast<int>(std::thread::hardware_concurrency());
  base.source = "hardware";
  return base;
}

}  // namespace

CpuBudget AvailableCpus(const CpuBudgetSources& sources) {
  CpuBudget budget;
  if (sources.cpu_count_override >= 0) {
    budget.cpus = sources.cpu_count_override;
    budget.source = "affinity";
  } else {
    budget = QuerySystem();
  }
  budget.cpus = std::max(budget.cpus, 1);

  int64_t v2 = CgroupV2Cpus(sources.cgroup_v2_cpu_max);
  if (v2 < budget.cpus) {
    budget.cpus = static_cast<int>(v2);
    budget.source = "cgroup v2 quota";
  }
  int64_t v1 = CgroupV1Cpus(sources.cgroup_v1_quota, sources.cgroup_v1_period);
  if (v1 < budget.cpus) {
    budget.cpus = static_cast<int>(v1);
    budget.source = "cgroup v1 quota";
  }
  budget.cpus = std::max(budget.cpus, 1);
  return budget;
}

int DefaultThreadCount(const CpuBudget& budget) {
  return std::clamp(budget.cpus, 2, 128);
}

}  // namespace pagespeed
