// GENERATED — DO NOT EDIT BY HAND. Synced by tools/sync-html-kernel.sh.
// Canonical source: mod_pagespeed 1.15, pagespeed/kernel/html/html_parse_probe.cc (#1130).
// Synced from the commit pinned in lib/html/HTML_KERNEL_PIN.
// Drift guard: CI re-runs tools/sync-html-kernel.sh and byte-compares.
// The canonical file's original license header is retained in full below;
// Apache-2.0 headed files: see lib/html/LICENSE.apache-2.0 for the license text.

// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.
//
// html_parse_probe: reference probe for the differential HTML parse
// harness. Reads an HTML file (argv[1], or stdin when omitted or "-"),
// runs it through HtmlParse with the recording filter, and writes the parse
// event stream (tools/html-parse-corpus/SPEC.md v1) to stdout.
//
// Exit codes (SPEC.md SS4):
//   0  clean parse ("flags none")
//   2  usage / I/O error / fixed base URL refused (harness error)
//   3  parse completed with "flags size-limit-exceeded"
//   crashes propagate as signals for the harness to record
//
// Determinism: same input -> byte-identical stream. Message-handler text is
// discarded (NullMessageHandler); the base URL is fixed, so neither seam
// can leak into the stream.

#include <cstdio>
#include <cstring>

#include "lib/html/compat/string.h"
#include "lib/html/html_parse_recorder.h"

namespace {

bool ReadAll(FILE* f, GoogleString* out) {
  char buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    out->append(buf, n);
  }
  return ferror(f) == 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 2) {
    fprintf(stderr, "usage: html_parse_probe [input.html|-]\n");
    return 2;
  }

  GoogleString input;
  if (argc == 2 && strcmp(argv[1], "-") != 0) {
    FILE* f = fopen(argv[1], "rb");
    if (f == nullptr) {
      fprintf(stderr, "cannot open %s\n", argv[1]);
      return 2;
    }
    const bool ok = ReadAll(f, &input);
    fclose(f);
    if (!ok) {
      fprintf(stderr, "cannot read %s\n", argv[1]);
      return 2;
    }
  } else {
    if (!ReadAll(stdin, &input)) {
      fprintf(stderr, "cannot read stdin\n");
      return 2;
    }
  }

  GoogleString stream;
  const int rc = net_instaweb::html_parse_probe::RunParseProbe(
      input.data(), input.size(), &stream);
  if (rc == net_instaweb::html_parse_probe::kExitHarnessError) {
    fprintf(stderr, "fixed base URL refused; harness bug\n");
    return 2;
  }
  fwrite(stream.data(), 1, stream.size(), stdout);
  return rc;
}
