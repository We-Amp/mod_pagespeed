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

#ifndef NET_INSTAWEB_REWRITER_PUBLIC_SEQUENCE_BOUND_CALLBACK_H_
#define NET_INSTAWEB_REWRITER_PUBLIC_SEQUENCE_BOUND_CALLBACK_H_

#include <memory>

#include "base/logging.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/function.h"
#include "pagespeed/kernel/thread/sequence.h"

namespace net_instaweb {

// SequenceBoundCallback is a Function specialization that re-queues Run() and
// Cancel() onto a Sequence before doing the actual work. Users are expected
// to interact with this via a purpose-specific subclass, eg:
// ExpensiveOperationCallback. It is used to decouple scheduling decisions
// (which may be made on an arbitrary thread, e.g. by a lock manager) from the
// execution of the work itself, which must happen on a particular Sequence.
//
// If the scheduler accepts the request, Run() will be called. At this point
// the scheduler may have allocated resources which must be returned. However,
// it is also possible that the callback will be load-shed from the Sequence.
// It is important that the scheduler is *always* notified when it can reclaim
// the resources, even if the actual operation is load-shed. This is where the
// TransactionContext comes in; it guarantees to notify the scheduler to
// release any held resources exactly once, either upon destruction of the
// context or by explicit calls from the consumer class(es). Construction and
// exact semantics of the TransactionContext are managed by the scheduling
// code (see e.g. WorkBoundExpensiveOperationController's call sites).
//
// The TransactionContext is also the way a caller can signal information
// back. For instance, it may implement a Success() or Failure() method. For
// the case where the operation performed by the caller outlives the Run()
// callback, a scoped_ptr to the context is passed into RunImpl(), which may
// "steal" the pointer.
//
// The scheduler also has the option of denying the operation, which will
// result in a call to Cancel(). It is the responsibility of the
// TransactionContext to clean up in the case where a denial occurs partway
// through a transaction.

template <typename TransactionContext>
class SequenceBoundCallback : public Function {
 public:
  ~SequenceBoundCallback() override;

  // Called by the scheduler at some point before Run or Cancel.
  // Takes ownership of the transaction context.
  // TODO(cheesy): It would be nice if this wasn't public, but that causes
  // mutual visibility headaches with the context implementations.
  void SetTransactionContext(TransactionContext* ctx);

 protected:
  explicit SequenceBoundCallback(Sequence* sequence);

  // Function interface. These may be invoked on an arbitrary scheduling
  // thread, so must be quick. They just enqueue calls on sequence_ to the
  // actual implementations (RunAfterRequeue & CancelAfterRequeue).
  void Run() override /* override */;
  void Cancel() override /* override */;

 private:
  // Subclasses should implement whatever functionality they need in these.
  // They are equivalent to the Run() and Cancel() methods on Function.
  virtual void RunImpl(std::unique_ptr<TransactionContext>* context) = 0;
  virtual void CancelImpl() = 0;

  // Invoked via sequence_ to do the typical Function operations.
  void RunAfterRequeue();
  void CancelAfterRequeue();

  Sequence* sequence_;
  std::unique_ptr<TransactionContext> context_;

  SequenceBoundCallback(const SequenceBoundCallback&) = delete;
  SequenceBoundCallback& operator=(const SequenceBoundCallback&) = delete;
};

template <typename TransactionContext>
SequenceBoundCallback<TransactionContext>::SequenceBoundCallback(
    Sequence* sequence)
    : sequence_(sequence) {
  set_delete_after_callback(false);
}

template <typename TransactionContext>
SequenceBoundCallback<TransactionContext>::~SequenceBoundCallback() {}

template <typename TransactionContext>
void SequenceBoundCallback<TransactionContext>::Run() {
  CHECK(context_ != NULL);
  // Now enqueue the call to actually run.
  // Will synchronously call CancelAfterRequeue if sequence_ is shutdown.
  sequence_->Add(MakeFunction(
      this, &SequenceBoundCallback<TransactionContext>::RunAfterRequeue,
      &SequenceBoundCallback<TransactionContext>::CancelAfterRequeue));
}

template <typename TransactionContext>
void SequenceBoundCallback<TransactionContext>::Cancel() {
  // Scheduler rejected the request. Enqueue a Cancellation.
  // Will synchronously call CancelAfterRequeue if sequence_ is shutdown.
  sequence_->Add(MakeFunction(
      this, &SequenceBoundCallback<TransactionContext>::CancelAfterRequeue,
      &SequenceBoundCallback<TransactionContext>::CancelAfterRequeue));
}

template <typename TransactionContext>
void SequenceBoundCallback<TransactionContext>::RunAfterRequeue() {
  // Actually run the callback. Note that RunImpl may steal the pointer.
  CHECK(context_ != NULL);
  RunImpl(&context_);
  delete this;
}

template <typename TransactionContext>
void SequenceBoundCallback<TransactionContext>::CancelAfterRequeue() {
  CancelImpl();
  delete this;
}

template <typename TransactionContext>
void SequenceBoundCallback<TransactionContext>::SetTransactionContext(
    TransactionContext* context) {
  CHECK(context_ == NULL);
  context_.reset(context);
}

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_REWRITER_PUBLIC_SEQUENCE_BOUND_CALLBACK_H_
