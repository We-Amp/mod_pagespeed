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

// The in-place record gate: which substrate a serving seam is on, and -- the
// property that matters -- that on the daemon substrates the classic recorder
// is not CONSTRUCTED.
//
// "Not constructed" and "not used" are different claims and only one of them
// is the contract.  A gate that built a recorder and then dropped it would
// pass every behavioural assertion while charging every in-place miss a
// recorder's worth of allocation, statistics increments and concurrency-limit
// accounting.  InPlaceResourceRecorder::num_constructed() is what separates
// the two, so every case below is written against that counter.

#include "pagespeed/system/ipro_record_gate.h"

#include <memory>

#include "net/instaweb/http/public/http_cache.h"
#include "net/instaweb/http/public/request_context.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/request_headers.h"
#include "pagespeed/system/daemon_adapter.h"
#include "pagespeed/system/in_place_resource_recorder.h"
#include "test/net/instaweb/rewriter/rewrite_test_base.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {

namespace {

const char kTestUrl[] = "http://www.example.com/a.css";

class IproRecordGateTest : public RewriteTestBase {
 protected:
  void SetUp() override {
    RewriteTestBase::SetUp();
    InPlaceResourceRecorder::InitStats(statistics());
  }

  // Calls the gate exactly as the Apache seam does.
  InPlaceResourceRecorder* CallGate(IproDisposition disposition) {
    RequestHeaders headers;
    return MakeIproRecorderIfClassic(
        disposition,
        RequestContext::NewTestRequestContext(
            server_context()->thread_system()),
        kTestUrl, rewrite_driver_->CacheFragment(), headers.GetProperties(),
        1024, 4 /* max_concurrent_recordings */, http_cache(), statistics(),
        message_handler());
  }
};

// --- the disposition mapping ------------------------------------------------

TEST_F(IproRecordGateTest, NoDaemonConfiguredKeepsTheClassicPath) {
  EXPECT_EQ(IproDisposition::kClassic,
            IproDispositionFor(DaemonHealth::kNotConfigured));
}

TEST_F(IproRecordGateTest, AReadyDaemonOwnsRecording) {
  EXPECT_EQ(IproDisposition::kDaemonSubstrate,
            IproDispositionFor(DaemonHealth::kReady));
}

TEST_F(IproRecordGateTest, AnUnavailableDaemonTurnsIproOff) {
  // Specifically NOT kClassic.  The classic recorder must not quietly come
  // back when the daemon an operator configured is missing.
  EXPECT_EQ(IproDisposition::kOff,
            IproDispositionFor(DaemonHealth::kUnavailable));
}

// --- non-instantiation ------------------------------------------------------

TEST_F(IproRecordGateTest, DaemonAbsentConstructsNoRecorder) {
  const int64 before = InPlaceResourceRecorder::num_constructed();
  for (int i = 0; i < 10; ++i) {
    EXPECT_TRUE(CallGate(IproDisposition::kOff) == nullptr);
  }
  EXPECT_EQ(before, InPlaceResourceRecorder::num_constructed());
}

TEST_F(IproRecordGateTest, DaemonSubstrateConstructsNoRecorder) {
  const int64 before = InPlaceResourceRecorder::num_constructed();
  for (int i = 0; i < 10; ++i) {
    EXPECT_TRUE(CallGate(IproDisposition::kDaemonSubstrate) == nullptr);
  }
  EXPECT_EQ(before, InPlaceResourceRecorder::num_constructed());
}

// The counter's own credibility: if it did not move on the one path that IS
// supposed to build a recorder, the two assertions above would be vacuous.
TEST_F(IproRecordGateTest, ClassicPathStillConstructsARecorder) {
  const int64 before = InPlaceResourceRecorder::num_constructed();
  std::unique_ptr<InPlaceResourceRecorder> recorder(
      CallGate(IproDisposition::kClassic));
  EXPECT_TRUE(recorder.get() != nullptr);
  EXPECT_EQ(before + 1, InPlaceResourceRecorder::num_constructed());
}

// --- end to end through the health verdict ----------------------------------

TEST_F(IproRecordGateTest, AnAbsentDaemonBuildsNoRecorderFromHealthOnward) {
  // The whole chain the serving seam runs: health -> disposition -> gate.
  DaemonAdapter adapter("/nonexistent/daemon.sock", "/nonexistent/volume",
                        message_handler());
  adapter.set_library_path("/nonexistent/libpagespeed.so");
  ASSERT_EQ(DaemonStartupStatus::kOk, adapter.StartupCheck());
  ASSERT_EQ(DaemonHealth::kUnavailable, adapter.health());

  const int64 before = InPlaceResourceRecorder::num_constructed();
  EXPECT_TRUE(CallGate(IproDispositionFor(adapter.health())) == nullptr);
  EXPECT_EQ(before, InPlaceResourceRecorder::num_constructed());
}

}  // namespace

}  // namespace net_instaweb
