// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Is this daemon running under a kernel syscall filter?
//
// The optimizer daemon does not install its own seccomp filter and does not
// own the control: on a package install the service manager installs one from
// the unit, before execve; in a container the container runtime's own default
// profile applies; started by hand there is none.  Because the daemon owns
// none of those, there is deliberately no flag here -- a flag naming a control
// the process cannot enforce is a flag that lies.
//
// What the daemon can do is answer the question honestly.  This module reads
// the kernel's own view of the calling process from /proc/self/status and
// publishes it as one read-only string on GET /v1/health and /v1/stats, so
// "is this daemon filtered?" is answerable by a monitoring check without
// reading a unit file on the host.
//
// MEASURED, and worth stating precisely because it is easy to get backwards:
//
//   * The shipped host unit carries an enforcing SystemCallFilter= (2.1)
//     AND a SystemCallLog=, and systemd implements both as seccomp filters,
//     so a package install reports "filtered" -- and keeps reporting it
//     when the operator installs the documented opt-out, because the log
//     filter stays.  The enforcing/opted-out question is answered by
//     `systemctl show -p SystemCallFilter`, never by this field.
//   * A stock container reports "filtered" too -- container runtimes apply
//     a default seccomp profile to every container (`Seccomp: 2` in
//     /proc/self/status).  It is not this daemon's profile.
//
// So "filtered" answers "is SOME kernel syscall filter attached to this
// process", not "is the optimizer's own hardening profile in force".  Those
// are different questions and this field only answers the first one.
//
// The value is never derived from configuration; only from /proc.

#ifndef PAGESPEED_SRC_WORKER_SYSCALL_FILTER_H_
#define PAGESPEED_SRC_WORKER_SYSCALL_FILTER_H_

#include <cstdint>
#include <string_view>

namespace pagespeed {

// What the kernel says about this process.
enum class SyscallFilterState : std::uint8_t {
  // The kernel does not report a filter state we can read: a non-Linux host,
  // an unreadable /proc, or a kernel too old to publish the field.  Reported
  // as its own value rather than folded into "none", because "we could not
  // look" and "we looked and there is no filter" are different answers.
  kUnknown,
  kNone,      // seccomp mode 0: no filter is attached to this process
  kFiltered,  // a seccomp filter (or strict mode) is in force
};

// Stable wire strings: "unknown" | "none" | "filtered".
const char* SyscallFilterStateName(SyscallFilterState state);

// Parses the contents of a /proc/<pid>/status document.  Split out from the
// read so the mapping is unit-testable without arranging a real filter.
//
// `Seccomp:` is authoritative (0 = disabled, 1 = strict, 2 = filter).  When
// it is absent -- some kernels omit it -- a non-zero `Seccomp_filters:` count
// still proves a filter is attached; anything else is kUnknown.
SyscallFilterState ParseSyscallFilterStatus(std::string_view proc_status);

// Reads /proc/self/status and maps it.  Returns kUnknown on non-Linux.
// Cheap (one small /proc read); safe to call per request, and deliberately
// read live rather than cached, so the answer cannot go stale if a filter is
// installed after startup.
SyscallFilterState DetectSyscallFilter();

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_WORKER_SYSCALL_FILTER_H_
