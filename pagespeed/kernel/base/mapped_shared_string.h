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

// MappedSharedString provides a string view that can be backed by either:
// 1. An owned SharedString (current behavior, with copy)
// 2. A borrowed view into memory-mapped storage (zero-copy)
//
// This class enables zero-copy reads from disk caches that use memory-mapped
// I/O, such as the Cyclone cache. When data is read from such a cache, it can
// be accessed directly through the mmap'd region without copying into a
// separate buffer.
//
// Usage:
//   // From owned SharedString (copies data)
//   SharedString owned("hello");
//   MappedSharedString mss(owned);
//   StringPiece view = mss.Value();  // Points to owned copy
//
//   // From mapped view (zero-copy)
//   MappedSharedString mss = MappedSharedString::FromMappedView(
//       data_ptr, size, release_callback, release_data);
//   StringPiece view = mss.Value();  // Points directly to mmap'd memory
//
// The release callback is invoked when the MappedSharedString is destroyed
// and no other copies exist. This allows proper cleanup of the underlying
// resource (e.g., closing a Cyclone read handle).
//
// Thread Safety:
// - Multiple threads may concurrently call const methods (Value(), size(),
//   data(), is_mapped(), ToOwned(), AsSharedString(), unique()) on the same
//   MappedSharedString instance.
// - Concurrent modification (copy/move assignment) to the same instance
//   requires external synchronization.
// - Different threads may safely hold and use separate copies of the same
//   MappedSharedString concurrently.
// - The release callback is invoked exactly once, from the thread that
//   destroys the last reference. The callback must be thread-safe if
//   it accesses shared state.
// - The underlying reference counting uses atomic operations.

#ifndef PAGESPEED_KERNEL_BASE_MAPPED_SHARED_STRING_H_
#define PAGESPEED_KERNEL_BASE_MAPPED_SHARED_STRING_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <variant>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/shared_string.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

// Result of the intent-checked lease renewal a zero-copy
// embedder must call before every aliased send of a borrowed region
// (RenewLeaseStrict).  Mirrors cyclone::LeaseRenewal (kept an independent
// embedder-side type so this header does not depend on cyclone).
enum class LeaseRenewal : std::uint8_t {
  kOk = 0,       // Keep aliasing: lease live, no wrap intent, epoch unchanged.
  kCopyNow = 1,  // A wrap is in flight (region intact) — de-alias by copying.
  kTorn = 2,     // Epoch moved — region may be overwritten — abort the serve.
  kLeasesOff = 3,  // No lease protection — copy but do NOT abort (legacy).
};

// Callback type for releasing mapped memory resources.
// The callback receives a user-provided data pointer.
using MappedReleaseCallback = void (*)(void* user_data);

// Zero-copy lease hooks (optional).  Re-stamp the read lease
// pinning the mapped region; returns nonzero if still valid, 0 if the
// region may have been overwritten (embedder must copy).
using MappedRenewCallback = int (*)(void* user_data);
// Ns until a ceiling-forced wrap could overwrite the mapped region;
// UINT64_MAX when none is deferred.
using MappedNsUntilForcedWrapCallback = uint64_t (*)(void* user_data);
// Intent-checked renewal for the aliased path; returns a
// LeaseRenewal value as int (see RenewLeaseStrict()).
using MappedRenewStrictCallback = int (*)(void* user_data);

// MappedSharedString can hold either an owned SharedString or a borrowed
// view into mapped memory. It provides uniform access via Value().
class MappedSharedString {
 public:
  // Default constructor creates an empty string.
  MappedSharedString();

  // Construct from an owned SharedString (copies the reference, not the data).
  explicit MappedSharedString(const SharedString& owned);

  // Move constructor and assignment.
  MappedSharedString(MappedSharedString&& other) noexcept;
  MappedSharedString& operator=(MappedSharedString&& other) noexcept;

  // Copy constructor and assignment.
  // For mapped views, this increments the reference count.
  MappedSharedString(const MappedSharedString& other);
  MappedSharedString& operator=(const MappedSharedString& other);

  ~MappedSharedString();

  // Create a MappedSharedString from a memory-mapped view.
  // The release_callback will be called with release_data when all references
  // to this mapped view are released.
  //
  // Parameters:
  //   data: Pointer to the mapped memory (must remain valid until released)
  //   size: Size of the data in bytes
  //   release_callback: Function to call when releasing (may be nullptr)
  //   release_data: User data to pass to release_callback
  static MappedSharedString FromMappedView(
      const char* data, size_t size, MappedReleaseCallback release_callback,
      void* release_data);

  // As above, additionally carrying the lease hooks so a zero-copy
  // embedder can renew the lease and query the force-wrap deadline while it
  // holds the borrow.  release_data is passed to all three callbacks.
  static MappedSharedString FromMappedView(
      const char* data, size_t size, MappedReleaseCallback release_callback,
      MappedRenewCallback renew_callback,
      MappedRenewStrictCallback renew_strict_callback,
      MappedNsUntilForcedWrapCallback ns_until_callback, void* release_data);

  // Returns the value as a StringPiece.
  // The returned StringPiece is valid as long as this MappedSharedString
  // (or any copy of it) is alive.
  StringPiece Value() const;

  // Returns the size of the string.
  size_t size() const;

  // Returns a pointer to the data.
  const char* data() const;

  // Returns true if the string is empty.
  bool empty() const { return size() == 0; }

  // Returns true if this is backed by mapped memory (zero-copy).
  // Returns false if backed by an owned SharedString.
  bool is_mapped() const;

  // Converts to an owned SharedString, copying the data if necessary.
  // If already owned, returns a copy of the SharedString (no data copy).
  // If mapped, copies the data into a new SharedString.
  SharedString ToOwned() const;

  // Returns the underlying SharedString if owned, or nullptr if mapped.
  // This is useful for code that needs to work with SharedString directly.
  const SharedString* AsSharedString() const;

  // Returns true if this is the only reference to the underlying storage.
  bool unique() const;

  // Re-stamp the read lease pinning a mapped view.  Returns true
  // if still valid (epoch unchanged), false if the region may have been
  // overwritten (embedder must copy) or not applicable (owned / no hook).
  bool RenewLease() const;
  // Intent-checked renewal for the ALIASED zero-copy
  // path.  The embedder MUST call this before every aliased send of the
  // borrowed region and act on the result (kOk keep aliasing; kCopyNow /
  // kLeasesOff de-alias by copying; kTorn abort).  Owned string / no hook
  // returns kLeasesOff (no lease to check).  See RenewLease() for the
  // epoch-only variant used by the copy-then-verify path.
  LeaseRenewal RenewLeaseStrict() const;
  // Ns until a ceiling-forced wrap could overwrite a mapped view;
  // UINT64_MAX when none deferred / not applicable (owned / no hook).
  uint64_t NsUntilForcedWrap() const;

 private:
  // Internal holder for mapped view with reference counting.
  struct MappedView {
    const char* data;
    size_t size;
    MappedReleaseCallback release_callback;
    void* release_data;
    MappedRenewCallback renew_callback = nullptr;
    MappedRenewStrictCallback renew_strict_callback = nullptr;
    MappedNsUntilForcedWrapCallback ns_until_callback = nullptr;

    MappedView(const char* d, size_t s, MappedReleaseCallback cb, void* ud,
               MappedRenewCallback rcb = nullptr,
               MappedRenewStrictCallback rscb = nullptr,
               MappedNsUntilForcedWrapCallback ncb = nullptr)
        : data(d),
          size(s),
          release_callback(cb),
          release_data(ud),
          renew_callback(rcb),
          renew_strict_callback(rscb),
          ns_until_callback(ncb) {}

    ~MappedView() {
      if (release_callback) {
        release_callback(release_data);
      }
    }

    // Non-copyable, non-movable (shared_ptr handles sharing)
    MappedView(const MappedView&) = delete;
    MappedView& operator=(const MappedView&) = delete;
  };

  // Storage: either an owned SharedString or a shared pointer to mapped view.
  std::variant<SharedString, std::shared_ptr<MappedView>> storage_;
};

// Verified de-alias, the one blessed way to turn borrowed mapped
// bytes into owned bytes: copies 'span' (which must alias the region pinned
// by 'keepalive') into *out, then re-checks the borrow AFTER the memcpy
// (copy-then-verify -- a ceiling-forced wrap ignores the read lease and can
// race the copy, so checking first proves nothing).  Returns false iff the
// borrow is torn (the epoch moved): *out may then hold garbage and must not
// be served, recorded, or cached.  An owned or hook-less keepalive always
// verifies (kLeasesOff: legacy posture, copy without failing).
bool CopyMappedVerified(const StringPiece& span,
                        const MappedSharedString& keepalive, GoogleString* out);

}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_BASE_MAPPED_SHARED_STRING_H_
