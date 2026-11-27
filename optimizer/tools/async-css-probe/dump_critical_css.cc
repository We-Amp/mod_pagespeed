// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// What does the heuristic critical-CSS extractor inline for a fixture?
//
// The rendered probe (probe_rendered.mjs) measures the fold with the deferred
// stylesheet not applying, i.e. with ONLY the block this extractor produced.
// When that measurement reports a flash, the first question is what the block
// contains, and the answer is buried in a served page behind a warm stack. This
// prints it directly, using the production classes end to end — the same
// scanner, the same combined-stylesheet assembly, the same extractor — so the
// bytes it prints are the bytes the worker inlines for that fixture when no
// browser profile exists (the probe stack has no browser, so that is always its
// case).
//
//   bazel build --config=opt //tools/async-css-probe:dump_critical_css
//   dump_critical_css --fixture tools/async-css-probe/fixtures/modpagespeed-com \
//       [--viewport mobile|tablet|desktop] [--max-elements N]
//       [--max-depth N] [--max-wholesale-media-bytes N]
//       [--layer-order computed|unknown] > critical.css
//
// Statistics (block bytes, rule counts, where <body> sits in the element list)
// go to stderr so stdout is the block and nothing else.
//
// Same divergence from production as measure_validation_threshold.cc, stated
// rather than hidden: the combined stylesheet is assembled from files on disk
// (inline CSS, then each declared sheet in document order, newline-joined)
// instead of by Worker::BuildCombinedCss. Right for this fixture (no @import,
// one sheet far under the cap); not for one with imports.
//
// The cascade-layer order is computed the way BuildCombinedCss computes it
// (BuildLayerOrderSources + ComputeCascadeLayerOrder, every declared sheet
// read), because it decides which @media blocks the block keeps.
// Pass --layer-order unknown to extract as if it were not proven.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "absl/strings/match.h"
#include "lib/classify/capability_mask.h"
#include "src/worker/cascade_layer_order.h"
#include "src/worker/critical_css_extractor.h"
#include "src/worker/html_scanner.h"
#include "src/worker/layer_order_sources.h"

namespace {

std::string ReadFile(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return {};
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

}  // namespace

int main(int argc, char** argv) {
  std::string fixture_dir = "tools/async-css-probe/fixtures/modpagespeed-com";
  pagespeed::CapabilityMask::Viewport viewport =
      pagespeed::CapabilityMask::Viewport::kDesktop;
  pagespeed::CriticalCssConfig config;
  bool compute_layer_order = true;
  auto parse_int = [](const char* flag, const char* text, int& out) {
    char* end = nullptr;
    long value = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || value < 0 || value > 1000000) {
      std::fprintf(stderr, "%s: not a non-negative integer: %s\n", flag, text);
      return false;
    }
    out = static_cast<int>(value);
    return true;
  };
  if (argc % 2 == 0) {
    std::fprintf(stderr, "%s takes a value\n", argv[argc - 1]);
    return 2;
  }
  for (int i = 1; i + 1 < argc; i += 2) {
    if (std::strcmp(argv[i], "--fixture") == 0) {
      fixture_dir = argv[i + 1];
    } else if (std::strcmp(argv[i], "--viewport") == 0) {
      std::string_view v = argv[i + 1];
      if (v == "mobile") {
        viewport = pagespeed::CapabilityMask::Viewport::kMobile;
      } else if (v == "tablet") {
        viewport = pagespeed::CapabilityMask::Viewport::kTablet;
      } else if (v == "desktop") {
        viewport = pagespeed::CapabilityMask::Viewport::kDesktop;
      } else {
        std::fprintf(stderr, "unknown viewport: %s\n", argv[i + 1]);
        return 2;
      }
    } else if (std::strcmp(argv[i], "--layer-order") == 0) {
      std::string_view v = argv[i + 1];
      if (v == "computed") {
        compute_layer_order = true;
      } else if (v == "unknown") {
        compute_layer_order = false;
      } else {
        std::fprintf(stderr, "unknown --layer-order: %s\n", argv[i + 1]);
        return 2;
      }
    } else if (std::strcmp(argv[i], "--max-elements") == 0) {
      if (!parse_int(argv[i], argv[i + 1], config.max_elements)) return 2;
    } else if (std::strcmp(argv[i], "--max-depth") == 0) {
      if (!parse_int(argv[i], argv[i + 1], config.max_depth)) return 2;
    } else if (std::strcmp(argv[i], "--max-wholesale-media-bytes") == 0) {
      if (!parse_int(argv[i], argv[i + 1], config.max_wholesale_media_bytes)) {
        return 2;
      }
    } else {
      std::fprintf(stderr, "unknown option: %s\n", argv[i]);
      return 2;
    }
  }

  std::filesystem::path dir(fixture_dir);
  std::string html = ReadFile(dir / "index.html");
  if (html.empty()) {
    std::fprintf(stderr, "cannot read %s/index.html\n", fixture_dir.c_str());
    return 2;
  }

  // The fixture mirrors the site's URL space, so a declared href is a path
  // under the capture directory.
  auto lookup = [&dir](std::string_view url) -> std::optional<std::string> {
    std::string rel(url);
    while (!rel.empty() && rel.front() == '/') rel.erase(0, 1);
    size_t q = rel.find('?');
    if (q != std::string::npos) rel.resize(q);
    std::string body = ReadFile(dir / rel);
    if (body.empty()) return std::nullopt;
    return body;
  };

  pagespeed::HtmlScanner scanner;
  pagespeed::HtmlScanResult scan =
      scanner.Scan("https://modpagespeed.com/", html);
  if (!scan.success) {
    std::fprintf(stderr, "scan failed: %s\n", scan.error_message.c_str());
    return 2;
  }

  std::string combined_css = scan.inline_css;
  std::vector<std::optional<pagespeed::GatheredSheet>> gathered(
      scan.stylesheets.size());
  for (size_t n = 0; n < scan.stylesheets.size(); ++n) {
    const auto& sheet = scan.stylesheets[n];
    if (sheet.href.empty()) continue;
    auto body = lookup(sheet.href);
    if (!body.has_value()) {
      std::fprintf(stderr, "stylesheet not in fixture: %s\n",
                   sheet.href.c_str());
      return 2;
    }
    if (!combined_css.empty()) combined_css.append("\n");
    combined_css.append(*body);
    gathered[n].emplace(pagespeed::GatheredSheet{*body, sheet.href});
  }

  pagespeed::CascadeLayerOrder layer_order;
  if (compute_layer_order) {
    std::vector<pagespeed::LayerOrderSource> sources =
        pagespeed::BuildLayerOrderSources(
            scan.stylesheet_sources, "https://modpagespeed.com/", gathered);
    // No fixture has an @import: an import is reported as missing.
    layer_order = pagespeed::ComputeCascadeLayerOrder(sources, {});
  }

  int body_index = -1;
  for (const auto& element : scan.elements) {
    if (absl::EqualsIgnoreCase(element.tag_name, "body")) {
      body_index = element.element_index;
      break;
    }
  }

  pagespeed::CriticalCssExtractor extractor(config);
  pagespeed::CriticalCssResult result =
      extractor.Extract(scan.elements, combined_css, viewport,
                        /*force_include=*/nullptr, &layer_order);
  if (!result.success) {
    std::fprintf(stderr, "extraction failed: %s\n",
                 result.error_message.c_str());
    return 2;
  }

  // critical_rules counts every retained style rule at any nesting depth;
  // total_rules counts the sheet's TOP-LEVEL rules only (a Tailwind sheet is
  // five @layer blocks), so the two are reported apart, not as a fraction.
  std::fprintf(stderr,
               "elements %zu (<body> at index %d), combined stylesheet %zu B "
               "(%d top-level rules), critical block %zu B (%d rules retained, "
               "counted at every nesting depth)\n",
               scan.elements.size(), body_index, combined_css.size(),
               result.total_rules, result.critical_css.size(),
               result.critical_rules);
  std::fprintf(stderr, "layer order %s%s, @media retained for %s\n",
               layer_order.proven ? "proven: " : "not proven: ",
               layer_order.proven ? layer_order.Statement().c_str()
                                  : layer_order.reason.c_str(),
               result.class_range_retention ? "the device class's windows"
                                            : "every width");
  std::fwrite(result.critical_css.data(), 1, result.critical_css.size(),
              stdout);
  return 0;
}
