// Copyright (c) 2024-2026 We-Amp B.V.
//
// Compile-time tracking metadata for license telemetry.
// Defines: PAGESPEED_SERVER, PAGESPEED_OS, PAGESPEED_ARCH, PAGESPEED_DISTRIBUTION.
//
// PAGESPEED_SERVER is set per-port via Bazel defines:
//   system         -> "nginx"
//   system_apache  -> "apache"
//   system_envoy   -> "envoy"
//   system_windows -> "iis"
//
// PAGESPEED_OS and PAGESPEED_ARCH are auto-detected from compiler predefined macros.
// PAGESPEED_DISTRIBUTION defaults to "source" (overridden to "binary" in release builds).

#ifndef PAGESPEED_KERNEL_LICENSE_V2_TRACKING_METADATA_H_
#define PAGESPEED_KERNEL_LICENSE_V2_TRACKING_METADATA_H_

// --- Server (set by per-port Bazel target) ---
#ifndef PAGESPEED_SERVER
#define PAGESPEED_SERVER "unknown"
#endif

// --- OS ---
#if defined(__linux__)
#define PAGESPEED_OS "linux"
#elif defined(__APPLE__)
#define PAGESPEED_OS "macos"
#elif defined(_WIN32)
#define PAGESPEED_OS "windows"
#else
#define PAGESPEED_OS ""
#endif

// --- Architecture ---
#if defined(__x86_64__) || defined(_M_X64)
#define PAGESPEED_ARCH "amd64"
#elif defined(__aarch64__) || defined(_M_ARM64)
#define PAGESPEED_ARCH "arm64"
#else
#define PAGESPEED_ARCH ""
#endif

// --- Distribution channel ---
#ifndef PAGESPEED_DISTRIBUTION
#define PAGESPEED_DISTRIBUTION "source"
#endif

// Compile-time verification: os and arch must be set for supported platforms.
// sizeof("linux") == 6, sizeof("") == 1.
static_assert(sizeof(PAGESPEED_OS) > 1, "PAGESPEED_OS must be set");
static_assert(sizeof(PAGESPEED_ARCH) > 1, "PAGESPEED_ARCH must be set");
static_assert(sizeof(PAGESPEED_DISTRIBUTION) > 1,
              "PAGESPEED_DISTRIBUTION must be set");

#endif  // PAGESPEED_KERNEL_LICENSE_V2_TRACKING_METADATA_H_
