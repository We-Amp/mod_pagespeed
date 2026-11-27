// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The sandbox mode surface and the startup probe.

#include "src/browser/browser_sandbox.h"

#include <string>

#include "gtest/gtest.h"

#ifdef __linux__
#include <unistd.h>
#endif

namespace pagespeed {
namespace {

TEST(BrowserSandboxMode, ParsesExactlyRequireAndOff) {
  BrowserSandboxMode mode = BrowserSandboxMode::kOff;
  EXPECT_TRUE(ParseBrowserSandboxMode("require", &mode));
  EXPECT_EQ(mode, BrowserSandboxMode::kRequire);

  mode = BrowserSandboxMode::kRequire;
  EXPECT_TRUE(ParseBrowserSandboxMode("off", &mode));
  EXPECT_EQ(mode, BrowserSandboxMode::kOff);
}

// The whole point of the flag: there is no mode that silently degrades to an
// unsandboxed browser.  `auto` must not parse, today or after a refactor.
TEST(BrowserSandboxMode, RejectsAutoAndEverythingElse) {
  BrowserSandboxMode mode = BrowserSandboxMode::kRequire;
  for (const char* bad : {"auto", "on", "yes", "true", "", "Require", "OFF",
                          "disabled", "no-sandbox"}) {
    EXPECT_FALSE(ParseBrowserSandboxMode(bad, &mode)) << "accepted: " << bad;
    // A rejected parse must not have written anything.
    EXPECT_EQ(mode, BrowserSandboxMode::kRequire) << "clobbered by: " << bad;
  }
  EXPECT_FALSE(ParseBrowserSandboxMode("require", nullptr));
}

// These strings are a wire contract: /v1/health and /v1/stats publish them and
// monitoring checks match on them.
TEST(BrowserSandboxState, WireNamesAreStable) {
  EXPECT_STREQ(BrowserSandboxStateName(BrowserSandboxState::kDisabled),
               "disabled");
  EXPECT_STREQ(BrowserSandboxStateName(BrowserSandboxState::kOn), "on");
  EXPECT_STREQ(BrowserSandboxStateName(BrowserSandboxState::kUnavailable),
               "unavailable");
  EXPECT_STREQ(BrowserSandboxStateName(BrowserSandboxState::kOff), "off");
}

// The probe now runs inside the daemon's syscall filter, and
// "the filter killed me" must not read the same as "the machine was slow".
// H4 shipped both as "did not complete"; the reason table is what fixes it.
// Asserted here rather than in the smoke script alone, because the packaging
// smoke greps for these exact clauses.
TEST(BrowserSandboxProbeReasonTest, EachFailureModeSaysSomethingDifferent) {
  const std::string denied =
      BrowserSandboxProbeReason(BrowserSandboxProbeOutcome::kDenied, 0);
  const std::string killed =
      BrowserSandboxProbeReason(BrowserSandboxProbeOutcome::kFilterKilled, 0);
  const std::string timed_out =
      BrowserSandboxProbeReason(BrowserSandboxProbeOutcome::kTimedOut, 0);
  const std::string signalled =
      BrowserSandboxProbeReason(BrowserSandboxProbeOutcome::kSignalled, 9);
  const std::string spawn_failed =
      BrowserSandboxProbeReason(BrowserSandboxProbeOutcome::kSpawnFailed, 0);

  for (const std::string* r :
       {&denied, &killed, &timed_out, &signalled, &spawn_failed}) {
    EXPECT_FALSE(r->empty());
  }
  EXPECT_NE(denied, killed);
  EXPECT_NE(killed, timed_out);
  EXPECT_NE(denied, timed_out);
  EXPECT_NE(signalled, timed_out);

  // An available probe has nothing to explain.
  EXPECT_TRUE(
      BrowserSandboxProbeReason(BrowserSandboxProbeOutcome::kAvailable, 0)
          .empty());
}

// The clause has to be actionable without promising anything that does not
// exist yet: it names the mechanism (SIGSYS, a syscall filter), the two knobs
// by kind, where to read more, and the container remedy.  A reword that drops
// them makes a red startup unactionable -- that is the H4 defect.
//
// It must NOT name a concrete drop-in file: the hardening profile and its
// drop-in are a later change, and a message pointing at a path that does not
// exist is worse than one that does not point at all.
TEST(BrowserSandboxProbeReasonTest, FilterKillIsActionableAndPromisesNothing) {
  const std::string killed =
      BrowserSandboxProbeReason(BrowserSandboxProbeOutcome::kFilterKilled, 0);
  EXPECT_NE(killed.find("SIGSYS"), std::string::npos) << killed;
  EXPECT_NE(killed.find("syscall filter"), std::string::npos) << killed;
  EXPECT_NE(killed.find("namespace restriction"), std::string::npos) << killed;
  EXPECT_NE(killed.find("browser-analysis"), std::string::npos) << killed;
  EXPECT_NE(killed.find("seccomp profile"), std::string::npos) << killed;
  // ...and must NOT read like a timeout, which is what it read like before.
  EXPECT_EQ(killed.find("did not complete"), std::string::npos) << killed;
  // ...and must not name a drop-in file that this change does not ship.
  EXPECT_EQ(killed.find(".conf"), std::string::npos) << killed;
}

// A timeout is still a timeout: it must not borrow the filter language and
// send an operator to edit a unit that is not the problem.
TEST(BrowserSandboxProbeReasonTest, TimeoutDoesNotBlameASyscallFilter) {
  const std::string timed_out =
      BrowserSandboxProbeReason(BrowserSandboxProbeOutcome::kTimedOut, 0);
  EXPECT_EQ(timed_out.find("SIGSYS"), std::string::npos) << timed_out;
  EXPECT_EQ(timed_out.find("syscall filter"), std::string::npos) << timed_out;
}

TEST(BrowserSandboxProbeReasonTest, OtherSignalIsNamedByNumber) {
  const std::string signalled =
      BrowserSandboxProbeReason(BrowserSandboxProbeOutcome::kSignalled, 9);
  EXPECT_NE(signalled.find('9'), std::string::npos) << signalled;
}

// The probe must always answer, must never leave a zombie, and must always
// carry a diagnostic string (the log line names it either way).
TEST(BrowserSandboxProbe, AlwaysAnswersAndIsRepeatable) {
  const BrowserSandboxProbe first = ProbeBrowserSandbox();
  const BrowserSandboxProbe second = ProbeBrowserSandbox();
  EXPECT_EQ(first.available, second.available);
  EXPECT_FALSE(first.diagnostics.empty());
  if (!first.available) {
    EXPECT_FALSE(first.reason.empty())
        << "an unavailable verdict must say why — the refusal log quotes it";
  }
}

// `available` and `outcome` are two views of one verdict and must not drift:
// callers switch on the outcome, the log quotes the reason.
TEST(BrowserSandboxProbe, OutcomeAgreesWithAvailable) {
  const BrowserSandboxProbe probe = ProbeBrowserSandbox();
  EXPECT_EQ(probe.available,
            probe.outcome == BrowserSandboxProbeOutcome::kAvailable);
  if (probe.available) {
    EXPECT_TRUE(probe.reason.empty());
  } else {
    EXPECT_FALSE(probe.reason.empty())
        << "an unavailable verdict must say why -- the refusal log quotes it";
  }
}

#ifdef __linux__
// Running as uid 0, the probe must report unavailable whatever the kernel
// allows: Chrome itself refuses to sandbox as root, so a "yes" here would
// become a Chrome crash on the first analysis instead of a named startup
// refusal.  (Only asserted when the test happens to run as root — CI runs
// both ways across its images.)
TEST(BrowserSandboxProbe, RootIsNeverSandboxable) {
  if (::geteuid() != 0) {
    GTEST_SKIP() << "not running as root";
  }
  const BrowserSandboxProbe probe = ProbeBrowserSandbox();
  EXPECT_FALSE(probe.available);
  EXPECT_NE(probe.reason.find("root"), std::string::npos) << probe.reason;
}

TEST(BrowserSandboxProbe, DiagnosticsNameTheKnobsAnOperatorWouldCheck) {
  const BrowserSandboxProbe probe = ProbeBrowserSandbox();
  EXPECT_NE(probe.diagnostics.find("unprivileged_userns_clone"),
            std::string::npos);
  EXPECT_NE(probe.diagnostics.find("apparmor_restrict_unprivileged_userns"),
            std::string::npos);
  EXPECT_NE(probe.diagnostics.find("max_user_namespaces"), std::string::npos);
}
#endif  // __linux__

}  // namespace
}  // namespace pagespeed
