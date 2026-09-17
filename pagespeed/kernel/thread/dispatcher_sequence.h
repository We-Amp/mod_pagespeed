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

// DispatcherSequence is a Sequence implementation that uses an EventDispatcher
// for task execution. It maintains the sequential execution guarantee: tasks
// added via Add() are executed one at a time in order, even though they may
// run asynchronously on the dispatcher's event loop.

#ifndef PAGESPEED_KERNEL_THREAD_DISPATCHER_SEQUENCE_H_
#define PAGESPEED_KERNEL_THREAD_DISPATCHER_SEQUENCE_H_

// Windows API defines InitiateShutdown as a macro in winreg.h
// We need to undefine it to avoid name collision with our method
#ifdef _WIN32
#ifdef InitiateShutdown
#undef InitiateShutdown
#endif
#endif

#include <deque>
#include <memory>

#include "pagespeed/kernel/base/abstract_mutex.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/thread/sequence.h"

namespace net_instaweb {

class EventDispatcher;
class Function;

// A Sequence implementation that uses an EventDispatcher to execute tasks.
// Tasks are queued internally and dispatched one at a time to maintain
// sequential execution order.
//
// Thread-safety: Add() can be called from any thread. Tasks run on the
// dispatcher's thread.
class DispatcherSequence : public Sequence {
 public:
  // Creates a sequence that will use the given dispatcher for task execution.
  // The dispatcher must outlive this sequence.
  DispatcherSequence(EventDispatcher* dispatcher, ThreadSystem* thread_system);
  ~DispatcherSequence() override;

  // Adds a function to the sequence. The function will be executed after
  // all previously added functions have completed.
  // Thread-safe.
  void Add(Function* function) override;

  // Initiates shutdown. New tasks will have Cancel() called immediately.
  // Tasks already in the queue will be cancelled.
  void InitiateShutdown();

  // Waits for shutdown to complete and all pending tasks to be cancelled.
  void WaitForShutdown();

 private:
  // Called by the dispatcher to run the next task in the queue.
  void RunNextTask();

  // Called after a task completes to potentially schedule the next one.
  void TaskComplete();

  EventDispatcher* dispatcher_;
  std::unique_ptr<AbstractMutex> mutex_;

  // Protected by mutex_
  std::deque<Function*> queue_;
  bool running_task_;       // True if a task is currently executing
  bool shutting_down_;      // True if InitiateShutdown() was called
  bool shutdown_complete_;  // True when all tasks have been cancelled

  DispatcherSequence(const DispatcherSequence&) = delete;
  DispatcherSequence& operator=(const DispatcherSequence&) = delete;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_THREAD_DISPATCHER_SEQUENCE_H_
