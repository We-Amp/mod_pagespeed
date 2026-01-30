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

// Windows shared memory implementation using Memory Mapped Files
// and CRITICAL_SECTION for inter-process synchronization.
//
// IMPORTANT: CRITICAL_SECTION on Windows x64 requires 16-byte alignment.
// This implementation ensures mutexes are placed at properly aligned offsets.
//
// Usage:
// - Parent process calls CreateSegment() to create shared memory
// - Child processes call AttachToSegment() to map the same memory
// - Mutexes in shared memory are initialized with InitializeSharedMutex()
//   and attached to with AttachToSharedMutex()
//
// Thread Safety:
// - Segment creation/destruction is not thread-safe
// - Mutex operations are thread-safe after initialization

#ifndef PAGESPEED_KERNEL_SHAREDMEM_WINDOWS_SHARED_MEM_H_
#define PAGESPEED_KERNEL_SHAREDMEM_WINDOWS_SHARED_MEM_H_

#ifdef _WIN32

#include <cstddef>
#include <map>

#include "pagespeed/kernel/base/abstract_shared_mem.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"

namespace net_instaweb {

class MessageHandler;

// Windows shared memory implementation using Memory Mapped Files.
// This enables inter-process communication for the shared memory cache
// when running as an IIS native module.
class WindowsSharedMem : public AbstractSharedMem {
 public:
  WindowsSharedMem();
  ~WindowsSharedMem() override;

  // Returns the size of a mutex in shared memory.
  // On Windows x64, CRITICAL_SECTION is 40 bytes but we round up to 48
  // to maintain 16-byte alignment for subsequent items.
  size_t SharedMutexSize() const override;

  // Creates a new shared memory segment with the given name and size.
  // Returns nullptr on failure.
  AbstractSharedMemSegment* CreateSegment(const GoogleString& name, size_t size,
                                          MessageHandler* handler) override;

  // Attaches to an existing shared memory segment.
  // The segment must have been created with CreateSegment().
  // Returns nullptr on failure.
  AbstractSharedMemSegment* AttachToSegment(const GoogleString& name,
                                            size_t size,
                                            MessageHandler* handler) override;

  // Destroys a shared memory segment.
  // All attached segments must be detached before calling this.
  void DestroySegment(const GoogleString& name,
                      MessageHandler* handler) override;

 private:
  // Internal structure tracking segment metadata.
  struct SegmentInfo;

  // Map of segment names to their metadata.
  std::map<GoogleString, SegmentInfo*> segments_;

  // Instance counter for unique segment naming across multiple factories.
  static size_t s_instance_count_;
  size_t instance_number_;

  // Prefixes segment name with instance number.
  GoogleString PrefixSegmentName(const GoogleString& name);

  DISALLOW_COPY_AND_ASSIGN(WindowsSharedMem);
};

}  // namespace net_instaweb

#endif  // _WIN32

#endif  // PAGESPEED_KERNEL_SHAREDMEM_WINDOWS_SHARED_MEM_H_
