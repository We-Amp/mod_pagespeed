// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// One source, TWO test targets (test/src/worker/BUILD):
//
//   syscall_selftest_test           links //src/worker:syscall_selftest
//                                   -- the SHIPPED library, switch off
//   syscall_selftest_enabled_test   links :syscall_selftest_enabled
//                                   -- local_defines = PAGESPEED_SELFTEST
//
// so both build configurations are compiled and asserted on every run.  The
// assertions are written against SyscallSelftestCompiledIn() rather than
// against the macro, which is what lets one file carry both: each property
// is stated as a relation ("recognises the switch IF AND ONLY IF it is
// compiled in") that has to hold in either variant.  That is also the exact
// property the rig's positive control rests on -- a control that quietly
// stopped recognising its own switch would pass forever.
//
// What is NOT here: the forbidden call.  Whether it is fatal depends on the
// environment, so a unit test that made it would either be a crash report or
// a vacuous pass.  It belongs to the rig, under a real unit, where the
// shipped default enforces and the documented opt-out is installed on purpose.

#include "src/worker/syscall_selftest.h"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

namespace pagespeed {
namespace {

// argv is char** by contract (main's signature), so the fixtures are
// mutable buffers rather than string literals.
class Argv {
 public:
  explicit Argv(std::vector<const char*> args) {
    storage_.reserve(args.size());
    for (const char* a : args) {
      storage_.emplace_back(a, a + std::strlen(a) + 1);
    }
    pointers_.reserve(storage_.size());
    for (auto& s : storage_) pointers_.push_back(s.data());
  }
  int argc() const { return static_cast<int>(pointers_.size()); }
  char** argv() { return pointers_.data(); }

 private:
  std::vector<std::vector<char>> storage_;
  std::vector<char*> pointers_;
};

TEST(SyscallSelftestTest, RecognisesTheSwitchIffCompiledIn) {
  const bool on = SyscallSelftestCompiledIn();

  Argv first({"factory_worker", "--selftest-forbidden-syscall"});
  EXPECT_EQ(SyscallSelftestRequested(first.argc(), first.argv()), on);

  Argv later({"factory_worker", "--cache-dir", "/tmp/x", "--log-level",
              "warning", "--selftest-forbidden-syscall"});
  EXPECT_EQ(SyscallSelftestRequested(later.argc(), later.argv()), on);
}

TEST(SyscallSelftestTest, NeverFiresWithoutTheExactSwitch) {
  // These must be false in BOTH variants.
  Argv plain({"factory_worker", "--cache-dir", "/tmp/x"});
  EXPECT_FALSE(SyscallSelftestRequested(plain.argc(), plain.argv()));

  // argv[0] is never a flag: a binary that happened to be installed under
  // this name must not trip the control.
  Argv argv0({"--selftest-forbidden-syscall"});
  EXPECT_FALSE(SyscallSelftestRequested(argv0.argc(), argv0.argv()));

  // Near-misses must not match; the rig's negative control depends on the
  // switch being exactly this string.
  Argv nearly({"factory_worker", "--selftest-forbidden-syscalls",
               "--selftest-forbidden", "selftest-forbidden-syscall",
               "--selftest-forbidden-syscall=1"});
  EXPECT_FALSE(SyscallSelftestRequested(nearly.argc(), nearly.argv()));
}

TEST(SyscallSelftestTest, RunnerDeclinesWithoutTheSwitch) {
  // The half of MaybeRunSyscallSelftest that is safe to call in any
  // environment: with no switch it must decline without touching
  // exit_code, so a normal daemon start is never diverted.
  Argv plain({"factory_worker", "--cache-dir", "/tmp/x"});
  int exit_code = -12345;
  EXPECT_FALSE(MaybeRunSyscallSelftest(plain.argc(), plain.argv(), &exit_code));
  EXPECT_EQ(exit_code, -12345);
}

}  // namespace
}  // namespace pagespeed
