// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Optimization Profile Implementation

#include "src/browser/optimization_profile.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "lib/classify/capability_mask.h"
#include "nlohmann/json.hpp"
#include "sha256.hpp"

namespace pagespeed {

namespace {

using json = nlohmann::json;

json ImageDimensionToJson(const ImageDimension& dim) {
  return json{
      {"selector", dim.selector},
      {"rendered_width", dim.rendered_width},
      {"rendered_height", dim.rendered_height},
      {"natural_width", dim.natural_width},
      {"natural_height", dim.natural_height},
  };
}

ImageDimension ImageDimensionFromJson(const json& j) {
  ImageDimension dim;
  dim.selector = j.value("selector", "");
  dim.rendered_width = j.value("rendered_width", 0u);
  dim.rendered_height = j.value("rendered_height", 0u);
  dim.natural_width = j.value("natural_width", 0u);
  dim.natural_height = j.value("natural_height", 0u);
  return dim;
}

json PreloadHintToJson(const PreloadHint& hint) {
  json j{{"url", hint.url}, {"as", hint.as}};
  if (!hint.type.empty()) {
    j["type"] = hint.type;
  }
  return j;
}

PreloadHint PreloadHintFromJson(const json& j) {
  PreloadHint hint;
  hint.url = j.value("url", "");
  hint.as = j.value("as", "");
  hint.type = j.value("type", "");
  return hint;
}

json ViewportProfileToJson(const ViewportProfile& vp) {
  json j;
  j["critical_css"] = vp.critical_css;
  j["lcp_selector"] = vp.lcp_selector;
  j["lcp_url"] = vp.lcp_url;

  j["above_fold_selectors"] = vp.above_fold_selectors;
  j["below_fold_selectors"] = vp.below_fold_selectors;

  json dims = json::array();
  for (const auto& dim : vp.image_dimensions) {
    dims.push_back(ImageDimensionToJson(dim));
  }
  j["image_dimensions"] = std::move(dims);

  j["css_coverage_ratio"] = vp.css_coverage_ratio;
  j["total_css_bytes"] = vp.total_css_bytes;
  j["unused_css_bytes"] = vp.unused_css_bytes;

  j["critical_css_validated"] = vp.critical_css_validated;
  j["validation_diff_ratio"] = vp.validation_diff_ratio;
  j["validated_critical_css_hash"] = vp.validated_critical_css_hash;
  j["validated_combined_css_hash"] = vp.validated_combined_css_hash;

  return j;
}

ViewportProfile ViewportProfileFromJson(const json& j) {
  ViewportProfile vp;
  vp.critical_css = j.value("critical_css", "");
  vp.lcp_selector = j.value("lcp_selector", "");
  vp.lcp_url = j.value("lcp_url", "");

  if (j.contains("above_fold_selectors")) {
    for (const auto& s : j["above_fold_selectors"]) {
      if (s.is_string()) {
        vp.above_fold_selectors.push_back(s.get<std::string>());
      }
    }
  }
  if (j.contains("below_fold_selectors")) {
    for (const auto& s : j["below_fold_selectors"]) {
      if (s.is_string()) {
        vp.below_fold_selectors.push_back(s.get<std::string>());
      }
    }
  }
  if (j.contains("image_dimensions")) {
    for (const auto& d : j["image_dimensions"]) {
      vp.image_dimensions.push_back(ImageDimensionFromJson(d));
    }
  }

  if (j.contains("css_coverage_ratio"))
    vp.css_coverage_ratio = j["css_coverage_ratio"].get<float>();
  if (j.contains("total_css_bytes"))
    vp.total_css_bytes = j["total_css_bytes"].get<size_t>();
  if (j.contains("unused_css_bytes"))
    vp.unused_css_bytes = j["unused_css_bytes"].get<size_t>();

  // Absent validation fields keep the struct's safe defaults: a profile
  // written before these existed reads back as NOT validated.
  vp.critical_css_validated = j.value("critical_css_validated", false);
  vp.validation_diff_ratio = j.value("validation_diff_ratio", -1.0f);
  vp.validated_critical_css_hash = j.value("validated_critical_css_hash", "");
  vp.validated_combined_css_hash = j.value("validated_combined_css_hash", "");

  return vp;
}

}  // namespace

std::string CombinedCssValidationHash(std::string_view css) {
  // Never-matchable for an empty sheet: see the header. A stored hash is
  // either 64 hex chars or absent, so "" can equal neither.
  if (css.empty()) return {};
  const std::array<std::byte, 32> digest =
      cyclone::crypto::SHA256::hash(std::span<const std::byte>(
          reinterpret_cast<const std::byte*>(css.data()), css.size()));
  std::string hex;
  hex.reserve(digest.size() * 2);
  for (std::byte b : digest) {
    absl::StrAppendFormat(&hex, "%02x", static_cast<unsigned>(b));
  }
  return hex;
}

std::string CombinedCssValidationHash(std::string_view css,
                                      std::string_view layer_order_binding) {
  if (css.empty()) return {};
  if (layer_order_binding.empty()) return CombinedCssValidationHash(css);
  // A comment after the sheet: bytes no stylesheet can end with by accident
  // collide with, and the binding cannot contain "*/" (it is layer names).
  return CombinedCssValidationHash(absl::StrCat(
      css, "\n/*pagespeed-layer-order:", layer_order_binding, "*/"));
}

bool CombinedCssHasStructuralEscape(std::string_view css) {
  size_t i = 0;
  while (i < css.size()) {
    const char c = css[i];
    if (c == '/' && i + 1 < css.size() && css[i + 1] == '*') {
      const size_t end = css.find("*/", i + 2);
      i = end == std::string_view::npos ? css.size() : end + 2;
      continue;
    }
    if (c == '"' || c == '\'') {
      // A string: its escapes are skipped, and it ends at the matching quote
      // or, as a bad string, at a newline (CSS Syntax 3 §4.3.5).
      for (++i; i < css.size(); ++i) {
        if (css[i] == '\\') {
          ++i;
          continue;
        }
        if (css[i] == c) {
          ++i;
          break;
        }
        if (css[i] == '\n' || css[i] == '\r' || css[i] == '\f') break;
      }
      continue;
    }
    if (c == '\\') {
      if (i + 1 < css.size()) {
        const char next = css[i + 1];
        if (next == '\'' || next == '"' || next == '{' || next == '}' ||
            next == ',') {
          return true;
        }
      }
      i += 2;
      continue;
    }
    ++i;
  }
  return false;
}

std::string ValidationBindingFor(std::string_view layer_order_binding,
                                 bool noscript_affects_render,
                                 std::string_view combined_css,
                                 CapabilityMask::Viewport viewport,
                                 bool anonymous_layers_dropped) {
  // A layer binding starts with "proven " or is "unproven", and a proven one
  // ends with the statement's ';', so a salt cannot be mistaken for one.
  std::string binding(layer_order_binding);
  auto salt = [&binding](std::string_view word) {
    if (!binding.empty()) binding += ' ';
    binding += word;
  };
  if (noscript_affects_render) salt("noscript-render");
  if (CombinedCssHasStructuralEscape(combined_css)) salt("css-escape");
  if (viewport == CapabilityMask::Viewport::kTablet) salt("tablet-mobile");
  if (viewport != CapabilityMask::Viewport::kDesktop) {
    salt("touch");  // touch emulation
    salt("ua");     // User-Agent emulation
  }
  if (anonymous_layers_dropped) salt("anon-layers-dropped");
  return binding;
}

bool AsyncCssValidatedForServedSheet(const ViewportProfile* vp,
                                     std::string_view combined_css,
                                     std::string_view layer_order_binding) {
  if (vp == nullptr || !vp->critical_css_validated) return false;
  if (vp->validated_combined_css_hash.empty()) return false;
  return vp->validated_combined_css_hash ==
         CombinedCssValidationHash(combined_css, layer_order_binding);
}

const ViewportProfile* AsyncCssRecordForDerivedBlock(
    const ViewportProfile* vp, std::string_view derived_critical_css) {
  if (derived_critical_css.empty()) return nullptr;
  return vp;
}

std::string OptimizationProfile::ToJson() const {
  json j;
  j["version"] = 1;
  j["template_hash"] = template_hash_hex;
  j["analyzed_url"] = analyzed_url;

  j["mobile"] = ViewportProfileToJson(mobile);
  j["tablet"] = ViewportProfileToJson(tablet);
  j["desktop"] = ViewportProfileToJson(desktop);

  json hints = json::array();
  for (const auto& hint : preload_hints) {
    hints.push_back(PreloadHintToJson(hint));
  }
  j["preload_hints"] = std::move(hints);

  j["defer_safe_scripts"] = defer_safe_scripts;

  j["created_at"] = created_at;
  j["expires_at"] = expires_at;

  return j.dump();
}

// static
absl::StatusOr<OptimizationProfile> OptimizationProfile::FromJson(
    std::string_view json_str) {
  json j;
  try {
    j = json::parse(json_str);
  } catch (const json::parse_error& e) {
    return absl::InvalidArgumentError(
        absl::StrCat("Invalid profile JSON: ", e.what()));
  }

  if (!j.is_object()) {
    return absl::InvalidArgumentError("Profile JSON must be an object");
  }

  int version = j.value("version", 0);
  if (version != 1) {
    return absl::InvalidArgumentError(
        absl::StrFormat("Unsupported profile version: %d", version));
  }

  try {
    OptimizationProfile profile;
    profile.template_hash_hex = j.value("template_hash", "");
    profile.analyzed_url = j.value("analyzed_url", "");

    if (j.contains("mobile")) {
      profile.mobile = ViewportProfileFromJson(j["mobile"]);
    }
    if (j.contains("tablet")) {
      profile.tablet = ViewportProfileFromJson(j["tablet"]);
    }
    if (j.contains("desktop")) {
      profile.desktop = ViewportProfileFromJson(j["desktop"]);
    }

    if (j.contains("preload_hints")) {
      for (const auto& h : j["preload_hints"]) {
        profile.preload_hints.push_back(PreloadHintFromJson(h));
      }
    }

    if (j.contains("defer_safe_scripts")) {
      for (const auto& s : j["defer_safe_scripts"]) {
        if (s.is_string()) {
          profile.defer_safe_scripts.push_back(s.get<std::string>());
        }
      }
    }

    profile.created_at = j.value("created_at", int64_t{0});
    profile.expires_at = j.value("expires_at", int64_t{0});

    return profile;
  } catch (const json::exception& e) {
    return absl::InvalidArgumentError(
        absl::StrCat("Malformed profile JSON: ", e.what()));
  }
}

// static
std::string OptimizationProfile::CacheUrl(uint64_t template_hash) {
  return absl::StrFormat("__pagespeed_profile__/%016x", template_hash);
}

}  // namespace pagespeed
