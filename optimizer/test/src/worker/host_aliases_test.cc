// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Host Aliases File Tests

#include "src/worker/host_aliases.h"

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include <filesystem>
#include <fstream>

#include "gtest/gtest.h"

namespace pagespeed {
namespace {

class HostAliasesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    tmp_dir_ = std::filesystem::temp_directory_path() /
               ("host_aliases_test_" + std::to_string(
#ifdef _WIN32
                                           _getpid()
#else
                                           getpid()
#endif
                                               ));
    std::filesystem::create_directories(tmp_dir_);
  }

  void TearDown() override { std::filesystem::remove_all(tmp_dir_); }

  std::filesystem::path tmp_dir_;
};

// ---------------------------------------------------------------------------
// HostAliasesFilePath tests
// ---------------------------------------------------------------------------

TEST_F(HostAliasesTest, FilePathNormal) {
  EXPECT_EQ(std::filesystem::path(
                HostAliasesFilePath("/var/cache/pagespeed/cyclone.db")),
            std::filesystem::path("/var/cache/pagespeed/pagespeed-hosts.conf"));
}

TEST_F(HostAliasesTest, FilePathEmpty) {
  EXPECT_EQ(HostAliasesFilePath(""), "");
}

// ---------------------------------------------------------------------------
// ParseHostAliases tests
// ---------------------------------------------------------------------------

TEST_F(HostAliasesTest, ParseBasic) {
  auto aliases = ParseHostAliases(
      "version=1\n"
      "www.example.com=example.com\n");
  ASSERT_EQ(aliases.size(), 1u);
  EXPECT_EQ(aliases["www.example.com"], "example.com");
}

TEST_F(HostAliasesTest, ParseMultipleEntries) {
  auto aliases = ParseHostAliases(
      "version=1\n"
      "www.example.com=example.com\n"
      "cdn1.example.com=example.com\n"
      "cdn2.example.com=example.com\n");
  ASSERT_EQ(aliases.size(), 3u);
  EXPECT_EQ(aliases["www.example.com"], "example.com");
  EXPECT_EQ(aliases["cdn1.example.com"], "example.com");
  EXPECT_EQ(aliases["cdn2.example.com"], "example.com");
}

TEST_F(HostAliasesTest, ParseCommentsAndEmptyLinesIgnored) {
  auto aliases = ParseHostAliases(
      "# This is a comment\n"
      "\n"
      "version=1\n"
      "# Another comment\n"
      "\n"
      "www.example.com=example.com\n"
      "\n");
  ASSERT_EQ(aliases.size(), 1u);
  EXPECT_EQ(aliases["www.example.com"], "example.com");
}

TEST_F(HostAliasesTest, ParseNoVersionLine) {
  // No version line; entries still parse normally.
  auto aliases = ParseHostAliases("www.example.com=example.com\n");
  ASSERT_EQ(aliases.size(), 1u);
  EXPECT_EQ(aliases["www.example.com"], "example.com");
}

TEST_F(HostAliasesTest, ParseVersionTwoReturnsEmpty) {
  auto aliases = ParseHostAliases(
      "version=2\n"
      "www.example.com=example.com\n");
  EXPECT_TRUE(aliases.empty());
}

TEST_F(HostAliasesTest, ParseNullByteInKeySkipped) {
  std::string content = "version=1\n";
  content += std::string("www\0evil.com", 12);
  content += "=example.com\n";
  content += "good.com=example.com\n";

  auto aliases = ParseHostAliases(content);
  ASSERT_EQ(aliases.size(), 1u);
  EXPECT_EQ(aliases["good.com"], "example.com");
}

TEST_F(HostAliasesTest, ParseNullByteInValueSkipped) {
  // Manually build line with embedded null byte in value.
  std::string line = "www.example.com=exam";
  line += '\0';
  line += "ple.com\n";
  std::string content = "version=1\n" + line + "good.com=example.com\n";

  auto aliases = ParseHostAliases(content);
  ASSERT_EQ(aliases.size(), 1u);
  EXPECT_EQ(aliases["good.com"], "example.com");
}

TEST_F(HostAliasesTest, ParseWindowsLineEndings) {
  auto aliases = ParseHostAliases(
      "version=1\r\n"
      "www.example.com=example.com\r\n");
  ASSERT_EQ(aliases.size(), 1u);
  EXPECT_EQ(aliases["www.example.com"], "example.com");
}

TEST_F(HostAliasesTest, ParseNoEqualsSign) {
  auto aliases = ParseHostAliases(
      "version=1\n"
      "this has no equals sign\n"
      "www.example.com=example.com\n");
  ASSERT_EQ(aliases.size(), 1u);
  EXPECT_EQ(aliases["www.example.com"], "example.com");
}

TEST_F(HostAliasesTest, ParseEmptyKeySkipped) {
  auto aliases = ParseHostAliases(
      "version=1\n"
      "=example.com\n"
      "www.example.com=example.com\n");
  ASSERT_EQ(aliases.size(), 1u);
  EXPECT_EQ(aliases["www.example.com"], "example.com");
}

TEST_F(HostAliasesTest, ParseEmptyValueSkipped) {
  auto aliases = ParseHostAliases(
      "version=1\n"
      "www.example.com=\n"
      "cdn.example.com=example.com\n");
  ASSERT_EQ(aliases.size(), 1u);
  EXPECT_EQ(aliases["cdn.example.com"], "example.com");
}

// ---------------------------------------------------------------------------
// Round-trip (Write + Read) tests
// ---------------------------------------------------------------------------

TEST_F(HostAliasesTest, WriteAndReadRoundTrip) {
  std::string path = (tmp_dir_ / "pagespeed-hosts.conf").string();
  std::vector<std::pair<std::string, std::string>> aliases = {
      {"www.example.com", "example.com"},
      {"cdn1.example.com", "example.com"},
  };

  ASSERT_TRUE(WriteHostAliasesFile(path, aliases));

  auto loaded = ReadHostAliasesFile(path);
  ASSERT_EQ(loaded.size(), 2u);
  EXPECT_EQ(loaded["www.example.com"], "example.com");
  EXPECT_EQ(loaded["cdn1.example.com"], "example.com");
}

TEST_F(HostAliasesTest, WriteEmptyAliasListHasJustHeader) {
  std::string path = (tmp_dir_ / "pagespeed-hosts.conf").string();
  std::vector<std::pair<std::string, std::string>> aliases;

  ASSERT_TRUE(WriteHostAliasesFile(path, aliases));

  // Read raw file content to verify it contains just the header.
  std::ifstream f(path);
  std::string content((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
  EXPECT_NE(content.find("# Written by pagespeed-worker"), std::string::npos);
  EXPECT_NE(content.find("version=1"), std::string::npos);

  // Reading it back should produce an empty map.
  auto loaded = ReadHostAliasesFile(path);
  EXPECT_TRUE(loaded.empty());
}

TEST_F(HostAliasesTest, ReadNonExistentFileReturnsEmpty) {
  auto loaded =
      ReadHostAliasesFile((tmp_dir_ / "nonexistent-hosts.conf").string());
  EXPECT_TRUE(loaded.empty());
}

TEST_F(HostAliasesTest, NewlineInjectionInKeyFails) {
  std::string path = (tmp_dir_ / "pagespeed-hosts.conf").string();
  std::vector<std::pair<std::string, std::string>> aliases = {
      {"www.example.com\nmalicious=evil", "example.com"},
  };

  EXPECT_FALSE(WriteHostAliasesFile(path, aliases));
  EXPECT_FALSE(std::filesystem::exists(path));
}

TEST_F(HostAliasesTest, NewlineInjectionInValueFails) {
  std::string path = (tmp_dir_ / "pagespeed-hosts.conf").string();
  std::vector<std::pair<std::string, std::string>> aliases = {
      {"www.example.com", "example.com\nmalicious=evil"},
  };

  EXPECT_FALSE(WriteHostAliasesFile(path, aliases));
  EXPECT_FALSE(std::filesystem::exists(path));
}

TEST_F(HostAliasesTest, CarriageReturnInjectionInKeyFails) {
  std::string path = (tmp_dir_ / "pagespeed-hosts.conf").string();
  std::vector<std::pair<std::string, std::string>> aliases = {
      {"www.example.com\revil", "example.com"},
  };

  EXPECT_FALSE(WriteHostAliasesFile(path, aliases));
  EXPECT_FALSE(std::filesystem::exists(path));
}

TEST_F(HostAliasesTest, CarriageReturnInjectionInValueFails) {
  std::string path = (tmp_dir_ / "pagespeed-hosts.conf").string();
  std::vector<std::pair<std::string, std::string>> aliases = {
      {"www.example.com", "example.com\revil"},
  };

  EXPECT_FALSE(WriteHostAliasesFile(path, aliases));
  EXPECT_FALSE(std::filesystem::exists(path));
}

// --- Missing coverage from QA review ---

TEST(ParseHostAliasesTest, NoTrailingNewline) {
  // Last line without trailing newline should still be parsed.
  auto result = ParseHostAliases("version=1\nwww.example.com=example.com");
  EXPECT_EQ(result.size(), 1u);
  EXPECT_EQ(result["www.example.com"], "example.com");
}

TEST(ParseHostAliasesTest, DuplicateKeysLastWins) {
  auto result = ParseHostAliases(
      "version=1\nwww.example.com=first.com\nwww.example.com=second.com\n");
  EXPECT_EQ(result.size(), 1u);
  EXPECT_EQ(result["www.example.com"], "second.com");
}

TEST(ParseHostAliasesTest, NonNumericVersion) {
  // Non-numeric version value: from_chars fails, line skipped, parsing continues.
  auto result = ParseHostAliases("version=abc\nwww.example.com=example.com\n");
  EXPECT_EQ(result.size(), 1u);
  EXPECT_EQ(result["www.example.com"], "example.com");
}

TEST(ParseHostAliasesTest, Version1ExplicitlyAccepted) {
  auto result = ParseHostAliases("version=1\na=b\n");
  EXPECT_EQ(result.size(), 1u);
  EXPECT_EQ(result["a"], "b");
}

TEST(ParseHostAliasesTest, OnlyCommentsAndEmptyLines) {
  auto result = ParseHostAliases("# comment\n\n# another\n\n");
  EXPECT_TRUE(result.empty());
}

TEST(ParseHostAliasesTest, EmptyInput) {
  auto result = ParseHostAliases("");
  EXPECT_TRUE(result.empty());
}

TEST_F(HostAliasesTest, ReadEmptyFile) {
  std::string path = (tmp_dir_ / "pagespeed-hosts.conf").string();
  // Write a zero-byte file.
  {
    std::ofstream f(path);
  }
  auto result = ReadHostAliasesFile(path);
  EXPECT_TRUE(result.empty());
}

TEST_F(HostAliasesTest, WriteEmptyPathReturnsFalse) {
  std::vector<std::pair<std::string, std::string>> aliases = {{"a", "b"}};
  EXPECT_FALSE(WriteHostAliasesFile("", aliases));
}

TEST_F(HostAliasesTest, WriteAndReadEmptyAliases) {
  // Writing an empty alias list should produce a valid file that reads back
  // as empty. This ensures stale aliases from a previous run are cleared.
  std::string path = (tmp_dir_ / "pagespeed-hosts.conf").string();
  std::vector<std::pair<std::string, std::string>> empty;
  EXPECT_TRUE(WriteHostAliasesFile(path, empty));
  auto result = ReadHostAliasesFile(path);
  EXPECT_TRUE(result.empty());
}

}  // namespace
}  // namespace pagespeed
