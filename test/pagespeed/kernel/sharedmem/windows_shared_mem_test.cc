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

// This tests the operation of WindowsSharedMem using threads.
// Windows doesn't have fork(), so we test cross-thread operation which
// exercises the same named mutex and memory mapping code paths that
// would be used across processes.

#ifdef _WIN32

#include "pagespeed/kernel/sharedmem/windows_shared_mem.h"

#include <windows.h>

#include <memory>
#include <vector>

#include "base/logging.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/function.h"
#include "pagespeed/kernel/base/stl_util.h"
#include "pagespeed/kernel/base/thread.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/util/platform.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/base/mock_message_handler.h"
#include "test/pagespeed/kernel/sharedmem/shared_circular_buffer_test_base.h"
#include "test/pagespeed/kernel/sharedmem/shared_dynamic_string_map_test_base.h"
#include "test/pagespeed/kernel/sharedmem/shared_mem_cache_data_test_base.h"
#include "test/pagespeed/kernel/sharedmem/shared_mem_cache_test_base.h"
#include "test/pagespeed/kernel/sharedmem/shared_mem_lock_manager_test_base.h"
#include "test/pagespeed/kernel/sharedmem/shared_mem_statistics_test_base.h"
#include "test/pagespeed/kernel/sharedmem/shared_mem_test_base.h"

namespace net_instaweb {

namespace {

// Test environment that uses Windows threads to simulate multi-process
// scenarios. While this doesn't fully test cross-process behavior (which
// would require separate executables), it does exercise the named mutex
// and memory mapping code paths.
class WindowsSharedMemEnv : public SharedMemTestEnv {
 public:
  WindowsSharedMemEnv() : thread_system_(Platform::CreateThreadSystem()) {}

  AbstractSharedMem* CreateSharedMemRuntime() override {
    return new WindowsSharedMem();
  }

  void ShortSleep() override { Sleep(1); }

  bool CreateChild(Function* callback) override {
    RunFunctionThread* thread =
        new RunFunctionThread(thread_system_.get(), callback);

    bool ok = thread->Start();
    if (!ok) {
      ADD_FAILURE() << "Problem starting child thread";
      return false;
    }
    child_threads_.push_back(thread);
    return true;
  }

  void WaitForChildren() override {
    for (size_t i = 0; i < child_threads_.size(); ++i) {
      child_threads_[i]->Join();
    }
    STLDeleteElements(&child_threads_);
  }

  void ChildFailed() override {
    // Unfortunately we don't have a clean way of signaling this.
    LOG(FATAL) << "Test failure in child thread";
  }

 protected:
  // Helper Thread subclass that just runs a function.
  class RunFunctionThread : public ThreadSystem::Thread {
   public:
    RunFunctionThread(ThreadSystem* runtime, Function* fn)
        : Thread(runtime, "windows_shm_test", ThreadSystem::kJoinable),
          fn_(fn) {}

    void Run() override {
      fn_->CallRun();
      fn_ = nullptr;
    }

   private:
    Function* fn_;
    RunFunctionThread(const RunFunctionThread&) = delete;
    RunFunctionThread& operator=(const RunFunctionThread&) = delete;
  };

  std::unique_ptr<ThreadSystem> thread_system_;
  std::vector<ThreadSystem::Thread*> child_threads_;
};

// Test basic WindowsSharedMem functionality
TEST(WindowsSharedMemTest, BasicCreateAndAttach) {
  WindowsSharedMem shm;
  std::unique_ptr<ThreadSystem> thread_system(Platform::CreateThreadSystem());
  MockMessageHandler handler(thread_system->NewMutex());

  // Create a segment
  AbstractSharedMemSegment* seg = shm.CreateSegment("test_basic", 4096, &handler);
  ASSERT_NE(nullptr, seg);

  // Write to it
  volatile char* base = seg->Base();
  ASSERT_NE(nullptr, base);
  base[0] = 'A';
  base[1] = 'B';
  base[2] = 'C';
  base[3] = '\0';

  // Attach to it
  AbstractSharedMemSegment* seg2 = shm.AttachToSegment("test_basic", 4096, &handler);
  ASSERT_NE(nullptr, seg2);

  // Read from attached segment
  volatile char* base2 = seg2->Base();
  ASSERT_NE(nullptr, base2);
  EXPECT_EQ('A', base2[0]);
  EXPECT_EQ('B', base2[1]);
  EXPECT_EQ('C', base2[2]);

  // Modify through second view
  base2[0] = 'X';
  EXPECT_EQ('X', base[0]);

  delete seg2;
  delete seg;
  shm.DestroySegment("test_basic", &handler);
}

// Test mutex initialization and locking
TEST(WindowsSharedMemTest, MutexOperations) {
  WindowsSharedMem shm;
  std::unique_ptr<ThreadSystem> thread_system(Platform::CreateThreadSystem());
  MockMessageHandler handler(thread_system->NewMutex());

  size_t mutex_size = shm.SharedMutexSize();
  EXPECT_EQ(16u, mutex_size);  // WindowsMutexSlot is 16 bytes

  // Create segment with room for mutex + data
  AbstractSharedMemSegment* seg = shm.CreateSegment("test_mutex", mutex_size + 64, &handler);
  ASSERT_NE(nullptr, seg);

  // Initialize mutex at offset 0
  ASSERT_TRUE(seg->InitializeSharedMutex(0, &handler));

  // Attach to mutex
  AbstractMutex* mutex = seg->AttachToSharedMutex(0);
  ASSERT_NE(nullptr, mutex);

  // Test lock/unlock
  mutex->Lock();
  // We should be able to modify protected data here
  volatile char* data = seg->Base() + mutex_size;
  data[0] = 'Z';
  mutex->Unlock();

  // Test TryLock
  EXPECT_TRUE(mutex->TryLock());
  mutex->Unlock();

  delete mutex;
  delete seg;
  shm.DestroySegment("test_mutex", &handler);
}

// Test that two mutexes in the same segment are independent
TEST(WindowsSharedMemTest, MultipleMutexes) {
  WindowsSharedMem shm;
  std::unique_ptr<ThreadSystem> thread_system(Platform::CreateThreadSystem());
  MockMessageHandler handler(thread_system->NewMutex());

  size_t mutex_size = shm.SharedMutexSize();

  // Create segment with room for 2 mutexes
  AbstractSharedMemSegment* seg = shm.CreateSegment("test_multi_mutex", mutex_size * 2 + 64, &handler);
  ASSERT_NE(nullptr, seg);

  // Initialize two mutexes
  ASSERT_TRUE(seg->InitializeSharedMutex(0, &handler));
  ASSERT_TRUE(seg->InitializeSharedMutex(mutex_size, &handler));

  // Attach to both
  AbstractMutex* mutex1 = seg->AttachToSharedMutex(0);
  AbstractMutex* mutex2 = seg->AttachToSharedMutex(mutex_size);
  ASSERT_NE(nullptr, mutex1);
  ASSERT_NE(nullptr, mutex2);

  // Lock mutex1, mutex2 should still be lockable
  mutex1->Lock();
  EXPECT_TRUE(mutex2->TryLock());
  mutex2->Unlock();
  mutex1->Unlock();

  delete mutex1;
  delete mutex2;
  delete seg;
  shm.DestroySegment("test_multi_mutex", &handler);
}

// Instantiate the standard shared memory test suites
INSTANTIATE_TYPED_TEST_SUITE_P(WindowsShm, SharedCircularBufferTestTemplate,
                               WindowsSharedMemEnv);
INSTANTIATE_TYPED_TEST_SUITE_P(WindowsShm, SharedDynamicStringMapTestTemplate,
                               WindowsSharedMemEnv);
INSTANTIATE_TYPED_TEST_SUITE_P(WindowsShm, SharedMemCacheTestTemplate,
                               WindowsSharedMemEnv);
INSTANTIATE_TYPED_TEST_SUITE_P(WindowsShm, SharedMemCacheDataTestTemplate,
                               WindowsSharedMemEnv);
INSTANTIATE_TYPED_TEST_SUITE_P(WindowsShm, SharedMemLockManagerTestTemplate,
                               WindowsSharedMemEnv);
INSTANTIATE_TYPED_TEST_SUITE_P(WindowsShm, SharedMemStatisticsTestTemplate,
                               WindowsSharedMemEnv);
INSTANTIATE_TYPED_TEST_SUITE_P(WindowsShm, SharedMemTestTemplate,
                               WindowsSharedMemEnv);

}  // namespace

}  // namespace net_instaweb

#endif  // _WIN32
