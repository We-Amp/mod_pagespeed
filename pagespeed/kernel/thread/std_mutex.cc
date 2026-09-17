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

#include "pagespeed/kernel/thread/std_mutex.h"

#include <mutex>

#include "pagespeed/kernel/base/condvar.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/thread/std_condvar.h"

namespace net_instaweb {

StdMutex::StdMutex() {}

StdMutex::~StdMutex() {}

bool StdMutex::TryLock() { return mutex_.try_lock(); }

void StdMutex::Lock() { mutex_.lock(); }

void StdMutex::Unlock() { mutex_.unlock(); }

ThreadSystem::Condvar* StdMutex::NewCondvar() { return new StdCondvar(this); }

}  // namespace net_instaweb
