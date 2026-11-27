// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The read-only "is this daemon filtered?" answer that
// /v1/health and /v1/stats publish.

#include "src/worker/syscall_filter.h"

#include <string>

#include "gtest/gtest.h"

#ifdef __linux__
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace pagespeed {
namespace {

// These strings are a wire contract: monitoring checks match on them.
TEST(SyscallFilterState, WireNamesAreStable) {
  EXPECT_STREQ(SyscallFilterStateName(SyscallFilterState::kUnknown), "unknown");
  EXPECT_STREQ(SyscallFilterStateName(SyscallFilterState::kNone), "none");
  EXPECT_STREQ(SyscallFilterStateName(SyscallFilterState::kFiltered),
               "filtered");
}

TEST(ParseSyscallFilterStatus, SeccompZeroIsNone) {
  EXPECT_EQ(ParseSyscallFilterStatus("Name:\tworker\nSeccomp:\t0\n"),
            SyscallFilterState::kNone);
}

TEST(ParseSyscallFilterStatus, SeccompFilterAndStrictAreBothFiltered) {
  EXPECT_EQ(ParseSyscallFilterStatus("Seccomp:\t2\nSeccomp_filters:\t1\n"),
            SyscallFilterState::kFiltered);
  EXPECT_EQ(ParseSyscallFilterStatus("Seccomp:\t1\n"),
            SyscallFilterState::kFiltered);
}

// The key is line-anchored: "Seccomp_filters:" must never be read as
// "Seccomp:", which would report a filtered process as unfiltered whenever
// the filter count happened to be 0.
TEST(ParseSyscallFilterStatus, FilterCountLineIsNotMistakenForTheModeLine) {
  EXPECT_EQ(ParseSyscallFilterStatus("Seccomp_filters:\t0\nSeccomp:\t2\n"),
            SyscallFilterState::kFiltered);
  EXPECT_EQ(ParseSyscallFilterStatus("Seccomp_filters:\t3\n"),
            SyscallFilterState::kFiltered);
}

// A field we cannot read is "unknown", never "none".  Reporting "none" for a
// document we failed to parse is exactly the silent lie this field exists to
// remove: it would tell a monitoring check the daemon is unfiltered.
TEST(ParseSyscallFilterStatus, UnreadableIsUnknownNotNone) {
  EXPECT_EQ(ParseSyscallFilterStatus(""), SyscallFilterState::kUnknown);
  EXPECT_EQ(ParseSyscallFilterStatus("Name:\tworker\nThreads:\t4\n"),
            SyscallFilterState::kUnknown);
  EXPECT_EQ(ParseSyscallFilterStatus("Seccomp:\tyes\n"),
            SyscallFilterState::kUnknown);
  EXPECT_EQ(ParseSyscallFilterStatus("Seccomp:\n"),
            SyscallFilterState::kUnknown);
  // A key that only appears mid-line is not a field.
  EXPECT_EQ(ParseSyscallFilterStatus("Note: Seccomp:\t2\n"),
            SyscallFilterState::kUnknown);
}

// Wraparound would report a garbled line as "none" -- the field claiming an
// unconfined process.  The honest answer for a line we cannot read is
// "unknown", so an over-long value must not accumulate back into range.
TEST(ParseSyscallFilterStatus, AnOverlongValueIsUnknownNotNone) {
  // 2^64, which wraps to exactly 0 if the digits are accumulated unchecked.
  EXPECT_EQ(ParseSyscallFilterStatus("Seccomp:\t18446744073709551616\n"),
            SyscallFilterState::kUnknown);
  EXPECT_EQ(
      ParseSyscallFilterStatus("Seccomp_filters:\t18446744073709551616\n"),
      SyscallFilterState::kUnknown);
  // ...while a value that is merely large but readable is still honoured.
  EXPECT_EQ(ParseSyscallFilterStatus("Seccomp_filters:\t100\n"),
            SyscallFilterState::kFiltered);
}

TEST(ParseSyscallFilterStatus, ToleratesCrlfAndAMissingFinalNewline) {
  EXPECT_EQ(ParseSyscallFilterStatus("Seccomp:\t0\r\nThreads:\t1\r\n"),
            SyscallFilterState::kNone);
  EXPECT_EQ(ParseSyscallFilterStatus("Threads:\t1\nSeccomp: 2"),
            SyscallFilterState::kFiltered);
}

#ifdef __linux__
// The end-to-end claim: an unfiltered process reports "none", and the same
// process reports "filtered" once the kernel actually has a filter attached.
// Run in a forked child because a seccomp filter cannot be removed, and the
// gtest binary runs every case in one process.
//
// This only PROVES anything when we start unfiltered.  Much of the time we do
// not: a container runtime's default seccomp profile, a build sandbox, or a
// CI host can all have a filter attached before this test runs, and asserting
// filtered -> filtered would pass no matter what the code did.  Rather than
// dress that up as a passing test, skip it and say why.
TEST(DetectSyscallFilter, FlipsToFilteredWhenTheKernelAttachesAFilter) {
  // Before: whatever the environment does to us, it must not be "unknown" --
  // /proc/self/status is readable on every kernel this daemon supports.
  const SyscallFilterState before = DetectSyscallFilter();
  ASSERT_NE(before, SyscallFilterState::kUnknown)
      << "/proc/self/status did not carry a readable Seccomp field";
  if (before != SyscallFilterState::kNone) {
    GTEST_SKIP() << "this process is already reported as '"
                 << SyscallFilterStateName(before)
                 << "' before the test attaches anything (a container runtime "
                    "default profile or build sandbox), so the none->filtered "
                    "transition cannot be proven here; the parse tests cover "
                    "the mapping";
  }

  const pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    // An allow-everything filter: it changes nothing about what the child may
    // do, and everything about what the kernel reports about it.  That is the
    // point -- we are testing the report, not a denial.
    struct sock_filter prog[] = {
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog fprog = {
        .len = static_cast<unsigned short>(sizeof(prog) / sizeof(prog[0])),
        .filter = prog,
    };
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) ::_exit(70);
    if (::syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &fprog) != 0) {
      if (::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &fprog) != 0) {
        ::_exit(70);  // no seccomp on this kernel; the parent skips
      }
    }
    ::_exit(DetectSyscallFilter() == SyscallFilterState::kFiltered ? 0 : 1);
  }

  int status = 0;
  ASSERT_EQ(::waitpid(pid, &status, 0), pid);
  ASSERT_TRUE(WIFEXITED(status)) << "probe child died on a signal";
  if (WEXITSTATUS(status) == 70) {
    GTEST_SKIP() << "this kernel would not install a seccomp filter";
  }
  EXPECT_EQ(WEXITSTATUS(status), 0)
      << "a process with a filter attached did not report syscall_filter="
         "filtered";
}
#endif  // __linux__

}  // namespace
}  // namespace pagespeed
