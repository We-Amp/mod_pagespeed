// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Writes each `*.html` file of an input directory, with its <noscript>
// elements removed by StripNoscriptElements (the removal the CSS coverage and
// the critical-CSS validation renders apply), to an output
// directory under the same name, and prints one line per file: how many were
// removed, whether the removal is reliable, and whether it agrees with the
// HtmlScanner's elements. noscript_compare.mjs drives it.
//
//   bazel build //tools/async-css-probe:strip_noscript
//   strip_noscript <input dir> <output dir>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "src/worker/html_scanner.h"
#include "src/worker/noscript_strip.h"

int main(int argc, char** argv) {
  namespace fs = std::filesystem;
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s <input dir> <output dir>\n", argv[0]);
    return 2;
  }
  int status = 0;
  for (const auto& entry : fs::directory_iterator(argv[1])) {
    if (entry.path().extension() != ".html") continue;
    std::ifstream in(entry.path(), std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string html = buffer.str();
    const pagespeed::NoscriptStripResult result =
        pagespeed::StripNoscriptElements(html);
    pagespeed::HtmlScanner scanner;
    const pagespeed::HtmlScanResult scan =
        scanner.Scan("http://example.com/", html);
    const bool agrees =
        pagespeed::NoscriptStripAgreesWithScan(result, scan.elements);
    std::printf("%s removed=%zu reliable=%d agrees=%d%s%s\n",
                entry.path().filename().string().c_str(), result.removed,
                result.reliable ? 1 : 0, agrees ? 1 : 0,
                result.reliable ? "" : " reason=",
                result.unreliable_reason.c_str());
    std::ofstream out(fs::path(argv[2]) / entry.path().filename(),
                      std::ios::binary);
    out << result.html;
    if (!out) status = 1;
  }
  return status;
}
