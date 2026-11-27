// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - device emulation for the browser-analysis renders.
//
// See the header for the rule and where its values come from.

#include "src/browser/device_emulation.h"

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>

#include "lib/classify/capability_mask.h"
#include "nlohmann/json.hpp"

namespace pagespeed {

DeviceEmulation EmulationForViewportWidth(uint32_t width) {
  DeviceEmulation emulation;
  emulation.mobile = width <= kMobileEmulationMaxWidth;
  emulation.has_touch = emulation.mobile && kMobileEmulatesTouch;
  if (emulation.mobile) {
    emulation.user_agent = width <= kPhoneMaxWidth
                               ? DeviceEmulation::UserAgent::kAndroidPhone
                               : DeviceEmulation::UserAgent::kAndroidTablet;
  }
  return emulation;
}

DeviceEmulation EmulationForViewport(CapabilityMask::Viewport viewport) {
  switch (viewport) {
    case CapabilityMask::Viewport::kMobile:
      return EmulationForViewportWidth(kAnalysisViewportWidths[0]);
    case CapabilityMask::Viewport::kTablet:
      return EmulationForViewportWidth(kAnalysisViewportWidths[1]);
    case CapabilityMask::Viewport::kDesktop:
      break;
  }
  return EmulationForViewportWidth(kAnalysisViewportWidths[2]);
}

nlohmann::json DeviceMetricsOverrideParams(uint32_t width, uint32_t height) {
  return {
      {"width", width},
      {"height", height},
      {"deviceScaleFactor", 1},
      {"mobile", EmulationForViewportWidth(width).mobile},
  };
}

nlohmann::json TouchEmulationParams(uint32_t width) {
  return {
      {"enabled", EmulationForViewportWidth(width).has_touch},
      {"maxTouchPoints", kMaxTouchPoints},
  };
}

std::optional<nlohmann::json> UserAgentOverrideParams(uint32_t width) {
  bool phone = false;
  switch (EmulationForViewportWidth(width).user_agent) {
    case DeviceEmulation::UserAgent::kBrowserDefault:
      return std::nullopt;
    case DeviceEmulation::UserAgent::kAndroidPhone:
      phone = true;
      break;
    case DeviceEmulation::UserAgent::kAndroidTablet:
      break;
  }
  return nlohmann::json{
      {"userAgent", phone ? kPhoneUserAgent : kTabletUserAgent},
      {"platform", kAndroidNavigatorPlatform},
      {"userAgentMetadata",
       {
           {"brands", nlohmann::json::array(
                          {{{"brand", kClientHintsBrand},
                            {"version", kClientHintsBrandVersion}},
                           {{"brand", kClientHintsGreaseBrand},
                            {"version", kClientHintsGreaseBrandVersion}}})},
           {"platform", kAndroidClientHintsPlatform},
           {"platformVersion", ""},
           {"architecture", ""},
           {"model", ""},
           {"mobile", phone},
       }},
  };
}

namespace device_emulation_internal {

std::string StringField(const nlohmann::json& json,
                        std::initializer_list<const char*> path) {
  const nlohmann::json* current = &json;
  for (const char* key : path) {
    if (!current->is_object()) return {};
    auto it = current->find(key);
    if (it == current->end()) return {};
    current = &*it;
  }
  return current->is_string() ? current->get<std::string>() : std::string();
}

}  // namespace device_emulation_internal

}  // namespace pagespeed
