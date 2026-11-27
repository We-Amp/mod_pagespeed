// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Compat header for the vendored 1.15 HTML kernel (#1130): stands in
// for the canonical pagespeed/kernel/http/google_url.h. The sync rewrites the
// vendored files' includes to land here.
//
// !!! THIS IS NOT google_url !!!
// Despite the class name, net_instaweb::GoogleUrl here is NOT the Chromium
// google-url library and performs NO real URL parsing or validation. It is
// the Tier-1 URL-validation policy seam: a spec holder whose validity is
// decided by a pluggable, process-wide UrlValidationPolicy. The class name
// intentionally matches canonical so the vendored kernel syncs with ZERO
// renames.
//
// DEFERRED product-visible decision (normative record): whether PageSpeed
// 2.0 restores real URL validation (a Chromium-google-url-grade parser) is
// UNDECIDED. The default policy below — NonEmptyUrlPolicy, valid iff the
// spec is non-empty — preserves 2.0's historical behavior EXACTLY: 2.0's
// curated HTML kernel never validated URLs beyond non-emptiness, and
// Normalize is the identity, so Spec() round-trips byte-identical. Plugging
// in a real policy later (SetPolicy) needs NO canonical-code change: the
// vendored kernel calls Reset/IsValid/Spec exactly as it does against
// canonical google-url, and only this header changes.
//
// Thread-safety: the policy is process-wide state, guarded by a plain
// std::mutex. The natural C++20 choice — std::atomic<std::shared_ptr<const
// UrlValidationPolicy>> — is unavailable in the libc++ (LLVM 18) the repo's
// --config=asan/--config=libc++ builds use, so the mutex is the simplest
// option that is correct on every toolchain this repo builds with. The lock
// is held only for the shared_ptr copy/store inside GetPolicy()/SetPolicy(),
// never while a policy's IsValid/Normalize runs. SetPolicy is expected to be
// called at init time, but is safe at any time.

#ifndef PAGESPEED_LIB_HTML_COMPAT_GOOGLE_URL_H_
#define PAGESPEED_LIB_HTML_COMPAT_GOOGLE_URL_H_

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace net_instaweb {

// Policy deciding whether a URL spec is valid, and how (if at all) it is
// normalized on ingestion. Implementations must be thread-safe: the active
// policy is shared process-wide.
class UrlValidationPolicy {
 public:
  virtual ~UrlValidationPolicy() = default;

  virtual bool IsValid(std::string_view spec) const = 0;

  // Defaults to the identity copy — no normalization, matching 2.0's
  // historical byte-identical spec round-trip.
  virtual std::string Normalize(std::string_view spec) const {
    return std::string(spec);
  }
};

// The default policy: a spec is valid iff it is non-empty. This is EXACTLY
// 2.0's historical (pre-vendoring) behavior and must stay the default until
// the deferred restore-real-validation decision lands.
class NonEmptyUrlPolicy final : public UrlValidationPolicy {
 public:
  bool IsValid(std::string_view spec) const override { return !spec.empty(); }
};

// Tier-1 stand-in for canonical google-url's GoogleUrl: holds a (possibly
// normalized) spec plus a validity bit computed by the active policy at
// Reset() time. See the file-top comment — this is NOT a URL parser.
class GoogleUrl {
 public:
  // Default-constructed URLs are invalid (canonical behavior).
  GoogleUrl() = default;
  explicit GoogleUrl(std::string_view spec) { Reset(spec); }

  // Stores Normalize(spec) under the active policy and sets the validity
  // bit to the policy's verdict on the stored spec. Returns IsValid().
  bool Reset(std::string_view spec) {
    const std::shared_ptr<const UrlValidationPolicy> policy = GetPolicy();
    spec_ = policy->Normalize(spec);
    valid_ = policy->IsValid(spec_);
    return valid_;
  }

  bool IsValid() const { return valid_; }
  // Canonical compatibility alias: identical to IsValid() in this seam.
  bool IsAnyValid() const { return IsValid(); }

  const std::string& Spec() const { return spec_; }

  void Swap(GoogleUrl* other) {
    spec_.swap(other->spec_);
    std::swap(valid_, other->valid_);
  }

  // Installs a new process-wide policy; SetPolicy(nullptr) restores the
  // default NonEmptyUrlPolicy. Safe to call concurrently with readers.
  // SetPolicy/GetPolicy (and thus GoogleUrl::Reset) must NOT be called from
  // static destructors: the policy statics are function-local and may already
  // be destroyed at static-teardown time.
  static void SetPolicy(std::shared_ptr<const UrlValidationPolicy> policy) {
    if (policy == nullptr) {
      policy = DefaultPolicy();
    }
    const std::lock_guard<std::mutex> lock(PolicyMutex());
    PolicyStorage() = std::move(policy);
  }

  static std::shared_ptr<const UrlValidationPolicy> GetPolicy() {
    const std::lock_guard<std::mutex> lock(PolicyMutex());
    return PolicyStorage();
  }

 private:
  static std::shared_ptr<const UrlValidationPolicy> DefaultPolicy() {
    static const std::shared_ptr<const UrlValidationPolicy> kDefault =
        std::make_shared<NonEmptyUrlPolicy>();
    return kDefault;
  }

  static std::mutex& PolicyMutex() {
    static std::mutex mutex;
    return mutex;
  }

  // Guarded by PolicyMutex().
  static std::shared_ptr<const UrlValidationPolicy>& PolicyStorage() {
    static std::shared_ptr<const UrlValidationPolicy> policy = DefaultPolicy();
    return policy;
  }

  std::string spec_;
  bool valid_ = false;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_LIB_HTML_COMPAT_GOOGLE_URL_H_
