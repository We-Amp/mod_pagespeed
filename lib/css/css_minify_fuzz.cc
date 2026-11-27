// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.
//
// Fuzz harness for the 2.0 streaming CSS minifier (lib/css/css_minify.cc),
// mirror of the lib/html/html_fuzz.cc pattern. The minifier runs on
// *untrusted* CSS and compiles standalone (no deps), so a libFuzzer target
// is cheap. Build optimized with a sanitizer so release behaviour is what
// gets exercised and AddressSanitizer is the memory-safety oracle, e.g.:
//
//     bazel build -c opt --config=asan //lib/css:css_minify_fuzz
//
// One source, two build modes (mirrors html_fuzz.cc):
//   * default                -> deterministic main() that replays every
//                               file named on the command line through the
//                               minifier. Hermetic; CI-friendly.
//                               `--dump_corpus=DIR` instead writes the
//                               embedded valid-CSS seed set as seed-NN
//                               files into DIR (created if missing) and
//                               exits (CI corpus seeding, mpp #659
//                               precedent).
//   * -DCSS_MINIFY_LIBFUZZER -> exposes LLVMFuzzerTestOneInput for
//                               coverage-guided discovery when linked
//                               with -fsanitize=fuzzer.
//
// Oracle: crash-free / no-throw, with the sanitizer as the memory-safety
// backstop. A rejection (MinifyCss returns false) is a legitimate outcome,
// not a failure.
//
// Idempotence — minify(minify(x)) == minify(x), proven on the replayed 1.15
// corpus by test/lib/css/css_minify_corpus_test.cc — is a HARD oracle here
// too. It was briefly downgraded to a counted diagnostic when the first
// smoke run exposed a one-trailing-space-per-pass collapse on inputs with
// an odd backslash run before a quote at end of input ("\x5c'  "). That was
// root-caused to Phase 1 and Phase 2 tokenizing differently — Phase 1
// lacked the backslash-escape guard Phase 2 got in 01d14490a, and Phase 2
// had no url() state to mirror Phase 1's — and fixed with it (issue
// #1133); a violation now aborts like any other oracle failure.
//
// Token-stream equivalence (audit PR-C, first cut): a css-syntax-3-LITE
// tokenizer (lib/css/css_token_stream.h, harness-local) compares the
// token streams of the input and of the minified output — the idempotence
// oracle above cannot see BEHAVIOR-PRESERVING corruption (valid input
// whose served bytes change in a single pass, e.g. #1158's "url(x;}y)"
// losing its ';').  It extracts four channels — url-token, string-token,
// number, and custom-property value regions — compared with
// SameChannelTokens semantics (sanctioned collapse reorder and
// identical-value dedup tolerated; the number channel simulates Phase
// 4's zero-deletion rule, and custom regions stay sequence-strict).
// See the header of css_token_stream.h for the current design, the
// sanctioned-edit reasoning, and the documented blind spots and
// residuals.  The two oracles compose: corruption
// involving no url/string/custom tokens (e.g. the since-fixed #1159
// comment-glue and #1162 collapse formatting) is owned by the strict
// oracle.  Every deterministic run
// first replays the valid-CSS seed set below through both oracles.
//
// Known finds from the audit — all fixed.  Retired entries are noted
// here with the fix that closed them.
// (#1156 was removed from this list when the AtCustomPropertyColon
// stop set grew to cover every char whose adjacent whitespace Phase 2
// can remove — combinators, '!', and calc-mode's '*'/'/'; #1158/#1160
// when Phases 3 and 5 gained url() opacity; #1159/#1162/#1164 when
// PR-D fixed calc comment-token glue, empty-value collapse, and
// escaped-char handling outside strings; #1163 when Phase 4's decimal
// rule learned the identifier boundary (#1174 — plain idents and
// adjacent pair escapes only; the hex-escape face continued as
// #1175); #1167 when PR-E made brace groups inside
// declaration values opaque — the token-stream oracle's last
// documented red-line; #1175 when Phase 4's decimal rule learned to
// decode hex-escape structure
// (#1185); #1170 when Phase 5's shorthand collapse grew the
// conservative-refusal guards for reorder-unstable garbage values
// (#1211 — the strict oracle's last documented red-line; verified
// zero idempotence violations across the 1,776-artifact PR-D-era
// sweep set on current main); again when Phase 5's declaration
// splitter learned paren/bracket depth (a ';' inside (...) or [...]
// is value content — "a{--x:(a;;b)}" lost a ';' on VALID input) and
// its top-level scan stopped opening a block on a '{' inside an
// unclosed paren (the artifact's face).  Nothing is tracked open any
// more.
// What stays live is tooling, not minifier classes: the two NAMED
// token-oracle over-catches
// (NormalizeUrl's escape-unaware ')' strip on unterminated url() at
// EOF; string-channel reorder under legitimate collapse) were fixed
// in the comparator — url/string channels tolerate sanctioned
// collapse reorder and identical-value dedup now (SameChannelTokens),
// and the ')' strip is escape-aware.  The remaining invalid-input
// over-catches (the classifier's known-token-overcatch bucket —
// escape glue, comment-removal glue, custom-region boundaries, url
// tokenization on NUL/0xff garbage) are now adjudicated by the oracle
// itself: the v3 garbage channel-agreement in css_token_stream.h
// ports the classifier's three reviewed known-bucket predicates
// (escape-glue whitespace-only diff, unterminated url( at EOF, and
// the garbage-input validity axis), so those shapes are EQ here and
// the nightly's known bucket drops to zero for oracle-owned classes.
// The classifier stays as the safety net for anything the widening
// missed; its self-test still runs in the PR smoke.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#include "lib/css/css_minify.h"
#include "lib/css/css_token_stream.h"

namespace {

// Valid-CSS-biased seed set (audit PR-C §4.4/§5): url tokens with
// ';{}' content, custom values with braces/parens/decimals, escaped
// idents, and an @layer/:where Tailwind-v4-style snippet (per
// lib/css/CLAUDE.md's testing advice).  It also holds regression
// artifacts from fixed fuzz findings, some of them garbage-input (stray
// escapes, tabs, CR, unbalanced brackets) rather than valid CSS, so the
// replay floor keeps checking the exact inputs that once failed.
// Replayed through BOTH oracles at the start of every deterministic
// main() run; also usable as the libFuzzer seed corpus (write to a
// corpus dir and pass it).
const char* const kValidCssSeeds[] = {
    "a{background:url(x;}y)}",
    // Parenthesized: adjacent literals in an array initializer trip
    // -Wstring-concatenation (macOS CI) otherwise.
    ("a{background:url(x{y});padding-top:1px;padding-right:1px;"
     "padding-bottom:1px;padding-left:1px}"),
    ":root{--x:0.5;--y:url( \"a)b\" );--z:{;}}",
    ".a\\0.5b{c:d}",
    "a{--x:foo(a;});b:c}",
    "@layer base{.prose :where(h2){margin:0}}",
    "a{width:calc(100% - 10px);background:url(  foo.png  ) no-repeat}",
    "a{margin:0.5px -0.5px;content:'it\\'s \"fine\"'}",
    "/* c */a/*x*/,b>.y~z+w{color:#fff!important}",
    "a{--x:  calc(1px +  2px) ;--y:url(a b.png) }",
    "a{filter:progid:DXImageTransform.Microsoft.Alpha(Opacity=50)}",
    // #1238 regression seed: an escaped char (escaped space) inside a
    // custom-property name — the name-scan must stay custom and keep
    // the value byte-verbatim.
    "a{--\\ \\>:0.}",
    // #1238 pass-ordering regression seeds: escape-terminated name
    // prefix + ws + name char must scan custom on pass 1 already, so
    // the pass is a fixpoint (kept-then-dropped value space was the
    // non-idempotence).
    "--\\>\tp: x",
    "--\\>\tp:\rr",
    "--\\>\tp:\t;",
    // Run-30913256122 regression seeds (#1238 triage): Phase 4's
    // leading-zero strip must NOT fire on a '0' between two dots —
    // ".0." stays byte-stable (the ".0" number-channel token
    // survives); valid-input strips ("0.5" -> ".5") are unaffected.
    "h:5-.0.",
    "a{b:.0.}",
    "a{b:0.0.}",
    "-0.--.0-.0..4",
    "a{bax{.0-0-.0-.0-5-.0.t0505-0-0c}",
    // Regression seeds: a ';' inside (...) or [...] in a
    // declaration value is value content, never a declaration boundary
    // (the empty segment between ";;" was dropped as an empty
    // declaration), and a '{' inside an unclosed paren is not a block
    // edge (the run-37097654289 artifact, crash-5e066d75).
    "a{--x:(a;;b)}",
    "a{--x:[a;;b]}",
    "3-0:/(;--:{!aar;;;;;;;;;;;;;;y}",
    // ...and the bracket twins found in review: Phase 5's brace
    // matcher and top-level scan count '[' like '(' (an unclosed '['
    // swallows the following '}'; a ')' inside a brace group inside
    // parens must not close the paren).
    "a{--x:[a}e{c:d;;}",
    "a{--x:(a{b)c;;d}e)}",
    // A "--a:" after a ';' inside a selector bracket is not a custom
    // property; the rule block's empty-declaration trim is sanctioned.
    "2[/ro;--y:t){;m0}",
    "[;--a:b]{;c}",
    // #954 (and the earlier auto-closed #848, #882, #907): a custom
    // property whose name holds an escaped char keeps its value's brace
    // group verbatim — Phase 5 must not collapse it as a nested block.
    "--\\--\\>\t--\\-p:>----{;]}",
    "--\\--\\>\t--\\-p:>--{ax:-;a;b:;}-----x",
    "--\\--\\>\t--\\-p:>--{ax4-;a;b;}---'q];",
    "--T--\\~--\\---\\,p:>-\\>\t--\\,p:>-{;'d'}rd",
    "--\\--\\>\t--\\-p:>--{ax:-;a;b;}--;a;\r\"a-x",
    "--\\--\\>\t--\\-p:>--{;b;}--ccccccccccc\t--\\-p:>--{;b;\\-",
    ":rpt{--x0\\:: { ;;}}",
    "--\\>:c{;d-}0-0/5.!0",
    "a-0:\\\\\\\\\\\t\\\r~; --\\\t\\&:{5;;}((",
    "a{--\\>p:{;b;}}",
    // #582: preserved comments (/*!, @license, @preserve) survive
    // byte-for-byte through the sentinel round trip; ordinary comments
    // still strip.
    "/*! banner */a{b:c}/* drop */d{e:f}",
    "/* @license MIT */ a{--x:/*!k*/ v;margin:0.5px}",
};

// Normalize-invariant diagnostic (glue-normalization cutover): the
// comparator requires the minified output to already be a Phase1/Phase2
// fixpoint (Normalize(output) == output).  A violation is a minifier
// whitespace-pass instability (#1241's class) — counted, reported in
// the run summary, and driven to exit 1, but NOT aborting mid-run so
// the count and every instance surface in one replay.
size_t g_invariant_violations = 0;

void MinifyOne(std::string_view input) {
  std::string once;
  if (!pagespeed::css::MinifyCss(input, &once)) {
    return;  // Rejected input is a legitimate outcome.
  }
  if (pagespeed::css::token_stream::Normalize(once) != once) {
    ++g_invariant_violations;
    std::fprintf(stderr, "normalize-invariant violation: [%.*s] -> [%.*s]\n",
                 static_cast<int>(input.size()), input.data(),
                 static_cast<int>(once.size()), once.data());
  }
  if (!pagespeed::css::token_stream::Equivalent(input, once)) {
    // Token-stream oracle (see header): the served url/string/
    // custom-value token content must survive minification.
    std::fprintf(stderr, "token-stream violation: [%.*s] -> [%.*s]\n",
                 static_cast<int>(input.size()), input.data(),
                 static_cast<int>(once.size()), once.data());
    std::abort();
  }
  std::string twice;
  if (pagespeed::css::MinifyCss(once, &twice) && once != twice) {
    // Hard idempotence oracle (see header): since the #1133 Phase 1
    // escape fix, minify(minify(x)) == minify(x) must hold on all input.
    std::fprintf(stderr, "idempotence violation: [%.*s] -> [%.*s] -> [%.*s]\n",
                 static_cast<int>(input.size()), input.data(),
                 static_cast<int>(once.size()), once.data(),
                 static_cast<int>(twice.size()), twice.data());
    std::abort();
  }
}

}  // namespace

#ifdef CSS_MINIFY_LIBFUZZER
// Coverage-guided entry point. Link with -fsanitize=fuzzer,address.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  MinifyOne(std::string_view(reinterpret_cast<const char*>(data), size));
  return 0;
}
#else
int main(int argc, char** argv) {
  size_t n = 0;
  // --dump_corpus=DIR: write the valid-CSS seed set as seed-NN files into
  // DIR (created if missing) and exit — lets CI seed a libFuzzer corpus
  // from the embedded set without scraping C source (mpp parser_fuzz.cc
  // #659 precedent).
  if (argc > 1) {
    std::string_view arg1(argv[1]);
    constexpr std::string_view kDumpPrefix = "--dump_corpus=";
    if (arg1.substr(0, kDumpPrefix.size()) == kDumpPrefix) {
      std::string dir(arg1.substr(kDumpPrefix.size()));
      // Create the directory ourselves (mpp DumpCorpus precedent): the
      // first dispatched nightly failed here because the workflow never
      // mkdir'd the seed dir (run 30631937209).
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
      if (ec) {
        std::fprintf(stderr, "dump_corpus: cannot create %s: %s\n", dir.c_str(),
                     ec.message().c_str());
        return 1;
      }
      size_t written = 0;
      for (const char* seed : kValidCssSeeds) {
        char path[4096];
        std::snprintf(path, sizeof(path), "%s/seed-%02zu", dir.c_str(),
                      written);
        FILE* f = std::fopen(path, "wb");
        if (f == nullptr) {
          std::fprintf(stderr, "dump_corpus: cannot write %s\n", path);
          return 1;
        }
        const size_t len = std::strlen(seed);
        if (std::fwrite(seed, 1, len, f) != len) {
          std::fprintf(stderr, "dump_corpus: short write on %s\n", path);
          std::fclose(f);
          return 1;
        }
        if (std::fclose(f) != 0) {
          std::fprintf(stderr, "dump_corpus: close failed on %s\n", path);
          return 1;
        }
        ++written;
      }
      std::fprintf(stderr, "dump_corpus: %zu seeds written to %s\n", written,
                   dir.c_str());
      return 0;
    }
  }
  // Replay the valid-CSS seed set through both oracles first.
  for (const char* seed : kValidCssSeeds) {
    MinifyOne(seed);
    ++n;
  }
  for (int i = 1; i < argc; ++i) {
    FILE* f = std::fopen(argv[i], "rb");
    if (f == nullptr) {
      std::fprintf(stderr, "skip: cannot open %s\n", argv[i]);
      continue;
    }
    std::string buf;
    char chunk[65536];
    size_t got;
    while ((got = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
      buf.append(chunk, got);
    }
    std::fclose(f);
    MinifyOne(buf);
    ++n;
  }
  if (g_invariant_violations != 0) {
    std::fprintf(stderr,
                 "css minify fuzz harness: %zu inputs processed, %zu "
                 "normalize-invariant violations\n",
                 n, g_invariant_violations);
    return 1;
  }
  std::fprintf(stderr, "css minify fuzz harness: %zu inputs processed clean\n",
               n);
  return 0;
}
#endif
