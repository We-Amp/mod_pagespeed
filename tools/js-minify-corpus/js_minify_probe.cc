// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// js_minify_probe: single-file front-end for the JS minifiers, built for
// corpus stress runs (see tools/js-minify-corpus/README.md).
//
// Unlike net/instaweb/rewriter/js_minify_main.cc (a user-facing CLI that
// replaces failed minifications with a trimmed copy of the input), this probe
// exposes the raw library contract so a harness can observe it:
//   * exit 0: minifier returned true, output is the minified bytes.
//   * exit 1: minifier returned false ("cannot model this input"). The output
//     file is still fully populated: minified prefix + unmodified remainder,
//     exactly as MinifyUtf8Js specifies.
//   * exit 2: usage or I/O error.
//   * crashes (asserts/segfaults) propagate as signals for the harness to
//     record.
//
// Usage:
//   js_minify_probe --mode=tokenizer <in.js> <out.js>
//
// A single machine-readable line is printed to stdout:
//   mode=tokenizer result=ok in_bytes=123 out_bytes=45

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/js/js_minify.h"
#include "pagespeed/kernel/js/js_tokenizer.h"

namespace {

bool ReadFile(const char* path, GoogleString* out) {
  FILE* f = fopen(path, "rb");
  if (f == nullptr) {
    return false;
  }
  char buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    out->append(buf, n);
  }
  const bool ok = ferror(f) == 0;
  fclose(f);
  return ok;
}

bool WriteFile(const char* path, const GoogleString& data) {
  FILE* f = fopen(path, "wb");
  if (f == nullptr) {
    return false;
  }
  const size_t written = fwrite(data.data(), 1, data.size(), f);
  const bool ok = (written == data.size()) && (fclose(f) == 0);
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  const char* mode = nullptr;
  const char* in_path = nullptr;
  const char* out_path = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (strncmp(argv[i], "--mode=", 7) == 0) {
      mode = argv[i] + 7;
    } else if (in_path == nullptr) {
      in_path = argv[i];
    } else if (out_path == nullptr) {
      out_path = argv[i];
    } else {
      fprintf(stderr, "unexpected argument: %s\n", argv[i]);
      return 2;
    }
  }
  if (mode == nullptr || in_path == nullptr || out_path == nullptr) {
    fprintf(stderr,
            "usage: js_minify_probe --mode=tokenizer <in.js> <out.js>\n");
    return 2;
  }

  GoogleString input;
  if (!ReadFile(in_path, &input)) {
    fprintf(stderr, "cannot read %s\n", in_path);
    return 2;
  }

  GoogleString output;
  bool result;
  if (strcmp(mode, "tokenizer") == 0) {
    pagespeed::js::JsTokenizerPatterns patterns;
    result = pagespeed::js::MinifyUtf8Js(&patterns, input, &output);
  } else {
    fprintf(stderr, "unknown mode: %s\n", mode);
    return 2;
  }

  if (!WriteFile(out_path, output)) {
    fprintf(stderr, "cannot write %s\n", out_path);
    return 2;
  }
  printf("mode=%s result=%s in_bytes=%zu out_bytes=%zu\n", mode,
         result ? "ok" : "unmodelable", input.size(), output.size());
  return result ? 0 : 1;
}
