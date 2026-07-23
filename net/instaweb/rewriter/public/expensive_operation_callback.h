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

#ifndef NET_INSTAWEB_REWRITER_PUBLIC_EXPENSIVE_OPERATION_CALLBACK_H_
#define NET_INSTAWEB_REWRITER_PUBLIC_EXPENSIVE_OPERATION_CALLBACK_H_

#include <memory>

#include "net/instaweb/rewriter/public/sequence_bound_callback.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/thread/sequence.h"

// Callback classes to support rate-limiting CPU intensive operations via
// WorkBoundExpensiveOperationController.

namespace net_instaweb {

// Passed to RunImpl for implementations of ExpensiveOperationCallback.
class ExpensiveOperationContext {
 public:
  virtual ~ExpensiveOperationContext();

  // Mark the expensive operation as complete. Automatically invoked at
  // destruction if not explicitly called.
  virtual void Done() = 0;

 protected:
  ExpensiveOperationContext();

 private:
  ExpensiveOperationContext(const ExpensiveOperationContext&) = delete;
  ExpensiveOperationContext& operator=(const ExpensiveOperationContext&) =
      delete;
};

// Implementor interface for expensive-operation scheduling.
class ExpensiveOperationCallback
    : public SequenceBoundCallback<ExpensiveOperationContext> {
 public:
  explicit ExpensiveOperationCallback(Sequence* sequence);
  ~ExpensiveOperationCallback() override;

 private:
  // SequenceBoundCallback interface.
  void RunImpl(std::unique_ptr<ExpensiveOperationContext>* context) override =
      0;
  void CancelImpl() override = 0;

  ExpensiveOperationCallback(const ExpensiveOperationCallback&) = delete;
  ExpensiveOperationCallback& operator=(const ExpensiveOperationCallback&) =
      delete;
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_REWRITER_PUBLIC_EXPENSIVE_OPERATION_CALLBACK_H_
