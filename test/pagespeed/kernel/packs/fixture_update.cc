// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Writes expected.module.html and expected.module.decisions.jsonl for the
// named fixture cases from the current filter, in the source tree:
//
//   bazel run //test/pagespeed/kernel/packs:fixture_update -- <case>...
//
// The shared expected files are never written. A case written here must also
// be listed, with its reason, in fixture_runner_test.cc.
//
//   bazel run //test/pagespeed/kernel/packs:fixture_update -- --new <case>...
//
// writes expected.html / expected.decisions.jsonl (and expected.headers) of a
// case that exists only in this module (a new directory with input.html and
// request.json); it overwrites those files, so review the diff.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "test/pagespeed/kernel/packs/fixture_runner.h"

int main(int argc, char** argv) {
  using pagespeed::packs::FixtureCase;
  const char* workspace = std::getenv("BUILD_WORKSPACE_DIRECTORY");
  const std::string root = workspace != nullptr ? workspace : ".";
  const std::string dir =
      root + "/test/pagespeed/kernel/packs/testdata/edge-seo/fixtures";
  bool new_case = false;
  int first = 1;
  if (argc > 1 && std::string(argv[1]) == "--new") {
    new_case = true;
    first = 2;
  }
  if (argc <= first) {
    std::fprintf(stderr, "usage: fixture_update [--new] <case>...\n");
    return 2;
  }
  int failures = 0;
  for (int i = first; i < argc; ++i) {
    const FixtureCase c{argv[i], dir + "/" + argv[i]};
    auto run = pagespeed::packs::RunCase(dir, c);
    if (!run.ok()) {
      std::fprintf(stderr, "%s: %s\n", argv[i],
                   std::string(run.status().message()).c_str());
      ++failures;
      continue;
    }
    if (new_case && run->pack_file == "pack.json" &&
        pagespeed::packs::ReadFile(c.dir + "/expected.html").ok()) {
      std::fprintf(stderr,
                   "%s: a case of the shared pack with an expected.html; "
                   "use fixture_update without --new (module override)\n",
                   argv[i]);
      ++failures;
      continue;
    }
    const auto s = new_case ? pagespeed::packs::WriteNewCaseExpected(c, *run)
                            : pagespeed::packs::WriteModuleExpected(c, *run);
    if (!s.ok()) {
      std::fprintf(stderr, "%s: %s\n", argv[i],
                   std::string(s.message()).c_str());
      ++failures;
      continue;
    }
    std::printf("wrote %s/expected.%s*\n", c.dir.c_str(),
                new_case ? "" : "module.");
  }
  return failures == 0 ? 0 : 1;
}
