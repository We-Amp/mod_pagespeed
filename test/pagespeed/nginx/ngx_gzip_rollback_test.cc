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

#include "pagespeed/nginx/ngx_gzip_rollback.h"

#include <cstdint>
#include <memory>
#include <vector>

#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

const intptr_t kUnsetFlag = -1;
const uintptr_t kUnsetUint = static_cast<uintptr_t>(-1);

// A configuration cycle's pool: the slots the setup writes into.  Freed
// when the cycle is replaced, like nginx's configuration pool.
struct CyclePool {
  intptr_t gzip = 0;
  intptr_t vary = 0;
  uintptr_t http_version = 0;
  void* types = nullptr;
};

// Rolls back the way NgxGZipSetter does: writes the unset value through
// every recorded slot.
std::vector<void*> RollBackAll(NgxGZipRollbackLog* log) {
  std::vector<void*> touched;
  log->RollBack(
      [&](intptr_t* slot) {
        *slot = kUnsetFlag;
        touched.push_back(slot);
      },
      [&](uintptr_t* slot) {
        *slot = kUnsetUint;
        touched.push_back(slot);
      },
      [&](void* slot) {
        *static_cast<void**>(slot) = nullptr;
        touched.push_back(slot);
      });
  return touched;
}

void SetUpLocation(NgxGZipRollbackLog* log, CyclePool* pool) {
  pool->gzip = 1;
  log->RecordFlag(&pool->gzip);
  pool->vary = 1;
  log->RecordFlag(&pool->vary);
  pool->http_version = 10;
  log->RecordUint(&pool->http_version);
  log->RecordTypes(&pool->types);
}

TEST(NgxGZipRollbackTest, RollBackResetsThisCyclesSlots) {
  NgxGZipRollbackLog log;
  log.BeginCycle(true);
  EXPECT_TRUE(log.enabled());
  CyclePool pool;
  SetUpLocation(&log, &pool);
  EXPECT_EQ(4u, RollBackAll(&log).size());
  EXPECT_EQ(kUnsetFlag, pool.gzip);
  EXPECT_EQ(kUnsetFlag, pool.vary);
  EXPECT_EQ(kUnsetUint, pool.http_version);
  EXPECT_FALSE(log.enabled());
}

TEST(NgxGZipRollbackTest, SecondCycleNeverTouchesFirstCyclesSlots) {
  NgxGZipRollbackLog log;
  log.BeginCycle(true);
  std::unique_ptr<CyclePool> first(new CyclePool);
  SetUpLocation(&log, first.get());
  const CyclePool* first_address = first.get();
  // Reload: the first cycle's pool goes away and a new cycle starts.
  first.reset();

  log.BeginCycle(true);
  EXPECT_EQ(0u, log.recorded());
  CyclePool second;
  SetUpLocation(&log, &second);
  std::vector<void*> touched = RollBackAll(&log);
  EXPECT_EQ(4u, touched.size());
  for (void* slot : touched) {
    EXPECT_GE(static_cast<char*>(slot), reinterpret_cast<char*>(&second));
    EXPECT_LT(static_cast<char*>(slot), reinterpret_cast<char*>(&second + 1));
    EXPECT_NE(static_cast<const void*>(first_address), slot);
  }
  EXPECT_EQ(kUnsetFlag, second.gzip);
}

TEST(NgxGZipRollbackTest, RollBackInOneCycleDoesNotDisableTheNext) {
  NgxGZipRollbackLog log;
  log.BeginCycle(true);
  CyclePool first;
  SetUpLocation(&log, &first);
  RollBackAll(&log);
  EXPECT_FALSE(log.enabled());

  // The next configuration may not carry an explicit gzip directive: the
  // automatic setup is armed again for it.
  log.BeginCycle(true);
  EXPECT_TRUE(log.enabled());
}

TEST(NgxGZipRollbackTest, UnavailableGzipStaysDisabled) {
  NgxGZipRollbackLog log;
  log.BeginCycle(false);
  EXPECT_FALSE(log.enabled());
  log.BeginCycle(true);
  EXPECT_TRUE(log.enabled());
  log.BeginCycle(false);
  EXPECT_FALSE(log.enabled());
}

}  // namespace
}  // namespace net_instaweb
