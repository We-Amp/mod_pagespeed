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

// Platform-aware alignment utilities for shared memory structures.
//
// Different platforms have different alignment requirements for synchronization
// primitives placed in shared memory:
//   - Linux/macOS: pthread_mutex_t requires 8-byte alignment
//   - Windows x64: CRITICAL_SECTION requires 16-byte alignment
//
// This header provides utilities to calculate proper offsets for placing
// mutexes and other aligned data in shared memory segments.

#ifndef PAGESPEED_KERNEL_BASE_ALIGNMENT_UTIL_H_
#define PAGESPEED_KERNEL_BASE_ALIGNMENT_UTIL_H_

#include <cstddef>
#include <cstdint>

namespace net_instaweb {

// Returns the required alignment for mutex types on the current platform.
// - Linux/macOS: 8 bytes (pthread_mutex_t)
// - Windows x64: 16 bytes (CRITICAL_SECTION)
inline constexpr size_t MutexAlignment() {
#ifdef _WIN32
  return 16;  // CRITICAL_SECTION requires 16-byte alignment on x64
#else
  return 8;   // pthread_mutex_t requires 8-byte alignment
#endif
}

// Returns the maximum alignment requirement for any shared memory component.
// Currently this is determined by mutex alignment requirements.
inline constexpr size_t SharedMemMaxAlignment() {
  return MutexAlignment();
}

// Aligns 'offset' up to the next multiple of 'alignment'.
// alignment must be a power of 2.
inline size_t AlignOffset(size_t offset, size_t alignment) {
  return (offset + (alignment - 1)) & ~(alignment - 1);
}

// Aligns offset to the platform's required mutex alignment.
inline size_t AlignForMutex(size_t offset) {
  return AlignOffset(offset, MutexAlignment());
}

// Aligns offset to 8-byte boundary (for int64, pointers, etc.)
inline size_t AlignTo8(size_t offset) {
  return AlignOffset(offset, 8);
}

// Checks if an address is properly aligned for a mutex.
inline bool IsMutexAligned(const void* ptr) {
  return (reinterpret_cast<uintptr_t>(ptr) & (MutexAlignment() - 1)) == 0;
}

// Checks if an address is aligned to the specified boundary.
inline bool IsAligned(const void* ptr, size_t alignment) {
  return (reinterpret_cast<uintptr_t>(ptr) & (alignment - 1)) == 0;
}

}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_BASE_ALIGNMENT_UTIL_H_
