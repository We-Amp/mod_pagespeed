// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Compat header for the vendored 1.15 HTML kernel (#1130): stands in
// for the canonical pagespeed/kernel/base/string.h header. The sync tool
// rewrites the vendored files' includes to point here; per-call-site edits to
// vendored files are never made. See lib/html/CLAUDE.md.
//
// Deliberately identical to lib/js/compat/string.h (the D1 alias): a TU that
// includes both compat string headers (e.g. src/worker/worker.cc, which
// includes both kernels' public headers) sees the SAME alias twice, which is
// a legal typedef-name redefinition, not an ODR violation.

#ifndef PAGESPEED_LIB_HTML_COMPAT_STRING_H_
#define PAGESPEED_LIB_HTML_COMPAT_STRING_H_

#include <string>

using GoogleString = std::string;

#endif  // PAGESPEED_LIB_HTML_COMPAT_STRING_H_
