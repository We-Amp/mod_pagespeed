// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Compat header for the vendored 1.15 JS kernel: stands in for
// the canonical pagespeed/kernel/base/basictypes.h header. Only the fixed-width
// aliases the vendored headers can reach are supplied (lib/js/source_map.h
// uses int32); identical typedefs to lib/base/basictypes.h, so a TU that
// includes both compiles fine. See lib/js/CLAUDE.md.

#ifndef PAGESPEED_LIB_JS_COMPAT_BASICTYPES_H_
#define PAGESPEED_LIB_JS_COMPAT_BASICTYPES_H_

#include <cstdint>

using int64 = int64_t;
using uint64 = uint64_t;
using int32 = int32_t;
using uint32 = uint32_t;

#endif  // PAGESPEED_LIB_JS_COMPAT_BASICTYPES_H_
