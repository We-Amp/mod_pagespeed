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

#include "pagespeed/kernel/thread/std_condvar.h"

#include <thread>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/condvar.h"
#include "pagespeed/kernel/base/std_timer.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/kernel/thread/std_mutex.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/thread/condvar_test_base.h"

namespace net_instaweb {

class StdCondvarTest : public CondvarTestBase {
 protected:
  StdCondvarTest()
      : std_mutex_(),
        std_startup_condvar_(&std_mutex_),
        std_condvar_(&std_mutex_) {
    Init(&std_mutex_, &std_startup_condvar_, &std_condvar_);
  }

  void CreateHelper() override {
    helper_thread_ = std::thread(&HelperThread, this);
  }

  void FinishHelper() override {
    if (helper_thread_.joinable()) {
      helper_thread_.join();
    }
  }

  Timer* timer() override { return &timer_; }

  StdMutex std_mutex_;
  StdCondvar std_startup_condvar_;
  StdCondvar std_condvar_;
  std::thread helper_thread_;
  StdTimer timer_;

 private:
  StdCondvarTest(const StdCondvarTest&) = delete;
  StdCondvarTest& operator=(const StdCondvarTest&) = delete;
};

TEST_F(StdCondvarTest, TestStartup) { StartupTest(); }

TEST_F(StdCondvarTest, BlindSignals) { BlindSignalsTest(); }

TEST_F(StdCondvarTest, BroadcastBlindSignals) {
  signal_method_ = &ThreadSystem::Condvar::Broadcast;
  BlindSignalsTest();
}

TEST_F(StdCondvarTest, TestPingPong) { PingPongTest(); }

TEST_F(StdCondvarTest, BroadcastTestPingPong) {
  signal_method_ = &ThreadSystem::Condvar::Broadcast;
  PingPongTest();
}

TEST_F(StdCondvarTest, TestTimeout) { TimeoutTest(); }

TEST_F(StdCondvarTest, TestLongTimeout1200) {
  // We pick a value over a second because the implementation special-cases
  // that situation.
  LongTimeoutTest(1100);
  LongTimeoutTest(100);
}

TEST_F(StdCondvarTest, TimeoutPingPong) { TimeoutPingPongTest(); }

TEST_F(StdCondvarTest, BroadcastTimeoutPingPong) {
  signal_method_ = &ThreadSystem::Condvar::Broadcast;
  TimeoutPingPongTest();
}

}  // namespace net_instaweb
