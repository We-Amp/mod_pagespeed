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

// Unit-test for QueuedWorkerPool

#include <memory>

#include "pagespeed/kernel/thread/queued_worker_pool.h"

#include "base/logging.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/function.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/thread/worker_test_base.h"

namespace net_instaweb {
namespace {

class QueuedWorkerPoolTest : public WorkerTestBase {
 public:
  QueuedWorkerPoolTest()
      : worker_(new QueuedWorkerPool(2, "queued_worker_pool_test",
                                     thread_runtime_.get())) {}

 protected:
  std::unique_ptr<QueuedWorkerPool> worker_;

  // Blocks mainline until a sequence completes all outstanding tasks.
  void WaitUntilSequenceCompletes(QueuedWorkerPool::Sequence* sequence) {
    SyncPoint done(thread_runtime_.get());
    sequence->Add(new NotifyRunFunction(&done));
    done.Wait();
  }

 private:
  QueuedWorkerPoolTest(const QueuedWorkerPoolTest&) = delete;
  QueuedWorkerPoolTest& operator=(const QueuedWorkerPoolTest&) = delete;
};

// A function that, without protection of a mutex, increments a shared
// integer.  The intent is that the QueuedWorkerPool::Sequence is
// enforcing the sequentiality on our behalf so we don't have to worry
// about mutexing in here.
class Increment : public Function {
 public:
  Increment(int expected_value, int* count)
      : expected_value_(expected_value), count_(count) {}

 protected:
  void Run() override {
    ++*count_;
    EXPECT_EQ(expected_value_, *count_);
  }
  void Cancel() override {
    *count_ -= 100;
    EXPECT_EQ(expected_value_, *count_);
  }

 private:
  int expected_value_;
  int* count_;

  Increment(const Increment&) = delete;
  Increment& operator=(const Increment&) = delete;
};

// Tests that all the jobs queued in one sequence should run sequentially.
TEST_F(QueuedWorkerPoolTest, BasicOperation) {
  const int kBound = 42;
  int count = 0;
  SyncPoint sync(thread_runtime_.get());

  QueuedWorkerPool::Sequence* sequence = worker_->NewSequence();
  for (int i = 0; i < kBound; ++i) {
    sequence->Add(new Increment(i + 1, &count));
  }

  sequence->Add(new NotifyRunFunction(&sync));
  sync.Wait();
  EXPECT_EQ(kBound, count);
  worker_->FreeSequence(sequence);
}

// Test ordinary and cancelled AddFunction callback.
TEST_F(QueuedWorkerPoolTest, AddFunctionTest) {
  const int kBound = 5;
  int count1 = 0;
  int count2 = 0;
  SyncPoint sync(thread_runtime_.get());

  QueuedWorkerPool::Sequence* sequence = worker_->NewSequence();
  for (int i = 0; i < kBound; ++i) {
    QueuedWorkerPool::Sequence::AddFunction add(sequence,
                                                new Increment(i + 1, &count1));
    add.set_delete_after_callback(false);
    add.CallRun();
    QueuedWorkerPool::Sequence::AddFunction cancel(
        sequence, new Increment(-100 * (i + 1), &count2));
    cancel.set_delete_after_callback(false);
    cancel.CallCancel();
  }

  sequence->Add(new NotifyRunFunction(&sync));
  sync.Wait();
  EXPECT_EQ(kBound, count1);
  EXPECT_EQ(-100 * kBound, count2);
  worker_->FreeSequence(sequence);
}

// Makes sure that even if one sequence is blocked, another can
// complete, because we have more than one thread at our disposal in
// this worker.
TEST_F(QueuedWorkerPoolTest, SlowAndFastSequences) {
  const int kBound = 42;
  int count = 0;
  SyncPoint sync(thread_runtime_.get());
  SyncPoint wait(thread_runtime_.get());

  QueuedWorkerPool::Sequence* slow_sequence = worker_->NewSequence();
  slow_sequence->Add(new WaitRunFunction(&wait));
  slow_sequence->Add(new NotifyRunFunction(&sync));

  QueuedWorkerPool::Sequence* fast_sequence = worker_->NewSequence();
  for (int i = 0; i < kBound; ++i) {
    fast_sequence->Add(new Increment(i + 1, &count));
  }

  // At this point the fast sequence is churning through its work, while the
  // slow sequence is blocked waiting for SyncPoint 'wait'.  Let the fast
  // sequence unblock it.
  fast_sequence->Add(new NotifyRunFunction(&wait));

  sync.Wait();
  EXPECT_EQ(kBound, count);
  worker_->FreeSequence(fast_sequence);
  worker_->FreeSequence(slow_sequence);
}

class MakeNewSequence : public Function {
 public:
  MakeNewSequence(WorkerTestBase::SyncPoint* sync, QueuedWorkerPool* pool,
                  QueuedWorkerPool::Sequence* sequence)
      : sync_(sync), pool_(pool), sequence_(sequence) {}

  void Run() override {
    pool_->FreeSequence(sequence_);
    pool_->NewSequence()->Add(new WorkerTestBase::NotifyRunFunction(sync_));
  }

 private:
  WorkerTestBase::SyncPoint* sync_;
  QueuedWorkerPool* pool_;
  QueuedWorkerPool::Sequence* sequence_;

  MakeNewSequence(const MakeNewSequence&) = delete;
  MakeNewSequence& operator=(const MakeNewSequence&) = delete;
};

TEST_F(QueuedWorkerPoolTest, RestartSequenceFromFunction) {
  SyncPoint sync(thread_runtime_.get());
  QueuedWorkerPool::Sequence* sequence = worker_->NewSequence();
  sequence->Add(new MakeNewSequence(&sync, worker_.get(), sequence));
  sync.Wait();
}

// Keeps track of whether run or cancel were called.
class LogOpsFunction : public Function {
 public:
  LogOpsFunction() : run_called_(false), cancel_called_(false) {
    set_delete_after_callback(false);
  }
  ~LogOpsFunction() override {}

  bool run_called() const { return run_called_; }
  bool cancel_called() const { return cancel_called_; }

 protected:
  void Run() override { run_called_ = true; }
  void Cancel() override { cancel_called_ = true; }

 private:
  bool run_called_;
  bool cancel_called_;
};

// Make sure calling add after worker was shut down Cancel()s the function
// properly.
TEST_F(QueuedWorkerPoolTest, AddAfterShutDown) {
  QueuedWorkerPool::Sequence* sequence = worker_->NewSequence();
  worker_->ShutDown();
  LogOpsFunction f;
  sequence->Add(&f);
  worker_.reset(nullptr);
  EXPECT_TRUE(f.cancel_called());
  EXPECT_FALSE(f.run_called());
}

TEST_F(QueuedWorkerPoolTest, LoadShedding) {
  const int kThresh = 100;
  worker_->SetLoadSheddingThreshold(kThresh);
  // Tests that load shedding works, and does so in FIFO order.
  // We do it by first wedging the queues by 2 (as many as we have threads)
  // sequences that wait on SyncPoints followed by 2*kThresh
  // independent LogOpsFunction instances (each in a separate sequence),
  // then a notify. If everything works fine, we'll cancel the first
  // kThresh + 1 LogOps, run the kThresh - 1 last LogOps, and the notify.
  SyncPoint wedge1_sync(thread_runtime_.get());
  SyncPoint wedge2_sync(thread_runtime_.get());
  QueuedWorkerPool::Sequence* wedge1 = worker_->NewSequence();
  wedge1->Add(new WaitRunFunction(&wedge1_sync));
  QueuedWorkerPool::Sequence* wedge2 = worker_->NewSequence();
  wedge2->Add(new WaitRunFunction(&wedge2_sync));

  std::vector<QueuedWorkerPool::Sequence*> log_ops;
  std::vector<LogOpsFunction*> log_ops_functions;
  for (int i = 0; i < 2 * kThresh; ++i) {
    LogOpsFunction* fn = new LogOpsFunction;
    QueuedWorkerPool::Sequence* log_op = worker_->NewSequence();
    log_op->Add(fn);
    log_ops.push_back(log_op);
    log_ops_functions.push_back(fn);
  }

  SyncPoint done_sync(thread_runtime_.get());
  QueuedWorkerPool::Sequence* done = worker_->NewSequence();
  done->Add(new NotifyRunFunction(&done_sync));

  wedge1_sync.Notify();
  wedge2_sync.Notify();
  done_sync.Wait();

  // We want to shutdown here since even though done_sync signaled, there
  // may still be a log op running in the 2nd thread. This will wait for it.
  worker_->ShutDown();

  worker_->FreeSequence(wedge1);
  worker_->FreeSequence(wedge2);

  for (int i = 0; i <= kThresh; ++i) {
    EXPECT_TRUE(log_ops_functions[i]->cancel_called());
    EXPECT_FALSE(log_ops_functions[i]->run_called());
    delete log_ops_functions[i];
    worker_->FreeSequence(log_ops[i]);
  }

  for (int i = kThresh + 1; i < 2 * kThresh; ++i) {
    EXPECT_FALSE(log_ops_functions[i]->cancel_called());
    EXPECT_TRUE(log_ops_functions[i]->run_called());
    delete log_ops_functions[i];
    worker_->FreeSequence(log_ops[i]);
  }

  worker_->FreeSequence(done);
}

class NotifyAndWait : public Function {
 public:
  NotifyAndWait(WorkerTestBase::SyncPoint* notify,
                WorkerTestBase::SyncPoint* wait)
      : notify_(notify), wait_(wait) {}

  void Run() override {
    notify_->Notify();
    wait_->Wait();
  }

  void Cancel() override { CHECK(false); }

 private:
  WorkerTestBase::SyncPoint* notify_;
  WorkerTestBase::SyncPoint* wait_;
};

TEST_F(QueuedWorkerPoolTest, MaxQueueSize) {
  SyncPoint started(thread_runtime_.get());
  SyncPoint wait(thread_runtime_.get());
  SyncPoint done(thread_runtime_.get());
  QueuedWorkerPool::Sequence* sequence = worker_->NewSequence();
  sequence->set_max_queue_size(4);
  int count = 0;
  sequence->Add(new NotifyAndWait(&started, &wait));
  started.Wait();
  sequence->Add(new Increment(-100, &count));  // will be canceled: -100.
  sequence->Add(new Increment(-99, &count));   // will be run: +1 == -99.
  sequence->Add(new Increment(-98, &count));   // will be run: +1 == -98.
  sequence->Add(new NotifyRunFunction(&done));
  sequence->Add(new Increment(-97, &count));  // Cancels first increment.
  wait.Notify();
  done.Wait();
  WaitUntilSequenceCompletes(sequence);
  EXPECT_EQ(-97, count);
}

TEST_F(QueuedWorkerPoolTest, CancelPending) {
  SyncPoint wait(thread_runtime_.get());
  SyncPoint done(thread_runtime_.get());
  QueuedWorkerPool::Sequence* sequence = worker_->NewSequence();
  int count = 0;
  sequence->Add(new WaitRunFunction(&wait));
  sequence->Add(new Increment(-100, &count));
  sequence->Add(new Increment(-200, &count));
  sequence->Add(new Increment(-300, &count));
  sequence->CancelPendingFunctions();
  sequence->Add(new NotifyRunFunction(&done));
  wait.Notify();
  done.Wait();
  EXPECT_EQ(-300, count);
}

// Regression test for the free-while-queued race behind the Apache system-test
// flake (random RemoteDisconnected on resource requests, root-caused to
// DCHECK(work_queue_.empty()) firing in Sequence::Reset()).  A Sequence with
// work still queued for an as-yet-unassigned worker must NOT be recycled onto
// free_sequences_ by FreeSequence(): it is still in the pool's queued_sequences_
// list, and the two lists are documented mutually exclusive.  Pre-fix,
// FreeSequence() recycled it anyway, so the next NewSequence() handed back a
// Sequence whose work_queue_ was non-empty -> Reset() DCHECK abort (debug) /
// silently double-owned sequence (opt).
TEST_F(QueuedWorkerPoolTest, FreeSequenceWhileQueuedIsNotRecycled) {
  // The fixture pool has 2 workers.  Wedge both so no dispatch can run, and
  // confirm both are actually occupied before proceeding.
  SyncPoint started1(thread_runtime_.get());
  SyncPoint started2(thread_runtime_.get());
  SyncPoint release1(thread_runtime_.get());
  SyncPoint release2(thread_runtime_.get());
  QueuedWorkerPool::Sequence* wedge1 = worker_->NewSequence();
  wedge1->Add(new NotifyAndWait(&started1, &release1));
  QueuedWorkerPool::Sequence* wedge2 = worker_->NewSequence();
  wedge2->Add(new NotifyAndWait(&started2, &release2));
  started1.Wait();
  started2.Wait();

  // victim: one function queued while both workers are busy, so it sits in
  // queued_sequences_ with active_ == false and a non-empty work_queue_.
  LogOpsFunction* fn = new LogOpsFunction;  // set_delete_after_callback(false)
  QueuedWorkerPool::Sequence* victim = worker_->NewSequence();
  victim->Add(fn);

  // Free it while it is still queued for dispatch.
  worker_->FreeSequence(victim);

  // It must not have been recycled onto free_sequences_ yet.  On the buggy code
  // this line either returns `victim` (opt) -- caught by EXPECT_NE -- or aborts
  // inside Reset()'s DCHECK(work_queue_.empty()) (debug) because `victim` still
  // holds `fn`.
  QueuedWorkerPool::Sequence* fresh = worker_->NewSequence();
  EXPECT_NE(victim, fresh);

  // Drain: unblock the workers and shut down.  The queued function on `victim`
  // is canceled exactly once as part of shutdown; it never runs.
  release1.Notify();
  release2.Notify();
  worker_->ShutDown();
  EXPECT_TRUE(fn->cancel_called());
  EXPECT_FALSE(fn->run_called());
  delete fn;
}

}  // namespace

}  // namespace net_instaweb
