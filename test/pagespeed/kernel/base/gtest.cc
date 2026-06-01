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

//

#include "test/pagespeed/kernel/base/gtest.h"

#ifdef _WIN32
#include <direct.h>   // For _getcwd, _mkdir
#include <process.h>  // For _getpid
#include <sys/stat.h>
#define getcwd _getcwd
#define getpid _getpid
#define mkdir(path, mode) _mkdir(path)
#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & _S_IFMT) == _S_IFDIR)
#endif
#else
#include <sys/stat.h>
#include <unistd.h>  // For getpid(), getcwd()
#endif

#include <algorithm>  // For std::replace
#include <cstdlib>    // For getenv
#include <vector>

#include "pagespeed/kernel/base/stack_buffer.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

GoogleString GTestSrcDir() {
  char cwd[kStackBufferSize];
  CHECK(getcwd(cwd, sizeof(cwd)) != nullptr);

  // This needs to return the root of the git checkout. In practice all the
  // tests are run automatically from there, so we just stat a few directories
  // to make sure it looks good and return getcwd(). An alternative might
  // be to return the value of $(git rev-parse --show-toplevel).

  // XXX(oschaaf): now that we run with bazel this is no longer a thing?
  // Previously checked that "third_party" and "pagespeed" directories existed
  // to verify we're running from the checkout root.
  return cwd;
}

GoogleString GTestTempDir() {
#ifdef _WIN32
  // Use Windows temp directory
  const char* temp = getenv("TEMP");
  if (temp == nullptr) {
    temp = getenv("TMP");
  }
  if (temp == nullptr) {
    temp = "C:/Temp";
  }
  GoogleString dir = absl::StrFormat("%s/gtest.%d", temp, getpid());
  // Normalize backslashes to forward slashes so RecursivelyMakeDir works
  // (it only searches for '/' as a path separator).
  std::replace(dir.begin(), dir.end(), '\\', '/');
#else
  GoogleString dir = absl::StrFormat("/tmp/gtest.%d", getpid());
#endif
  struct stat info;
  if (stat(dir.c_str(), &info) != 0) {
    CHECK(!mkdir(dir.c_str(), 0777));
  }
  return dir;
}

}  // namespace net_instaweb
