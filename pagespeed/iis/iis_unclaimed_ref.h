// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_IIS_IIS_UNCLAIMED_REF_H_
#define PAGESPEED_IIS_IIS_UNCLAIMED_REF_H_

namespace net_instaweb {

// Holds one reference to a reference-counted object (AddRef/ReleaseRef)
// until another owner claims it. A reference that is never claimed is
// released when the holder is destroyed, so a caller that gave up half way
// (for example because the object failed to initialize) cannot keep the
// object alive for good.
//
// Header-only and free of IIS/Windows headers so the ownership rules are
// unit-testable on every platform.
template <typename T>
class UnclaimedRef {
 public:
  UnclaimedRef() : ptr_(nullptr) {}
  ~UnclaimedRef() { Reset(); }

  // Takes over `ptr`, which carries the reference this holder now owns.
  // Any reference held before is released.
  void Adopt(T* ptr) {
    Reset();
    ptr_ = ptr;
  }

  T* get() const { return ptr_; }

  // Hands the reference to the caller, who becomes responsible for
  // releasing it. The holder forgets the object.
  T* Claim() {
    T* ptr = ptr_;
    ptr_ = nullptr;
    return ptr;
  }

  // Releases the held reference, if any.
  void Reset() {
    T* ptr = ptr_;
    ptr_ = nullptr;
    if (ptr != nullptr) {
      ptr->ReleaseRef();
    }
  }

 private:
  T* ptr_;

  UnclaimedRef(const UnclaimedRef&) = delete;
  UnclaimedRef& operator=(const UnclaimedRef&) = delete;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_IIS_IIS_UNCLAIMED_REF_H_
