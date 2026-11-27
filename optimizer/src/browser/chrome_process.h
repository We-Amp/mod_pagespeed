// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Chrome Process Manager
//
// Manages a headless Chrome process with CDP pipe transport.
// Handles spawning, crash recovery, RSS monitoring, and recycling.

#ifndef PAGESPEED_SRC_BROWSER_CHROME_PROCESS_H_
#define PAGESPEED_SRC_BROWSER_CHROME_PROCESS_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "src/browser/cdp_client.h"
#include "src/browser/cdp_types.h"
#include "uv.h"

namespace pagespeed {

// What to do about a profile directory's `SingletonLock` before launching
// Chrome into it.  Chrome writes the lock as a symlink whose target is
// `<hostname>-<pid>`; a lock left behind by a Chrome that is no longer running
// (a recreated container with a new hostname, a host reboot, a profile
// restored from another machine) makes every later launch exit with status
// 21 (PROFILE_IN_USE) until the entries are removed.
enum class SingletonLockAction : std::uint8_t {
  kNoLock,       // No lock present: nothing to do.
  kRemoveStale,  // Lock belongs to another host or to a pid that is not alive.
  kKeepLive,     // Lock belongs to a live process on this host: real conflict.
};

struct SingletonLockVerdict {
  SingletonLockAction action = SingletonLockAction::kNoLock;
  // Parsed owner, for the log line.  `owner_pid` is 0 when unparseable.
  std::string owner_host;
  int owner_pid = 0;
};

// Manages a headless Chrome process with CDP pipe transport.
//
// Handles:
// - Chrome spawning with --remote-debugging-pipe
// - CDP pipe setup (FD 3 read, FD 4 write)
// - Process exit detection and crash recovery
// - RSS memory monitoring with configurable limit
// - Page count tracking for recycling
//
// Usage:
//   ChromeProcess chrome(loop, config);
//   chrome.SetExitCallback([](int64_t s, int sig) { ... });
//   auto status = chrome.Start();
//   if (status.ok()) {
//     chrome.cdp_client()->SendCommand(...);
//   }
class ChromeProcess {
 public:
  using ExitCallback =
      std::function<void(int64_t exit_status, int term_signal)>;

  ChromeProcess(uv_loop_t* loop, ChromeProcessConfig config);
  ~ChromeProcess();

  ChromeProcess(const ChromeProcess&) = delete;
  ChromeProcess& operator=(const ChromeProcess&) = delete;

  // Spawn Chrome and set up CDP pipe transport.
  absl::Status Start();

  // Gracefully stop Chrome (SIGTERM, then SIGKILL after 5s).
  void Stop();

  // Set callback for Chrome process exit/crash.
  void SetExitCallback(ExitCallback callback);

  // CDP client connected to this Chrome instance.
  // Only valid after successful Start() and before Stop().
  CdpClient* cdp_client() { return cdp_client_.get(); }

  // Increment page counter. Returns true if recycle threshold
  // reached.
  bool IncrementPageCount();

  // Current RSS in MB. Linux only (reads /proc/pid/status);
  // returns 0 on other platforms. Note: monitors main Chrome
  // process only, not renderer subprocesses.
  int rss_mb() const { return current_rss_mb_; }

  // Is Chrome process running?
  bool running() const { return running_; }

  // Chrome PID (0 if not running).
  int pid() const { return process_.pid; }

  // True once Stop() has been called on this instance (RSS cap, shutdown):
  // the exit that follows was asked for, not a launch failure.  Sticky, so it
  // is still readable from inside the exit callback.
  bool stop_requested() const { return stop_requested_; }

  // Build the full Chrome argv (argv[0] == config.chrome_path) for `config`.
  // Exposed as a pure static helper so tests can assert the launch flags — in
  // particular the dead-resolver SSRF backstop `--host-resolver-rules=MAP *
  // ~NOTFOUND`, which is UNCONDITIONAL (not driven by ChromeProcessConfig). The
  // agent_optimize render relies on this: with all egress going through the
  // out-of-Chrome IP-pinned fetcher, Chrome itself must never resolve a hostname.
  static std::vector<std::string> BuildChromeArgs(
      const ChromeProcessConfig& config);

  // Build the environment Chrome is spawned with, from `base` (a NULL-
  // terminated "KEY=VALUE" array, normally `environ`).  Chrome resolves its
  // crash-report database from $XDG_CONFIG_HOME, else $HOME/.config, and when
  // that directory cannot be created the crash handler it spawns exits at once
  // and the browser CHECK-fails (SIGTRAP) before the DevTools pipe opens.  A
  // daemon that has dropped privileges typically still carries the launcher's
  // HOME (e.g. /root), which its own uid cannot write, so when a profile
  // directory is pinned HOME is pointed at it and the XDG_* home overrides are
  // removed.  With no pinned profile the environment passes through unchanged.
  // Exposed as a pure static helper so the contract is unit-testable.
  static std::vector<std::string> BuildChromeEnv(
      const ChromeProcessConfig& config, char* const* base);

  // Decide what to do with a `SingletonLock` whose symlink target is
  // `lock_target` (empty: no lock), given this host's name and a predicate
  // that says whether a pid is a live process.  Pure and unit-testable.  A
  // target whose pid does not parse is treated as stale: nothing running can
  // own a lock Chrome would not have written.
  static SingletonLockVerdict ClassifySingletonLock(
      std::string_view lock_target, std::string_view our_hostname,
      const std::function<bool(int)>& pid_alive);

  // Inspect `<user_data_dir>/SingletonLock` and, when the verdict is
  // kRemoveStale, unlink `SingletonLock`, `SingletonSocket` and
  // `SingletonCookie` (a missing entry is not an error).  `hostname` and
  // `pid_alive` default to this host and kill(pid, 0) when unset.  Returns the
  // verdict so the caller can log it; `removed` is set iff entries were
  // unlinked, and `error` names the first unlink that failed for a reason
  // other than the entry being absent.  A `SingletonLock` that is not a
  // symlink (Chrome never writes one; it can only be planted or restored) is
  // removed as stale too.  No-op on Windows (Chrome uses a different lock
  // there).
  //
  // A profile directory must not be shared between optimizer instances: a
  // lock owned by ANOTHER host is removed even if that host is live, by
  // design — the only way another hostname appears in the lock is a previous
  // container or a restored directory, and a lock this process cannot ask
  // about would otherwise block the browser forever.
  struct SingletonLockReport {
    SingletonLockVerdict verdict;
    bool removed = false;
    std::string error;
  };
  static SingletonLockReport ReconcileSingletonLock(
      const std::string& user_data_dir, std::string_view hostname = {},
      const std::function<bool(int)>& pid_alive = nullptr);

 private:
  static void OnProcessExit(uv_process_t* process, int64_t exit_status,
                            int term_signal);
  static void OnRssCheck(uv_timer_t* timer);
  static void OnKillTimeout(uv_timer_t* timer);

  void UpdateRss();
  void CleanupProcess();

  uv_loop_t* loop_;
  ChromeProcessConfig config_;

  uv_process_t process_{};
  bool running_ = false;

  // CDP pipe transport handles (owned by this class).
  uv_pipe_t read_pipe_{};   // Parent reads Chrome responses
  uv_pipe_t write_pipe_{};  // Parent sends Chrome commands
  bool pipes_initialized_ = false;

  std::unique_ptr<CdpClient> cdp_client_;

  // RSS monitoring timer.
  uv_timer_t rss_timer_{};
  bool rss_timer_active_ = false;
  int current_rss_mb_ = 0;

  // Kill timeout timer (for graceful shutdown).
  uv_timer_t kill_timer_{};
  bool kill_timer_active_ = false;

  // Page count for recycling.
  int page_count_ = 0;

  // Re-entrancy guard for Stop().
  bool stopping_ = false;
  bool stop_requested_ = false;

  ExitCallback exit_callback_;
};

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_BROWSER_CHROME_PROCESS_H_
