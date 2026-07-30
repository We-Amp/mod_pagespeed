// Copyright 2026 We-Amp B.V.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Memory-safety harness for the CSS parser.
//
// webutil/css/parser.cc is a hand-written recursive-descent parser that runs on
// *untrusted* author CSS on the rewrite hot path. Many of its cursor advances
// (`in_++`) and look-ahead reads are bounds-guarded only by DCHECK, which
// compiles out under -c opt (NDEBUG). Build this harness with
//
//     bazel build --config=asan -c opt //third_party/css_parser:parser_fuzz
//
// so DCHECKs are OFF (release behaviour) and AddressSanitizer is the oracle for
// the resulting out-of-bounds-read / use-after-free risk.
//
// One source, two build modes:
//   * default            -> a deterministic main() that drives an embedded
//                           adversarial corpus (and, if given, every file under
//                           argv[1]) through the parser. Hermetic; CI-friendly.
//                           `--dump_corpus=<dir>` instead writes the embedded
//                           corpus to <dir> (libFuzzer seed support, #645).
//   * -DCSS_PARSER_LIBFUZZER -> exposes LLVMFuzzerTestOneInput for coverage-
//                           guided discovery when linked with -fsanitize=fuzzer.
//
// The parser's *return value* is intentionally ignored: malformed input legally
// yields an empty/partial parse. The sanitizer — not the output — is the oracle.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "third_party/css_parser/src/webutil/css/parser.h"

namespace {

// Drive one untrusted byte range through every public entry point of the
// parser, under each mode whose code path differs materially. The constructor
// taking (begin, end) bounds the scan by `end`, so the input need not be
// NUL-terminated or valid UTF-8 — exactly the adversarial case.
void ParseOne(const char* data, size_t size) {
  const char* begin = data;
  const char* end = data + size;

  // Full stylesheet, quirks mode (the default), both preservation states —
  // preservation_mode_ gates a distinct set of "unparseable section" paths.
  for (bool preserve : {false, true}) {
    Css::Parser parser(begin, end);
    parser.set_preservation_mode(preserve);
    // Keep recursion bounded so a pathological input can't OOM the harness;
    // the depth cap itself is part of what we are exercising.
    parser.set_max_function_depth(64);
    std::unique_ptr<Css::Stylesheet> sheet(parser.ParseStylesheet());
  }

  // Standard (non-quirks) stylesheet path — quirks_mode_ branches in colour and
  // value parsing.
  {
    Css::Parser parser(begin, end);
    parser.set_quirks_mode(false);
    std::unique_ptr<Css::Stylesheet> sheet(parser.ParseStylesheet());
  }

  // Inline declarations — the style="..." attribute path.
  {
    Css::Parser parser(begin, end);
    std::unique_ptr<Css::Declarations> decls(parser.ParseDeclarations());
  }

  // @media query path.
  {
    Css::Parser parser(begin, end);
    std::unique_ptr<Css::MediaQueries> mq(parser.ParseMediaQueries());
  }
}

// A small, deterministic corpus of inputs aimed at the cursor-advance and
// look-ahead hazards the DCHECKs are supposed to catch: truncation right after
// a token-introducing byte, unterminated constructs, escapes at EOF, deep
// nesting, and malformed UTF-8. Kept literal so the test is hermetic.
const char* const kCorpus[] = {
    // Empty / minimal.
    "",
    " ",
    "{",
    "}",
    ":",
    ";",
    "/",
    "\\",
    "@",
    "#",
    ".",

    // Unterminated constructs (cursor runs to EOF mid-token).
    "/* comment never closed",
    "a { content: \"unterminated string",
    "a { content: 'unterminated string",
    "a { background: url(unterminated",
    "a { background: url(\"unterminated",
    "@media screen and (",
    "@media",
    "@import",
    "@font-face",
    "@keyframes",
    "@keyframes x {",
    "a[",
    "a[attr",
    "a[attr=",
    "a[attr=\"",

    // Escapes at / past the edge.
    "a { content: \"\\",
    "a { content: \\",
    "\\41",
    "\\",
    "a\\",
    "a { width: \\000041",
    "a { content: \"\\26",
    "url(\\",

    // Numbers / units truncated.
    "a { width: 1",
    "a { width: 1.",
    "a { width: .e",
    "a { width: 1e",
    "a { width: +",
    "a { width: -",
    "a { width: 0x",
    "a { color: #",
    "a { color: #f",
    "a { color: #ggg }",
    // Regression: out-of-range double->int in rgb() color parsing tripped UBSan
    // float-cast-overflow at value.cc GetIntegerValue (workflow NUMCOLOR-1).
    "a{color:rgb(99999999999,0,0)}",
    "a{color:rgb(-99999999999,0,0)}",
    "a { color: rgb(1e308, 0, 0) }",

    // Functions / parens nesting (depth + unbalanced).
    "a { width: calc(",
    "a { width: calc(calc(calc(",
    "a { background: rgb(",
    "a { transform: translate(translate(translate(translate(",

    // Malformed UTF-8 (lone continuation, truncated multibyte, overlong-ish).
    "a { content: \"\x80\" }",
    "a { content: \"\xC0\" }",
    "a { content: \"\xE0\x80\" }",
    "a { content: \"\xF0\x80\x80\" }",
    "\xFF\xFE",
    "@media \xC2",

    // Selectors / combinators truncated.
    "a >",
    "a >>",
    "a ~",
    "a +",
    "a::",
    "a:not(",
    "* |",
    "|",

    // Plausible-but-broken rules.
    "a { ; ; ; }",
    "a {{{{{{{{",
    "}}}}}}}}",
    "a { color: red !",
    "a { color: red !important",
    "@charset",
    "@charset \"",
    "@namespace",

    // Conditional group rules (@supports/@layer/@container): truncated
    // preludes, statement forms, unterminated bodies, strings that hide
    // block/paren delimiters, and MQ4 raw media expressions.
    "@supports",
    "@supports (",
    "@supports (a:b)",
    "@supports (a:b){",
    "@layer",
    "@layer a,",
    "@layer a;@layer b{",
    "@container (width >",
    "@media (width >= ",
    "@media (400px<=width<=700px){",
    "@supports (\"{\"):{",
    "@supports \"unterminated string {",
    "@supports (a:b){@import url(x);@charset \"utf-8\";}",
    "@layer a.b{@media screen{@supports (c:d){e{f:url(g)}}}}",

    // Stray ';' at statement position inside an @media body, crossed with
    // truncation: the recovery skip must not run off the end of the buffer.
    "@media screen{.a{b:c};",
    "@media screen{;",
    "@media screen{;}",
    "@media screen{.a{b:c}; ) }",
};

// Build progressively deeper nested inputs at run time to probe the recursion /
// function-depth guard without baking giant literals into the binary.
std::vector<std::string> DeepNesters() {
  std::vector<std::string> out;
  for (int depth : {16, 64, 256, 1024, 4096}) {
    out.emplace_back("a { width: " + std::string(depth, '(') + "1" +
                     std::string(depth, ')') + " }");
    out.emplace_back(std::string(depth, '{'));
    out.emplace_back("a { content: " + std::string(depth, '[') + "x" + " }");
  }
  // Nested group rules hammer the statement-recursion cap
  // (Parser::kMaxGroupRuleDepth): beyond it the parser must consume the rest
  // iteratively, whatever the depth. Both unterminated and terminated forms.
  for (int depth : {16, 64, 256, 1024}) {
    std::string open;
    for (int i = 0; i < depth; ++i) {
      open += "@supports (a:b){";
    }
    out.emplace_back(open);
    out.emplace_back(open + "e{f:g}" + std::string(depth, '}'));
  }
  return out;
}

// CI seed support: write the embedded corpus (kCorpus plus the
// generated DeepNesters) to one file per input under `dir`, so the scheduled
// libFuzzer lane can seed its persisted corpus directory without re-parsing
// this source file. Returns the number of files written, or -1 on error.
int DumpCorpus(const char* dir) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    std::fprintf(stderr, "dump_corpus: cannot create %s: %s\n", dir,
                 ec.message().c_str());
    return -1;
  }
  int n = 0;
  auto write_one = [&](const char* data, size_t size) -> bool {
    char name[32];
    std::snprintf(name, sizeof(name), "seed-%04d", n);
    const std::filesystem::path path = std::filesystem::path(dir) / name;
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
      std::fprintf(stderr, "dump_corpus: cannot open %s\n", path.c_str());
      return false;
    }
    const size_t wrote = std::fwrite(data, 1, size, f);
    std::fclose(f);
    if (wrote != size) {
      std::fprintf(stderr, "dump_corpus: short write on %s\n", path.c_str());
      return false;
    }
    ++n;
    return true;
  };
  for (const char* s : kCorpus) {
    if (!write_one(s, std::strlen(s))) return -1;
  }
  for (const std::string& s : DeepNesters()) {
    if (!write_one(s.data(), s.size())) return -1;
  }
  return n;
}

}  // namespace

#ifdef CSS_PARSER_LIBFUZZER
// Coverage-guided entry point. Link with -fsanitize=fuzzer,address.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  ParseOne(reinterpret_cast<const char*>(data), size);
  return 0;
}
#else
int main(int argc, char** argv) {
  // `--dump_corpus=<dir>` writes the embedded corpus to <dir> and exits —
  // used by the scheduled fuzz lane to seed libFuzzer's corpus directory.
  for (int i = 1; i < argc; ++i) {
    constexpr char kDumpPrefix[] = "--dump_corpus=";
    if (std::strncmp(argv[i], kDumpPrefix, sizeof(kDumpPrefix) - 1) == 0) {
      const int dumped = DumpCorpus(argv[i] + sizeof(kDumpPrefix) - 1);
      if (dumped < 0) return 1;
      std::fprintf(stderr, "css parser fuzz harness: dumped %d seed inputs\n",
                   dumped);
      return 0;
    }
  }
  size_t n = 0;
  for (const char* s : kCorpus) {
    ParseOne(s, std::strlen(s));
    ++n;
  }
  for (const std::string& s : DeepNesters()) {
    ParseOne(s.data(), s.size());
    ++n;
  }
  // Optional: replay an external corpus directory (e.g. a libFuzzer find dir).
  // Each regular file is one input; read with fread so embedded NULs survive.
  for (int i = 1; i < argc; ++i) {
    FILE* f = std::fopen(argv[i], "rb");
    if (f == nullptr) {
      std::fprintf(stderr, "skip: cannot open %s\n", argv[i]);
      continue;
    }
    std::string buf;
    char chunk[4096];
    size_t got;
    while ((got = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
      buf.append(chunk, got);
    }
    std::fclose(f);
    ParseOne(buf.data(), buf.size());
    ++n;
  }
  std::fprintf(stderr, "css parser fuzz harness: %zu inputs parsed clean\n", n);
  return 0;
}
#endif
