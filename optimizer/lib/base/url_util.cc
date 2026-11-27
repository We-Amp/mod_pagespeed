// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - URL utility functions

#include "lib/base/url_util.h"

#include <string>
#include <string_view>

namespace pagespeed {

std::string_view UrlDirectory(std::string_view url) {
  // Strip query string and fragment.
  size_t q = url.find('?');
  if (q != std::string_view::npos) url = url.substr(0, q);
  q = url.find('#');
  if (q != std::string_view::npos) url = url.substr(0, q);

  size_t last_slash = url.rfind('/');
  if (last_slash == std::string_view::npos) return {};
  return url.substr(0, last_slash + 1);
}

std::string ResolvePath(std::string_view base_dir, std::string_view relative) {
  if (relative.empty()) return std::string(base_dir);

  // Absolute path or URL: return as-is.
  if (relative[0] == '/' || relative.find("://") != std::string_view::npos) {
    return std::string(relative);
  }

  std::string result(base_dir);
  result.append(relative);

  // Normalize ../
  // Repeatedly find "X/../" and collapse, but never traverse above the root.
  // For URLs with a scheme (e.g., "http://host/"), the root is after the host.
  // For absolute paths, the root is "/".
  size_t root_pos = 0;
  size_t scheme_end = result.find("://");
  if (scheme_end != std::string::npos) {
    // Skip past scheme://host/
    size_t host_slash = result.find('/', scheme_end + 3);
    if (host_slash != std::string::npos) {
      root_pos = host_slash;
    }
  }
  for (;;) {
    size_t dotdot = result.find("/../");
    if (dotdot == std::string::npos || dotdot == 0) break;
    // Find the previous directory.
    size_t prev_slash = result.rfind('/', dotdot - 1);
    if (prev_slash == std::string::npos) break;
    // Don't collapse above the root.
    if (prev_slash < root_pos) break;
    result.erase(prev_slash, dotdot + 3 - prev_slash);
  }

  // Strip any residual /../ at the root boundary (prevents path traversal).
  while (result.size() > root_pos + 3 &&
         result.compare(root_pos, 4, "/../") == 0) {
    result.erase(root_pos + 1, 3);
  }
  // Handle trailing /.. at the root boundary.
  if (result.size() == root_pos + 3 &&
      result.compare(root_pos, 3, "/..") == 0) {
    result.erase(root_pos + 1, 2);
  }

  // Normalize ./ segments.
  size_t pos = 0;
  while ((pos = result.find("/./", pos)) != std::string::npos) {
    result.erase(pos, 2);
  }

  return result;
}

std::string_view UrlHostname(std::string_view url) {
  // Find scheme separator "://".
  size_t scheme_end = url.find("://");
  if (scheme_end == std::string_view::npos) return {};

  // Host starts after "://".
  size_t host_start = scheme_end + 3;
  if (host_start >= url.size()) return {};

  // Host ends at next '/', '?', '#', or end of string.
  size_t host_end = url.find_first_of("/?#", host_start);
  if (host_end == std::string_view::npos) host_end = url.size();

  // Strip userinfo (user:pass@) if present.
  size_t at = url.find('@', host_start);
  if (at != std::string_view::npos && at < host_end) {
    host_start = at + 1;
  }

  return url.substr(host_start, host_end - host_start);
}

}  // namespace pagespeed
