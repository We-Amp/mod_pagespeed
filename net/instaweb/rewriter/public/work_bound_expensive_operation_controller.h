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

#ifndef NET_INSTAWEB_REWRITER_PUBLIC_WORK_BOUND_EXPENSIVE_OPERATION_CONTROLLER_H_
#define NET_INSTAWEB_REWRITER_PUBLIC_WORK_BOUND_EXPENSIVE_OPERATION_CONTROLLER_H_

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/function.h"
#include "pagespeed/kernel/base/statistics.h"

namespace net_instaweb {

// Rate-limits multiple simultaneous expensive jobs using a statistic.
// Named after now removed WorkBound class. This uses Statistics to
// communicate between multiple worker processes so does not have the
// cross-process constraints of a queue-based implementation. However, this
// implementation does not queue requests, instead observing the count of
// in-progress operations and canceling the request if that number is too
// great.
class WorkBoundExpensiveOperationController {
 public:
  static const char kCurrentExpensiveOperations[];

  WorkBoundExpensiveOperationController(int max_expensive_operations,
                                        Statistics* stats);
  ~WorkBoundExpensiveOperationController();

  // Runs callback at an indeterminate time in the future when it is safe
  // to perform a CPU intensive operation. Cancels the callback instead if
  // the number of in-progress operations meets or exceeds the bound.
  void ScheduleExpensiveOperation(Function* callback);

  // Inform controller that the operation has been completed.
  // Should only be called if Run() was invoked on callback above.
  void NotifyExpensiveOperationComplete();

  static void InitStats(Statistics* stats);

 private:
  bool TryToWork();

  const int bound_;
  UpDownCounter* counter_;

  WorkBoundExpensiveOperationController(
      const WorkBoundExpensiveOperationController&) = delete;
  WorkBoundExpensiveOperationController& operator=(
      const WorkBoundExpensiveOperationController&) = delete;
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_REWRITER_PUBLIC_WORK_BOUND_EXPENSIVE_OPERATION_CONTROLLER_H_
