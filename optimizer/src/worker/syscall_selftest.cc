// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "src/worker/syscall_selftest.h"

#ifdef PAGESPEED_SELFTEST
#include <cstdio>
#include <cstring>
#if defined(__linux__)
#include <sys/swap.h>
#endif
#endif

namespace pagespeed {

bool SyscallSelftestCompiledIn() {
#ifdef PAGESPEED_SELFTEST
  return true;
#else
  return false;
#endif
}

bool SyscallSelftestRequested([[maybe_unused]] int argc,
                              [[maybe_unused]] char** argv) {
#ifndef PAGESPEED_SELFTEST
  // Not merely disabled: the flag string lives inside the #ifdef, so a
  // shipped binary does not contain it at all and `strings` can prove that.
  return false;
#else
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--selftest-forbidden-syscall") == 0) return true;
  }
  return false;
#endif
}

bool MaybeRunSyscallSelftest([[maybe_unused]] int argc,
                             [[maybe_unused]] char** argv,
                             [[maybe_unused]] int* exit_code) {
#ifndef PAGESPEED_SELFTEST
  return false;
#else
  if (!SyscallSelftestRequested(argc, argv)) return false;

#if defined(__linux__)
  // swapon(2) is the control call, chosen for three reasons: it is outside
  // systemd's @system-service group on every systemd version this product
  // targets (so the enforcing profile denies it), it needs no argument the
  // kernel could reject before the filter runs, and it has no side effect
  // when it is merely permitted -- an unprivileged caller with a
  // non-existent path gets EPERM or ENOENT and nothing is swapped on.
  //
  // Under the enforcing profile this line never returns: the kernel raises
  // SIGSYS and the service dies with ExecMainStatus=31.
  std::fprintf(stderr,
               "syscall selftest: calling swapon(2), which the enforcing "
               "profile denies\n");
  std::fflush(stderr);
  const int rc = ::swapon("/nonexistent/pagespeed-selftest", 0);
  std::fprintf(stderr,
               "syscall selftest: swapon returned %d -- NOT killed, so no "
               "enforcing syscall filter is in force here\n",
               rc);
#else
  std::fprintf(stderr,
               "syscall selftest: no forbidden-call control on this "
               "platform\n");
#endif
  std::fflush(stderr);
  // Surviving the call is the negative control, and it is a legitimate
  // outcome (the documented opt-out drop-in, or no unit at all).  Exit 0 and
  // let the caller decide: the rig asserts 31 under the shipped default and
  // 0 under the opt-out, so a leg that cannot tell the two apart fails.
  if (exit_code != nullptr) *exit_code = 0;
  return true;
#endif  // PAGESPEED_SELFTEST
}

}  // namespace pagespeed
