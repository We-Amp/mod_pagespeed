// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// This file is derived from mod_pagespeed and has been substantially modified.
// Originally licensed under Apache License, Version 2.0.
// Copyright (c) 2010-2017 Google Inc.
// Copyright (c) 2018 The Apache Software Foundation.

// Printf format string annotation macros

#ifndef PAGESPEED_LIB_BASE_PRINTF_FORMAT_H_
#define PAGESPEED_LIB_BASE_PRINTF_FORMAT_H_

#ifdef __GNUC__

// Tell the compiler a function is using a printf-style format string.
// |format_param| is the one-based index of the format string parameter;
// |dots_param| is the one-based index of the "..." parameter.
// For v*printf functions (which take a va_list), pass 0 for dots_param.
#define INSTAWEB_PRINTF_FORMAT(format_param, dots_param) \
  __attribute__((format(printf, format_param, dots_param)))

#else  // Not GCC

#define INSTAWEB_PRINTF_FORMAT(x, y)

#endif

#endif  // PAGESPEED_LIB_BASE_PRINTF_FORMAT_H_
