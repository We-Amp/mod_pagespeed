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

// DaemonAdapterPool: server contexts naming the same optimizer socket and
// volume share one adapter (and so one volume handle per process), and
// contexts naming different ones do not.  Every case here stays on the
// adapter's no-I/O paths: nothing configured, or half a configuration.

#include "pagespeed/system/daemon_adapter_pool.h"

#include <memory>

#include "pagespeed/kernel/base/null_mutex.h"
#include "pagespeed/system/daemon_adapter.h"
#include "pagespeed/system/daemon_health.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/base/mock_message_handler.h"

namespace net_instaweb {
namespace {

class DaemonAdapterPoolTest : public testing::Test {
 protected:
  DaemonAdapterPoolTest() : handler_(new NullMutex) {
    DaemonAdapter::ResetAnnouncementsForTesting();
  }

  MockMessageHandler handler_;
  DaemonAdapterPool pool_;
};

TEST_F(DaemonAdapterPoolTest, SamePairSharesOneAdapter) {
  // Many server blocks, one volume: the issue this pool exists for.
  const DaemonAdapterPool::Checked first =
      pool_.GetOrCheck("/run/a.sock", "", &handler_);
  ASSERT_NE(nullptr, first.adapter);
  for (int i = 0; i < 100; ++i) {
    const DaemonAdapterPool::Checked again =
        pool_.GetOrCheck("/run/a.sock", "", &handler_);
    EXPECT_EQ(first.adapter.get(), again.adapter.get());
    EXPECT_EQ(first.status, again.status);
  }
  EXPECT_EQ(1u, pool_.size());
  EXPECT_EQ(DaemonHealth::kUnavailable, first.adapter->health());
  // The check ran once, so its one loud line was said once.
  EXPECT_EQ(1, handler_.MessagesOfType(kError));
}

TEST_F(DaemonAdapterPoolTest, DifferentPairsGetDifferentAdapters) {
  const DaemonAdapterPool::Checked none = pool_.GetOrCheck("", "", &handler_);
  const DaemonAdapterPool::Checked socket_a =
      pool_.GetOrCheck("/run/a.sock", "", &handler_);
  const DaemonAdapterPool::Checked socket_b =
      pool_.GetOrCheck("/run/b.sock", "", &handler_);
  const DaemonAdapterPool::Checked volume_a =
      pool_.GetOrCheck("", "/var/cache/a", &handler_);
  EXPECT_NE(none.adapter.get(), socket_a.adapter.get());
  EXPECT_NE(socket_a.adapter.get(), socket_b.adapter.get());
  EXPECT_NE(socket_a.adapter.get(), volume_a.adapter.get());
  EXPECT_EQ(4u, pool_.size());
  EXPECT_EQ(DaemonHealth::kNotConfigured, none.adapter->health());
  EXPECT_EQ(DaemonHealth::kUnavailable, socket_b.adapter->health());
  EXPECT_EQ("/run/b.sock", socket_b.adapter->socket_path());
  EXPECT_EQ("/var/cache/a", volume_a.adapter->volume_path());
}

TEST_F(DaemonAdapterPoolTest, AdapterOutlivesThePool) {
  std::shared_ptr<DaemonAdapter> held;
  {
    DaemonAdapterPool pool;
    held = pool.GetOrCheck("", "", &handler_).adapter;
  }
  // A context that outlives its configuration's pool keeps a valid adapter.
  ASSERT_NE(nullptr, held);
  EXPECT_EQ(DaemonHealth::kNotConfigured, held->health());
}

}  // namespace
}  // namespace net_instaweb
