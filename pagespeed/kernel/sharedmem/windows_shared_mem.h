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
// and Named Mutexes for inter-process synchronization.
//
// Unlike CRITICAL_SECTION (which only works within a single process),
// Named Mutexes are kernel objects that properly support cross-process
// synchronization. This is essential for IIS app pool worker processes
// that need to share cache data.
//
// Design:
// - Memory mapping: CreateFileMapping/MapViewOfFile for shared memory
// - Synchronization: Named Mutexes (CreateMutex/OpenMutex)
//
// The mutex slot in shared memory stores a MutexSlot structure containing:
// - Magic number to verify initialization
// - Unique identifier used to construct the named mutex name
//
// Usage:
// - Parent process calls CreateSegment() to create shared memory
// - InitializeSharedMutex() creates a named mutex for each slot
// - Child processes call AttachToSegment() to map the same memory
// - AttachToSharedMutex() opens the existing named mutex
//
// Thread Safety:
// - Segment creation/destruction is not thread-safe
// - Mutex operations are thread-safe after initialization

#ifndef PAGESPEED_KERNEL_SHAREDMEM_WINDOWS_SHARED_MEM_H_
#define PAGESPEED_KERNEL_SHAREDMEM_WINDOWS_SHARED_MEM_H_

#ifdef _WIN32

#include <cstddef>
#include <cstdint>
#include <map>

#include "pagespeed/kernel/base/abstract_shared_mem.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"

namespace net_instaweb {

class MessageHandler;

// Structure stored in the shared memory slot where a mutex would go.
// This provides the information needed to create/open the named mutex.
struct WindowsMutexSlot {
  // Magic number to verify the slot is initialized (0xDEADBEEF)
  uint32_t magic;
  // Unique identifier for this mutex (derived from segment name + offset)
  uint32_t mutex_id;
  // Padding to ensure proper alignment for next item
  uint64_t padding;
};

// Windows shared memory implementation using Memory Mapped Files.
// This enables inter-process communication for the shared memory cache
// when running as an IIS native module.
class WindowsSharedMem : public AbstractSharedMem {
 public:
  WindowsSharedMem();
  ~WindowsSharedMem() override;

  // Returns the size of a mutex slot in shared memory.
  // We store a MutexSlot structure (16 bytes) which is used to
  // identify the named mutex for cross-process access.
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

  // Counter for generating unique mutex IDs within this instance.
  uint32_t next_mutex_id_;

  // Prefixes segment name with instance number.
  GoogleString PrefixSegmentName(const GoogleString& name);

  // Gets the prefixed name for this instance (without modifying name).
  // Used by segment classes to generate mutex names.
  const GoogleString& GetInstancePrefix() const { return instance_prefix_; }
  GoogleString instance_prefix_;

  WindowsSharedMem(const WindowsSharedMem&) = delete;
  WindowsSharedMem& operator=(const WindowsSharedMem&) = delete;
};

}  // namespace net_instaweb

#endif  // _WIN32

#endif  // PAGESPEED_KERNEL_SHAREDMEM_WINDOWS_SHARED_MEM_H_
