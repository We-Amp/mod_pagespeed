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

#include "pagespeed/kernel/base/hostname_util.h"

#include <climits>
// The following break portability.

// Windows doesn't use <unistd.h> nor does it define HOST_NAME_MAX.
#if defined(_WIN32)
// winsock2.h must be included BEFORE windows.h to avoid redefinition conflicts
// clang-format off
#include <winsock2.h>
#include <windows.h>
// clang-format on
#define HOST_NAME_MAX (MAX_COMPUTERNAME_LENGTH + 1)
#else
// POSIX systems need unistd.h for gethostname
#if defined(unix) || defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif
// MacOS does not defined HOST_NAME_MAX so fall back to the POSIX value.
// We are supposed to use sysconf(_SC_HOST_NAME_MAX) but we use this value
// to size an automatic array and we can't portably use variables for that.
#if !defined(HOST_NAME_MAX)
#define HOST_NAME_MAX _POSIX_HOST_NAME_MAX
#endif  // HOST_NAME_MAX
#endif  // _WIN32

#include "base/logging.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

GoogleString GetHostname() {
  char hostname[HOST_NAME_MAX + 1];
  hostname[sizeof(hostname) - 1] = '\0';

#if defined(_WIN32)
  // Use GetComputerNameA on Windows which doesn't require Winsock initialization.
  // gethostname requires WSAStartup to be called first.
  DWORD size = sizeof(hostname);
  if (!GetComputerNameA(hostname, &size)) {
    LOG(WARNING) << "GetComputerNameA failed: " << GetLastError();
    hostname[0] = '\0';
  }
#else
  // This call really shouldn't fail, so crash if it does under Debug,
  // but ensure an empty (safe) value under Release.
  int err = gethostname(hostname, sizeof(hostname) - 1);
  if (err != 0) {
    LOG(DFATAL) << "gethostname failed: " << err;
    hostname[0] = '\0';
  }
#endif

  return GoogleString(hostname);
}

bool IsLocalhost(StringPiece host_to_test, StringPiece hostname) {
  return (host_to_test == "localhost" || host_to_test == "127.0.0.1" ||
          host_to_test == "::1" || host_to_test == hostname);
}

}  // namespace net_instaweb
