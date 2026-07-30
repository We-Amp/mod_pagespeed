// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) 2026 We-Amp B.V.
//
// Memory-safety harness for the HTML lexer/parser.
//
// pagespeed/kernel/html/html_lexer.cc is a hand-written state machine that
// runs on *untrusted* HTML. Build with
//
//     bazel build --config=macos-asan -c opt //pagespeed/kernel/html:html_fuzz
//
// so DCHECKs are OFF (release behaviour) and AddressSanitizer is the oracle.
//
// One source, two build modes (mirrors third_party/css_parser parser_fuzz):
//   * default                  -> deterministic main() that replays every
//                                 file named on the command line (e.g.
//                                 tools/html-parse-corpus/seeds/*) through
//                                 the probe path. Hermetic; CI-friendly.
//   * -DHTML_PARSE_LIBFUZZER   -> exposes LLVMFuzzerTestOneInput for
//                                 coverage-guided discovery when linked
//                                 with -fsanitize=fuzzer.
//
// The entry point exercises the same path as html_parse_probe (parse +
// recording filter, SPEC.md session setup); the stream is computed and
// discarded. The sanitizer — not the output — is the oracle here; the
// goldens gate output equivalence.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/html/html_parse_recorder.h"

namespace {

void ParseOne(const char* data, size_t size) {
  GoogleString stream;
  net_instaweb::html_parse_probe::RunParseProbe(data, size, &stream);
}

}  // namespace

#ifdef HTML_PARSE_LIBFUZZER
// Coverage-guided entry point. Link with -fsanitize=fuzzer,address.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  ParseOne(reinterpret_cast<const char*>(data), size);
  return 0;
}
#else
int main(int argc, char** argv) {
  size_t n = 0;
  for (int i = 1; i < argc; ++i) {
    FILE* f = std::fopen(argv[i], "rb");
    if (f == nullptr) {
      std::fprintf(stderr, "skip: cannot open %s\n", argv[i]);
      continue;
    }
    GoogleString buf;
    char chunk[65536];
    size_t got;
    while ((got = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
      buf.append(chunk, got);
    }
    std::fclose(f);
    ParseOne(buf.data(), buf.size());
    ++n;
  }
  std::fprintf(stderr, "html parse fuzz harness: %zu inputs parsed clean\n", n);
  return 0;
}
#endif
