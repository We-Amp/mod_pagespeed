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

#ifndef NET_INSTAWEB_REWRITER_PUBLIC_NAMED_LOCK_SCHEDULE_REWRITE_CONTROLLER_H_
#define NET_INSTAWEB_REWRITER_PUBLIC_NAMED_LOCK_SCHEDULE_REWRITE_CONTROLLER_H_

#include <memory>
#include <unordered_set>

#include "absl/container/flat_hash_map.h"
#include "pagespeed/kernel/base/abstract_mutex.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/function.h"
#include "pagespeed/kernel/base/named_lock_manager.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_hash.h"
#include "pagespeed/kernel/base/thread_annotations.h"
#include "pagespeed/kernel/base/thread_system.h"

namespace net_instaweb {

// Schedules rewrites by wrapping NamedLockManager, ensuring that only one
// rewrite runs concurrently for each supplied key. A lock held longer than
// kStealMs may be stolen by a later request for the same key.
class NamedLockScheduleRewriteController {
 public:
  static const char kLocksGranted[];
  static const char kLocksDenied[];
  static const char kLocksStolen[];
  static const char kLocksReleasedWhenNotHeld[];
  static const char kLocksCurrentlyHeld[];

  static const int kStealMs;

  NamedLockScheduleRewriteController(NamedLockManager* lock_manager,
                                     ThreadSystem* thread_system,
                                     Statistics* statistics);
  ~NamedLockScheduleRewriteController();

  // Run callback at an indeterminate time in the future when the rewrite
  // for the supplied key should be performed. Calls Cancel immediately
  // if the supplied key is currently in progress.
  void ScheduleRewrite(const GoogleString& key, Function* callback);

  // Inform controller that the rewrite has been completed. Should only be
  // called if Run() was invoked on callback above. Failure should not be
  // used in the case of permanent failure, such as a badly formed input.
  void NotifyRewriteComplete(const GoogleString& key);
  void NotifyRewriteFailed(const GoogleString& key);

  // Cancels any pending operations ASAP, and configures the object to
  // immediately reject new incoming ones.
  void ShutDown();

  static void InitStats(Statistics* stats);

 private:
  struct LockInfo {
    LockInfo() : pin_count(0) {}
    // lock is only non-NULL when we have successfully obtained it.
    std::unique_ptr<NamedLock> lock;

    std::unordered_set<Function*> pending_callbacks;

    // "Extra" refcount on top of lock and pending_callbacks we need
    // occassionally to protect this data against a temporary lock
    // relinquishment.
    int pin_count;

   private:
    LockInfo(const LockInfo&) = delete;
    LockInfo& operator=(const LockInfo&) = delete;
  };

  using LockMap =
      absl::flat_hash_map<GoogleString, LockInfo*, CasePreserveStringHash>;

  void LockObtained(Function* callback, const GoogleString key, NamedLock* lock)
      LOCKS_EXCLUDED(mutex_);
  void LockFailed(Function* callback, const GoogleString key, NamedLock* lock)
      LOCKS_EXCLUDED(mutex_);

  LockInfo* GetLockInfo(const GoogleString& key)
      EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  void DeleteInfoIfUnused(LockInfo* info, const GoogleString& key)
      EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  std::unique_ptr<AbstractMutex> mutex_;
  NamedLockManager* lock_manager_;
  LockMap locks_ GUARDED_BY(mutex_);
  bool shut_down_ GUARDED_BY(mutex_);

  TimedVariable* locks_granted_;
  TimedVariable* locks_denied_;
  TimedVariable* locks_stolen_;
  TimedVariable* locks_released_when_not_held_;
  UpDownCounter* locks_currently_held_;

  NamedLockScheduleRewriteController(
      const NamedLockScheduleRewriteController&) = delete;
  NamedLockScheduleRewriteController& operator=(
      const NamedLockScheduleRewriteController&) = delete;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_CONTROLLER_NAMED_LOCK_SCHEDULE_REWRITE_CONTROLLER_H_
