// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - stylesheet sources as cascade-layer inputs. See the header.

#include "src/worker/layer_order_sources.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/strings/str_cat.h"
#include "src/worker/cascade_layer_order.h"
#include "src/worker/html_scanner.h"

namespace pagespeed {

std::vector<LayerOrderSource> BuildLayerOrderSources(
    const std::vector<StylesheetSource>& sources, std::string_view page_url,
    std::vector<std::optional<GatheredSheet>>& gathered) {
  std::vector<LayerOrderSource> out;
  out.reserve(sources.size());
  for (const StylesheetSource& source : sources) {
    LayerOrderSource src;
    if (source.may_insert_stylesheet) {
      src.is_script = true;
      out.push_back(std::move(src));
      continue;
    }
    src.conditional = !LayerOrderMediaIsUnconditional(source.media);
    if (source.titled) {
      src.unavailable_reason =
          "a titled stylesheet belongs to a sheet set, and a browser applies "
          "only the preferred set";
    } else if (!source.is_link) {
      if (source.truncated) {
        src.unavailable_reason =
            "an inline <style> is larger than the scanner keeps";
      } else {
        src.available = true;
        src.css = source.css;
        src.base_url = std::string(page_url);
      }
    } else if (source.href.empty()) {
      continue;  // loads nothing
    } else if (source.not_plain_stylesheet) {
      src.unavailable_reason = absl::StrCat(
          "<link href=\"", source.href,
          "\"> is not a plain active stylesheet (an alternate or a disabled "
          "sheet)");
    } else if (source.stylesheet_index < 0 ||
               static_cast<size_t>(source.stylesheet_index) >=
                   gathered.size() ||
               !gathered[source.stylesheet_index].has_value()) {
      src.unavailable_reason =
          absl::StrCat("the sheet \"", source.href,
                       "\" was not gathered (cross-origin, not cached yet, or "
                       "over the combined-CSS cap)");
    } else {
      GatheredSheet& sheet = *gathered[source.stylesheet_index];
      src.available = true;
      src.css = std::move(sheet.css);
      src.base_url = std::move(sheet.url);
      gathered[source.stylesheet_index].reset();
    }
    out.push_back(std::move(src));
  }
  return out;
}

}  // namespace pagespeed
