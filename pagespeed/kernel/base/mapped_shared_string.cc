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

}  // namespace net_instaweb
