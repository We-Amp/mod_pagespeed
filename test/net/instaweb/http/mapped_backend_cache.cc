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

#include "test/net/instaweb/http/mapped_backend_cache.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "base/logging.h"

namespace net_instaweb {

#ifdef _WIN32

char* MappedBackendCache::MapPages(size_t min_len, size_t* map_len) {
  SYSTEM_INFO info;
  GetSystemInfo(&info);
  const size_t page = info.dwPageSize;
  *map_len = (min_len / page + 1) * page;
  void* base =
      VirtualAlloc(nullptr, *map_len, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  CHECK(base != nullptr) << "MappedBackendCache VirtualAlloc failed";
  return static_cast<char*>(base);
}

void MappedBackendCache::PoisonPages(char* base, size_t map_len) {
  DWORD old_protect;
  CHECK(VirtualProtect(base, map_len, PAGE_NOACCESS, &old_protect))
      << "MappedBackendCache VirtualProtect failed";
}

void MappedBackendCache::UnmapPages(char* base, size_t /*map_len*/) {
  // MEM_RELEASE takes the whole reservation (size 0) whatever its protection.
  VirtualFree(base, 0, MEM_RELEASE);
}

#else  // !_WIN32

char* MappedBackendCache::MapPages(size_t min_len, size_t* map_len) {
  static const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  *map_len = (min_len / page + 1) * page;
  void* base = mmap(nullptr, *map_len, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(base != MAP_FAILED) << "MappedBackendCache mmap failed";
  return static_cast<char*>(base);
}

void MappedBackendCache::PoisonPages(char* base, size_t map_len) {
  mprotect(base, map_len, PROT_NONE);
}

void MappedBackendCache::UnmapPages(char* base, size_t map_len) {
  // Restore RW so munmap on a PROT_NONE region is unambiguous, then release
  // the mapping.
  mprotect(base, map_len, PROT_READ | PROT_WRITE);
  munmap(base, map_len);
}

#endif  // _WIN32

}  // namespace net_instaweb
