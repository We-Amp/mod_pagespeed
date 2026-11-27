// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Font URL Extractor
//
// Extracts font URLs from CSS @font-face blocks. Prefers woff2 format,
// deduplicates, and caps at a maximum count. Used for font preload
// injection and Early Hints.

#ifndef PAGESPEED_SRC_WORKER_FONT_URL_EXTRACTOR_H_
#define PAGESPEED_SRC_WORKER_FONT_URL_EXTRACTOR_H_

#include <string>
#include <string_view>
#include <vector>

namespace pagespeed {

// Extract font URLs from CSS content by scanning @font-face blocks.
// Prefers woff2 format when multiple sources are listed.
// Deduplicates and caps at kMaxFontUrls (10).
// Skips data: URLs.
std::vector<std::string> ExtractFontUrls(std::string_view css);

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_WORKER_FONT_URL_EXTRACTOR_H_
