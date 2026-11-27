// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "src/worker/windows_service.h"

#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace pagespeed {
namespace {

ServiceArgs Parse(std::vector<std::string> args) {
  std::vector<char*> argv;
  argv.reserve(args.size());
  for (std::string& arg : args) argv.push_back(arg.data());
  return ParseServiceArgs(static_cast<int>(argv.size()), argv.data());
}

TEST(WindowsServiceArgsTest, AnOrdinaryCommandLineIsNotAService) {
  ServiceArgs parsed = Parse({"factory_worker", "--socket", "p", "--help"});
  EXPECT_TRUE(parsed.ok);
  EXPECT_FALSE(parsed.service);
  EXPECT_TRUE(parsed.log_file.empty());
  EXPECT_EQ(parsed.rest, (std::vector<std::string>{"factory_worker", "--socket",
                                                   "p", "--help"}));
}

TEST(WindowsServiceArgsTest, BothOptionsAreTakenOutWhereverTheyStand) {
  ServiceArgs parsed =
      Parse({"factory_worker", "--socket", "p", "--service", "--log-file",
             "C:\\logs\\worker.log", "--cache-dir", "C:\\cache"});
  ASSERT_TRUE(parsed.ok) << parsed.error;
  EXPECT_TRUE(parsed.service);
  EXPECT_EQ(parsed.log_file, "C:\\logs\\worker.log");
  EXPECT_EQ(parsed.rest,
            (std::vector<std::string>{"factory_worker", "--socket", "p",
                                      "--cache-dir", "C:\\cache"}));
}

TEST(WindowsServiceArgsTest, TheProgramNameIsNeverAnOption) {
  ServiceArgs parsed = Parse({"--service"});
  EXPECT_TRUE(parsed.ok);
  EXPECT_FALSE(parsed.service);
  EXPECT_EQ(parsed.rest, (std::vector<std::string>{"--service"}));
}

TEST(WindowsServiceArgsTest, ALogFileWithoutAPathIsRefused) {
  ServiceArgs parsed = Parse({"factory_worker", "--service", "--log-file"});
  EXPECT_FALSE(parsed.ok);
  EXPECT_EQ(parsed.error, "--log-file needs a path");
}

TEST(WindowsServiceArgsTest, AnOptionIsNotALogFilePath) {
  ServiceArgs parsed = Parse({"factory_worker", "--log-file", "--service"});
  EXPECT_FALSE(parsed.ok);
  EXPECT_EQ(parsed.error, "--log-file needs a path");
}

TEST(WindowsServiceArgsTest, ALogFileOutsideServiceModeIsRefused) {
  ServiceArgs parsed = Parse({"factory_worker", "--log-file", "w.log"});
  EXPECT_FALSE(parsed.ok);
  EXPECT_EQ(parsed.error, "--log-file is only valid with --service");
}

#ifndef _WIN32
TEST(WindowsServiceArgsTest, OffWindowsServiceModeIsNeverEntered) {
  std::vector<std::string> args = {"factory_worker", "--service"};
  std::vector<char*> argv;
  argv.reserve(args.size());
  for (std::string& arg : args) argv.push_back(arg.data());
  int exit_code = 7;
  EXPECT_FALSE(RunAsServiceIfRequested(
      static_cast<int>(argv.size()), argv.data(), [](int, char**) { return 3; },
      &exit_code));
  EXPECT_EQ(exit_code, 7);
}
#endif

}  // namespace
}  // namespace pagespeed
