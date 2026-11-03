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

#ifndef NET_INSTAWEB_REWRITER_PUBLIC_PAGE_WORK_BUDGET_H_
#define NET_INSTAWEB_REWRITER_PUBLIC_PAGE_WORK_BUDGET_H_

#include <deque>
#include <mutex>

#include "pagespeed/kernel/base/basictypes.h"

namespace net_instaweb {

class Function;

// The rewrite work one HTML document may cause, shared by every rewrite of
// that document, top-level and nested. A RewriteDriver owns one; rewrites
// done to answer a .pagespeed. fetch do not use it. The limits are fixed: no
// option, request or configuration file changes them. Safe to call from any
// thread.
class PageWorkBudget {
 public:
  struct Limits {
    // Bytes of input the rewrites of one document may load. Once this much
    // is loaded no further input load starts.
    int64 input_bytes = 128 * 1024 * 1024;
    // Input loads one document may have in progress at once. A rewrite that
    // needs another waits for one to finish.
    int input_loads_in_flight = 4;
    // Rewrites one document may start, top-level and nested.
    int64 contexts = 32768;
    // Partition entries the rewrites of one document may copy or report.
    int64 partition_entries = 256 * 1024;
  };

  PageWorkBudget();
  ~PageWorkBudget();

  const Limits& limits() const { return limits_; }
  void set_limits_for_testing(const Limits& limits);

  // Starts the counts of a new document. Input loads in progress and
  // rewrites waiting for one are left alone: they finish on their own.
  void Reset();

  // Takes one rewrite from the budget and returns true, or returns false
  // when the document has started as many as it may.
  bool TakeContext();

  // True if TakeContext() would succeed now; takes nothing.
  bool HasContextLeft() const;

  // Takes num_entries partition entries and returns true, or returns false
  // and spends the rest of the budget when they do not fit, so that later
  // callers stop early.
  bool TakePartitionEntries(int64 num_entries);

  // True once the inputs loaded for the document reach the byte budget.
  bool InputBytesSpent() const;

  // Counts one more input load in progress and returns true, or returns
  // false if as many as allowed are in progress already.
  bool TryStartInputLoad();

  // Keeps resume to be run when an input load finishes. If one may start
  // already, returns resume for the caller to run now instead.
  Function* WaitForInputLoad(Function* resume);

  // Ends an input load that left num_bytes loaded. Returns a waiting resume
  // function the caller must run (on its rewrite sequence), or nullptr.
  Function* InputLoadDone(int64 num_bytes);

  // Returns a waiting resume function to run when another input load may
  // start, or nullptr. Called by a resumed rewrite that took no load, so
  // that its turn passes on.
  Function* NextWaitingIfRoom();

  // For tests.
  int64 input_bytes_loaded() const;
  int max_input_loads_in_flight() const;
  int64 contexts_taken() const;
  int64 contexts_refused() const;
  int64 partition_entries_taken() const;

 private:
  Function* PopWaitingIfRoomLocked();

  mutable std::mutex mutex_;
  Limits limits_;
  int64 input_bytes_loaded_ = 0;
  int input_loads_in_flight_ = 0;
  int max_input_loads_in_flight_ = 0;
  int64 contexts_taken_ = 0;
  int64 contexts_refused_ = 0;
  int64 partition_entries_taken_ = 0;
  std::deque<Function*> waiting_;

  PageWorkBudget(const PageWorkBudget&) = delete;
  PageWorkBudget& operator=(const PageWorkBudget&) = delete;
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_REWRITER_PUBLIC_PAGE_WORK_BUDGET_H_
