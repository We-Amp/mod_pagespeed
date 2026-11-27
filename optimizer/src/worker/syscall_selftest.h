// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// The positive control for the syscall profile: a switch that
// deliberately makes a call the profile denies, so that "the filter is
// installed and it kills" is something a test can OBSERVE rather than infer
// from a unit file.  A control that cannot fail proves nothing, so this is
// the leg that must go red when the profile is loosened.
//
// It is COMPILED OUT of every shipped binary.  The forbidden call, and even
// the flag string that reaches it, exist only under -DPAGESPEED_SELFTEST,
// which no release or CI build config defines.  A production binary that
// carried a documented "make me crash" switch would be a liability out of
// all proportion to the test it enables; the deb and rpm self-tests assert
// its absence in the payload, and a rig leg asserts the installed binary
// rejects the flag.
//
// Build the enabled form for a soak or a local investigation with:
//
//   bazel build --copt=-DPAGESPEED_SELFTEST //src/worker:factory_worker
//
// Both forms are compiled in every CI run (syscall_selftest_test builds the
// enabled variant through a second cc_library with local_defines), so the
// disabled default cannot drift into code that no longer compiles.

#ifndef SRC_WORKER_SYSCALL_SELFTEST_H_
#define SRC_WORKER_SYSCALL_SELFTEST_H_

namespace pagespeed {

// True only in a -DPAGESPEED_SELFTEST build.  Callers use it to decide
// whether to mention the switch in --help, and tests use it to assert which
// variant they linked.
bool SyscallSelftestCompiledIn();

// Recognises the selftest switch anywhere in argv, WITHOUT acting on it.  In
// a shipped binary this is always false and the flag string is not in the
// binary at all, so the normal parser reaches it and reports it as an
// unknown option.  Split out from the runner below so a unit test can cover
// the recognition half in both build variants without the test process
// making a call its own environment might be configured to kill.
bool SyscallSelftestRequested(int argc, char** argv);

// Acts on the switch: if it is present (and this is a -DPAGESPEED_SELFTEST
// build), makes the forbidden call and returns true.
//
// When it returns true it has ALREADY made the forbidden call.  Under an
// enforcing SystemCallFilter= the process is dead by then (SIGSYS, exit
// status 31) and nothing after this point runs.  It returns true, and sets
// *exit_code, only when the call was allowed to proceed -- which is the
// NEGATIVE control: the profile is not enforcing.
bool MaybeRunSyscallSelftest(int argc, char** argv, int* exit_code);

}  // namespace pagespeed

#endif  // SRC_WORKER_SYSCALL_SELFTEST_H_
