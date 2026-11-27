// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Optimization Policy Engine
//
// A stateless scoring function that maps browser analysis data to
// per-template optimization decisions. Policy only STRENGTHENS
// defaults, never overrides user disables.

#ifndef PAGESPEED_SRC_WORKER_OPTIMIZATION_POLICY_H_
#define PAGESPEED_SRC_WORKER_OPTIMIZATION_POLICY_H_

#include <string>
#include <string_view>

#include "absl/status/statusor.h"
#include "src/browser/optimization_profile.h"

namespace pagespeed {

struct OptimizationPolicy {
  // CSS
  bool async_css_recommended = false;
  float css_unused_ratio = 0.0f;

  // Scripts
  bool script_deferral_recommended = false;
  int scripts_deferrable = 0;

  // Images
  bool lcp_is_image = false;
  int above_fold_image_count = 0;

  // Overall (0-100)
  int optimization_score = 0;
  int confidence = 0;

  static OptimizationPolicy Compute(const OptimizationProfile& profile);
  std::string ToJson() const;
  static absl::StatusOr<OptimizationPolicy> FromJson(std::string_view json);
};

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_WORKER_OPTIMIZATION_POLICY_H_
