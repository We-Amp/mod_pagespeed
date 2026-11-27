// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// This file is derived from mod_pagespeed and has been substantially modified.
// Originally licensed under Apache License, Version 2.0.
// Copyright (c) 2010-2017 Google Inc.
// Copyright (c) 2018 The Apache Software Foundation.

// Basic types and macros for PageSpeed 2.0

#ifndef PAGESPEED_LIB_BASE_BASICTYPES_H_
#define PAGESPEED_LIB_BASE_BASICTYPES_H_

#include <cstddef>
#include <cstdint>

typedef int64_t int64;
typedef uint64_t uint64;
typedef uint32_t uint32;
typedef int32_t int32;
typedef uint8_t uint8;
typedef int8_t int8;

#define arraysize(a)            \
  ((sizeof(a) / sizeof(*(a))) / \
   static_cast<size_t>(!(sizeof(a) % sizeof(*(a)))))

// Fallthrough annotation for switch statements. C++17 [[fallthrough]] is
// understood by every compiler this tree builds with; the old clang-only
// definition degraded to a no-op on gcc, which then (correctly) refused
// the unannotated fallthrough under -Werror=implicit-fallthrough.
#define FALLTHROUGH_INTENDED [[fallthrough]]

// Lazily-initialized boolean value
enum LazyBool : std::int8_t { kNotSet = -1, kFalse = 0, kTrue = 1 };

// Disable copy and assignment
#define DISALLOW_COPY_AND_ASSIGN(TypeName) \
  TypeName(const TypeName&) = delete;      \
  void operator=(const TypeName&) = delete

#endif  // PAGESPEED_LIB_BASE_BASICTYPES_H_
