// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.
//
// Fuzz harness for InlineStyleText (pagespeed/kernel/html/
// inline_style_text.h): the text PageSpeed writes into <style> elements.
//
//     bazel build --config=clang-asan -c opt \
//         //pagespeed/kernel/html:inline_style_text_fuzz
//
// One source, two build modes (as html_fuzz.cc and js_minify_fuzz.cc):
//   * default                   -> deterministic main(): replays the
//                                  embedded seeds, then every file named on
//                                  the command line; `--dump_corpus=DIR`
//                                  writes the seeds into DIR and exits.
//   * -DINLINE_STYLE_TEXT_LIBFUZZER -> LLVMFuzzerTestOneInput, for linking
//                                  with -fsanitize=fuzzer.
//
// The first input byte picks the mode (its low bit: HTML or strict); the
// rest is the CSS. Every input is checked against these properties, and any
// violation aborts:
//   1. accepted text passes IsSafeInlineStyleText(), and a second statement
//      of that property written here;
//   2. strict mode accepts only text with no '<', and returns it unchanged;
//   3. preparing accepted text again accepts it and returns it unchanged;
//   4. the output follows the input byte for byte, except that a '<', or a
//      backslash and a '<', may become the escape "\3c ";
//   5. text that strict mode accepts, HTML mode accepts unchanged.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/html/inline_style_text.h"

namespace {

using net_instaweb::InlineStyleMode;
using net_instaweb::InlineStyleText;
using net_instaweb::IsSafeInlineStyleText;

constexpr std::string_view kEsc = "\\3c ";

// Mode byte first, then the CSS.
const char* const kSeeds[] = {
    "\x01@property --x{syntax:\"<length>\";inherits:false}",
    "\x01@media (width<48rem){a{b:c}}@media (width < 600px){p{margin:0}}",
    "\x01@media (400px<=width<=700px){a{b:c}}",
    "\x01.content-\\[\\'\\<\\'\\]{content:'<'}",
    "\x01/*! licence <me@example.com> */a{b:c}",
    "\x01"
    "a{b:url(x<y)}c{d:url( \"<e\")}",
    "\x01"
    "a{b:\"c\\\n<d\"}e{f:'g\\'<h'}",
    "\x01"
    "a{content:\"</style>\"}/*</style>*/",
    "\x01<!--a{b:c}-->",
    "\x01"
    "a{b:c}</style><b>",
    "\x00"
    "@media (width < 600px){a{b:c}}",
    "\x00"
    "a{b:c}",
    "\x01"
    "a{b:\"\xc3\xa9<\"}",
    "\x01"
    "a{b:\"\xe9<\"}",
    "\x01"
    "a<\x1b(Bb",
};

[[noreturn]] void Fail(const char* what, std::string_view css) {
  std::fprintf(stderr, "inline style text property violated: %s (%zu bytes)\n",
               what, css.size());
  std::abort();
}

bool IndependentlySafe(std::string_view text, bool html) {
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '<') continue;
    if (!html || i + 1 >= text.size()) return false;
    const char c = text[i + 1];
    if (!(c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
          c == '=' || c == '.' || (c >= '0' && c <= '9'))) {
      return false;
    }
  }
  return true;
}

bool InLockstep(std::string_view in, std::string_view out) {
  size_t i = 0, j = 0;
  while (i < in.size()) {
    const bool out_has_esc = out.substr(j).substr(0, kEsc.size()) == kEsc;
    if (in[i] == '\\' && i + 1 < in.size() && in[i + 1] == '<' && out_has_esc) {
      i += 2;
      j += kEsc.size();
    } else if (in[i] == '<' && out_has_esc) {
      i += 1;
      j += kEsc.size();
    } else if (j < out.size() && in[i] == out[j]) {
      ++i;
      ++j;
    } else {
      return false;
    }
  }
  return j == out.size();
}

void CheckOne(const uint8_t* data, size_t size) {
  if (size == 0) return;
  const bool html = (data[0] & 1) != 0;
  const InlineStyleMode mode =
      html ? InlineStyleMode::kHtml : InlineStyleMode::kStrict;
  const std::string_view css(reinterpret_cast<const char*>(data + 1), size - 1);
  InlineStyleText out;
  const char* reason = nullptr;
  const bool ok = InlineStyleText::Prepare(css, mode, &out, &reason);

  InlineStyleText strict;
  const bool strict_ok =
      InlineStyleText::Prepare(css, InlineStyleMode::kStrict, &strict, nullptr);
  InlineStyleText as_html;
  const bool html_ok =
      InlineStyleText::Prepare(css, InlineStyleMode::kHtml, &as_html, nullptr);
  if (strict_ok && (!html_ok || as_html.text() != css)) {
    Fail("strict accepted, html did not accept it unchanged", css);  // 5
  }
  if (strict_ok && strict.text() != css) {
    Fail("strict changed the text", css);  // 2
  }
  if (strict_ok != (css.find('<') == std::string_view::npos)) {
    Fail("strict decision", css);  // 2
  }
  if (!ok) {
    if (reason == nullptr || !out.text().empty()) Fail("refusal", css);
    return;
  }
  const std::string_view text = out.text();
  if (!IsSafeInlineStyleText(text, mode) || !IndependentlySafe(text, html)) {
    Fail("unsafe text accepted", css);  // 1
  }
  if (!InLockstep(css, text)) Fail("not in lockstep", css);  // 4
  InlineStyleText again;
  if (!InlineStyleText::Prepare(text, mode, &again, nullptr) ||
      again.text() != text) {
    Fail("not idempotent", css);  // 3
  }
}

void CheckString(std::string_view s) {
  CheckOne(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

// The seeds carry a NUL mode byte, so their lengths are spelled out.
size_t SeedSize(const char* seed) {
  return seed[0] == '\0' ? 1 + std::strlen(seed + 1) : std::strlen(seed);
}

}  // namespace

#ifdef INLINE_STYLE_TEXT_LIBFUZZER
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  CheckOne(data, size);
  return 0;
}
#else
int main(int argc, char** argv) {
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
      for (const char* seed : kSeeds) {
        char path[4096];
        std::snprintf(path, sizeof(path), "%s/seed-%02zu", dir.c_str(),
                      written);
        FILE* f = std::fopen(path, "wb");
        if (f == nullptr) {
          std::fprintf(stderr, "dump_corpus: cannot write %s\n", path);
          return 1;
        }
        const size_t len = SeedSize(seed);
        const bool short_write = std::fwrite(seed, 1, len, f) != len;
        if (std::fclose(f) != 0 || short_write) {
          std::fprintf(stderr, "dump_corpus: write failed on %s\n", path);
          return 1;
        }
        ++written;
      }
      std::fprintf(stderr, "dump_corpus: wrote %zu seeds to %s\n", written,
                   dir.c_str());
      return 0;
    }
  }
  for (const char* seed : kSeeds) {
    CheckString(std::string_view(seed, SeedSize(seed)));
  }
  size_t files = 0;
  for (int i = 1; i < argc; ++i) {
    FILE* f = std::fopen(argv[i], "rb");
    if (f == nullptr) {
      std::fprintf(stderr, "skip: cannot read %s\n", argv[i]);
      continue;
    }
    std::string input;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
      input.append(buf, n);
    }
    std::fclose(f);
    CheckString(input);
    ++files;
  }
  std::fprintf(stderr,
               "inline style text fuzz harness: %zu seeds clean; %zu files "
               "replayed clean\n",
               sizeof(kSeeds) / sizeof(kSeeds[0]), files);
  return 0;
}
#endif
