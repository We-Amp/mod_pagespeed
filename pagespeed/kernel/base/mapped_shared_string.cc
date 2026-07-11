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

#include "pagespeed/kernel/base/mapped_shared_string.h"

#include <cstdint>
#include <utility>

namespace net_instaweb {

MappedSharedString::MappedSharedString() : storage_(SharedString()) {}

MappedSharedString::MappedSharedString(const SharedString& owned)
    : storage_(owned) {}

MappedSharedString::MappedSharedString(MappedSharedString&& other) noexcept
    : storage_(std::move(other.storage_)) {
  // Leave other in a valid empty state
  other.storage_ = SharedString();
}

MappedSharedString& MappedSharedString::operator=(
    MappedSharedString&& other) noexcept {
  if (this != &other) {
    storage_ = std::move(other.storage_);
    other.storage_ = SharedString();
  }
  return *this;
}

MappedSharedString::MappedSharedString(const MappedSharedString& other)
    : storage_(other.storage_) {}

MappedSharedString& MappedSharedString::operator=(
    const MappedSharedString& other) {
  if (this != &other) {
    storage_ = other.storage_;
  }
  return *this;
}

MappedSharedString::~MappedSharedString() = default;

// static
MappedSharedString MappedSharedString::FromMappedView(
    const char* data, size_t size, MappedReleaseCallback release_callback,
    void* release_data) {
  MappedSharedString result;
  result.storage_ =
      std::make_shared<MappedView>(data, size, release_callback, release_data);
  return result;
}

// static
MappedSharedString MappedSharedString::FromMappedView(
    const char* data, size_t size, MappedReleaseCallback release_callback,
    MappedRenewCallback renew_callback,
    MappedRenewStrictCallback renew_strict_callback,
    MappedNsUntilForcedWrapCallback ns_until_callback, void* release_data) {
  MappedSharedString result;
  result.storage_ = std::make_shared<MappedView>(
      data, size, release_callback, release_data, renew_callback,
      renew_strict_callback, ns_until_callback);
  return result;
}

StringPiece MappedSharedString::Value() const {
  if (std::holds_alternative<SharedString>(storage_)) {
    return std::get<SharedString>(storage_).Value();
  } else {
    const auto& view = std::get<std::shared_ptr<MappedView>>(storage_);
    return StringPiece(view->data, view->size);
  }
}

size_t MappedSharedString::size() const {
  if (std::holds_alternative<SharedString>(storage_)) {
    return std::get<SharedString>(storage_).size();
  } else {
    return std::get<std::shared_ptr<MappedView>>(storage_)->size;
  }
}

const char* MappedSharedString::data() const {
  if (std::holds_alternative<SharedString>(storage_)) {
    return std::get<SharedString>(storage_).data();
  } else {
    return std::get<std::shared_ptr<MappedView>>(storage_)->data;
  }
}

bool MappedSharedString::is_mapped() const {
  return std::holds_alternative<std::shared_ptr<MappedView>>(storage_);
}

SharedString MappedSharedString::ToOwned() const {
  if (std::holds_alternative<SharedString>(storage_)) {
    return std::get<SharedString>(storage_);
  } else {
    // Copy the mapped data into a new SharedString
    const auto& view = std::get<std::shared_ptr<MappedView>>(storage_);
    return SharedString(StringPiece(view->data, view->size));
  }
}

const SharedString* MappedSharedString::AsSharedString() const {
  if (std::holds_alternative<SharedString>(storage_)) {
    return &std::get<SharedString>(storage_);
  }
  return nullptr;
}

bool MappedSharedString::unique() const {
  if (std::holds_alternative<SharedString>(storage_)) {
    return std::get<SharedString>(storage_).unique();
  } else {
    return std::get<std::shared_ptr<MappedView>>(storage_).use_count() == 1;
  }
}

bool MappedSharedString::RenewLease() const {
  if (std::holds_alternative<std::shared_ptr<MappedView>>(storage_)) {
    const auto& view = std::get<std::shared_ptr<MappedView>>(storage_);
    if (view->renew_callback != nullptr) {
      return view->renew_callback(view->release_data) != 0;
    }
  }
  return false;
}

LeaseRenewal MappedSharedString::RenewLeaseStrict() const {
  if (std::holds_alternative<std::shared_ptr<MappedView>>(storage_)) {
    const auto& view = std::get<std::shared_ptr<MappedView>>(storage_);
    if (view->renew_strict_callback != nullptr) {
      return static_cast<LeaseRenewal>(
          view->renew_strict_callback(view->release_data));
    }
  }
  return LeaseRenewal::kLeasesOff;
}

uint64_t MappedSharedString::NsUntilForcedWrap() const {
  if (std::holds_alternative<std::shared_ptr<MappedView>>(storage_)) {
    const auto& view = std::get<std::shared_ptr<MappedView>>(storage_);
    if (view->ns_until_callback != nullptr) {
      return view->ns_until_callback(view->release_data);
    }
  }
  return UINT64_MAX;
}

bool CopyMappedVerified(const StringPiece& span,
                        const MappedSharedString& keepalive,
                        GoogleString* out) {
  span.CopyToString(out);
  // Copy-then-verify: only a genuinely torn borrow (epoch moved under the
  // copy) fails; kOk / kCopyNow (wrap deferred or in flight, region intact)
  // and kLeasesOff (no lease protection configured) serve the copy.
  return keepalive.RenewLeaseStrict() != LeaseRenewal::kTorn;
}

}  // namespace net_instaweb
