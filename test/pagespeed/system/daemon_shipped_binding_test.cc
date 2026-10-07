/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

// The binding test against the SHIPPED optimizer client library: load the
// real artefact through the real loader, read its version, and resolve the
// whole entry-point surface (the loader's binding sequence fails on the
// first required symbol it cannot find; the optional surface is reported
// per predicate below).
//
// The artefact's path comes ONLY from the environment variable
// PAGESPEED_DAEMON_LIBRARY_UNDER_TEST: the case SKIPS with a plain reason
// when it is unset or names no file, so it never reddens a machine that
// does not have the library.  It is tagged manual: not part of any default
// CI label.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/system/daemon_abi.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

constexpr char kLibraryEnvVar[] = "PAGESPEED_DAEMON_LIBRARY_UNDER_TEST";

TEST(DaemonShippedBinding, TheShippedLibraryBindsItsWholeSurface) {
#ifdef _WIN32
  const char* path = getenv(kLibraryEnvVar);
  if (path == nullptr || path[0] == '\0') {
    GTEST_SKIP() << "set " << kLibraryEnvVar
                 << " to the shipped optimizer library to run this";
  }
  const std::filesystem::path fs_path = std::filesystem::path(
      std::u8string(reinterpret_cast<const char8_t*>(path)));
  if (!std::filesystem::exists(fs_path)) {
    GTEST_SKIP() << "no optimizer library at " << path << " (from "
                 << kLibraryEnvVar << ")";
  }
  GoogleString error;
  std::unique_ptr<DaemonAbi> abi(LoadDaemonAbi(path, &error));
  ASSERT_TRUE(abi != nullptr)
      << "the shipped library at " << path << " did not bind: " << error;
  const int major = abi->VersionMajor();
  const int minor = abi->VersionMinor();
  printf("shipped %s: reports ABI %d.%d (floor %d.%d)\n", path, major, minor,
         kRequiredAbiMajor, kRequiredAbiMinor);
  EXPECT_EQ(kRequiredAbiMajor, major);
  EXPECT_LE(kRequiredAbiMinor, minor);
  // The required surface is proven by the bind itself (LoadDaemonAbi refuses
  // a library that lacks any of it).  The optional surface is reported per
  // predicate: PUBLISHED, not merely callable (a library can export the
  // reporter and have nothing to say, which a call cannot tell apart).
  printf(
      "optional surface: sized_cache_config_init=%d volume_size=%d "
      "generation=%d last_error_message exported=%d\n",
      abi->HasSizedCacheConfigInit() ? 1 : 0,
      abi->PublishesVolumeSize() ? 1 : 0, abi->PublishesGeneration() ? 1 : 0,
      abi->PublishesLastErrorMessage() ? 1 : 0);
#else
  // The target is Windows-only, so this branch never runs; it exists so the
  // source compiles anywhere the build graph takes it.
  GTEST_SKIP() << "the shipped-library binding test is Windows-only";
#endif
}

}  // namespace
}  // namespace net_instaweb
