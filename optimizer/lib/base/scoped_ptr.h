// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// This file is derived from mod_pagespeed and has been substantially modified.
// Originally licensed under Apache License, Version 2.0.
// Copyright (c) 2010-2017 Google Inc.
// Copyright (c) 2018 The Apache Software Foundation.

// Compatibility layer mapping legacy scoped_ptr/scoped_array to std::unique_ptr

#ifndef PAGESPEED_LIB_BASE_SCOPED_PTR_H_
#define PAGESPEED_LIB_BASE_SCOPED_PTR_H_

#include <memory>

namespace net_instaweb {

// scoped_array is a wrapper around std::unique_ptr<T[]> for compatibility
// with legacy code that used scoped_array.
template <typename T>
class scoped_array : public std::unique_ptr<T[]> {
 public:
  scoped_array() : std::unique_ptr<T[]>() {}
  explicit scoped_array(T* t) : std::unique_ptr<T[]>(t) {}
};

}  // namespace net_instaweb

#endif  // PAGESPEED_LIB_BASE_SCOPED_PTR_H_
