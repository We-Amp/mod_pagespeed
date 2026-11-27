// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.
//
// Dual-oracle fuzz harness for the vendored JS minifier
// (lib/js/js_minify.cc).  2.0-owned MANUAL MIRROR of the canonical 1.15
// harness (mod_pagespeed pagespeed/kernel/js/js_minify_fuzz.cc, mirrored
// at canonical commit 3938c4352, mpp #688) — NOT produced by
// tools/sync-js-kernel.sh and deliberately NOT in SYNCED_FILES.txt (the
// lib/html/html_fuzz.cc precedent; see CLAUDE.md).  Divergence vs
// canonical is limited to the include lines (rewritten per the sync
// tool's mapping table) and this header's repo references; logic is
// byte-similar and changes land canonically first, then are re-mirrored.
// The minifier runs on *untrusted* JavaScript; the sanitizer is the
// memory-safety backstop:
//
//     bazel build --config=asan -c opt //lib/js:js_minify_fuzz
//
// One source, two build modes (mirrors html_fuzz.cc):
//   * default               -> deterministic main(): first replays the
//                              embedded valid-JS seed set through BOTH
//                              oracles (hard abort on any trip — the
//                              valid-input battery), then replays every
//                              file named on the command line with
//                              per-oracle trip COUNTS (corpus accounting;
//                              exit 1 when any trip was seen).
//                              `--dump_corpus=DIR` instead writes the
//                              seed set as seed-NN files into DIR
//                              (created if missing) and exits (libFuzzer
//                              corpus seeding, #1176 precedent).
//   * -DJS_MINIFY_LIBFUZZER -> exposes LLVMFuzzerTestOneInput for
//                              coverage-guided discovery when linked
//                              with -fsanitize=fuzzer; trips abort like
//                              any other oracle failure.
//
// Oracle 1 — strict idempotence, modeled on the full DECLINE contract
// (js_minify.h: "the output string will still be fully populated from the
// input; the portion of the input up to the parse error will be minified,
// and the remainder will be passed through unmodified"; the error token
// IS the entire remainder, js_tokenizer.h).  The oracle requires
// minify(minify(x)) == minify(x) on BOTH the success flag and the output
// bytes, on every input: on the success path that is ordinary
// idempotence; on the decline path, x -> false + O, re-minifying O must
// decline at the same position producing O again (the already-minified
// prefix is byte-stable and the remainder starts with the same
// un-modelable construct).  A shifted error position at the seam, a
// changed flag, or a byte difference is a contract violation and aborts.
//
// Oracle 2 — token-channel equivalence (in-process, no node subprocess;
// run_oracles.py's node exec/parse oracles are the out-of-process
// alternative for full-program semantics).  JsTokenizer tokenizes input
// and output and two channels are compared:
//   * semantic channel: the sequence of tokens EXCLUDING whitespace-class
//     ones (kWhitespace, kLineSeparator, kSemiInsert, and comments) must
//     match kind+text one-for-one, and an error token (decline-path
//     remainder passthrough) must match byte-for-byte.  Comments are
//     dropped by the minifier EXCEPT IE conditional-compilation comments
//     (/*@...@*/) and the leading #! hashbang, which it must retain — so
//     those two are semantic-channel members (a dropped CC comment or
//     hashbang flags).
//   * ASI channel: whitespace and comments are the sanctioned-different
//     channel, but a linebreak the minifier keeps for automatic semicolon
//     insertion is semantic.  For every restricted-production keyword
//     (break/continue/debugger/return/throw/yield — js_minify.cc's
//     IsAsiKeyword) and every speculatively-classified operator word
//     (await/yield/for-of "of" — JsTokenizer::
//     LastTokenWasSpeculativeOperator), if the input has a linebreak or a
//     newline-containing comment after it before the next semantic token,
//     the output must still have one.  Mirroring the minifier's own
//     exception (js_minify.cc: a real linebreak after such a keyword is
//     emitted directly "except before `;` or `}`"), the requirement is
//     waived when the next semantic token is `;` or `}`.
// Decline-path inputs tokenize identically up to the error position, so
// both channels apply to the minified prefix; the passthrough remainder
// is pinned by the byte-for-byte error-token comparison.
//
// Invalid-input policy (v1): both oracles run on ALL input including
// garbage and report trips as violations (the css_minify_fuzz.cc v1
// posture — red-line, classify later).  The embedded seed battery and the
// existing corpus (lib/js/corpus) are valid JavaScript plus the
// documented decline-path shapes, which are contract-covered, so a trip
// there is a real bug by construction.  Fuzz-discovered trips on garbage
// are expected to be dominated by disclosed classes at first (see below)
// and get the css-lane classification treatment in follow-up lane work.
//
// Documented v1 blind spots (by design):
//   * A wrongly-RETAINED comment that is not an IE-CC comment or hashbang
//     is invisible (comments are the skip channel); a dropped CC
//     comment/hashbang does flag.
//   * Corruption whose only effect is whitespace that neither changes a
//     token nor an ASI requirement (e.g. a retained-but-unneeded
//     linebreak) is sanctioned whitespace churn, not corruption.
//   * kSemiInsert classification differences that do not change the
//     ASI-anchor outcome are invisible; the ASI channel checks the
//     minifier's own restricted-production requirement only.
//   * A same-text kLet<->kIdentifier kind flip is tolerated (RC-B, mpp
//     #691 triage): the tokenizer's `let` declaration lookahead
//     (js_tokenizer.cc) skips whitespace but NOT comments, so
//     `let/*c*/ v` classifies `let` as kIdentifier on input but kLet
//     after the sanctioned comment drop.  The flip is reachable on valid
//     JS (`let /*c*/ v = 1;`) and is a tokenizer context artifact, not
//     corruption — the semantic channel treats exactly this kind pair
//     (either direction, identical text only) as equal.  Any other kind
//     mismatch still flags; `let` -> `lets` (text differs) still flags.
//   * Let-binding ASI is not an ASI-channel anchor: the ASI channel models
//     only the minifier's own restricted productions (IsAsiKeyword plus
//     the speculative-operator words), so a dropped linebreak that is
//     load-bearing because the previous token was a DECLARATION BINDING
//     (`let row\n+4` must not become `let row+4`) is invisible here.
//     This was reachable while the `let` declaration lookahead
//     misclassified through comments (RC-E, fixed in js_tokenizer.cc);
//     it stays disclosed rather than modeled — full-ASI modeling is out
//     of scope for this oracle.
// Residual decline-path family, from the 1.15
// tools/js-minify-corpus/findings/repros/README.md (the repros themselves
// are vendored under lib/js/corpus/findings/repros/): of the 26 repros
// collected on 2026-07-21, only n4-yield-brace-linebreak-decline.js still
// declines (exit 1, pass-through; verified against the vendored kernel at
// pin 77b5c96dd, 2026-08-03) and exercises the decline contract.  K3
// (export default function(){} + newline + regex), K4 (class bodies), and
// the other n1-n7 shapes now minify successfully (the minifier learned
// them since the original report) and replay clean through the minified
// path under both oracles.
// Disclosed, not yet modeled (mpp #691 triage, invalid-input-only):
//   * RC-D: comment removal can shift the DECLINE position between
//     minify passes (e.g. `({*//\n$...` declines before the member name,
//     its comment-free output declines after it), so idempotence on the
//     flag/bytes can trip on garbage input.  Valid-input analogs minify
//     cleanly; the idempotence oracle stays the decline-contract
//     guardian.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "lib/js/compat/string.h"
#include "lib/js/compat/string_util.h"
#include "lib/js/js_keywords.h"
#include "lib/js/js_minify.h"
#include "lib/js/js_tokenizer.h"

namespace {

using pagespeed::JsKeywords;
using pagespeed::js::JsTokenizerPatterns;

// Valid-JS-biased seed set: ASI restricted productions, comment/newline
// promotion, hashbang and IE-CC retention, ES2020 syntax, regex-vs-
// division, operator-glue hazards, and modern class/generator shapes
// (drawn from the 1.15 tools/js-minify-corpus/generate_synthetic.py's
// shape families and findings/repros).  Replayed through BOTH oracles at
// the start of every deterministic main() run; also usable as the
// libFuzzer seed corpus (write to a corpus dir and pass it).  One
// deliberate DECLINE-path seed (the n4 yield-brace-linebreak-decline
// repro) pins the contract modeling.
const char* const kValidJsSeeds[] = {
    "function f() {\n  return\n  42;\n}",
    "return/*\n*/(x)",
    // Decline-path contract repro (lib/js/corpus/findings/repros/
    // n4-yield-brace-linebreak-decline.js): the minifier cannot model
    // this input, so output = minified prefix + unmodified remainder and
    // both oracles must hold across the pass-through.
    "var await = 1;\nawait\n{ } / x /.test(y);",
    "#!/usr/bin/env node\nvar x = 1;",
    "import def from './m.js'\n/re/.source;\nexport {};",
    "a?.b?.(x)\nlet y = a ?? b ?? 0;",
    "for (const x of [1,2,3]) console.log(x);",
    "class A { #p = 0; static { this.x = 1; } get x() { return this.#p; } }",
    "async function* g() { yield await Promise.resolve(1); }",
    "const t = `hello ${name} ${a + b} world`;",
    "var r = /a+b*/gi; x = y / 2 / z;",
    "x = 'it\\'s \"fine\"';\ny = \"a\xE2\x80\xA8\xE2\x80\xA9z\";",
    "x+++++y;",
    "a / b / c;",
    "var n = 0.5, h = 0x1F, e = 1e3, u = 1_000_000, o = 0755;",
    "/* c */a/*x*/++/*\n*/b;",
    // RC-B regression seeds (mpp #691 triage): the `let` declaration
    // lookahead does not see through comments, so the input tokenizes
    // `let` as kIdentifier while the comment-free output tokenizes it as
    // kLet — a sanctioned same-text kind flip, tolerated by the semantic
    // channel (see the file header).  Both are valid JS.
    "let /*c*/ v = 1;",
    "let//\nv",
    // RC-E regression seed (mpp #691 confirmation-nightly triage): the
    // minimized idempotence repro.  Invalid input, accept path; the
    // comment between the two `let`s made pass 1 and pass 2 disagree on
    // whether the second `let` was a declaration (binding ASI linebreak
    // kept) or merged into the first (droppable).  With the lookahead
    // fix both passes classify identically and the battery must stay
    // idempotent.
    "let//\nlet\nr\n+",
};

// Valid battery plus the decline-path contract pins: these must hold for
// the battery to pass; the file-replay counts below distinguish them
// per oracle.
enum class OracleTrip { kNone, kIdempotence, kTokenChannel };

// Whitespace-class tokens: the sanctioned-different channel (spaces,
// linebreaks, semicolon-insertion markers, and comments).  Comments are
// dropped by the minifier EXCEPT IE conditional-compilation comments
// (/*@...@*/) and the leading #! hashbang, which it must retain, so
// those two are NOT skipped here — they belong to the semantic channel.
bool IsRetainedComment(std::string_view t) {
  return t.substr(0, 2) == "#!" || (t.size() >= 6 && t.substr(0, 3) == "/*@" &&
                                    t.substr(t.size() - 3) == "@*/");
}

bool IsWsClass(JsKeywords::Type t, std::string_view text) {
  if (t == JsKeywords::kWhitespace || t == JsKeywords::kLineSeparator ||
      t == JsKeywords::kSemiInsert) {
    return true;
  }
  return t == JsKeywords::kComment && !IsRetainedComment(text);
}

// A linebreak-capable whitespace token (kLineSeparator or kSemiInsert),
// or a comment containing a line terminator — js_minify.cc promotes the
// latter to a linebreak for ASI (\n, \r, U+2028, U+2029).
bool IsLinebreakCapable(JsKeywords::Type t, std::string_view text) {
  if (t == JsKeywords::kLineSeparator || t == JsKeywords::kSemiInsert) {
    return true;
  }
  if (t != JsKeywords::kComment) {
    return false;
  }
  return text.find('\n') != std::string_view::npos ||
         text.find('\r') != std::string_view::npos ||
         text.find("\xE2\x80\xA8") != std::string_view::npos ||
         text.find("\xE2\x80\xA9") != std::string_view::npos;
}

// Restricted productions: a linebreak after these always induces ASI
// (mirrors js_minify.cc's IsAsiKeyword).
bool IsAsiKeyword(JsKeywords::Type t) {
  switch (t) {
    case JsKeywords::kBreak:
    case JsKeywords::kContinue:
    case JsKeywords::kDebugger:
    case JsKeywords::kReturn:
    case JsKeywords::kThrow:
    case JsKeywords::kYield:
      return true;
    default:
      return false;
  }
}

// The minifier's own exception: it emits an ASI linebreak directly
// except before `;` or `}`, where keeping it is merely harmless.
bool IsAsiException(std::string_view next) {
  return next == ";" || next == "}";
}

struct Token {
  JsKeywords::Type kind;
  std::string_view text;
  bool speculative;  // JsTokenizer::LastTokenWasSpeculativeOperator().
};

std::vector<Token> TokenizeAll(const JsTokenizerPatterns* patterns,
                               std::string_view input) {
  std::vector<Token> out;
  pagespeed::js::JsTokenizer tokenizer(patterns, input);
  StringPiece token;
  while (true) {
    const JsKeywords::Type kind = tokenizer.NextToken(&token);
    if (kind == JsKeywords::kEndOfInput || kind == JsKeywords::kError) {
      out.push_back(
          {kind, std::string_view(token.data(), token.size()), false});
      break;
    }
    out.push_back({kind, std::string_view(token.data(), token.size()),
                   tokenizer.LastTokenWasSpeculativeOperator()});
  }
  return out;
}

// True iff some token strictly between `from` and `to` is
// linebreak-capable.
bool HasLinebreakBetween(const std::vector<Token>& tokens, size_t from,
                         size_t to) {
  for (size_t k = from; k < to; ++k) {
    if (IsLinebreakCapable(tokens[k].kind, tokens[k].text)) {
      return true;
    }
  }
  return false;
}

// The token-channel oracle (see the file header for the design).
bool ChannelsEquivalent(const JsTokenizerPatterns* patterns,
                        std::string_view input, std::string_view output) {
  const std::vector<Token> in = TokenizeAll(patterns, input);
  const std::vector<Token> out = TokenizeAll(patterns, output);

  // Semantic channel: indices of tokens outside the whitespace class and
  // outside the terminal tokens (kEndOfInput / kError).
  std::vector<size_t> in_sem, out_sem;
  for (size_t i = 0; i < in.size(); ++i) {
    if (!IsWsClass(in[i].kind, in[i].text) &&
        in[i].kind != JsKeywords::kEndOfInput &&
        in[i].kind != JsKeywords::kError) {
      in_sem.push_back(i);
    }
  }
  for (size_t i = 0; i < out.size(); ++i) {
    if (!IsWsClass(out[i].kind, out[i].text) &&
        out[i].kind != JsKeywords::kEndOfInput &&
        out[i].kind != JsKeywords::kError) {
      out_sem.push_back(i);
    }
  }
  if (in_sem.size() != out_sem.size()) {
    return false;
  }
  for (size_t k = 0; k < in_sem.size(); ++k) {
    const Token& a = in[in_sem[k]];
    const Token& b = out[out_sem[k]];
    if (a.text != b.text) {
      return false;
    }
    // RC-B tolerance (see the file header): a same-text kLet<->kIdentifier
    // kind flip is a tokenizer context artifact of the sanctioned comment
    // drop (the `let` declaration lookahead does not see through
    // comments), not corruption.  Exactly this pair, either direction;
    // every other kind mismatch flags.
    if (a.kind != b.kind &&
        !((a.kind == JsKeywords::kLet || a.kind == JsKeywords::kIdentifier) &&
          (b.kind == JsKeywords::kLet || b.kind == JsKeywords::kIdentifier))) {
      return false;
    }
  }
  // Terminal tokens: both streams must end the same way; a decline-path
  // error token (the passthrough remainder) must match byte-for-byte.
  const Token& in_last = in.back();
  const Token& out_last = out.back();
  if (in_last.kind != out_last.kind) {
    return false;
  }
  if (in_last.kind == JsKeywords::kError && in_last.text != out_last.text) {
    return false;
  }

  // ASI channel: every restricted/speculative anchor with a linebreak
  // after it in the input must keep one in the output (waived before
  // `;`/`}` per the minifier's own exception).  Anchors are aligned by
  // semantic index; the sequences are byte-identical by the check above.
  for (size_t k = 0; k < in_sem.size(); ++k) {
    const Token& anchor = in[in_sem[k]];
    if (!IsAsiKeyword(anchor.kind) && !anchor.speculative) {
      continue;
    }
    const std::string_view next_text =
        (k + 1 < in_sem.size()) ? in[in_sem[k + 1]].text : std::string_view();
    if (IsAsiException(next_text)) {
      continue;
    }
    const size_t in_to =
        (k + 1 < in_sem.size()) ? in_sem[k + 1] : in.size() - 1;
    if (!HasLinebreakBetween(in, in_sem[k] + 1, in_to)) {
      continue;  // No linebreak after the anchor in the input; nothing due.
    }
    const size_t out_to =
        (k + 1 < out_sem.size()) ? out_sem[k + 1] : out.size() - 1;
    if (!HasLinebreakBetween(out, out_sem[k] + 1, out_to)) {
      return false;
    }
  }
  return true;
}

JsTokenizerPatterns& Patterns() {
  static JsTokenizerPatterns* patterns = new JsTokenizerPatterns();
  return *patterns;
}

// Seed-battery path: any trip aborts.
void MinifyOne(std::string_view input) {
  GoogleString once;
  const bool ok1 = pagespeed::js::MinifyUtf8Js(&Patterns(), input, &once);
  if (!ChannelsEquivalent(&Patterns(), input, once)) {
    std::fprintf(stderr, "token-channel violation: [%.*s] -> [%.*s]\n",
                 static_cast<int>(input.size()), input.data(),
                 static_cast<int>(once.size()), once.data());
    std::abort();
  }
  GoogleString twice;
  const bool ok2 = pagespeed::js::MinifyUtf8Js(&Patterns(), once, &twice);
  if (ok1 != ok2 || once != twice) {
    std::fprintf(stderr, "idempotence violation: [%.*s] -> [%.*s] -> [%.*s]\n",
                 static_cast<int>(input.size()), input.data(),
                 static_cast<int>(once.size()), once.data(),
                 static_cast<int>(twice.size()), twice.data());
    std::abort();
  }
}

// Corpus-replay path: count trips per oracle instead of aborting.
OracleTrip MinifyOneCounted(std::string_view input) {
  GoogleString once;
  const bool ok1 = pagespeed::js::MinifyUtf8Js(&Patterns(), input, &once);
  if (!ChannelsEquivalent(&Patterns(), input, once)) {
    return OracleTrip::kTokenChannel;
  }
  GoogleString twice;
  const bool ok2 = pagespeed::js::MinifyUtf8Js(&Patterns(), once, &twice);
  if (ok1 != ok2 || once != twice) {
    return OracleTrip::kIdempotence;
  }
  return OracleTrip::kNone;
}

bool ReadFile(const char* path, GoogleString* out) {
  FILE* f = std::fopen(path, "rb");
  if (f == nullptr) {
    return false;
  }
  char buf[65536];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
    out->append(buf, n);
  }
  const bool ok = std::ferror(f) == 0;
  std::fclose(f);
  return ok;
}

}  // namespace

#ifdef JS_MINIFY_LIBFUZZER
// Coverage-guided entry point.  Link with -fsanitize=fuzzer,address;
// trips abort like any other oracle failure.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  MinifyOne(std::string_view(reinterpret_cast<const char*>(data), size));
  return 0;
}
#else
int main(int argc, char** argv) {
  // --dump_corpus=DIR: write the seed set as seed-NN files into DIR
  // (created if missing) and exit — lets CI seed a libFuzzer corpus from
  // the embedded set without scraping C source (#1177 precedent).
  if (argc > 1) {
    const std::string_view arg1(argv[1]);
    constexpr std::string_view kDumpPrefix = "--dump_corpus=";
    if (arg1.substr(0, kDumpPrefix.size()) == kDumpPrefix) {
      const std::string dir(arg1.substr(kDumpPrefix.size()));
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
      if (ec) {
        std::fprintf(stderr, "dump_corpus: cannot create %s: %s\n", dir.c_str(),
                     ec.message().c_str());
        return 1;
      }
      size_t written = 0;
      for (const char* seed : kValidJsSeeds) {
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
      std::fprintf(stderr, "dump_corpus: wrote %zu seeds to %s\n", written,
                   dir.c_str());
      return 0;
    }
  }

  // Valid-input battery first: hard abort on any oracle trip.
  for (const char* seed : kValidJsSeeds) {
    MinifyOne(seed);
  }

  // Corpus replay with per-oracle trip counts.
  size_t files = 0, idem_trips = 0, chan_trips = 0;
  for (int i = 1; i < argc; ++i) {
    GoogleString input;
    if (!ReadFile(argv[i], &input)) {
      std::fprintf(stderr, "skip: cannot read %s\n", argv[i]);
      continue;
    }
    const OracleTrip trip = MinifyOneCounted(input);
    if (trip == OracleTrip::kIdempotence) {
      ++idem_trips;
      std::fprintf(stderr, "idempotence trip: %s\n", argv[i]);
    } else if (trip == OracleTrip::kTokenChannel) {
      ++chan_trips;
      std::fprintf(stderr, "token-channel trip: %s\n", argv[i]);
    }
    ++files;
  }
  std::fprintf(stderr,
               "js minify fuzz harness: %zu seeds clean; %zu files replayed, "
               "%zu idempotence trips, %zu token-channel trips\n",
               sizeof(kValidJsSeeds) / sizeof(kValidJsSeeds[0]), files,
               idem_trips, chan_trips);
  return (idem_trips + chan_trips) == 0 ? 0 : 1;
}
#endif
