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

#include "pagespeed/kernel/util/platform.h"

#include "pagespeed/kernel/base/checking_thread_system.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/base/timer.h"

// Platform-specific includes
#if defined(_WIN32)
// Windows: Use cross-platform C++ standard library implementations
#include "pagespeed/kernel/base/std_timer.h"
#include "pagespeed/kernel/thread/std_thread_system.h"
#else
// POSIX (Linux, macOS, etc.): Use POSIX-specific implementations
#include "pagespeed/kernel/base/posix_timer.h"
#include "pagespeed/kernel/thread/pthread_thread_system.h"
#endif

namespace net_instaweb {

ThreadSystem* Platform::CreateThreadSystem() {
#if defined(_WIN32)
  // Windows: Use C++ standard library based threading
  ThreadSystem* impl = new StdThreadSystem;
#else
  // POSIX: Use pthreads-based threading
  ThreadSystem* impl = new PthreadThreadSystem;
#endif

#ifdef NDEBUG
  return impl;
#else
  return new CheckingThreadSystem(impl);
#endif
}

Timer* Platform::CreateTimer() {
#if defined(_WIN32)
  // Windows: Use C++ standard library based timer
  return new StdTimer;
#else
  // POSIX: Use POSIX-specific timer
  return new PosixTimer;
#endif
}

}  // namespace net_instaweb
