// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/tools/cli/apache_reader.h"

#include <filesystem>
#include <fstream>

#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace lint {
namespace {

namespace fs = std::filesystem;

struct Seen {
  GoogleString file;
  int line;
  GoogleString scope;
  GoogleString name;
  std::vector<GoogleString> args;
};

class Recorder : public ApacheLineVisitor {
 public:
  void Directive(const GoogleString& file, int line, const Placement& placement,
                 const GoogleString& name,
                 const std::vector<GoogleString>& args) override {
    seen.push_back({file, line, ScopeLabel(placement), name, args});
  }
  void Conditional(const GoogleString& file, int line,
                   const Placement& placement,
                   const std::vector<GoogleString>& args) override {
    seen.push_back({file, line, ScopeLabel(placement), "<if>", args});
  }
  std::vector<Seen> seen;
};

class ApacheReaderTest : public testing::Test {
 protected:
  void SetUp() override {
    root_ = fs::path(GTestTempDir()) /
            StrCat("reader_",
                   testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(root_);
    fs::create_directories(root_);
  }
  void Write(const GoogleString& relative, const GoogleString& content) {
    const fs::path p = root_ / relative;
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << content;
  }
  bool Read(const GoogleString& config) {
    ApacheConfigReader reader(root_, &mask_, &report_, &recorder_);
    return reader.ReadConfig(config);
  }
  std::vector<GoogleString> Rules() const {
    std::vector<GoogleString> out;
    for (const Finding& f : report_.findings()) {
      out.push_back(StrCat(f.rule, ":", f.line));
    }
    return out;
  }

  fs::path root_;
  ValueMask mask_;
  Report report_;
  Recorder recorder_;
};

TEST_F(ApacheReaderTest, WordsFollowApacheQuoting) {
  EXPECT_EQ(std::vector<GoogleString>({"A", "b c", "d'e", "f"}),
            ApacheConfigReader::Words("A \"b c\"  'd\\'e'\tf"));
  EXPECT_EQ(std::vector<GoogleString>({"x", ""}),
            ApacheConfigReader::Words("x \"\""));
  EXPECT_TRUE(ApacheConfigReader::Words("   ").empty());
}

TEST_F(ApacheReaderTest, PlacementAndContinuations) {
  Write("a.conf",
        "ModPagespeed on\n"
        "  # comment\n"
        "<VirtualHost *:80>\n"
        "  ModPagespeedA 1\n"
        "  <Directory /x>\n"
        "    ModPagespeedB \\\n"
        "2\n"
        "  </Directory>\n"
        "</VirtualHost>\n"
        "<ModPagespeedIf spdy>\n"
        "  ModPagespeedC 3\n"
        "</ModPagespeedIf>\n"
        "<IfModule x>\n"
        "  ModPagespeedD 4\n"
        "</IfModule>\n");
  ASSERT_TRUE(Read("a.conf"));
  EXPECT_TRUE(report_.findings().empty());
  ASSERT_EQ(6u, recorder_.seen.size());
  EXPECT_EQ("server", recorder_.seen[0].scope);
  EXPECT_EQ("virtualhost", recorder_.seen[1].scope);
  EXPECT_EQ(4, recorder_.seen[1].line);
  EXPECT_EQ("virtualhost/directory", recorder_.seen[2].scope);
  EXPECT_EQ(6, recorder_.seen[2].line);
  EXPECT_EQ(std::vector<GoogleString>({"2"}), recorder_.seen[2].args);
  EXPECT_EQ("<if>", recorder_.seen[3].name);
  EXPECT_EQ(std::vector<GoogleString>({"spdy"}), recorder_.seen[3].args);
  EXPECT_EQ("server+conditional", recorder_.seen[4].scope);
  EXPECT_EQ("server", recorder_.seen[5].scope);
}

TEST_F(ApacheReaderTest, IncludesAreSortedAndRooted) {
  Write("main.conf",
        "Include /conf.d/*.conf\n"
        "Include dir\n"
        "Include c:\\win\\x.conf\n");
  Write("conf.d/b.conf", "ModPagespeedB 1\n");
  Write("conf.d/a.conf", "ModPagespeedA 1\n");
  Write("conf.d/skip.txt", "ModPagespeedSkip 1\n");
  Write("dir/sub/z.conf", "ModPagespeedZ 1\n");
  Write("dir/y.conf", "ModPagespeedY 1\n");
  Write("win/x.conf", "ModPagespeedX 1\n");
  ASSERT_TRUE(Read("main.conf"));
  EXPECT_TRUE(report_.findings().empty());
  std::vector<GoogleString> files;
  for (const Seen& s : recorder_.seen) {
    files.push_back(s.file);
  }
  EXPECT_EQ(std::vector<GoogleString>({"conf.d/a.conf", "conf.d/b.conf",
                                       "dir/sub/z.conf", "dir/y.conf",
                                       "win/x.conf"}),
            files);
  ASSERT_EQ(6u, report_.inputs().size());
  EXPECT_EQ("config", report_.inputs()[0].kind);
  EXPECT_EQ("include", report_.inputs()[1].kind);
}

TEST_F(ApacheReaderTest, IncludeProblems) {
  Write("main.conf",
        "Include missing.conf\n"
        "IncludeOptional missing.conf\n"
        "Include ../outside.conf\n"
        "Include Self.conf\n");
  Write("self.conf", "Include main.conf\n");
  ASSERT_TRUE(Read("main.conf"));
  // PS902: Self.conf matched self.conf by letter case only; reading it then
  // includes main.conf again, a cycle (PS904).
  EXPECT_EQ(std::vector<GoogleString>(
                {"PS901:1", "PS903:3", "PS902:4", "PS904:0"}),
            Rules());
}

TEST_F(ApacheReaderTest, SectionProblemsMaskUnknownNames) {
  Write("main.conf",
        "</VirtualHost>\n"
        "</s3cret-closer>\n"
        "<Directory /x\n"
        "<VirtualHost *:80>\n");
  ASSERT_TRUE(Read("main.conf"));
  EXPECT_EQ(std::vector<GoogleString>(
                {"PS104:1", "PS104:2", "PS104:3", "PS104:4"}),
            Rules());
  for (const Finding& f : report_.findings()) {
    EXPECT_EQ(GoogleString::npos, f.message.find("s3cret")) << f.message;
  }
}

TEST_F(ApacheReaderTest, CrlfAndUnreadable) {
  Write("crlf.conf", "ModPagespeedA 1\r\nModPagespeedB 2\r\n");
  ASSERT_TRUE(Read("crlf.conf"));
  ASSERT_EQ(2u, recorder_.seen.size());
  EXPECT_EQ(std::vector<GoogleString>({"2"}), recorder_.seen[1].args);
  EXPECT_FALSE(Read("nope.conf"));
  EXPECT_FALSE(Read("../crlf.conf"));
}

}  // namespace
}  // namespace lint
}  // namespace net_instaweb
