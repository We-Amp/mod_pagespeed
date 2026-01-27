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

#include "pagespeed/kernel/thread/std_rw_lock.h"

#include <condition_variable>
#include <mutex>

namespace net_instaweb {

StdRWLock::StdRWLock()
    : readers_count_(0), writers_waiting_(0), writer_active_(false) {}

StdRWLock::~StdRWLock() {}

bool StdRWLock::TryLock() {
  std::unique_lock<std::mutex> lock(mutex_);
  if (writer_active_ || readers_count_ > 0) {
    return false;
  }
  writer_active_ = true;
  return true;
}

void StdRWLock::Lock() {
  std::unique_lock<std::mutex> lock(mutex_);
  ++writers_waiting_;
  writers_cv_.wait(lock,
                   [this] { return !writer_active_ && readers_count_ == 0; });
  --writers_waiting_;
  writer_active_ = true;
}

void StdRWLock::Unlock() {
  std::unique_lock<std::mutex> lock(mutex_);
  writer_active_ = false;
  // Prefer waking up another writer if one is waiting, otherwise wake all
  // readers. This maintains writer priority.
  if (writers_waiting_ > 0) {
    writers_cv_.notify_one();
  } else {
    readers_cv_.notify_all();
  }
}

bool StdRWLock::ReaderTryLock() {
  std::unique_lock<std::mutex> lock(mutex_);
  // Don't allow readers if a writer is active or waiting (writer priority)
  if (writer_active_ || writers_waiting_ > 0) {
    return false;
  }
  ++readers_count_;
  return true;
}

void StdRWLock::ReaderLock() {
  std::unique_lock<std::mutex> lock(mutex_);
  // Wait until no writer is active and no writers are waiting (writer priority)
  readers_cv_.wait(lock,
                   [this] { return !writer_active_ && writers_waiting_ == 0; });
  ++readers_count_;
}

void StdRWLock::ReaderUnlock() {
  std::unique_lock<std::mutex> lock(mutex_);
  --readers_count_;
  // If this was the last reader and a writer is waiting, wake it up
  if (readers_count_ == 0 && writers_waiting_ > 0) {
    writers_cv_.notify_one();
  }
}

}  // namespace net_instaweb
