// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Goldens for `pagespeed lint`: each case's JSON must equal
// testdata/golden/<case>.json byte for byte, on every platform and on every
// run.  The fixtures are copied into a scratch root first, so the linter
// walks a real directory tree wherever the test runs.
//
// To regenerate after an intended change, run the test with
// --test_env=PAGESPEED_LINT_WRITE_GOLDENS=1; it writes <case>.json to the
// test's undeclared outputs (test.outputs/outputs.zip under bazel-testlogs),
// to be reviewed and copied to testdata/golden/.

#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

#include "test/pagespeed/tools/cli/lint_test_util.h"

namespace net_instaweb {
namespace lint {
namespace {

constexpr char kTestData[] = "test/pagespeed/tools/cli/testdata/";

struct Case {
  const char* name;
  const char* config;             // "" for none
  std::vector<GoogleString> htaccess;
  // Some (rule, line) pairs the case must report, as a readable check next
  // to the golden.
  std::vector<std::pair<const char*, int>> expect;
};

const std::vector<Case>& Cases() {
  static const std::vector<Case>* cases = new std::vector<Case>{
      {"good", "good/pagespeed.conf", {}, {}},
      {"01-unknown", "seeded/01-unknown.conf", {}, {{"PS101", 2}}},
      {"02-invalid-value", "seeded/02-invalid-value.conf", {}, {{"PS102", 2}}},
      {"03-arity", "seeded/03-arity.conf", {}, {{"PS103", 2}}},
      {"04-arity-take2", "seeded/04-arity-take2.conf", {}, {{"PS103", 2}}},
      {"05-deprecated-filter", "seeded/05-deprecated-filter.conf", {},
       {{"PS204", 2}}},
      {"06-removed-property", "seeded/06-removed-property.conf", {},
       {{"PS202", 2}}},
      {"07-deprecated-spelling", "seeded/07-deprecated-spelling.conf", {},
       {{"PS201", 2}}},
      {"08-deprecated-option", "seeded/08-deprecated-option.conf", {},
       {{"PS202", 2}}},
      {"09-directory", "seeded/09-directory.conf", {}, {{"PS301", 3}}},
      {"10-vhost-strict", "seeded/10-vhost-strict.conf", {}, {{"PS302", 3}}},
      {"11-vhost-legacy", "seeded/11-vhost-legacy.conf", {}, {{"PS303", 3}}},
      {"12-htaccess", "", {"seeded/12-htaccess.htaccess"},
       {{"PS304", 1}, {"PS304", 2}, {"PS304", 3}}},
      {"14-nested-conditional", "seeded/14-nested-conditional.conf", {},
       {{"PS305", 3}}},
      {"15-global-in-conditional", "seeded/15-global-in-conditional.conf", {},
       {{"PS306", 2}}},
      {"16-other-in-conditional", "seeded/16-other-in-conditional.conf", {},
       {{"PS307", 2}}},
      {"17-conditional-argument", "seeded/17-conditional-argument.conf", {},
       {{"PS102", 1}}},
      {"18-include-missing", "seeded/18-include-missing.conf", {},
       {{"PS901", 2}}},
      {"19-include-case", "seeded/19-include-case.conf", {},
       {{"PS902", 1}, {"PS101", 2}}},
      {"20-include-escapes", "seeded/20-include-escapes.conf", {},
       {{"PS903", 1}}},
      {"21-unclosed", "seeded/21-unclosed.conf", {}, {{"PS104", 1}}},
      {"22-enabled-value", "seeded/22-enabled-value.conf", {}, {{"PS102", 1}}},
      {"23-vhost-ignored", "seeded/23-vhost-ignored.conf", {}, {{"PS303", 3}}},
      {"24-unknown-filter", "seeded/24-unknown-filter.conf", {},
       {{"PS105", 1}}},
      {"25-glued-equals", "seeded/25-glued-equals.conf", {}, {{"PS106", 1}}},
      {"26-limit", "seeded/26-limit.conf", {}, {{"PS301", 3}}},
      {"27-vhost-thread-count", "seeded/27-vhost-thread-count.conf", {},
       {{"PS302", 2}}},
      {"28-unexpected-closer", "seeded/28-unexpected-closer.conf", {},
       {{"PS104", 2}, {"PS104", 3}}},
      {"29-include-wildcard", "seeded/29-include-wildcard.conf", {}, {}},
      {"30-continuation", "seeded/30-continuation.conf", {}, {}},
      {"31-crlf", "seeded/31-crlf.conf", {}, {{"PS204", 3}}},
  };
  return *cases;
}

GoogleString ReadRunfile(const GoogleString& relative) {
  std::ifstream in(RunfilePath(StrCat(kTestData, relative)), std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

class LintGoldenTest : public LintTestBase {
 protected:
  void SetUp() override {
    LintTestBase::SetUp();
    std::istringstream files(ReadRunfile("files.txt"));
    GoogleString file;
    int n = 0;
    while (std::getline(files, file)) {
      if (!file.empty()) {
        WriteFile(file, ReadRunfile(file));
        ++n;
      }
    }
    ASSERT_GT(n, 30) << "testdata/files.txt lists the fixture files";
  }
};

TEST_F(LintGoldenTest, MatchesGoldens) {
  const char* write = getenv("PAGESPEED_LINT_WRITE_GOLDENS");
  const char* write_dir = write != nullptr && StringPiece(write) == "1"
                              ? getenv("TEST_UNDECLARED_OUTPUTS_DIR")
                              : nullptr;
  for (const Case& c : Cases()) {
    SCOPED_TRACE(c.name);
    bool readable = false;
    const GoogleString json = Lint(c.config, c.htaccess, &readable);
    EXPECT_TRUE(readable);
    if (write_dir != nullptr) {
      std::ofstream out(std::filesystem::path(write_dir) /
                            StrCat(c.name, ".json"),
                        std::ios::binary);
      out << json;
    }
    EXPECT_EQ(ReadRunfile(StrCat("golden/", c.name, ".json")), json);
    std::set<std::pair<GoogleString, int>> found;
    for (const Finding& f : last_report_.findings()) {
      found.insert({f.rule, f.line});
    }
    for (const auto& [rule, line] : c.expect) {
      EXPECT_EQ(1u, found.count({rule, line})) << rule << " at line " << line;
    }
    if (c.expect.empty() && StringPiece(c.name) != "19-include-case") {
      EXPECT_EQ(0, last_report_.Count(Severity::kError));
    }
  }
}

TEST_F(LintGoldenTest, SameOutputOnEveryRun) {
  for (const Case& c : Cases()) {
    SCOPED_TRACE(c.name);
    const GoogleString first = Lint(c.config, c.htaccess);
    for (int i = 0; i < 20; ++i) {
      ASSERT_EQ(first, Lint(c.config, c.htaccess));
    }
  }
}

TEST_F(LintGoldenTest, OutputIsPrintableAscii) {
  for (const Case& c : Cases()) {
    const GoogleString json = Lint(c.config, c.htaccess);
    for (char ch : json) {
      const unsigned char u = static_cast<unsigned char>(ch);
      ASSERT_TRUE(u == '\n' || (u >= 0x20 && u < 0x7f)) << c.name;
    }
  }
}

TEST_F(LintGoldenTest, UnreadableInputIsReported) {
  bool readable = true;
  Lint("seeded/no-such-file.conf", {}, &readable);
  EXPECT_FALSE(readable);
  Lint("", {"no-such.htaccess"}, &readable);
  EXPECT_FALSE(readable);
}

TEST_F(LintGoldenTest, ExitCodes) {
  Lint("good/pagespeed.conf");
  EXPECT_EQ(kExitClean, last_report_.ExitCode(Severity::kError));
  Lint("seeded/23-vhost-ignored.conf");  // a warning only
  EXPECT_EQ(kExitClean, last_report_.ExitCode(Severity::kError));
  EXPECT_EQ(kExitFindings, last_report_.ExitCode(Severity::kWarning));
  Lint("seeded/01-unknown.conf");
  EXPECT_EQ(kExitFindings, last_report_.ExitCode(Severity::kError));
}

}  // namespace
}  // namespace lint
}  // namespace net_instaweb
