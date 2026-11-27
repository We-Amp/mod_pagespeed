// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Q3: what threshold should the critical-CSS validation gate use?
//
// The gate inherited VisualRegressionGate::kDefaultThreshold = 0.005 and had
// never been run against a real page.  This measures it, using the PRODUCTION
// classes end to end — the same coverage extraction, the same DOM-matched
// derivation, the same document synthesis, the same SSRF-hardened render, the
// same pixel comparison — so the number it prints is the number the shipped
// gate will see, not an approximation of it.
//
// It measures two blocks per viewport:
//
//   good    the block the extractor actually produces for the fixture
//   gutted  the same block with its layout-bearing rules removed, standing in
//           for the pre-PR-A extraction that caused the incident
//
// Run it manually, never in CI (it needs a browser and a byte-frozen fixture):
//
//   bazel build --config=opt //tools/async-css-probe:measure_validation_threshold
//   measure_validation_threshold \
//       --fixture tools/async-css-probe/fixtures/modpagespeed-com \
//       --chrome /path/to/chrome-headless-shell \
//       [--dump /dir]   # also writes each viewport's screenshots and the
//                       # derived block (<viewport>-block.css), so a block can
//                       # be read against the render that validated it
//
// The result table belongs in the PR description and in the comment beside
// kDefaultValidationDiffThreshold. Report the block and sheet byte sizes with
// it: a diff ratio without them says nothing about how much of the sheet the
// "good" block actually was.
//
// ONE DIVERGENCE FROM PRODUCTION, stated rather than hidden: the combined
// stylesheet is assembled here from files on disk (inline CSS, then each
// declared sheet in document order, newline-joined) instead of by
// Worker::BuildCombinedCss, which needs a live cache and a worker. That
// reproduces points 1, 3 and 4 of the byte contract next to
// CombinedCssValidationHash and NOT points 2/5 (@import flattening — the
// capture has no @import), 6 (the 10 MiB cap — the sheet is ~115 KB) or the
// minified-variant instability. It is the right bytes for THIS fixture and
// would not be for a fixture with imports. Nothing here writes a validation
// record, so no record can be bound to these bytes.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "src/browser/browser_css_extractor.h"
#include "src/browser/chrome_process.h"
#include "src/browser/critical_css_validator.h"
#include "src/browser/visual_regression_gate.h"
#include "src/worker/cascade_layer_order.h"
#include "src/worker/css_cache_inliner.h"
#include "src/worker/html_scanner.h"
#include "src/worker/layer_order_sources.h"
#include "uv.h"

namespace {

using pagespeed::BrowserCssResult;
using pagespeed::BuildValidationDocuments;
using pagespeed::ChromeProcess;
using pagespeed::ChromeProcessConfig;
using pagespeed::DeriveDomMatchedCriticalCss;
using pagespeed::HtmlScanner;
using pagespeed::HtmlScanResult;
using pagespeed::RegressionResult;
using pagespeed::ValidationDocuments;
using pagespeed::VisualRegressionGate;

struct Viewport {
  const char* name;
  uint32_t width;
  uint32_t height;
  pagespeed::CapabilityMask::Viewport enum_value;
};

constexpr Viewport kViewports[] = {
    {"mobile", 375, 667, pagespeed::CapabilityMask::Viewport::kMobile},
    {"tablet", 768, 1024, pagespeed::CapabilityMask::Viewport::kTablet},
    {"desktop", 1440, 900, pagespeed::CapabilityMask::Viewport::kDesktop},
};

std::string ReadFile(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return {};
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// Drive the loop until `done` or the budget runs out.
void Pump(uv_loop_t* loop, const bool& done, int max_ms = 120000) {
  for (int i = 0; i < max_ms && !done; ++i) {
    uv_run(loop, UV_RUN_NOWAIT);
    uv_sleep(1);
  }
}

// The pre-fix extraction shape: the block keeps its colour/typography rules and
// loses the layout ones. This is the failure the incident was — a thin block of
// colour overrides inlined while the fold's flex/grid/spacing rules were
// deferred with the sheet.
//
// It gutts DECLARATIONS, not whole rules, and it understands at-rule nesting.
// A naive "drop any rule mentioning display:" pass over a Tailwind sheet — one
// giant `@layer utilities { ... }` — matches the layer's opening brace against
// the first inner rule's closing one and emits brace-unbalanced garbage. CSS a
// browser rejects wholesale is not a thin critical block; it is no critical
// block, and the number it produces measures nothing.
bool IsLayoutProperty(std::string_view name) {
  static constexpr std::string_view kExact[] = {
      "display",        "position",   "top",
      "right",          "bottom",     "left",
      "float",          "clear",      "order",
      "box-sizing",     "width",      "height",
      "min-width",      "min-height", "max-width",
      "max-height",     "flex",       "flex-basis",
      "flex-direction", "flex-flow",  "flex-grow",
      "flex-shrink",    "flex-wrap",  "gap",
      "row-gap",        "column-gap",
  };
  static constexpr std::string_view kPrefixes[] = {
      "margin", "padding",  "inset",  "grid",
      "align-", "justify-", "place-", "overflow",
  };
  for (std::string_view p : kExact) {
    if (name == p) return true;
  }
  for (std::string_view p : kPrefixes) {
    if (name.size() >= p.size() && name.compare(0, p.size(), p) == 0) {
      return true;
    }
  }
  return false;
}

std::string_view Trim(std::string_view s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string_view::npos) return {};
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

// Index of the '}' matching the '{' at `open`, tracking nesting, strings and
// comments. npos when unbalanced.
size_t MatchBrace(std::string_view css, size_t open) {
  int depth = 0;
  char quote = '\0';
  for (size_t i = open; i < css.size(); ++i) {
    char c = css[i];
    if (quote != '\0') {
      if (c == '\\') {
        ++i;
      } else if (c == quote) {
        quote = '\0';
      }
      continue;
    }
    if (c == '"' || c == '\'') {
      quote = c;
      continue;
    }
    if (c == '/' && i + 1 < css.size() && css[i + 1] == '*') {
      size_t end = css.find("*/", i + 2);
      if (end == std::string_view::npos) return std::string_view::npos;
      i = end + 1;
      continue;
    }
    if (c == '{') {
      ++depth;
    } else if (c == '}') {
      if (--depth == 0) return i;
    }
  }
  return std::string_view::npos;
}

bool IsContainerAtRule(std::string_view prelude) {
  std::string_view p = Trim(prelude);
  if (p.empty() || p[0] != '@') return false;
  static constexpr std::string_view kContainers[] = {
      "@media", "@layer", "@supports", "@container", "@scope"};
  for (std::string_view c : kContainers) {
    if (p.size() >= c.size() && p.compare(0, c.size(), c) == 0) return true;
  }
  return false;
}

// Drop layout declarations from a style rule's body (the text between braces),
// leaving the rule itself intact.
std::string GutDeclarations(std::string_view body) {
  std::string out;
  size_t pos = 0;
  while (pos < body.size()) {
    size_t end = body.find(';', pos);
    std::string_view decl = end == std::string_view::npos
                                ? body.substr(pos)
                                : body.substr(pos, end - pos);
    std::string_view trimmed = Trim(decl);
    if (!trimmed.empty()) {
      size_t colon = trimmed.find(':');
      std::string_view name = colon == std::string_view::npos
                                  ? trimmed
                                  : Trim(trimmed.substr(0, colon));
      if (!IsLayoutProperty(name)) {
        out.append(trimmed);
        out.push_back(';');
      }
    }
    if (end == std::string_view::npos) break;
    pos = end + 1;
  }
  return out;
}

std::string GutLayoutRules(std::string_view css) {
  std::string out;
  size_t pos = 0;
  while (pos < css.size()) {
    size_t open = css.find('{', pos);
    if (open == std::string_view::npos) {
      out.append(css.substr(pos));
      break;
    }
    size_t close = MatchBrace(css, open);
    if (close == std::string_view::npos) {
      // Unbalanced input: copy the remainder rather than invent structure.
      out.append(css.substr(pos));
      break;
    }
    std::string_view prelude = css.substr(pos, open - pos);
    std::string_view body = css.substr(open + 1, close - open - 1);
    out.append(prelude);
    out.push_back('{');
    if (IsContainerAtRule(prelude)) {
      out.append(GutLayoutRules(body));  // recurse; nesting preserved
    } else {
      out.append(GutDeclarations(body));
    }
    out.push_back('}');
    pos = close + 1;
  }
  return out;
}

// Net brace balance; 0 means the gutting produced structurally valid CSS.
int BraceBalance(std::string_view css) {
  int depth = 0;
  char quote = '\0';
  for (size_t i = 0; i < css.size(); ++i) {
    char c = css[i];
    if (quote != '\0') {
      if (c == '\\') {
        ++i;
      } else if (c == quote) {
        quote = '\0';
      }
      continue;
    }
    if (c == '"' || c == '\'') {
      quote = c;
      continue;
    }
    if (c == '{') ++depth;
    if (c == '}') --depth;
  }
  return depth;
}

struct Measurement {
  bool ok = false;
  float diff_ratio = -1.0f;
  uint32_t diff_pixels = 0;
  uint32_t total_pixels = 0;
  std::string error;
  double wall_ms = 0.0;
};

Measurement MeasureOne(uv_loop_t* loop, pagespeed::CdpClient* client,
                       const ValidationDocuments& docs, const Viewport& vp) {
  Measurement m;
  if (!docs.ok) {
    m.error = docs.error;
    return m;
  }
  uint64_t start = uv_hrtime();
  bool done = false;
  VisualRegressionGate gate(client);
  gate.Compare(
      docs.reference, docs.candidate, vp.width, vp.height,
      [&](absl::StatusOr<RegressionResult> r) {
        if (!r.ok()) {
          m.error = std::string(r.status().message());
        } else {
          m.ok = true;
          m.diff_ratio = r->diff_ratio;
          m.diff_pixels = r->diff_pixels;
          m.total_pixels = r->total_pixels;
        }
        done = true;
      },
      // Threshold does not affect the measured ratio; the verdicts are derived
      // from the ratio below, at every candidate threshold at once.
      /*threshold=*/1.0f);
  Pump(loop, done);
  m.wall_ms = static_cast<double>(uv_hrtime() - start) / 1e6;
  if (!done && m.error.empty()) m.error = "timed out";
  return m;
}

const char* Verdict(const Measurement& m, float threshold) {
  if (!m.ok) return "n/a";
  return m.diff_ratio <= threshold ? "VALIDATED" : "refused";
}

// Capture one document on its own and write the PNG, so a suspicious ratio can
// be looked at rather than argued about.
void DumpShot(uv_loop_t* loop, pagespeed::CdpClient* client,
              const std::string& doc, const Viewport& vp,
              const std::string& path) {
  bool done = false;
  VisualRegressionGate gate(client);
  gate.CaptureScreenshot(
      doc, vp.width, vp.height,
      [&](absl::StatusOr<pagespeed::ScreenshotResult> r) {
        if (r.ok()) {
          std::ofstream out(path, std::ios::binary);
          out.write(reinterpret_cast<const char*>(r->png_data.data()),
                    static_cast<std::streamsize>(r->png_data.size()));
          std::fprintf(stdout, "    wrote %s (%zu B)\n", path.c_str(),
                       r->png_data.size());
        } else {
          std::fprintf(stdout, "    capture failed: %s\n",
                       std::string(r.status().message()).c_str());
        }
        done = true;
      });
  Pump(loop, done);
}

}  // namespace

int main(int argc, char** argv) {
  std::string fixture_dir = "tools/async-css-probe/fixtures/modpagespeed-com";
  std::string chrome_path = "/usr/bin/chrome-headless-shell";
  const char* dump_dir = nullptr;
  for (int i = 1; i + 1 < argc; i += 2) {
    if (std::strcmp(argv[i], "--fixture") == 0) fixture_dir = argv[i + 1];
    if (std::strcmp(argv[i], "--chrome") == 0) chrome_path = argv[i + 1];
    if (std::strcmp(argv[i], "--dump") == 0) dump_dir = argv[i + 1];
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

  const std::string page_url = "https://modpagespeed.com/";

  // Event-loop cost. Browser analysis runs on the main libuv loop, so every
  // synchronous step the confirmation adds is a stall for everything else on
  // it. Measured, not assumed.
  uint64_t t0 = uv_hrtime();
  HtmlScanner scanner;
  HtmlScanResult scan = scanner.Scan(page_url, html);
  double scan_ms = static_cast<double>(uv_hrtime() - t0) / 1e6;
  if (!scan.success) {
    std::fprintf(stderr, "scan failed: %s\n", scan.error_message.c_str());
    return 2;
  }

  // The combined sheet, assembled the way the serve path assembles it: inline
  // CSS first, then each declared sheet in document order, newline-joined.
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
  // The page's cascade-layer order, computed the way Worker::BuildCombinedCss
  // computes it (as dump_critical_css does). No fixture has an @import, so an
  // import would be reported as missing.
  const pagespeed::CascadeLayerOrder layer_order =
      pagespeed::ComputeCascadeLayerOrder(
          pagespeed::BuildLayerOrderSources(scan.stylesheet_sources, page_url,
                                            gathered),
          {});
  double assemble_ms = static_cast<double>(uv_hrtime() - t0) / 1e6;
  std::fprintf(stdout, "page %zu B, combined stylesheet %zu B\n", html.size(),
               combined_css.size());
  std::fprintf(stdout,
               "event-loop cost: scan %.2f ms, scan+sheet assembly %.2f ms\n",
               scan_ms, assemble_ms);

  uv_loop_t loop;
  uv_loop_init(&loop);

  ChromeProcessConfig chrome_config;
  chrome_config.chrome_path = chrome_path;
  chrome_config.startup_timeout_ms = 30000;
  chrome_config.recycle_after_pages = 10000;
  ChromeProcess chrome(&loop, chrome_config);
  auto status = chrome.Start();
  if (!status.ok()) {
    std::fprintf(stderr, "chrome failed to start: %s\n",
                 std::string(status.message()).c_str());
    return 2;
  }

  // The coverage render needs the stylesheet in the document, exactly as the
  // analysis pipeline arranges it. Timed as the BASELINE the confirmation's own
  // event-loop cost has to be judged against: this step already runs
  // synchronously on the same loop, for every analysed page, today.
  uint64_t t_inline = uv_hrtime();
  std::string coverage_html =
      pagespeed::InlineCachedStylesheets(html, page_url, lookup);
  std::fprintf(stdout,
               "event-loop baseline (already on the loop today): "
               "InlineCachedStylesheets %.2f ms\n",
               static_cast<double>(uv_hrtime() - t_inline) / 1e6);

  std::fprintf(stdout, "\n%-8s %-8s %10s %10s %12s %12s %10s\n", "viewport",
               "block", "diff_ratio", "diff_px", "@0.005", "@0.002", "wall_ms");

  double total_wall = 0.0;
  int renders = 0;
  for (const Viewport& vp : kViewports) {
    // Production step 1: Chrome CSS coverage at this viewport.
    bool extracted = false;
    BrowserCssResult coverage;
    pagespeed::BrowserCssExtractor extractor(chrome.cdp_client());
    extractor.Extract(
        coverage_html, vp.width, vp.height,
        [&](absl::StatusOr<BrowserCssResult> r) {
          if (r.ok()) coverage = std::move(*r);
          extracted = true;
        },
        60000);
    Pump(&loop, extracted);
    ++renders;

    // Production step 2: the DOM-matched derivation the serve path inlines.
    uint64_t t1 = uv_hrtime();
    // No measured fold: this tool has no ViewportProfile, so it derives
    // against the extractor's own fold estimate, explicitly.
    std::string good =
        DeriveDomMatchedCriticalCss(
            scan.elements, combined_css, coverage.critical_css, vp.enum_value,
            /*measured_above_fold_selectors=*/{}, layer_order)
            .critical_css;
    double derive_ms = static_cast<double>(uv_hrtime() - t1) / 1e6;
    if (dump_dir != nullptr) {
      const std::string path =
          std::string(dump_dir) + "/" + vp.name + "-block.css";
      std::ofstream out(path, std::ios::binary);
      out << good;
      std::fprintf(stdout, "  [%s] wrote %s (%zu B)\n", vp.name, path.c_str(),
                   good.size());
    }
    uint64_t t2 = uv_hrtime();
    ValidationDocuments timing_docs =
        BuildValidationDocuments(html, combined_css, good);
    double documents_ms = static_cast<double>(uv_hrtime() - t2) / 1e6;
    (void)timing_docs;
    std::fprintf(stdout,
                 "  [%s] event-loop cost: derive %.2f ms, documents %.2f ms\n",
                 vp.name, derive_ms, documents_ms);
    std::string gutted = GutLayoutRules(good);

    struct Case {
      const char* name;
      const std::string& block;
    } cases[] = {{"good", good}, {"gutted", gutted}};

    for (const Case& c : cases) {
      ValidationDocuments docs =
          BuildValidationDocuments(html, combined_css, c.block);
      std::fprintf(stdout,
                   "  [%s/%s] ref %zu B, cand %zu B, links stripped %zu, "
                   "styles stripped %zu, coverage %.3f (%zu B)\n",
                   vp.name, c.name, docs.reference.size(),
                   docs.candidate.size(), docs.stylesheet_links_removed,
                   docs.style_blocks_removed, coverage.coverage_ratio,
                   coverage.critical_css.size());
      Measurement m = MeasureOne(&loop, chrome.cdp_client(), docs, vp);
      std::fprintf(stdout, "  [%s/%s] total_pixels %u\n", vp.name, c.name,
                   m.total_pixels);
      if (dump_dir != nullptr) {
        DumpShot(&loop, chrome.cdp_client(), docs.reference, vp,
                 std::string(dump_dir) + "/" + vp.name + "-" + c.name +
                     "-reference.png");
        DumpShot(&loop, chrome.cdp_client(), docs.candidate, vp,
                 std::string(dump_dir) + "/" + vp.name + "-" + c.name +
                     "-candidate.png");
      }
      total_wall += m.wall_ms;
      renders += 2;
      if (!m.ok) {
        std::fprintf(stdout, "%-8s %-8s %10s %10s %12s %12s %10.0f  (%s)\n",
                     vp.name, c.name, "-", "-", "-", "-", m.wall_ms,
                     m.error.c_str());
        continue;
      }
      std::fprintf(stdout, "%-8s %-8s %10.5f %10u %12s %12s %10.0f\n", vp.name,
                   c.name, m.diff_ratio, m.diff_pixels, Verdict(m, 0.005f),
                   Verdict(m, 0.002f), m.wall_ms);
      std::fflush(stdout);
    }
    std::fprintf(
        stdout,
        "  (%s: block %zu B good / %zu B gutted of a %zu B sheet; gutted brace "
        "balance %d)\n",
        vp.name, good.size(), gutted.size(), combined_css.size(),
        BraceBalance(gutted));
  }

  std::fprintf(stdout,
               "\nvalidation wall-clock: %.0f ms over %d comparisons "
               "(%d renders incl. coverage)\n",
               total_wall, 6, renders);

  chrome.Stop();
  uv_run(&loop, UV_RUN_DEFAULT);
  uv_loop_close(&loop);
  return 0;
}
