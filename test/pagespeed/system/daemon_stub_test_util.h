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

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {

// The stub library's runfiles path for LoadDaemonAbi.  Bazel names a
// linkshared cc_binary's output after the TARGET NAME verbatim, on both
// platforms -- when the target name already ends in an extension-looking
// suffix (libdaemon_stub.so), no platform extension is appended on Windows
// either.  ONE place for the spelling, so no test builds the name by hand.
inline GoogleString StubLibraryPath(const char* target_basename) {
  return StrCat(GTestSrcDir(), "/test/pagespeed/system/", target_basename);
}

}  // namespace net_instaweb

#endif  // TEST_PAGESPEED_SYSTEM_DAEMON_STUB_TEST_UTIL_H_
