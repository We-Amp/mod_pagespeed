// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Optimization Policy Engine Implementation

#include "src/worker/optimization_policy.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "nlohmann/json.hpp"
#include "src/browser/optimization_profile.h"
#include "src/worker/json_dump.h"

namespace pagespeed {

OptimizationPolicy OptimizationPolicy::Compute(
    const OptimizationProfile& profile) {
  OptimizationPolicy policy;

  // CSS coverage: average across viewports that have data.
  float total_coverage = 0.0f;
  int coverage_viewports = 0;
  for (const auto* vp : {&profile.mobile, &profile.tablet, &profile.desktop}) {
    if (vp->total_css_bytes > 0) {
      total_coverage += vp->css_coverage_ratio;
      ++coverage_viewports;
    }
  }
  if (coverage_viewports > 0) {
    float avg_coverage =
        total_coverage / static_cast<float>(coverage_viewports);
    policy.css_unused_ratio = 1.0f - avg_coverage;
    // Recommend async CSS if average coverage < 50% (>50% unused).
    policy.async_css_recommended = avg_coverage < 0.50f;
  }

  // Script deferral: recommended if defer_safe_scripts is non-empty.
  policy.scripts_deferrable =
      static_cast<int>(profile.defer_safe_scripts.size());
  policy.script_deferral_recommended = !profile.defer_safe_scripts.empty();

  // Images: check if LCP is an image across any viewport.
  for (const auto* vp : {&profile.mobile, &profile.tablet, &profile.desktop}) {
    if (!vp->lcp_url.empty()) {
      policy.lcp_is_image = true;
      break;
    }
  }

  // Above-fold image count: max across viewports.
  for (const auto* vp : {&profile.mobile, &profile.tablet, &profile.desktop}) {
    int count = static_cast<int>(vp->image_dimensions.size());
    if (count > policy.above_fold_image_count) {
      policy.above_fold_image_count = count;
    }
  }

  // Confidence: data completeness scoring (0-100).
  // Browser CSS analysis available: +30
  if (coverage_viewports > 0) policy.confidence += 30;
  // Script analysis available: +20
  if (!profile.defer_safe_scripts.empty()) policy.confidence += 20;
  // All 3 viewports have data: +30
  if (coverage_viewports == 3) {
    policy.confidence += 30;
  } else if (coverage_viewports > 0) {
    policy.confidence += coverage_viewports * 10;
  }
  // Profile not expired: +20 (always true for freshly-computed profiles).
  if (profile.expires_at > 0) policy.confidence += 20;
  policy.confidence = std::clamp(policy.confidence, 0, 100);

  // Overall optimization score: weighted sum (0-100).
  // CSS waste 30%, scripts 20%, images 20%, LCP 15%, preconnect 15%
  float score = 0.0f;
  // CSS waste: higher unused ratio = more optimization opportunity.
  score += policy.css_unused_ratio * 30.0f;
  // Scripts: up to 20 points based on deferrable count (capped at 5).
  score += static_cast<float>(std::min(policy.scripts_deferrable, 5)) * 4.0f;
  // Images: up to 20 points based on above-fold images needing dimensions.
  score +=
      static_cast<float>(std::min(policy.above_fold_image_count, 5)) * 4.0f;
  // LCP: 15 points if LCP is an image (preload opportunity).
  if (policy.lcp_is_image) score += 15.0f;
  // Preconnect: 15 points if third-party origins found.
  if (!profile.preload_hints.empty()) score += 15.0f;

  // Guard against NaN/Inf from uninitialized or corrupted profile data.
  if (!std::isfinite(score)) score = 0.0f;
  policy.optimization_score = std::clamp(static_cast<int>(score), 0, 100);

  return policy;
}

std::string OptimizationPolicy::ToJson() const {
  nlohmann::json j;
  j["async_css_recommended"] = async_css_recommended;
  j["css_unused_ratio"] = css_unused_ratio;
  j["script_deferral_recommended"] = script_deferral_recommended;
  j["scripts_deferrable"] = scripts_deferrable;
  j["lcp_is_image"] = lcp_is_image;
  j["above_fold_image_count"] = above_fold_image_count;
  j["optimization_score"] = optimization_score;
  j["confidence"] = confidence;
  return DumpJson(j);
}

absl::StatusOr<OptimizationPolicy> OptimizationPolicy::FromJson(
    std::string_view json) {
  try {
    nlohmann::json j = nlohmann::json::parse(json);

    OptimizationPolicy policy;
    if (j.contains("async_css_recommended"))
      policy.async_css_recommended = j["async_css_recommended"].get<bool>();
    if (j.contains("css_unused_ratio"))
      policy.css_unused_ratio = j["css_unused_ratio"].get<float>();
    if (j.contains("script_deferral_recommended"))
      policy.script_deferral_recommended =
          j["script_deferral_recommended"].get<bool>();
    if (j.contains("scripts_deferrable"))
      policy.scripts_deferrable = j["scripts_deferrable"].get<int>();
    if (j.contains("lcp_is_image"))
      policy.lcp_is_image = j["lcp_is_image"].get<bool>();
    if (j.contains("above_fold_image_count"))
      policy.above_fold_image_count = j["above_fold_image_count"].get<int>();
    if (j.contains("optimization_score"))
      policy.optimization_score = j["optimization_score"].get<int>();
    if (j.contains("confidence"))
      policy.confidence = j["confidence"].get<int>();
    return policy;
  } catch (const nlohmann::json::exception& e) {
    return absl::InvalidArgumentError(e.what());
  }
}

}  // namespace pagespeed
