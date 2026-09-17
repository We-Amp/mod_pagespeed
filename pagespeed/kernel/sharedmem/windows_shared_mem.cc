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

// Magic number to verify mutex slot is initialized.
constexpr uint32_t kMutexSlotMagic = 0xDEADBEEF;

// Verify structure size at compile time.
static_assert(sizeof(WindowsMutexSlot) == 16,
              "WindowsMutexSlot should be 16 bytes for alignment");
static_assert(alignof(WindowsMutexSlot) <= 8,
              "WindowsMutexSlot alignment should not exceed 8 bytes");

// Wrapper around a Windows Named Mutex for cross-process synchronization.
// Named Mutexes are kernel objects that work correctly across processes,
// unlike CRITICAL_SECTION which only works within a single process.
class WindowsNamedMutex : public AbstractMutex {
 public:
  // Takes ownership of the mutex handle.
  explicit WindowsNamedMutex(HANDLE mutex_handle)
      : mutex_handle_(mutex_handle) {
    DCHECK(mutex_handle_ != nullptr);
  }

  ~WindowsNamedMutex() override {
    if (mutex_handle_ != nullptr) {
      CloseHandle(mutex_handle_);
      mutex_handle_ = nullptr;
    }
  }

  bool TryLock() override {
    DWORD result = WaitForSingleObject(mutex_handle_, 0);
    return result == WAIT_OBJECT_0;
  }

  void Lock() override {
    DWORD result = WaitForSingleObject(mutex_handle_, INFINITE);
    // WAIT_ABANDONED means we got the mutex but the previous owner died
    // without releasing it. We still own it now, so treat as success.
    DCHECK(result == WAIT_OBJECT_0 || result == WAIT_ABANDONED);
  }

  void Unlock() override { ReleaseMutex(mutex_handle_); }

 private:
  HANDLE mutex_handle_;

  WindowsNamedMutex(const WindowsNamedMutex&) = delete;
  WindowsNamedMutex& operator=(const WindowsNamedMutex&) = delete;
};

// Represents a view into a shared memory segment.
class WindowsSharedMemSegment : public AbstractSharedMemSegment {
 public:
  // Takes ownership of the mapped view. Does not own the segment name.
  WindowsSharedMemSegment(char* base, size_t size,
                          const GoogleString& segment_name)
      : base_(base), size_(size), segment_name_(segment_name) {}

  ~WindowsSharedMemSegment() override {
    if (base_ != nullptr) {
      UnmapViewOfFile(base_);
      base_ = nullptr;
    }
  }

  volatile char* Base() override { return base_; }

  size_t SharedMutexSize() const override {
    // We store a MutexSlot structure in the shared memory, which contains
    // the information needed to open the corresponding named mutex.
    // The actual mutex is a kernel object, not stored in shared memory.
    return sizeof(WindowsMutexSlot);
  }

  bool InitializeSharedMutex(size_t offset, MessageHandler* handler) override {
    // Verify offset is within bounds
    if (offset + sizeof(WindowsMutexSlot) > size_) {
      handler->Message(kError,
                       "InitializeSharedMutex: offset %zu + slot size %zu "
                       "exceeds segment size %zu",
                       offset, sizeof(WindowsMutexSlot), size_);
      return false;
    }

    // Get the mutex slot in shared memory
    WindowsMutexSlot* slot =
        reinterpret_cast<WindowsMutexSlot*>(base_ + offset);

    // Generate a unique mutex name based on segment name and offset
    GoogleString mutex_name = GenerateMutexName(offset);

    // Create the named mutex
    // Using NULL security attributes means the mutex can be accessed by
    // any process running under the same user account.
    HANDLE mutex_handle = CreateMutexA(nullptr,  // Default security
                                       FALSE,    // Not initially owned
                                       mutex_name.c_str());

    if (mutex_handle == nullptr) {
      DWORD error = GetLastError();
      handler->Message(kError, "CreateMutex failed for '%s' with error %lu",
                       mutex_name.c_str(), error);
      return false;
    }

    // Check if mutex already existed (this shouldn't happen for initialize)
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
      handler->Message(kWarning,
                       "Named mutex '%s' already existed during initialize",
                       mutex_name.c_str());
      // This is ok - we'll use the existing mutex
    }

    // Note: We intentionally keep one handle open so the named mutex kernel
    // object persists until the segment is destroyed. Without this, the mutex
    // could be destroyed between InitializeSharedMutex and AttachToSharedMutex
    // if no handles are open, causing a race condition.
    // The handle is leaked intentionally; the mutex is cleaned up when the
    // process exits or the segment is destroyed.

    // Write the slot info to shared memory
    slot->magic = kMutexSlotMagic;
    slot->mutex_id = static_cast<uint32_t>(offset);  // Use offset as ID
    slot->padding = 0;

    return true;
  }

  AbstractMutex* AttachToSharedMutex(size_t offset) override {
    // Verify the slot has been initialized
    WindowsMutexSlot* slot =
        reinterpret_cast<WindowsMutexSlot*>(base_ + offset);

    if (slot->magic != kMutexSlotMagic) {
      LOG(ERROR) << "AttachToSharedMutex: slot at offset " << offset
                 << " not initialized (magic=" << std::hex << slot->magic
                 << ", expected=" << kMutexSlotMagic << ")";
      return nullptr;
    }

    // Generate the mutex name from segment name and offset
    GoogleString mutex_name = GenerateMutexName(offset);

    // Use CreateMutexA which either creates a new mutex or opens an existing
    // one. This is more robust than OpenMutexA because it handles the case
    // where the mutex kernel object was destroyed (all handles closed).
    HANDLE mutex_handle = CreateMutexA(nullptr, FALSE, mutex_name.c_str());

    if (mutex_handle == nullptr) {
      LOG(ERROR) << "Failed to create/open mutex '" << mutex_name
                 << "' with error " << GetLastError();
      return nullptr;
    }

    return new WindowsNamedMutex(mutex_handle);
  }

 private:
  // Generates a unique name for a mutex at the given offset.
  // Format: "Local\PageSpeed_mutex_{segment_name}_{offset}"
  // Using "Local\" prefix means the mutex is per-session (which is what
  // we want for IIS worker processes in the same app pool).
  GoogleString GenerateMutexName(size_t offset) const {
    return StrCat("Local\\PageSpeed_mutex_", segment_name_, "_",
                  IntegerToString(offset));
  }

  char* base_;
  size_t size_;
  GoogleString segment_name_;

  WindowsSharedMemSegment(const WindowsSharedMemSegment&) = delete;
  WindowsSharedMemSegment& operator=(const WindowsSharedMemSegment&) = delete;
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

WindowsSharedMem::WindowsSharedMem() : next_mutex_id_(0) {
  instance_number_ = ++s_instance_count_;
  instance_prefix_ = StrCat("ps", IntegerToString(instance_number_), "_");
}

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
  // Size of the MutexSlot structure stored in shared memory.
  // The actual mutex is a kernel object (named mutex).
  return sizeof(WindowsMutexSlot);
}

// Sanitize a string for use in a Windows named object name.
// After the "Local\" namespace prefix, backslashes are treated as namespace
// separators and colons/spaces/slashes are invalid.  Replace them all with
// underscores so the name is unique but safe.
static GoogleString SanitizeObjectName(const GoogleString& raw) {
  GoogleString out = raw;
  for (size_t i = 0; i < out.size(); ++i) {
    char c = out[i];
    if (c == '\\' || c == '/' || c == ':' || c == ' ') {
      out[i] = '_';
    }
  }
  return out;
}

GoogleString WindowsSharedMem::PrefixSegmentName(const GoogleString& name) {
  // Use Local\ prefix for session-local shared memory (works within IIS app
  // pool). Combine instance number and sanitized name for uniqueness.
  return StrCat("Local\\PageSpeed_", IntegerToString(instance_number_), "_",
                SanitizeObjectName(name));
}

AbstractSharedMemSegment* WindowsSharedMem::CreateSegment(
    const GoogleString& name, size_t size, MessageHandler* handler) {
  GoogleString prefixed_name = PrefixSegmentName(name);

  // Create a file mapping object backed by the paging file.
  HANDLE file_mapping = CreateFileMappingA(
      INVALID_HANDLE_VALUE,            // Use paging file
      nullptr,                         // Default security
      PAGE_READWRITE,                  // Read/write access
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

  bool already_exists = (GetLastError() == ERROR_ALREADY_EXISTS);
  if (already_exists) {
    handler->Message(kWarning,
                     "Shared memory segment '%s' already exists, reusing",
                     prefixed_name.c_str());
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

  // Always zero-initialize the memory. For new segments this provides clean
  // state; for existing segments this ensures predictable behavior when
  // CreateSegment is called multiple times (matching POSIX semantics where
  // shmget + IPC_CREAT returns zeroed memory).
  memset(base, 0, size);

  // Store segment info.
  SegmentInfo* info = new SegmentInfo();
  info->file_mapping = file_mapping;
  info->base = base;
  info->size = size;

  // Check for existing segment with same name in our local map.
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

  // Return segment with a simplified name for mutex naming (without Local\
  // prefix).  Sanitize so mutex names are also valid Windows object names.
  GoogleString simple_name =
      SanitizeObjectName(StrCat(IntegerToString(instance_number_), "_", name));
  return new WindowsSharedMemSegment(base, size, simple_name);
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
    GoogleString simple_name = SanitizeObjectName(
        StrCat(IntegerToString(instance_number_), "_", name));
    return new WindowsSharedMemSegment(base, size, simple_name);
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

  GoogleString simple_name =
      SanitizeObjectName(StrCat(IntegerToString(instance_number_), "_", name));
  return new WindowsSharedMemSegment(base, size, simple_name);
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

  // Note: Named mutexes are automatically destroyed when all handles are
  // closed. Since we create fresh handles in AttachToSharedMutex and close
  // them in ~WindowsNamedMutex, the mutexes will be cleaned up as the
  // AbstractMutex objects are deleted.

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
