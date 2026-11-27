// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Config File Persistence

#include "src/worker/config_file.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include "lib/base/atomic_file_writer.h"
#include "nlohmann/json.hpp"
#include "src/worker/html_mode.h"
#include "src/worker/json_dump.h"
#include "src/worker/worker.h"

namespace pagespeed {

using json = nlohmann::json;

bool IsNonReloadable(const std::string& key) {
  static const char* const kNonReloadable[] = {
      "socket_path",
      "cache_path",
      "cache_size_bytes",
      "ram_cache_size",
      // Read-lease wrap gating (issue #934): consumed once at cache open
      // (Worker::Initialize), so CLI-only — restart to change.
      "read_lease_duration_ms",
      "lease_wrap_ceiling_ms",
      "num_threads",
      "max_connections",
      "max_buffer_size",
      "connection_timeout_ms",
      "shutdown_timeout_ms",
      "max_request_size",
      "api_read_open",
      "strip_query_extensions",
      "strip_query_groups",
      "strip_query_params",
      "host_aliases",
      // Web Bot Auth: the key-refresh timer is wired at
      // Initialize, so these are CLI-only for now (restart to change).
      "web_bot_auth",
      "web_bot_auth_key_directories",
      "web_bot_auth_verified_bots",
      // Web Bot Auth opt-in counter (experimental): CLI-only for
      // now (restart to change).  The gating bearer token is env-only.
      "web_bot_auth_public_counter",
      // RSL-CAP enforcement (experimental): the key-refresh timer
      // is wired at Initialize, so these are CLI/env-only for now (restart to
      // change).
      "rsl_cap_enforcement",
      "rsl_cap_key_directories",
      "rsl_cap_requested_license",
      "rsl_cap_requested_scope",
      "rsl_cap_issuer",
  };
  for (const char* k : kNonReloadable) {
    if (key == k) return true;
  }
  return false;
}

std::string ConfigFilePath(const std::string& cache_path) {
  if (cache_path.empty()) {
    return {};
  }
  std::filesystem::path p(cache_path);
  return (p.parent_path() / "pagespeed.json").string();
}

json ConfigToJsonPersistable(const WorkerConfig& c) {
  json j;
  j["jpeg_quality"] = c.jpeg_quality;
  j["webp_quality"] = c.webp_quality;
  j["avif_quality"] = c.avif_quality;
  j["avif_speed"] = c.avif_speed;
  j["savedata_jpeg_quality"] = c.savedata_jpeg_quality;
  j["savedata_webp_quality"] = c.savedata_webp_quality;
  j["savedata_avif_quality"] = c.savedata_avif_quality;
  j["mobile_width"] = c.mobile_width;
  j["tablet_width"] = c.tablet_width;
  j["desktop_width"] = c.desktop_width;
  j["proactive_image_variants"] = c.proactive_image_variants;
  j["proactive_viewport_variants"] = c.proactive_viewport_variants;
  j["proactive_savedata_variants"] = c.proactive_savedata_variants;
  j["proactive_density_variants"] = c.proactive_density_variants;
  j["content_analysis"] = c.content_analysis;
  j["quality_verify"] = c.quality_verify;
  j["target_ssimulacra2"] = c.target_ssimulacra2;
  j["ssimulacra2_tolerance"] = c.ssimulacra2_tolerance;
  j["learned_quality"] = c.learned_quality;
  j["learned_quality_jpeg"] = c.learned_quality_jpeg;
  j["learned_quality_webp"] = c.learned_quality_webp;
  j["learned_quality_avif"] = c.learned_quality_avif;
  j["savedata_score_reduction"] = c.savedata_score_reduction;
  j["enable_warmup"] = c.enable_warmup;
  j["cache_mode"] = c.cache_mode;
  j["html_mode"] = std::string(HtmlModeName(c.html_mode));
  // Derived, for a reader (an older worker loading this file, an older
  // console) that predates html_mode.  On load html_mode wins.
  j["disable_html"] = HtmlModeDisablesHtml(c.html_mode);
  j["disable_css"] = c.disable_css;
  j["disable_js"] = c.disable_js;
  j["disable_image"] = c.disable_image;
  j["disable_lazy_load"] = c.disable_lazy_load;
  j["disable_image_dimensions"] = c.disable_image_dimensions;
  j["disable_lcp_preload"] = c.disable_lcp_preload;
  j["disable_preconnect_injection"] = c.disable_preconnect_injection;
  j["enable_speculation_rules"] = c.enable_speculation_rules;
  j["disable_async_css"] = c.disable_async_css;
  j["async_css_min_coverage"] = c.async_css_min_coverage;
  j["async_css_min_deferred_bytes"] = c.async_css_min_deferred_bytes;
  j["disable_script_deferral"] = c.disable_script_deferral;
  j["disable_css_import_flattening"] = c.disable_css_import_flattening;
  // SVG auto-vectorization
  switch (c.svg_mode) {
    case SvgMode::kDetect:
      j["svg_mode"] = "detect";
      break;
    case SvgMode::kPreview:
      j["svg_mode"] = "preview";
      break;
    case SvgMode::kAuto:
      j["svg_mode"] = "auto";
      break;
  }
  j["svg_candidacy_threshold"] = c.svg_candidacy_threshold;
  j["svg_max_pixels"] = c.svg_max_pixels;
  j["svg_preset"] = c.svg_preset;
  j["svg_color_precision"] = c.svg_color_precision;
  j["svg_filter_speckle"] = c.svg_filter_speckle;
  j["svg_max_paths"] = c.svg_max_paths;
  j["svg_max_svg_bytes"] = c.svg_max_svg_bytes;
  j["svg_fidelity_threshold"] = c.svg_fidelity_threshold;
  j["svg_exclude_lcp"] = c.svg_exclude_lcp;
  j["svg_timeout_ms"] = c.svg_timeout_ms;
  j["gzip_level"] = c.gzip_level;
  j["brotli_level"] = c.brotli_level;
  j["max_url_length"] = c.max_url_length;
  j["max_html_size"] = c.max_html_size;
  j["max_css_size"] = c.max_css_size;
  j["max_js_size"] = c.max_js_size;
  j["max_image_size"] = c.max_image_size;
  j["quality_cap_margin"] = c.quality_cap_margin;
  j["no_quality_cap"] = c.no_quality_cap;
  return j;
}

bool IsRetiredLicensingKey(std::string_view key) {
  return key == "license_key" || key == "license_renewal_url" ||
         key == "fastspring_storefront" || key == "fastspring_product";
}

const char* const kRetiredLicensingKeyReason =
    "removed in 2.1; the setting is ignored and can be deleted from your "
    "configuration";

const char* const kQualityCapMarginDeprecatedWarning =
    "quality_cap_margin is deprecated and ignored: it no longer changes the "
    "quality cap; use no_quality_cap to turn the cap off";

ConfigApplyResult ApplyConfigJson(const json& body, WorkerConfig* config) {
  json applied = json::object();
  json rejected = json::object();
  json warnings = json::array();

  // Clamp `raw` into [lo, hi] and emit a warning if the value was truncated.
  // The warning names the field, original value, clamped value, and range so
  // the client can distinguish a clamp from an accepted value (see issue #166).
  auto clamp_with_warning = [&warnings](const std::string& key, int raw, int lo,
                                        int hi) {
    int clamped = std::clamp(raw, lo, hi);
    if (clamped != raw) {
      warnings.push_back(key + ": clamped " + std::to_string(raw) + " to " +
                         std::to_string(clamped) + " (valid range: " +
                         std::to_string(lo) + "-" + std::to_string(hi) + ")");
    }
    return clamped;
  };

  // html_mode wins over disable_html when both are given, whatever their
  // order, so the explicit mode is found before the loop.  An html_mode that
  // is rejected (wrong type, unknown value) does not count as given, and
  // disable_html then applies as before.
  std::optional<HtmlMode> explicit_html_mode;
  if (auto it = body.find("html_mode"); it != body.end() && it->is_string()) {
    explicit_html_mode = ParseHtmlMode(it->get<std::string>());
  }

  for (auto& [key, value] : body.items()) {
    if (IsNonReloadable(key)) {
      rejected[key] = "Cannot change at runtime";
      continue;
    }

    // Retired 2.0 licensing settings: a stale pagespeed.json or an
    // old console may still send them.  Never fatal — surface them as
    // rejected with a reason that names the change, so the operator can
    // clean the setting up, and move on.
    if (IsRetiredLicensingKey(key)) {
      rejected[key] = kRetiredLicensingKeyReason;
      continue;
    }

    bool recognized = true;

    // Quality settings
    if (key == "jpeg_quality" && value.is_number_integer()) {
      int v = clamp_with_warning(key, value.get<int>(), 1, 100);
      config->jpeg_quality = v;
      applied[key] = v;
      if (v < 30) warnings.push_back("jpeg_quality: Very low quality");
    } else if (key == "webp_quality" && value.is_number_integer()) {
      int v = clamp_with_warning(key, value.get<int>(), 0, 100);
      config->webp_quality = v;
      applied[key] = v;
      if (v < 30) warnings.push_back("webp_quality: Very low quality");
    } else if (key == "avif_quality" && value.is_number_integer()) {
      int v = clamp_with_warning(key, value.get<int>(), 0, 100);
      config->avif_quality = v;
      applied[key] = v;
      if (v < 30) warnings.push_back("avif_quality: Very low quality");
    } else if (key == "avif_speed" && value.is_number_integer()) {
      config->avif_speed = clamp_with_warning(key, value.get<int>(), 0, 10);
      applied[key] = config->avif_speed;
    } else if (key == "savedata_jpeg_quality" && value.is_number_integer()) {
      config->savedata_jpeg_quality =
          clamp_with_warning(key, value.get<int>(), 1, 100);
      applied[key] = config->savedata_jpeg_quality;
    } else if (key == "savedata_webp_quality" && value.is_number_integer()) {
      config->savedata_webp_quality =
          clamp_with_warning(key, value.get<int>(), 0, 100);
      applied[key] = config->savedata_webp_quality;
    } else if (key == "savedata_avif_quality" && value.is_number_integer()) {
      config->savedata_avif_quality =
          clamp_with_warning(key, value.get<int>(), 0, 100);
      applied[key] = config->savedata_avif_quality;
    }
    // Viewport widths
    else if (key == "mobile_width" && value.is_number_unsigned()) {
      uint32_t v = value.get<uint32_t>();
      if (v != 0 && (v < 160 || v > 10000)) {
        rejected[key] = "Must be 0 or 160-10000";
      } else {
        config->mobile_width = v;
        applied[key] = v;
      }
    } else if (key == "tablet_width" && value.is_number_unsigned()) {
      uint32_t v = value.get<uint32_t>();
      if (v != 0 && (v < 160 || v > 10000)) {
        rejected[key] = "Must be 0 or 160-10000";
      } else {
        config->tablet_width = v;
        applied[key] = v;
      }
    } else if (key == "desktop_width" && value.is_number_unsigned()) {
      uint32_t v = value.get<uint32_t>();
      if (v != 0 && (v < 160 || v > 10000)) {
        rejected[key] = "Must be 0 or 160-10000";
      } else {
        config->desktop_width = v;
        applied[key] = v;
      }
    }
    // Feature toggles (booleans)
    else if (key == "proactive_image_variants" && value.is_boolean()) {
      config->proactive_image_variants = value.get<bool>();
      applied[key] = config->proactive_image_variants;
    } else if (key == "proactive_viewport_variants" && value.is_boolean()) {
      config->proactive_viewport_variants = value.get<bool>();
      applied[key] = config->proactive_viewport_variants;
    } else if (key == "proactive_savedata_variants" && value.is_boolean()) {
      config->proactive_savedata_variants = value.get<bool>();
      applied[key] = config->proactive_savedata_variants;
    } else if (key == "proactive_density_variants" && value.is_boolean()) {
      config->proactive_density_variants = value.get<bool>();
      applied[key] = config->proactive_density_variants;
    } else if (key == "content_analysis" && value.is_boolean()) {
      config->content_analysis = value.get<bool>();
      applied[key] = config->content_analysis;
    } else if (key == "quality_verify" && value.is_boolean()) {
      config->quality_verify = value.get<bool>();
      applied[key] = config->quality_verify;
    } else if (key == "learned_quality" && value.is_boolean()) {
      config->learned_quality = value.get<bool>();
      applied[key] = config->learned_quality;
    } else if (key == "learned_quality_jpeg" && value.is_boolean()) {
      config->learned_quality_jpeg = value.get<bool>();
      applied[key] = config->learned_quality_jpeg;
    } else if (key == "learned_quality_webp" && value.is_boolean()) {
      config->learned_quality_webp = value.get<bool>();
      applied[key] = config->learned_quality_webp;
    } else if (key == "learned_quality_avif" && value.is_boolean()) {
      config->learned_quality_avif = value.get<bool>();
      applied[key] = config->learned_quality_avif;
    } else if (key == "cache_mode" && value.is_string()) {
      std::string mode = value.get<std::string>();
      if (mode == "safe" || mode == "aggressive") {
        config->cache_mode = mode;
        applied[key] = mode;
      } else {
        rejected[key] = "Must be 'safe' or 'aggressive'";
      }
    } else if (key == "html_mode" && value.is_string()) {
      if (explicit_html_mode.has_value()) {
        config->html_mode = *explicit_html_mode;
        applied[key] = std::string(HtmlModeName(*explicit_html_mode));
      } else {
        rejected[key] = "Must be one of: " + std::string(kHtmlModeValues);
      }
    } else if (key == "disable_html" && value.is_boolean()) {
      // The legacy form: true = off, false = full.
      const bool disable = value.get<bool>();
      if (!explicit_html_mode.has_value()) {
        config->html_mode = HtmlModeFromDisableHtml(disable);
        applied[key] = disable;
      } else if (disable == HtmlModeDisablesHtml(*explicit_html_mode)) {
        // Agrees with html_mode (what every writer of this file emits).
        applied[key] = disable;
      } else {
        warnings.push_back(
            "disable_html: ignored because html_mode is also set and takes "
            "precedence");
      }
    } else if (key == "disable_css" && value.is_boolean()) {
      config->disable_css = value.get<bool>();
      applied[key] = config->disable_css;
    } else if (key == "disable_js" && value.is_boolean()) {
      config->disable_js = value.get<bool>();
      applied[key] = config->disable_js;
    } else if (key == "disable_image" && value.is_boolean()) {
      config->disable_image = value.get<bool>();
      applied[key] = config->disable_image;
    } else if (key == "disable_lazy_load" && value.is_boolean()) {
      config->disable_lazy_load = value.get<bool>();
      applied[key] = config->disable_lazy_load;
    } else if (key == "disable_image_dimensions" && value.is_boolean()) {
      config->disable_image_dimensions = value.get<bool>();
      applied[key] = config->disable_image_dimensions;
    } else if (key == "disable_lcp_preload" && value.is_boolean()) {
      config->disable_lcp_preload = value.get<bool>();
      applied[key] = config->disable_lcp_preload;
    } else if (key == "disable_preconnect_injection" && value.is_boolean()) {
      config->disable_preconnect_injection = value.get<bool>();
      applied[key] = config->disable_preconnect_injection;
    } else if (key == "enable_speculation_rules" && value.is_boolean()) {
      config->enable_speculation_rules = value.get<bool>();
      applied[key] = config->enable_speculation_rules;
    } else if (key == "disable_async_css" && value.is_boolean()) {
      config->disable_async_css = value.get<bool>();
      applied[key] = config->disable_async_css;
    } else if (key == "disable_script_deferral" && value.is_boolean()) {
      config->disable_script_deferral = value.get<bool>();
      applied[key] = config->disable_script_deferral;
    } else if (key == "disable_css_import_flattening" && value.is_boolean()) {
      config->disable_css_import_flattening = value.get<bool>();
      applied[key] = config->disable_css_import_flattening;
    } else if (key == "async_css_min_coverage" && value.is_number()) {
      float v = value.get<float>();
      if (!std::isfinite(v) || v < 0.0f || v > 1.0f) {
        rejected[key] = "Must be 0.0-1.0";
      } else {
        config->async_css_min_coverage = v;
        applied[key] = v;
      }
    } else if (key == "async_css_min_deferred_bytes" &&
               value.is_number_integer()) {
      // is_number_integer accepts both signed and unsigned JSON integers
      // (programmatic int vs parsed-JSON unsigned); guard against negatives.
      int64_t v = value.get<int64_t>();
      if (v < 0) {
        rejected[key] = "Must be >= 0";
      } else {
        config->async_css_min_deferred_bytes = static_cast<size_t>(v);
        applied[key] = config->async_css_min_deferred_bytes;
      }
    }
    // Analysis settings (floats — range-checked, reject NaN/Inf)
    else if (key == "target_ssimulacra2" && value.is_number()) {
      float v = value.get<float>();
      if (!std::isfinite(v) || v < 0.0f || v > 100.0f) {
        rejected[key] = "Must be 0-100";
      } else {
        config->target_ssimulacra2 = v;
        applied[key] = v;
        if (v > 90.0f) {
          warnings.push_back(
              "target_ssimulacra2 > 90: May prevent effective compression");
        }
      }
    } else if (key == "denoise_threshold" || key == "denoise_sigma_spatial" ||
               key == "denoise_sigma_range") {
      // Removed options: noisy images are no longer smoothed before
      // encoding.  Existing configs that still set them keep loading.
      warnings.push_back(key + " is deprecated and ignored");
    } else if (key == "savedata_score_reduction" && value.is_number()) {
      float v = value.get<float>();
      if (!std::isfinite(v) || v < 0.0f || v > 50.0f) {
        rejected[key] = "Must be 0-50";
      } else {
        config->savedata_score_reduction = v;
        applied[key] = v;
      }
    } else if (key == "enable_warmup" && value.is_boolean()) {
      config->enable_warmup = value.get<bool>();
      applied[key] = config->enable_warmup;
    } else if (key == "ssimulacra2_tolerance" && value.is_number()) {
      float v = value.get<float>();
      if (!std::isfinite(v) || v < 0.0f || v > 50.0f) {
        rejected[key] = "Must be 0-50";
      } else {
        config->ssimulacra2_tolerance = v;
        applied[key] = v;
      }
    }
    // Compression levels
    else if (key == "gzip_level" && value.is_number_integer()) {
      config->gzip_level = clamp_with_warning(key, value.get<int>(), 0, 9);
      applied[key] = config->gzip_level;
    } else if (key == "brotli_level" && value.is_number_integer()) {
      config->brotli_level = clamp_with_warning(key, value.get<int>(), 0, 11);
      applied[key] = config->brotli_level;
    }
    // SVG auto-vectorization settings
    else if (key == "svg_mode" && value.is_string()) {
      std::string mode = value.get<std::string>();
      if (mode == "detect") {
        config->svg_mode = SvgMode::kDetect;
        applied[key] = mode;
      } else if (mode == "preview") {
        config->svg_mode = SvgMode::kPreview;
        applied[key] = mode;
      } else if (mode == "auto") {
        config->svg_mode = SvgMode::kAuto;
        applied[key] = mode;
      } else {
        rejected[key] = "Must be detect, preview, or auto";
      }
    } else if (key == "svg_candidacy_threshold" && value.is_number_integer()) {
      int v = clamp_with_warning(key, value.get<int>(), 0, 100);
      config->svg_candidacy_threshold = v;
      applied[key] = v;
    } else if (key == "svg_max_pixels" && value.is_number_integer()) {
      int v = value.get<int>();
      if (v < 1 || v > 16777216) {
        rejected[key] = "Must be 1-16777216";
      } else {
        config->svg_max_pixels = v;
        applied[key] = v;
      }
    } else if (key == "svg_preset" && value.is_number_integer()) {
      int v = value.get<int>();
      if (v < 0 || v > 2) {
        rejected[key] = "Must be 0-2";
      } else {
        config->svg_preset = v;
        applied[key] = v;
      }
    } else if (key == "svg_color_precision" && value.is_number_integer()) {
      int v = value.get<int>();
      if (v < 0 || v > 8) {
        rejected[key] = "Must be 0-8";
      } else {
        config->svg_color_precision = v;
        applied[key] = v;
      }
    } else if (key == "svg_filter_speckle" && value.is_number_integer()) {
      int v = value.get<int>();
      if (v < 0 || v > 1000) {
        rejected[key] = "Must be 0-1000";
      } else {
        config->svg_filter_speckle = v;
        applied[key] = v;
      }
    } else if (key == "svg_max_paths" && value.is_number_integer()) {
      int v = value.get<int>();
      if (v < 1 || v > 100000) {
        rejected[key] = "Must be 1-100000";
      } else {
        config->svg_max_paths = v;
        applied[key] = v;
      }
    } else if (key == "svg_max_svg_bytes" && value.is_number_integer()) {
      int v = value.get<int>();
      if (v < 1024 || v > 16 * 1024 * 1024) {
        rejected[key] = "Must be 1KB-16MB";
      } else {
        config->svg_max_svg_bytes = v;
        applied[key] = v;
      }
    } else if (key == "svg_fidelity_threshold" && value.is_number()) {
      float v = value.get<float>();
      if (!std::isfinite(v) || v < 0.0f || v > 100.0f) {
        rejected[key] = "Must be 0-100";
      } else {
        config->svg_fidelity_threshold = v;
        applied[key] = v;
      }
    } else if (key == "svg_exclude_lcp" && value.is_boolean()) {
      config->svg_exclude_lcp = value.get<bool>();
      applied[key] = config->svg_exclude_lcp;
    } else if (key == "svg_timeout_ms" && value.is_number_integer()) {
      int v = value.get<int>();
      if (v < 10 || v > 60000) {
        rejected[key] = "Must be 10-60000";
      } else {
        config->svg_timeout_ms = v;
        applied[key] = v;
      }
    }
    // Quality cap
    else if (key == "quality_cap_margin" && value.is_number_integer()) {
      int v = clamp_with_warning(key, value.get<int>(), 0, 50);
      config->quality_cap_margin = v;
      // Saved configs always carry the key at its default, so only a
      // non-default value means the operator set it.
      if (v != WorkerConfig{}.quality_cap_margin) {
        warnings.push_back(kQualityCapMarginDeprecatedWarning);
      }
      applied[key] = v;
    } else if (key == "no_quality_cap" && value.is_boolean()) {
      config->no_quality_cap = value.get<bool>();
      applied[key] = config->no_quality_cap;
    }
    // Security limits (bounded to prevent misuse)
    else if (key == "max_url_length" && value.is_number_unsigned()) {
      size_t v = value.get<size_t>();
      if (v < 64 || v > 64ULL * 1024) {
        rejected[key] = "Must be 64-65536";
      } else {
        config->max_url_length = v;
        applied[key] = v;
      }
    } else if (key == "max_html_size" && value.is_number_unsigned()) {
      size_t v = value.get<size_t>();
      if (v < 1024 || v > 64ULL * 1024 * 1024) {
        rejected[key] = "Must be 1KB-64MB";
      } else {
        config->max_html_size = v;
        applied[key] = v;
      }
    } else if (key == "max_css_size" && value.is_number_unsigned()) {
      size_t v = value.get<size_t>();
      if (v < 1024 || v > 64ULL * 1024 * 1024) {
        rejected[key] = "Must be 1KB-64MB";
      } else {
        config->max_css_size = v;
        applied[key] = v;
      }
    } else if (key == "max_js_size" && value.is_number_unsigned()) {
      size_t v = value.get<size_t>();
      if (v < 1024 || v > 64ULL * 1024 * 1024) {
        rejected[key] = "Must be 1KB-64MB";
      } else {
        config->max_js_size = v;
        applied[key] = v;
      }
    } else if (key == "max_image_size" && value.is_number_unsigned()) {
      size_t v = value.get<size_t>();
      if (v < 1024 || v > 256ULL * 1024 * 1024) {
        rejected[key] = "Must be 1KB-256MB";
      } else {
        config->max_image_size = v;
        applied[key] = v;
      }
    } else {
      recognized = false;
    }

    if (!recognized) {
      rejected[key] = "Unknown or invalid type for field";
    }
  }

  return {std::move(applied), std::move(rejected), std::move(warnings)};
}

// Maximum config file size (1 MB).  A legitimate pagespeed.json is a few KB;
// anything larger is corrupted or adversarial.
static constexpr size_t kMaxConfigFileSize = 1024UL * 1024UL;

// Maximum JSON nesting depth.  Matches the limit used by the HTTP API
// (JsonNestingTooDeep in http_server.h) but implemented locally to avoid
// pulling in the HTTP server header.
static constexpr int kMaxJsonNestingDepth = 32;

// Quick scan for excessive JSON nesting depth.  Approximate: does not account
// for braces inside string literals, but erring on the side of rejection is
// acceptable for a config file.
static bool ConfigJsonNestingTooDeep(std::string_view s) {
  int depth = 0;
  bool in_string = false;
  bool escape_next = false;
  for (char c : s) {
    if (escape_next) {
      escape_next = false;
      continue;
    }
    if (in_string) {
      if (c == '\\') {
        escape_next = true;
      } else if (c == '"') {
        in_string = false;
      }
      continue;
    }
    if (c == '"') {
      in_string = true;
    } else if (c == '{' || c == '[') {
      if (++depth > kMaxJsonNestingDepth) return true;
    } else if (c == '}' || c == ']') {
      if (depth > 0) --depth;
    }
  }
  return false;
}

json ReadConfigFile(const std::string& path, std::string* error) {
  if (path.empty()) {
    return json::object();
  }
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f.is_open()) {
    // Missing file is normal (first run).
    return json::object();
  }

  // Check file size before reading.
  auto size = f.tellg();
  if (size < 0 || static_cast<size_t>(size) > kMaxConfigFileSize) {
    if (error) *error = "Config file exceeds 1 MB size limit";
    return json::object();
  }

  // Read entire file into a string.
  f.seekg(0, std::ios::beg);
  std::string content(static_cast<size_t>(size), '\0');
  if (!f.read(content.data(), size)) {
    if (error) *error = "Failed to read config file";
    return json::object();
  }

  // Check nesting depth before parsing (prevents stack overflow in parser).
  if (ConfigJsonNestingTooDeep(content)) {
    if (error) *error = "Config file JSON nesting exceeds depth limit";
    return json::object();
  }

  try {
    json j = json::parse(content);
    if (!j.is_object()) {
      if (error) *error = "Config file must contain a JSON object";
      return json::object();
    }
    return j;
  } catch (const json::exception& e) {
    if (error) *error = e.what();
    return json::object();
  }
}

bool WriteConfigFile(const std::string& path, const json& j) {
  if (path.empty()) {
    return false;
  }
  // 0644: world-readable config (not secret, unlike license).
  // Keep errno intact across the destruction of the serialized text, so the
  // caller reads the cause of the failure and not a side effect of freeing.
  bool ok;
  int saved_errno;
  {
    const std::string content = DumpJson(j, 2) + "\n";
    ok = AtomicWriteFile(path, content, 0644);
    saved_errno = errno;
  }
  errno = saved_errno;
  return ok;
}

}  // namespace pagespeed
