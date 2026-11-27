// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "src/nginx/cache_generation_check.h"

#include <string>
#include <string_view>

#include "src/worker/shared_config.h"

namespace pagespeed {

int ModuleCacheDirGeneration() { return kCacheDirGeneration; }

const char* CacheGenerationStateName(CacheGenerationState state) {
  switch (state) {
    case CacheGenerationState::kMatch:
      return "match";
    case CacheGenerationState::kMismatch:
      return "mismatch";
    case CacheGenerationState::kUnknown:
      break;
  }
  return "unknown";
}

CacheGenerationVerdict EvaluateCacheGeneration(int optimizer_generation,
                                               int module_generation) {
  CacheGenerationVerdict v;
  v.module_generation = module_generation;
  if (optimizer_generation <= 0) {
    v.state = CacheGenerationState::kUnknown;
    return v;
  }
  v.optimizer_generation = optimizer_generation;
  v.state = optimizer_generation == module_generation
                ? CacheGenerationState::kMatch
                : CacheGenerationState::kMismatch;
  return v;
}

std::string DescribeCacheGenerationVerdict(
    const CacheGenerationVerdict& v, CacheGenerationState previous,
    std::string_view shared_config_path) {
  const std::string module_gen = std::to_string(v.module_generation);
  const std::string path(shared_config_path);
  switch (v.state) {
    case CacheGenerationState::kMismatch: {
      const std::string opt_gen = std::to_string(v.optimizer_generation);
      const char* older = v.optimizer_generation < v.module_generation
                              ? "the optimizer is older than this module"
                              : "this module is older than the optimizer";
      return "pagespeed: cache-directory generation mismatch: this module "
             "is generation " +
             module_gen + ", the optimizer (" + path + ") is generation " +
             opt_gen + " -- " + older +
             ". They use different cache formats and would open different "
             "cache files, so the cache is DISABLED: requests pass through "
             "to the origin unoptimized, and nothing is read from, stored "
             "in, or sent to the optimizer until the generations match. "
             "Fix: upgrade the optimizer and the nginx module to the same "
             "release (packages), or run both from the same image tag "
             "(containers). Rechecked whenever the optimizer rewrites its "
             "shared config; no nginx reload needed.";
    }
    case CacheGenerationState::kMatch:
      if (previous == CacheGenerationState::kMismatch) {
        return "pagespeed: cache-directory generation now matches the "
               "optimizer (" +
               path + "): both are generation " + module_gen +
               "; the cache is enabled again";
      }
      return "pagespeed: cache-directory generation " + module_gen +
             " matches the optimizer (" + path + ")";
    case CacheGenerationState::kUnknown:
      break;
  }
  return "pagespeed: cannot verify the cache-directory generation: " + path +
         " states none (an optimizer older than this check, or it has not "
         "written its shared config yet). Using the cache as before; this "
         "module is generation " +
         module_gen +
         ". If the optimizer is older, upgrade both to the same release.";
}

bool CacheGenerationMonitor::Update(int optimizer_generation) {
  const CacheGenerationVerdict next =
      EvaluateCacheGeneration(optimizer_generation, module_generation_);
  const bool changed = !has_verdict_ || next != verdict_;
  has_verdict_ = true;
  verdict_ = next;
  if (changed) {
    log_pending_ = true;
  }
  return changed;
}

}  // namespace pagespeed
