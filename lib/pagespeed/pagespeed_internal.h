// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// pagespeed_internal.h -- Internal C++ structs for the C API.
// NOT part of the public API. NOT installed.

#ifndef PAGESPEED_INTERNAL_H_
#define PAGESPEED_INTERNAL_H_

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "lib/cache/cache.h"
#include "lib/classify/alternate_metadata.h"
#include "lib/classify/capability_mask.h"
#include "lib/pagespeed/pagespeed.h"
#include "src/worker/html_scanner.h"
#include "src/worker/html_transform_filter.h"

// --------------- opaque handle structs ---------------

struct ps_cache_s {
  std::unique_ptr<pagespeed::PageSpeedCache> cache;
};

struct ps_read_result_s {
  pagespeed::ReadResult result;
  std::string origin_ct_cstr;  // NUL-terminated copy for C callers
};

struct ps_write_handle_s {
  pagespeed::WriteResult write_result;
  bool closed = false;
  bool errored = false;
};

struct ps_scan_result {
  pagespeed::HtmlScanResult scan;
  // Pre-computed C string arrays for each element's class list.
  std::vector<std::vector<const char*>> class_ptrs;
};

struct ps_html_result {
  std::string output_html;
  std::string early_hints;
  bool modified = false;
  bool critical_css_injected = false;
  bool needs_revalidation = false;
};

struct ps_critical_css_result {
  std::string critical_css;
  int total_rules = 0;
  int critical_rules = 0;
};

struct ps_html_transform {
  // Stored config and data for deferred transform creation.
  pagespeed::HtmlTransformConfig transform_config;
  std::string critical_css;
  pagespeed::PageSpeedCache* cache_ptr = nullptr;
  std::string hostname;
  pagespeed::LcpCandidate lcp_candidate;
  std::vector<pagespeed::PreconnectOrigin> preconnect_origins;
  std::vector<std::string> speculation_urls;
  bool enable_critical_css = true;
  bool enable_lazy_load = true;
  bool enable_image_dimensions = true;
  bool enable_lcp_preload = true;
  bool enable_preconnect = true;
  bool enable_speculation_rules = false;
  bool last_modified = false;
};

// --------------- thread-local error state ---------------

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
inline thread_local std::string g_last_error_message;

inline void SetLastError(const std::string& msg) { g_last_error_message = msg; }

inline void ClearLastError() { g_last_error_message.clear(); }

// --------------- error mapping ---------------

inline ps_error_t MapError(cyclone::CacheError err) {
  switch (err) {
    case cyclone::CacheError::NotFound:
    case cyclone::CacheError::AlternateNotFound:
      return PS_ERR_NOT_FOUND;
    case cyclone::CacheError::IoError:
    case cyclone::CacheError::VolumeFileRefused:
      return PS_ERR_IO;
    case cyclone::CacheError::Corrupted:
    case cyclone::CacheError::ChainCorrupted:
      return PS_ERR_CORRUPTED;
    case cyclone::CacheError::NoSpace:
      return PS_ERR_NO_SPACE;
    case cyclone::CacheError::InvalidKey:
    case cyclone::CacheError::InvalidArgument:
    case cyclone::CacheError::InvalidConfiguration:
      return PS_ERR_INVALID_ARG;
    case cyclone::CacheError::Busy:
    case cyclone::CacheError::Timeout:
      return PS_ERR_BUSY;
    case cyclone::CacheError::Closed:
    case cyclone::CacheError::NotInitialized:
      return PS_ERR_CLOSED;
    case cyclone::CacheError::TooManyAlternates:
      return PS_ERR_TOO_MANY_ALTERNATES;
    case cyclone::CacheError::Exists:
    case cyclone::CacheError::AlreadyOpen:
      return PS_ERR_EXISTS;
    case cyclone::CacheError::NotOwned:
      return PS_ERR_NOT_OWNED;
    case cyclone::CacheError::IncompatibleVersion:
      return PS_ERR_VERSION_MISMATCH;
    default:
      return PS_ERR_INTERNAL;
  }
}

// --------------- input validation ---------------

inline constexpr size_t kMaxUrlLength = 8192;
inline constexpr size_t kMaxHostnameLength = 512;

inline ps_error_t ValidateUrlHostname(const char* url, const char* hostname) {
  if (!url || !hostname) return PS_ERR_INVALID_ARG;
  if (std::strlen(url) > kMaxUrlLength) {
    SetLastError("URL exceeds maximum length of 8192 bytes");
    return PS_ERR_INVALID_ARG;
  }
  if (std::strlen(hostname) > kMaxHostnameLength) {
    SetLastError("Hostname exceeds maximum length of 512 bytes");
    return PS_ERR_INVALID_ARG;
  }
  return PS_OK;
}

inline ps_error_t ValidateContentType(ps_content_type_t ct) {
  if (ct > PS_CONTENT_OTHER) {
    SetLastError("content_type out of range");
    return PS_ERR_INVALID_ARG;
  }
  return PS_OK;
}

// --------------- HTML keywords init guard ---------------

inline std::once_flag g_html_keywords_init;

#endif  // PAGESPEED_INTERNAL_H_
