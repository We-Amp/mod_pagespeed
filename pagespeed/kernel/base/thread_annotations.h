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

#pragma once

#include "absl/base/thread_annotations.h"
#include "absl/base/attributes.h"

// Define legacy thread annotation macros using ABSL_ prefixed versions
// since ABSL_LEGACY_THREAD_ANNOTATIONS is no longer supported

#ifndef GUARDED_BY
#define GUARDED_BY(x) ABSL_GUARDED_BY(x)
#endif

#ifndef PT_GUARDED_BY
#define PT_GUARDED_BY(x) ABSL_PT_GUARDED_BY(x)
#endif

#ifndef ACQUIRED_AFTER
#define ACQUIRED_AFTER(...) ABSL_ACQUIRED_AFTER(__VA_ARGS__)
#endif

#ifndef ACQUIRED_BEFORE
#define ACQUIRED_BEFORE(...) ABSL_ACQUIRED_BEFORE(__VA_ARGS__)
#endif

#ifndef EXCLUSIVE_LOCKS_REQUIRED
#define EXCLUSIVE_LOCKS_REQUIRED(...) ABSL_EXCLUSIVE_LOCKS_REQUIRED(__VA_ARGS__)
#endif

#ifndef SHARED_LOCKS_REQUIRED
#define SHARED_LOCKS_REQUIRED(...) ABSL_SHARED_LOCKS_REQUIRED(__VA_ARGS__)
#endif

#ifndef LOCKS_EXCLUDED
#define LOCKS_EXCLUDED(...) ABSL_LOCKS_EXCLUDED(__VA_ARGS__)
#endif

#ifndef LOCK_RETURNED
#define LOCK_RETURNED(x) ABSL_LOCK_RETURNED(x)
#endif

#ifndef LOCKABLE
#define LOCKABLE ABSL_LOCKABLE
#endif

#ifndef SCOPED_LOCKABLE
#define SCOPED_LOCKABLE ABSL_SCOPED_LOCKABLE
#endif

#ifndef EXCLUSIVE_LOCK_FUNCTION
#define EXCLUSIVE_LOCK_FUNCTION(...) ABSL_EXCLUSIVE_LOCK_FUNCTION(__VA_ARGS__)
#endif

#ifndef SHARED_LOCK_FUNCTION
#define SHARED_LOCK_FUNCTION(...) ABSL_SHARED_LOCK_FUNCTION(__VA_ARGS__)
#endif

#ifndef UNLOCK_FUNCTION
#define UNLOCK_FUNCTION(...) ABSL_UNLOCK_FUNCTION(__VA_ARGS__)
#endif

#ifndef EXCLUSIVE_TRYLOCK_FUNCTION
#define EXCLUSIVE_TRYLOCK_FUNCTION(...) ABSL_EXCLUSIVE_TRYLOCK_FUNCTION(__VA_ARGS__)
#endif

#ifndef SHARED_TRYLOCK_FUNCTION
#define SHARED_TRYLOCK_FUNCTION(...) ABSL_SHARED_TRYLOCK_FUNCTION(__VA_ARGS__)
#endif

#ifndef ASSERT_EXCLUSIVE_LOCK
#define ASSERT_EXCLUSIVE_LOCK(...) ABSL_ASSERT_EXCLUSIVE_LOCK(__VA_ARGS__)
#endif

#ifndef ASSERT_SHARED_LOCK
#define ASSERT_SHARED_LOCK(...) ABSL_ASSERT_SHARED_LOCK(__VA_ARGS__)
#endif

#ifndef NO_THREAD_SAFETY_ANALYSIS
#define NO_THREAD_SAFETY_ANALYSIS ABSL_NO_THREAD_SAFETY_ANALYSIS
#endif

// TODO(oschaaf): move this out of here.
#define WARN_UNUSED_RESULT ABSL_MUST_USE_RESULT
