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

#ifdef _WIN32

#include "pagespeed/kernel/sharedmem/windows_shared_mem.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>

#include "base/logging.h"
#include "pagespeed/kernel/base/abstract_mutex.h"
#include "pagespeed/kernel/base/abstract_shared_mem.h"
#include "pagespeed/kernel/base/alignment_util.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

namespace {

// Verify alignment requirements at compile time.
static_assert(alignof(CRITICAL_SECTION) <= 16,
              "CRITICAL_SECTION alignment requirement changed");

// Wrapper around a CRITICAL_SECTION in shared memory.
// Unlike std::mutex, CRITICAL_SECTION can be used in shared memory
// across processes when properly initialized.
class WindowsSharedMemMutex : public AbstractMutex {
 public:
  explicit WindowsSharedMemMutex(CRITICAL_SECTION* cs) : cs_(cs) {}
  ~WindowsSharedMemMutex() override {}

  bool TryLock() override { return TryEnterCriticalSection(cs_) != 0; }

  void Lock() override { EnterCriticalSection(cs_); }

  void Unlock() override { LeaveCriticalSection(cs_); }

 private:
  CRITICAL_SECTION* cs_;

  DISALLOW_COPY_AND_ASSIGN(WindowsSharedMemMutex);
};

// Represents a view into a shared memory segment.
class WindowsSharedMemSegment : public AbstractSharedMemSegment {
 public:
  // Takes ownership of the mapped view but not the file mapping handle.
  WindowsSharedMemSegment(char* base, size_t size)
      : base_(base), size_(size) {}

  ~WindowsSharedMemSegment() override {
    if (base_ != nullptr) {
      UnmapViewOfFile(base_);
      base_ = nullptr;
    }
  }

  volatile char* Base() override { return base_; }

  size_t SharedMutexSize() const override {
    // Round up CRITICAL_SECTION size to maintain 16-byte alignment.
    // CRITICAL_SECTION is 40 bytes on x64, so we round to 48.
    return AlignOffset(sizeof(CRITICAL_SECTION), 16);
  }

  bool InitializeSharedMutex(size_t offset, MessageHandler* handler) override {
    // Verify alignment
    char* mutex_addr = base_ + offset;
    if (!IsMutexAligned(mutex_addr)) {
      handler->Message(kError,
                       "CRITICAL_SECTION at offset %zu is not 16-byte aligned. "
                       "Address: %p",
                       offset, static_cast<void*>(mutex_addr));
      return false;
    }

    CRITICAL_SECTION* cs = reinterpret_cast<CRITICAL_SECTION*>(mutex_addr);

    // Initialize the critical section.
    // Note: For cross-process sharing, we'd typically need to use
    // InitializeCriticalSectionAndSpinCount or a named mutex.
    // CRITICAL_SECTION alone only works within a single process.
    // For true cross-process support, consider using named mutexes.
    if (!InitializeCriticalSectionAndSpinCount(cs, 4000)) {
      DWORD error = GetLastError();
      handler->Message(kError,
                       "InitializeCriticalSectionAndSpinCount failed with "
                       "error %lu",
                       error);
      return false;
    }

    return true;
  }

  AbstractMutex* AttachToSharedMutex(size_t offset) override {
    CRITICAL_SECTION* cs = reinterpret_cast<CRITICAL_SECTION*>(base_ + offset);
    return new WindowsSharedMemMutex(cs);
  }

 private:
  char* base_;
  size_t size_;

  DISALLOW_COPY_AND_ASSIGN(WindowsSharedMemSegment);
};

}  // namespace

// Structure to track segment metadata.
struct WindowsSharedMem::SegmentInfo {
  HANDLE file_mapping;
  char* base;
  size_t size;

  SegmentInfo() : file_mapping(nullptr), base(nullptr), size(0) {}
};

size_t WindowsSharedMem::s_instance_count_ = 0;

WindowsSharedMem::WindowsSharedMem() { instance_number_ = ++s_instance_count_; }

WindowsSharedMem::~WindowsSharedMem() {
  // Clean up any remaining segments.
  for (auto& pair : segments_) {
    SegmentInfo* info = pair.second;
    if (info->base != nullptr) {
      UnmapViewOfFile(info->base);
    }
    if (info->file_mapping != nullptr) {
      CloseHandle(info->file_mapping);
    }
    delete info;
  }
  segments_.clear();
}

size_t WindowsSharedMem::SharedMutexSize() const {
  // Round up to 16-byte boundary for alignment.
  return AlignOffset(sizeof(CRITICAL_SECTION), 16);
}

GoogleString WindowsSharedMem::PrefixSegmentName(const GoogleString& name) {
  return StrCat("Local\\PageSpeed_", IntegerToString(instance_number_), "_",
                name);
}

AbstractSharedMemSegment* WindowsSharedMem::CreateSegment(
    const GoogleString& name, size_t size, MessageHandler* handler) {
  GoogleString prefixed_name = PrefixSegmentName(name);

  // Create a file mapping object backed by the paging file.
  HANDLE file_mapping = CreateFileMappingA(
      INVALID_HANDLE_VALUE,  // Use paging file
      nullptr,               // Default security
      PAGE_READWRITE,        // Read/write access
      static_cast<DWORD>(size >> 32),  // High-order DWORD of size
      static_cast<DWORD>(size),        // Low-order DWORD of size
      prefixed_name.c_str());          // Name of mapping object

  if (file_mapping == nullptr) {
    DWORD error = GetLastError();
    handler->Message(kError,
                     "CreateFileMapping failed for segment '%s' with error %lu",
                     prefixed_name.c_str(), error);
    return nullptr;
  }

  // Map the view of the file.
  char* base = static_cast<char*>(
      MapViewOfFile(file_mapping, FILE_MAP_ALL_ACCESS, 0, 0, size));

  if (base == nullptr) {
    DWORD error = GetLastError();
    CloseHandle(file_mapping);
    handler->Message(kError,
                     "MapViewOfFile failed for segment '%s' with error %lu",
                     prefixed_name.c_str(), error);
    return nullptr;
  }

  // Zero-initialize the memory.
  memset(base, 0, size);

  // Store segment info.
  SegmentInfo* info = new SegmentInfo();
  info->file_mapping = file_mapping;
  info->base = base;
  info->size = size;

  // Check for existing segment with same name.
  auto it = segments_.find(prefixed_name);
  if (it != segments_.end()) {
    handler->Message(kWarning,
                     "CreateSegment called twice for '%s', replacing old",
                     prefixed_name.c_str());
    if (it->second->base != nullptr) {
      UnmapViewOfFile(it->second->base);
    }
    if (it->second->file_mapping != nullptr) {
      CloseHandle(it->second->file_mapping);
    }
    delete it->second;
  }

  segments_[prefixed_name] = info;

  return new WindowsSharedMemSegment(base, size);
}

AbstractSharedMemSegment* WindowsSharedMem::AttachToSegment(
    const GoogleString& name, size_t size, MessageHandler* handler) {
  GoogleString prefixed_name = PrefixSegmentName(name);

  // Look up in our local map first (for in-process case).
  auto it = segments_.find(prefixed_name);
  if (it != segments_.end()) {
    SegmentInfo* info = it->second;
    if (info->size != size) {
      handler->Message(kError,
                       "AttachToSegment: size mismatch for '%s' "
                       "(expected %zu, got %zu)",
                       prefixed_name.c_str(), info->size, size);
      return nullptr;
    }
    // Create a new view of the same mapping.
    char* base = static_cast<char*>(
        MapViewOfFile(info->file_mapping, FILE_MAP_ALL_ACCESS, 0, 0, size));
    if (base == nullptr) {
      DWORD error = GetLastError();
      handler->Message(kError,
                       "MapViewOfFile failed in AttachToSegment for '%s' "
                       "with error %lu",
                       prefixed_name.c_str(), error);
      return nullptr;
    }
    return new WindowsSharedMemSegment(base, size);
  }

  // Try to open an existing file mapping (cross-process case).
  HANDLE file_mapping =
      OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, prefixed_name.c_str());

  if (file_mapping == nullptr) {
    DWORD error = GetLastError();
    handler->Message(kError,
                     "OpenFileMapping failed for segment '%s' with error %lu",
                     prefixed_name.c_str(), error);
    return nullptr;
  }

  char* base = static_cast<char*>(
      MapViewOfFile(file_mapping, FILE_MAP_ALL_ACCESS, 0, 0, size));

  if (base == nullptr) {
    DWORD error = GetLastError();
    CloseHandle(file_mapping);
    handler->Message(kError,
                     "MapViewOfFile failed for segment '%s' with error %lu",
                     prefixed_name.c_str(), error);
    return nullptr;
  }

  // Store for future reference.
  SegmentInfo* info = new SegmentInfo();
  info->file_mapping = file_mapping;
  info->base = base;
  info->size = size;
  segments_[prefixed_name] = info;

  return new WindowsSharedMemSegment(base, size);
}

void WindowsSharedMem::DestroySegment(const GoogleString& name,
                                      MessageHandler* handler) {
  GoogleString prefixed_name = PrefixSegmentName(name);

  auto it = segments_.find(prefixed_name);
  if (it == segments_.end()) {
    handler->Message(kWarning, "DestroySegment: segment '%s' not found",
                     prefixed_name.c_str());
    return;
  }

  SegmentInfo* info = it->second;

  // Note: We can't truly destroy CRITICAL_SECTIONs from here since we don't
  // know which offsets have them. The memory will be reclaimed when all
  // handles are closed.

  if (info->base != nullptr) {
    UnmapViewOfFile(info->base);
    info->base = nullptr;
  }

  if (info->file_mapping != nullptr) {
    CloseHandle(info->file_mapping);
    info->file_mapping = nullptr;
  }

  delete info;
  segments_.erase(it);
}

}  // namespace net_instaweb

#endif  // _WIN32
