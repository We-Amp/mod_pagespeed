// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// 2. Fetch.enable + Fetch.failRequest for all requests — intercept layer
// 3. --host-resolver-rules="MAP * ~NOTFOUND" on Chrome — blocks DNS
// JavaScript is NOT disabled (unlike CSS extractor) — need JS for
// accurate text rendering. Content is loaded via Page.setDocumentContent.

#ifndef PAGESPEED_SRC_BROWSER_FONT_GLYPH_SCANNER_H_
#define PAGESPEED_SRC_BROWSER_FONT_GLYPH_SCANNER_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"

namespace pagespeed {

class CdpClient;

// Usage information for a single @font-face declaration.
struct FontUsage {
  std::string family;   // Font family name
  std::string src_url;  // @font-face src URL
  std::string format;   // "woff2", "woff", "truetype"
  std::string weight;   // e.g. "400", "700"
  std::string style;    // "normal", "italic"
  std::string
      unicode_range;  // Used glyphs from page (same for all fonts; see CLAUDE.md limitations)
  size_t total_glyphs =
      0;  // Glyphs in original font (requires font parsing; currently 0)
  size_t used_glyphs = 0;  // Glyphs actually used on page
};

// Complete font glyph scanning result.
struct FontGlyphResult {
  std::vector<FontUsage> fonts;
  std::string all_used_codepoints;  // Union across all fonts
  size_t total_text_nodes = 0;
};

// Scans a page in headless Chrome to identify font usage and the
// Unicode code points actually rendered in text nodes.
//
// Usage:
//   FontGlyphScanner scanner(cdp_client);
//   scanner.Scan(html, 1440, 900,
//       [](absl::StatusOr<FontGlyphResult> result) {
//         if (result.ok()) { /* use result->fonts */ }
//       });
//
// Each Scan() call creates a new browser tab, runs the analysis,
// and closes the tab. Only one scan may run at a time per CdpClient
// (the event callback is single-subscriber).
class FontGlyphScanner {
 public:
  using Callback = std::function<void(absl::StatusOr<FontGlyphResult>)>;

  explicit FontGlyphScanner(CdpClient* client);
  ~FontGlyphScanner() = default;

  FontGlyphScanner(const FontGlyphScanner&) = delete;
  FontGlyphScanner& operator=(const FontGlyphScanner&) = delete;

  // Default overall session timeout (60 seconds).
  static constexpr uint32_t kDefaultTimeoutMs = 60000;

  // Scan a page for font usage at a specific viewport.
  // html_content: full HTML to render.
  // The callback is invoked on the main event loop when scanning
  // completes or fails. timeout_ms is an overall session timeout
  // (0 = no timeout).
  void Scan(std::string_view html_content, uint32_t viewport_width,
            uint32_t viewport_height, Callback callback,
            uint32_t timeout_ms = kDefaultTimeoutMs);

  // Convert a set of code points to a compact unicode-range string.
  // Groups consecutive code points into ranges:
  //   {0x20, 0x21, ..., 0x7E} -> "U+0020-007E"
  //   {0x41} -> "U+0041"
  static std::string CodePointsToUnicodeRange(
      const std::vector<uint32_t>& codepoints);

  // How long after the page's load event the render collects when the page
  // never reaches networkIdle (kSettleAfterLoadMs). For tests.
  void set_settle_after_load_ms_for_testing(uint64_t ms) {
    settle_after_load_ms_ = ms;
  }

 private:
  struct Session;
  CdpClient* client_;
  uint64_t settle_after_load_ms_;
};

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_BROWSER_FONT_GLYPH_SCANNER_H_
