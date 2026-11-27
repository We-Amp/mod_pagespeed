// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_VERSION_H_
#define PAGESPEED_VERSION_H_

// kProductVersion is generated from the workspace VERSION.txt by the
// //src/product_version:gen_product_version genrule. kPageSpeedVersion below
// is an alias for it so existing callers keep their name. The version string
// is NOT hand-edited here — bump VERSION.txt instead.
#include "src/product_version/product_version_string.h"

namespace pagespeed {

inline constexpr const char* kPageSpeedVersion = kProductVersion;
inline constexpr char kPageSpeedProductId[] = "mps2";

// Tracking metadata constants (Phase 3b)
// PAGESPEED_SERVER can be overridden at compile time (e.g. via
// --copt=-DPAGESPEED_SERVER='"aspnetcore"' for the NuGet build). Defaults to
// "nginx" for backward compatibility with the historical nginx-only build.
#ifndef PAGESPEED_SERVER
#define PAGESPEED_SERVER "nginx"
#endif
inline constexpr char kPageSpeedServer[] = PAGESPEED_SERVER;

#if defined(__linux__) && defined(__x86_64__)
inline constexpr char kPageSpeedOs[] = "linux";
inline constexpr char kPageSpeedArch[] = "amd64";
#elif defined(__linux__) && defined(__aarch64__)
inline constexpr char kPageSpeedOs[] = "linux";
inline constexpr char kPageSpeedArch[] = "arm64";
#elif defined(__APPLE__) && defined(__aarch64__)
inline constexpr char kPageSpeedOs[] = "macos";
inline constexpr char kPageSpeedArch[] = "arm64";
#elif defined(_WIN32) && defined(_M_X64)
inline constexpr char kPageSpeedOs[] = "windows";
inline constexpr char kPageSpeedArch[] = "amd64";
#else
inline constexpr char kPageSpeedOs[] = "";
inline constexpr char kPageSpeedArch[] = "";
#endif

#ifndef PAGESPEED_DISTRIBUTION
#define PAGESPEED_DISTRIBUTION "source"
#endif
inline constexpr char kPageSpeedDistribution[] = PAGESPEED_DISTRIBUTION;

// Compile-time verification
static_assert(sizeof(kPageSpeedServer) > 1, "server must be set");
static_assert(sizeof(kPageSpeedOs) > 1, "os must be set");
static_assert(sizeof(kPageSpeedArch) > 1, "arch must be set");
static_assert(sizeof(kPageSpeedDistribution) > 1, "distribution must be set");

}  // namespace pagespeed

#endif  // PAGESPEED_VERSION_H_
