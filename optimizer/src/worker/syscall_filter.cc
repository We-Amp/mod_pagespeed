// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "src/worker/syscall_filter.h"

#include <cstdio>
#include <string>
#include <string_view>

namespace pagespeed {
namespace {

// Returns the value text of the first `key` line in a /proc status document,
// or an empty view when the key is absent.  Keys are line-anchored, so
// "Seccomp:" never matches inside "Seccomp_filters:".
std::string_view FieldValue(std::string_view doc, std::string_view key) {
  size_t pos = 0;
  while (pos <= doc.size()) {
    const size_t eol = doc.find('\n', pos);
    const std::string_view line =
        doc.substr(pos, eol == std::string_view::npos ? std::string_view::npos
                                                      : eol - pos);
    if (line.size() > key.size() && line.starts_with(key)) {
      std::string_view value = line.substr(key.size());
      while (!value.empty() &&
             (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
      }
      while (!value.empty() && (value.back() == ' ' || value.back() == '\t' ||
                                value.back() == '\r')) {
        value.remove_suffix(1);
      }
      return value;
    }
    if (eol == std::string_view::npos) break;
    pos = eol + 1;
  }
  return {};
}

// Parses a small non-negative decimal.  Returns false for empty, non-numeric,
// or implausibly long text -- a field we cannot read is "unknown", never
// "none".
//
// The length cap is what stops silent wraparound, and the failure direction is
// the reason it matters: without it "Seccomp: 18446744073709551616" accumulates
// back to 0, and a garbled /proc line would be reported as "none" -- the field
// claiming an unconfined process.  Nine digits cannot overflow a 64-bit
// unsigned and is far above any real seccomp mode (0-2) or attached-filter
// count, so nothing legitimate is rejected.
bool ParseSmallUnsigned(std::string_view text, unsigned long* out) {
  constexpr size_t kMaxDigits = 9;
  if (text.empty() || text.size() > kMaxDigits) return false;
  unsigned long value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
    value = value * 10 + static_cast<unsigned long>(c - '0');
  }
  *out = value;
  return true;
}

}  // namespace

const char* SyscallFilterStateName(SyscallFilterState state) {
  switch (state) {
    case SyscallFilterState::kUnknown:
      return "unknown";
    case SyscallFilterState::kNone:
      return "none";
    case SyscallFilterState::kFiltered:
      return "filtered";
  }
  return "unknown";
}

SyscallFilterState ParseSyscallFilterStatus(std::string_view proc_status) {
  unsigned long mode = 0;
  if (ParseSmallUnsigned(FieldValue(proc_status, "Seccomp:"), &mode)) {
    // 0 = SECCOMP_MODE_DISABLED, 1 = STRICT, 2 = FILTER.  Strict counts as
    // filtered: something is denying syscalls, which is what the field is
    // asked.  An unexpected future mode is reported as filtered for the same
    // reason -- the honest failure direction is "there is a filter".
    return mode == 0 ? SyscallFilterState::kNone
                     : SyscallFilterState::kFiltered;
  }
  unsigned long filters = 0;
  if (ParseSmallUnsigned(FieldValue(proc_status, "Seccomp_filters:"),
                         &filters) &&
      filters > 0) {
    return SyscallFilterState::kFiltered;
  }
  return SyscallFilterState::kUnknown;
}

SyscallFilterState DetectSyscallFilter() {
#ifndef __linux__
  return SyscallFilterState::kUnknown;
#else
  FILE* f = std::fopen("/proc/self/status", "re");
  if (f == nullptr) return SyscallFilterState::kUnknown;
  std::string doc;
  char buf[1024];
  size_t got = 0;
  while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0) {
    doc.append(buf, got);
  }
  std::fclose(f);
  return ParseSyscallFilterStatus(doc);
#endif
}

}  // namespace pagespeed
