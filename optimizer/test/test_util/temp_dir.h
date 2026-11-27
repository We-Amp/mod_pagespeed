// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef TEST_TEST_UTIL_TEMP_DIR_H_
#define TEST_TEST_UTIL_TEMP_DIR_H_

#include <filesystem>
#include <random>
#include <string>

namespace pagespeed::test {

// Creates a unique temporary directory and returns its path.
// The caller is responsible for cleanup (use TempDir RAII wrapper below).
inline std::string MakeTempDir() {
  auto base = std::filesystem::temp_directory_path();
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<uint64_t> dist;
  std::string dir;
  do {
    dir = (base / ("ps_test_" + std::to_string(dist(gen)))).string();
  } while (std::filesystem::exists(dir));
  std::filesystem::create_directories(dir);
  return dir;
}

// RAII wrapper that creates a temp directory on construction and removes it on
// destruction. Use dir() to get the path.
class TempDir {
 public:
  TempDir() : path_(MakeTempDir()) {}
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

}  // namespace pagespeed::test

#endif  // TEST_TEST_UTIL_TEMP_DIR_H_
