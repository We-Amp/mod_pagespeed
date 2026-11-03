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

#include "net/instaweb/rewriter/public/page_work_budget.h"

#include <algorithm>

#include "base/logging.h"
#include "pagespeed/kernel/base/function.h"

namespace net_instaweb {

PageWorkBudget::PageWorkBudget() {}

PageWorkBudget::~PageWorkBudget() {
  DCHECK(waiting_.empty());
  DCHECK_EQ(0, input_loads_in_flight_);
}

void PageWorkBudget::set_limits_for_testing(const Limits& limits) {
  std::lock_guard<std::mutex> lock(mutex_);
  limits_ = limits;
}

void PageWorkBudget::Reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  input_bytes_loaded_ = 0;
  max_input_loads_in_flight_ = input_loads_in_flight_;
  contexts_taken_ = 0;
  contexts_refused_ = 0;
  partition_entries_taken_ = 0;
}

bool PageWorkBudget::TakeContext() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (contexts_taken_ >= limits_.contexts) {
    ++contexts_refused_;
    return false;
  }
  ++contexts_taken_;
  return true;
}

bool PageWorkBudget::HasContextLeft() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return contexts_taken_ < limits_.contexts;
}

bool PageWorkBudget::TakePartitionEntries(int64 num_entries) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (num_entries < 0 ||
      num_entries > limits_.partition_entries - partition_entries_taken_) {
    partition_entries_taken_ =
        std::max(partition_entries_taken_, limits_.partition_entries);
    return false;
  }
  partition_entries_taken_ += num_entries;
  return true;
}

bool PageWorkBudget::InputBytesSpent() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return input_bytes_loaded_ >= limits_.input_bytes;
}

bool PageWorkBudget::TryStartInputLoad() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (input_loads_in_flight_ >= limits_.input_loads_in_flight) {
    return false;
  }
  ++input_loads_in_flight_;
  max_input_loads_in_flight_ =
      std::max(max_input_loads_in_flight_, input_loads_in_flight_);
  return true;
}

Function* PageWorkBudget::WaitForInputLoad(Function* resume) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (input_loads_in_flight_ < limits_.input_loads_in_flight) {
    // A load ended since the caller looked; it need not wait.
    return resume;
  }
  waiting_.push_back(resume);
  return nullptr;
}

Function* PageWorkBudget::InputLoadDone(int64 num_bytes) {
  std::lock_guard<std::mutex> lock(mutex_);
  DCHECK_LT(0, input_loads_in_flight_);
  --input_loads_in_flight_;
  if (num_bytes > 0) {
    input_bytes_loaded_ += num_bytes;
  }
  return PopWaitingIfRoomLocked();
}

Function* PageWorkBudget::NextWaitingIfRoom() {
  std::lock_guard<std::mutex> lock(mutex_);
  return PopWaitingIfRoomLocked();
}

Function* PageWorkBudget::PopWaitingIfRoomLocked() {
  if (waiting_.empty() ||
      input_loads_in_flight_ >= limits_.input_loads_in_flight) {
    return nullptr;
  }
  Function* resume = waiting_.front();
  waiting_.pop_front();
  return resume;
}

int64 PageWorkBudget::input_bytes_loaded() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return input_bytes_loaded_;
}

int PageWorkBudget::max_input_loads_in_flight() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return max_input_loads_in_flight_;
}

int64 PageWorkBudget::contexts_taken() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return contexts_taken_;
}

int64 PageWorkBudget::contexts_refused() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return contexts_refused_;
}

int64 PageWorkBudget::partition_entries_taken() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return partition_entries_taken_;
}

}  // namespace net_instaweb
