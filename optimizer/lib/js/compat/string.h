// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Compat header for the vendored 1.15 JS kernel: stands in for
// the canonical pagespeed/kernel/base/string.h header. The sync tool rewrites the
// vendored files' includes to point here; per-call-site edits to vendored
// files are never made. See lib/js/CLAUDE.md.

#ifndef PAGESPEED_LIB_JS_COMPAT_STRING_H_
#define PAGESPEED_LIB_JS_COMPAT_STRING_H_

#include <string>

using GoogleString = std::string;

#endif  // PAGESPEED_LIB_JS_COMPAT_STRING_H_
