// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - the scanner's stylesheet sources as cascade-layer inputs
//
// Turns HtmlScanResult::stylesheet_sources plus the sheets the combined-CSS
// gather read into the document-order input of ComputeCascadeLayerOrder.
// Kept apart from both ends so the worker and the tests build
// the input the same way.

#ifndef PAGESPEED_SRC_WORKER_LAYER_ORDER_SOURCES_H_
#define PAGESPEED_SRC_WORKER_LAYER_ORDER_SOURCES_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "src/worker/cascade_layer_order.h"
#include "src/worker/html_scanner.h"

namespace pagespeed {

// A sheet the combined-CSS gather read: its CSS as fetched (before @import
// flattening) and its resolved URL.
struct GatheredSheet {
  std::string css;
  std::string url;
};

// One LayerOrderSource per stylesheet source, in document order:
//   - a <style>: its body, resolved against `page_url` (the document base URL:
//     the page URL or its <base href>); unavailable when the
//     scanner truncated it;
//   - a <link> with no href loads nothing and is left out;
//   - a <link> that is not a plain active stylesheet, and any titled source
//     (a sheet set), is unavailable;
//   - a script that may insert a stylesheet becomes an is_script entry;
//   - any other <link> is available when `gathered[stylesheet_index]` holds
//     its sheet, and unavailable (cross-origin, not cached yet, over the cap)
//     otherwise.
// A source is conditional under a media attribute other than `all`. `gathered` is indexed like HtmlScanResult::stylesheets; the sheets are
// moved out of it.
std::vector<LayerOrderSource> BuildLayerOrderSources(
    const std::vector<StylesheetSource>& sources, std::string_view page_url,
    std::vector<std::optional<GatheredSheet>>& gathered);

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_WORKER_LAYER_ORDER_SOURCES_H_
