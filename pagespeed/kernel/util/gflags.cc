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

#include "pagespeed/kernel/util/gflags.h"

#include <vector>

#include "absl/flags/parse.h"

namespace net_instaweb {

void ParseGflags(const char* progname, int* argc, char*** argv) {
  // absl::ParseCommandLine strips the flags it recognizes and returns the
  // remaining positional arguments, with the program name first.  Copy them
  // back into the caller's argv, which always has room for at least as many
  // entries as the parser was given.
  std::vector<char*> positional = absl::ParseCommandLine(*argc, *argv);
  *argc = static_cast<int>(positional.size());
  for (int i = 0; i < *argc; ++i) {
    (*argv)[i] = positional[i];
  }
  // Keep argv null-terminated, as it was on entry; *argc never grows, so
  // this slot is within the caller's original array.
  (*argv)[*argc] = nullptr;
}

}  // namespace net_instaweb
