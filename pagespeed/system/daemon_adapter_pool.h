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

#ifndef PAGESPEED_SYSTEM_DAEMON_ADAPTER_POOL_H_
#define PAGESPEED_SYSTEM_DAEMON_ADAPTER_POOL_H_

#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <utility>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/system/daemon_adapter.h"

namespace net_instaweb {

class MessageHandler;

// One DaemonAdapter per distinct (socket path, volume path) pair, shared by
// every server context that names that pair.
//
// WHY.  An adapter's volume handle is per-process state, and opening one
// starts the storage layer's background threads.  With one adapter per
// server context, a process serving N server blocks (or virtual hosts) that
// all point at the same optimizer volume held N handles on that one volume,
// with 2N background threads waking on their own timers, so idle CPU grew
// with the number of sites.  Everything an adapter decides -- the start-up
// verdict, the retry schedule, the replaced-volume check -- depends only on
// the pair (and the fixed library path), so contexts with the same pair can
// share one adapter, and one handle, without any change in behaviour.
//
// SHARED MEANS SHARED.  A caller that closes the handle (CloseRecordCache)
// closes it for every context holding the adapter, so only teardown that
// closes all of them at once (a worker exit) may call it.
//
// LIFETIME.  Each context holds a shared_ptr, so an adapter lives as long
// as the longest of the pool and the contexts using it.  A pool is meant to
// live as long as one configuration: a new configuration builds a new pool,
// so its start-up check runs again.
class DaemonAdapterPool {
 public:
  DaemonAdapterPool() = default;
  DaemonAdapterPool(const DaemonAdapterPool&) = delete;
  DaemonAdapterPool& operator=(const DaemonAdapterPool&) = delete;

  struct Checked {
    std::shared_ptr<DaemonAdapter> adapter;
    DaemonStartupStatus status = DaemonStartupStatus::kOk;
  };

  // The adapter for (socket_path, volume_path).  The first call for a pair
  // builds the adapter with `handler` and runs its StartupCheck(); later
  // calls for that pair return the same adapter and the first call's
  // verdict, and do not use their own `handler`.  Thread-safe; the check
  // runs under the pool's lock.
  Checked GetOrCheck(StringPiece socket_path, StringPiece volume_path,
                     MessageHandler* handler);

  // The number of distinct adapters built so far.
  size_t size() const;

 private:
  mutable std::mutex mu_;
  std::map<std::pair<GoogleString, GoogleString>, Checked> entries_;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_SYSTEM_DAEMON_ADAPTER_POOL_H_
