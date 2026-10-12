// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.
//
// Memory-safety harness for the rule-pack loader.
//
// LoadPack() parses a pack file with a JSON reader, compiles regular
// expressions and globs from it, and builds templates, all from operator
// input that must never be trusted to be well formed. Build with
//
//     bazel build --config=clang-asan -c opt //pagespeed/kernel/packs:pack_loader_fuzz
//
// so AddressSanitizer is the oracle.
//
// One source, two build modes (the same split as the HTML parse harness):
//   * default                  -> deterministic main() that replays every
//                                 file named on the command line through
//                                 LoadPack(). Hermetic; CI-friendly.
//   * -DPACK_LOADER_LIBFUZZER  -> exposes LLVMFuzzerTestOneInput for
//                                 coverage-guided discovery when linked
//                                 with -fsanitize=fuzzer.
//
// Beyond not crashing, the entry point checks that a pack that loads has
// at most kMaxRules rules, at least one site, and a kind on every rule.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#include "pagespeed/kernel/packs/engine_version.h"
#include "pagespeed/kernel/packs/pack.h"
#include "pagespeed/kernel/packs/pack_loader.h"

namespace {

// Returns whether the pack loaded.
bool LoadOne(const char* data, size_t size) {
  using pagespeed::packs::LoadPack;
  auto pack = LoadPack(std::string_view(data, size),
                       pagespeed::packs::EngineVersion(), "fuzz.json");
  if (!pack.ok()) return false;
  if (pack->rules.size() > pagespeed::packs::kMaxRules || pack->sites.empty()) {
    std::fprintf(stderr, "loader accepted a pack that breaks its limits\n");
    std::abort();
  }
  for (const auto& rule : pack->rules) {
    if (rule.kind == nullptr) {
      std::fprintf(stderr, "loader accepted a rule without a kind\n");
      std::abort();
    }
  }
  return true;
}

// In replay mode a seed's file name states the expected outcome: "error-*"
// must be refused and "valid-*" (or the example pack) must load, so the seed
// names are verified on every replay.
void CheckExpectedOutcome(const char* path, bool loaded) {
  std::string name(path);
  const size_t slash = name.find_last_of("/\\");
  if (slash != std::string::npos) name.erase(0, slash + 1);
  const bool want_error = name.rfind("error-", 0) == 0;
  const bool want_valid =
      name.rfind("valid-", 0) == 0 || name == "pack.example.json";
  if ((want_error && loaded) || (want_valid && !loaded)) {
    std::fprintf(stderr, "seed %s: expected %s but the loader %s it\n", path,
                 want_error ? "an error" : "a valid pack",
                 loaded ? "accepted" : "refused");
    std::abort();
  }
}

}  // namespace

#ifdef PACK_LOADER_LIBFUZZER
// Coverage-guided entry point. Link with -fsanitize=fuzzer,address.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  LoadOne(reinterpret_cast<const char*>(data), size);
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
    std::string buf;
    char chunk[65536];
    size_t got;
    while ((got = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
      buf.append(chunk, got);
    }
    std::fclose(f);
    CheckExpectedOutcome(argv[i], LoadOne(buf.data(), buf.size()));
    ++n;
  }
  std::fprintf(stderr, "pack loader fuzz harness: %zu inputs processed clean\n",
               n);
  return n == 0 ? 1 : 0;
}
#endif
