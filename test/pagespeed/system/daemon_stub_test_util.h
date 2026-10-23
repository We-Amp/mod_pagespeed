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

#ifndef TEST_PAGESPEED_SYSTEM_DAEMON_STUB_TEST_UTIL_H_
#define TEST_PAGESPEED_SYSTEM_DAEMON_STUB_TEST_UTIL_H_

#ifdef _WIN32
#include <cstdlib>
#include <fstream>
#include <string>
#endif

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {

// The stub library's runfiles path for LoadDaemonAbi.  Bazel names a
// linkshared cc_binary's output after the TARGET NAME verbatim, on both
// platforms -- when the target name already ends in an extension-looking
// suffix (libdaemon_stub.so), no platform extension is appended on Windows
// either.  ONE place for the spelling, so no test builds the name by hand.
//
// On Windows the path comes from the runfiles manifest, i.e. the real output
// file.  Bazel builds the runfiles tree in-process there, and an entry whose
// target does not exist yet when the tree is made (an output that is still to
// be fetched from a remote cache) becomes a directory junction rather than a
// file link.  The tree is only rebuilt when its manifest changes, so the
// junction stays, and LoadLibrary on a junction fails with "Access is
// denied".  The manifest names the real file, which Bazel has fetched by the
// time the test runs.  The tree path stays the fallback.
inline GoogleString StubLibraryPath(const char* target_basename) {
  const GoogleString relative =
      StrCat("test/pagespeed/system/", target_basename);
#ifdef _WIN32
  GoogleString manifest;
  const char* manifest_env = getenv("RUNFILES_MANIFEST_FILE");
  const char* srcdir_env = getenv("TEST_SRCDIR");
  if (manifest_env != nullptr && *manifest_env != '\0') {
    manifest = manifest_env;
  } else if (srcdir_env != nullptr && *srcdir_env != '\0') {
    manifest = StrCat(srcdir_env, "/MANIFEST");
  }
  if (!manifest.empty()) {
    // Lines are "<repo>/<path> <absolute target>"; the repository name
    // (_main under bzlmod, the workspace name under WORKSPACE) is not
    // matched, only the path after it.
    const GoogleString suffix = StrCat("/", relative);
    std::ifstream in(manifest);
    std::string line;
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      const size_t space = line.find(' ');
      if (space == std::string::npos || space < suffix.size()) {
        continue;
      }
      if (line.compare(space - suffix.size(), suffix.size(), suffix) == 0) {
        return line.substr(space + 1);
      }
    }
  }
#endif
  return StrCat(GTestSrcDir(), "/", relative);
}

}  // namespace net_instaweb

#endif  // TEST_PAGESPEED_SYSTEM_DAEMON_STUB_TEST_UTIL_H_
