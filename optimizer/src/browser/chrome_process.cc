// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Chrome Process Manager Implementation

#include "src/browser/chrome_process.h"

#ifdef _WIN32
#include <io.h>
#define access _access
#ifndef X_OK
#define X_OK 0  // Windows: check existence only
#endif
#ifndef SIGKILL
#define SIGKILL 9  // libuv maps this to TerminateProcess on Windows
#endif
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "src/worker/uv_helpers.h"

#ifdef _WIN32
#include <cstdlib>
#define PAGESPEED_ENVIRON _environ
#else
extern char** environ;
#define PAGESPEED_ENVIRON environ
#endif

#ifdef __linux__
#include <fstream>
#include <string>
#endif

namespace pagespeed {

namespace {
constexpr int kRssCheckIntervalMs = 5000;  // Check RSS every 5s.
constexpr int kKillTimeoutMs = 5000;       // SIGKILL after 5s.
}  // namespace

ChromeProcess::ChromeProcess(uv_loop_t* loop, ChromeProcessConfig config)
    : loop_(loop), config_(std::move(config)) {}

ChromeProcess::~ChromeProcess() {
  // All handles should have been closed by Stop()/CleanupProcess() or
  // by Worker's uv_walk cleanup.  Guard against handles that uv_walk
  // closed (setting running_/active_ flags stale) by checking
  // uv_is_closing before touching any handle.
  if (running_) {
    if (!IsClosing(&process_)) {
      uv_process_kill(&process_, SIGKILL);
    }
  }
  if (rss_timer_active_) {
    SafeTimerClose(&rss_timer_);
    rss_timer_active_ = false;
  }
  if (kill_timer_active_) {
    SafeTimerClose(&kill_timer_);
    kill_timer_active_ = false;
  }
  // CdpClient cleanup (stops reading, cancels pending).
  cdp_client_.reset();
  if (pipes_initialized_) {
    SafeClose(&read_pipe_);
    SafeClose(&write_pipe_);
    pipes_initialized_ = false;
  }
}

std::vector<std::string> ChromeProcess::BuildChromeArgs(
    const ChromeProcessConfig& config) {
  std::vector<std::string> arg_strings;
  arg_strings.push_back(config.chrome_path);
  if (config.headless) {
    arg_strings.push_back("--headless=new");
  }
  arg_strings.push_back("--remote-debugging-pipe");
  arg_strings.push_back("--disable-gpu");
  // CONDITIONAL.  This used to be unconditional, which made
  // every headless render unsandboxed.  It is emitted only when the operator
  // explicitly opted out (--browser-sandbox=off); in the default `require`
  // mode a browser that cannot be sandboxed is not started at all.  Asserted
  // by chrome_process_test's NoSandboxFlagOnlyWhenOptedOut.
  if (config.no_sandbox) {
    arg_strings.push_back("--no-sandbox");
  }
  arg_strings.push_back("--disable-dev-shm-usage");
  arg_strings.push_back("--disable-extensions");
  arg_strings.push_back("--disable-background-networking");
  arg_strings.push_back("--disable-default-apps");
  arg_strings.push_back("--disable-sync");
  arg_strings.push_back("--disable-translate");
  arg_strings.push_back("--metrics-recording-only");
  arg_strings.push_back("--no-first-run");
  if (!config.user_data_dir.empty()) {
    arg_strings.push_back(
        absl::StrCat("--user-data-dir=", config.user_data_dir));
  }
  arg_strings.push_back("--disable-field-trial-config");
  arg_strings.push_back("--disable-component-update");
  arg_strings.push_back("--disable-ipc-flooding-protection");
  arg_strings.push_back("--mute-audio");
  // Block all DNS resolution (SSRF defense — prevents DNS exfiltration even if
  // offline emulation is bypassed). MUST remain unconditional; asserted by
  // chrome_process_test's DeadResolverFlagAlwaysPresent.
  arg_strings.push_back("--host-resolver-rules=MAP * ~NOTFOUND");
  // Keep the render's WebRTC to proxied connections only.  MUST remain
  // unconditional; asserted by chrome_process_test and
  // tools/packaging/smoke-chrome-webrtc.sh against a real Chrome.
  arg_strings.push_back("--webrtc-ip-handling-policy=disable_non_proxied_udp");
  arg_strings.push_back("about:blank");
  return arg_strings;
}

std::vector<std::string> ChromeProcess::BuildChromeEnv(
    const ChromeProcessConfig& config, char* const* base) {
  std::vector<std::string> env;
  const bool pin_home = !config.user_data_dir.empty();
  for (char* const* e = base; e != nullptr && *e != nullptr; ++e) {
    std::string_view entry(*e);
    if (pin_home && (absl::StartsWith(entry, "HOME=") ||
                     absl::StartsWith(entry, "XDG_CONFIG_HOME=") ||
                     absl::StartsWith(entry, "XDG_CACHE_HOME=") ||
                     absl::StartsWith(entry, "XDG_DATA_HOME="))) {
      continue;
    }
    env.emplace_back(entry);
  }
  if (pin_home) {
    env.push_back(absl::StrCat("HOME=", config.user_data_dir));
  }
  return env;
}

SingletonLockVerdict ChromeProcess::ClassifySingletonLock(
    std::string_view lock_target, std::string_view our_hostname,
    const std::function<bool(int)>& pid_alive) {
  SingletonLockVerdict v;
  if (lock_target.empty()) return v;
  // `<hostname>-<pid>`: split at the LAST dash, hostnames may contain dashes.
  const size_t dash = lock_target.rfind('-');
  if (dash == std::string_view::npos) {
    v.owner_host = std::string(lock_target);
    v.action = SingletonLockAction::kRemoveStale;
    return v;
  }
  v.owner_host = std::string(lock_target.substr(0, dash));
  const std::string_view pid_str = lock_target.substr(dash + 1);
  int pid = 0;
  const bool pid_ok =
      !pid_str.empty() && absl::SimpleAtoi(pid_str, &pid) && pid > 0;
  if (!pid_ok) {
    v.action = SingletonLockAction::kRemoveStale;
    return v;
  }
  v.owner_pid = pid;
  if (v.owner_host != our_hostname || !pid_alive(pid)) {
    v.action = SingletonLockAction::kRemoveStale;
  } else {
    v.action = SingletonLockAction::kKeepLive;
  }
  return v;
}

ChromeProcess::SingletonLockReport ChromeProcess::ReconcileSingletonLock(
    const std::string& user_data_dir, std::string_view hostname,
    const std::function<bool(int)>& pid_alive) {
  SingletonLockReport report;
#ifdef _WIN32
  (void)user_data_dir;
  (void)hostname;
  (void)pid_alive;
  return report;
#else
  if (user_data_dir.empty()) return report;
  const std::string lock_path = absl::StrCat(user_data_dir, "/SingletonLock");
  char target_buf[1024];
  const ssize_t n =
      ::readlink(lock_path.c_str(), target_buf, sizeof(target_buf) - 1);
  std::string_view target;
  if (n < 0) {
    // ENOENT: no lock.  Anything else is not readable as Chrome's symlink.
    if (errno == ENOENT) return report;
    // EINVAL: a regular file (or directory) named SingletonLock.  Chrome never
    // writes one; it treats it as a lock from an old version and still ends
    // in PROFILE_IN_USE, so it can only be a planted or restored artefact and
    // it never clears itself.  Remove it as stale, with an empty owner.
    report.verdict.action = SingletonLockAction::kRemoveStale;
    report.verdict.owner_host = "(not a symlink)";
  } else {
    target = std::string_view(target_buf, static_cast<size_t>(n));
  }

  std::string our_host(hostname);
  if (our_host.empty()) {
    char host_buf[256];
    if (::gethostname(host_buf, sizeof(host_buf)) == 0) {
      host_buf[sizeof(host_buf) - 1] = '\0';
      our_host = host_buf;
    }
  }
  const std::function<bool(int)> alive = pid_alive ? pid_alive : [](int pid) {
    return ::kill(pid, 0) == 0 || errno != ESRCH;
  };
  if (n >= 0) report.verdict = ClassifySingletonLock(target, our_host, alive);
  if (report.verdict.action != SingletonLockAction::kRemoveStale) return report;

  for (const char* name :
       {"SingletonLock", "SingletonSocket", "SingletonCookie"}) {
    const std::string path = absl::StrCat(user_data_dir, "/", name);
    if (::unlink(path.c_str()) == 0) {
      report.removed = true;
    } else if (errno != ENOENT && report.error.empty()) {
      report.error = absl::StrCat(path, ": ", std::strerror(errno));
    }
  }
  return report;
#endif
}

absl::Status ChromeProcess::Start() {
  if (running_) {
    return absl::FailedPreconditionError("Chrome already running");
  }

  // Check binary exists before uv_spawn. On some libuv versions, uv_spawn's
  // error path asserts when cleaning up pre-initialized pipe handles.
  if (access(config_.chrome_path.c_str(), X_OK) != 0) {
    return absl::NotFoundError(absl::StrCat(
        "Chrome binary not found or not executable: ", config_.chrome_path));
  }

  page_count_ = 0;
  stopping_ = false;

  // Initialize pipe handles for CDP transport.
  int rc = uv_pipe_init(loop_, &read_pipe_, 0);
  if (rc != 0) {
    return absl::InternalError(
        absl::StrCat("uv_pipe_init (read) failed: ", uv_strerror(rc)));
  }
  rc = uv_pipe_init(loop_, &write_pipe_, 0);
  if (rc != 0) {
    uv_close(reinterpret_cast<uv_handle_t*>(&read_pipe_), nullptr);
    return absl::InternalError(
        absl::StrCat("uv_pipe_init (write) failed: ", uv_strerror(rc)));
  }
  pipes_initialized_ = true;

  // Set up stdio: stdin=ignore, stdout=ignore, stderr=ignore,
  // fd3=pipe(child reads), fd4=pipe(child writes).
  uv_stdio_container_t child_stdio[5];
  child_stdio[0].flags = UV_IGNORE;
  child_stdio[1].flags = UV_IGNORE;
  child_stdio[2].flags = UV_IGNORE;

  // FD 3: Chrome reads commands from parent.
  child_stdio[3].flags =
      static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_READABLE_PIPE);
  child_stdio[3].data.stream = reinterpret_cast<uv_stream_t*>(&write_pipe_);

  // FD 4: Chrome writes responses to parent.
  child_stdio[4].flags =
      static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
  child_stdio[4].data.stream = reinterpret_cast<uv_stream_t*>(&read_pipe_);

  // Build Chrome command-line arguments (single source of truth — testable).
  std::vector<std::string> arg_strings = BuildChromeArgs(config_);

  // Convert to char* array for uv_spawn.
  std::vector<char*> args;
  args.reserve(arg_strings.size() + 1);
  for (auto& s : arg_strings) {
    args.push_back(s.data());
  }
  args.push_back(nullptr);

  // Explicit environment: see BuildChromeEnv for why HOME is pinned.
  std::vector<std::string> env_strings =
      BuildChromeEnv(config_, PAGESPEED_ENVIRON);
  std::vector<char*> env;
  env.reserve(env_strings.size() + 1);
  for (auto& s : env_strings) {
    env.push_back(s.data());
  }
  env.push_back(nullptr);

  uv_process_options_t options{};
  options.exit_cb = OnProcessExit;
  options.file = config_.chrome_path.c_str();
  options.args = args.data();
  options.env = env.data();
  options.stdio_count = 5;
  options.stdio = child_stdio;

  process_.data = this;

  rc = uv_spawn(loop_, &process_, &options);
  if (rc != 0) {
    // Close pipe handles that were initialized.
    SafeClose(&read_pipe_);
    SafeClose(&write_pipe_);
    pipes_initialized_ = false;
    return absl::InternalError(
        absl::StrCat("Failed to spawn Chrome: ", uv_strerror(rc)));
  }

  running_ = true;

  // Create CDP client and attach to pipes.
  cdp_client_ = std::make_unique<CdpClient>(loop_);
  auto status = cdp_client_->AttachPipes(&read_pipe_, &write_pipe_);
  if (!status.ok()) {
    uv_process_kill(&process_, SIGKILL);
    SafeClose(&read_pipe_);
    SafeClose(&write_pipe_);
    pipes_initialized_ = false;
    running_ = false;
    return status;
  }

  // Start RSS monitoring timer (best-effort; non-fatal if it
  // fails — Chrome just won't be RSS-monitored).
  rc = uv_timer_init(loop_, &rss_timer_);
  if (rc == 0) {
    rss_timer_.data = this;
    rc = uv_timer_start(&rss_timer_, OnRssCheck, kRssCheckIntervalMs,
                        kRssCheckIntervalMs);
    rss_timer_active_ = (rc == 0);
  }

  return absl::OkStatus();
}

void ChromeProcess::Stop() {
  if (!running_ || stopping_) return;
  stopping_ = true;
  stop_requested_ = true;

  // Stop RSS monitoring.
  if (rss_timer_active_) {
    uv_timer_stop(&rss_timer_);
    rss_timer_active_ = false;
    uv_close(reinterpret_cast<uv_handle_t*>(&rss_timer_), nullptr);
  }

  // Cancel all pending CDP commands.
  if (cdp_client_) {
    cdp_client_->CancelAll(absl::CancelledError("Chrome stopping"));
  }

  // Send SIGTERM for graceful shutdown.
  uv_process_kill(&process_, SIGTERM);

  // Start kill timeout — SIGKILL if Chrome doesn't exit.
  int rc = uv_timer_init(loop_, &kill_timer_);
  if (rc == 0) {
    kill_timer_.data = this;
    rc = uv_timer_start(&kill_timer_, OnKillTimeout, kKillTimeoutMs, 0);
    if (rc == 0) {
      kill_timer_active_ = true;
    } else {
      // Timer failed — kill immediately.
      uv_close(reinterpret_cast<uv_handle_t*>(&kill_timer_), nullptr);
      uv_process_kill(&process_, SIGKILL);
    }
  } else {
    // Timer init failed — kill immediately.
    uv_process_kill(&process_, SIGKILL);
  }
}

void ChromeProcess::SetExitCallback(ExitCallback callback) {
  exit_callback_ = std::move(callback);
}

bool ChromeProcess::IncrementPageCount() {
  page_count_++;
  return config_.recycle_after_pages > 0 &&
         page_count_ >= config_.recycle_after_pages;
}

// static
void ChromeProcess::OnProcessExit(uv_process_t* process, int64_t exit_status,
                                  int term_signal) {
  if (!process->data) return;
  auto* self = static_cast<ChromeProcess*>(process->data);
  self->CleanupProcess();

  if (self->exit_callback_) {
    self->exit_callback_(exit_status, term_signal);
  }
}

// static
void ChromeProcess::OnRssCheck(uv_timer_t* timer) {
  if (!timer->data) return;
  auto* self = static_cast<ChromeProcess*>(timer->data);
  if (!self->running_) return;

  self->UpdateRss();

  if (self->config_.max_rss_mb > 0 &&
      self->current_rss_mb_ > self->config_.max_rss_mb) {
    self->Stop();
  }
}

// static
void ChromeProcess::OnKillTimeout(uv_timer_t* timer) {
  if (!timer->data) return;
  auto* self = static_cast<ChromeProcess*>(timer->data);
  if (self->running_) {
    uv_process_kill(&self->process_, SIGKILL);
  }
  self->kill_timer_active_ = false;
  SafeClose(timer);
}

void ChromeProcess::UpdateRss() {
#ifdef __linux__
  std::string path = absl::StrCat("/proc/", process_.pid, "/status");
  std::ifstream f(path);
  if (!f.is_open()) {
    current_rss_mb_ = 0;
    return;
  }
  std::string line;
  while (std::getline(f, line)) {
    if (line.starts_with("VmRSS:")) {
      long kb = 0;
      if (sscanf(line.c_str(), "VmRSS: %ld kB", &kb) == 1) {
        current_rss_mb_ = static_cast<int>(kb / 1024);
      }
      break;
    }
  }
#else
  current_rss_mb_ = 0;
#endif
}

void ChromeProcess::CleanupProcess() {
  running_ = false;
  stopping_ = false;

  // Close CDP client first (stops reading, cancels commands).
  if (cdp_client_) {
    cdp_client_->CancelAll(absl::UnavailableError("Chrome exited"));
  }

  // Close pipe handles.
  if (pipes_initialized_) {
    SafeClose(&read_pipe_);
    SafeClose(&write_pipe_);
    pipes_initialized_ = false;
  }

  // Stop RSS timer (may already be closed by Stop()).
  if (rss_timer_active_) {
    SafeTimerClose(&rss_timer_);
    rss_timer_active_ = false;
  }

  // Stop kill timer if active.
  if (kill_timer_active_) {
    SafeTimerClose(&kill_timer_);
    kill_timer_active_ = false;
  }

  // Close process handle.
  SafeClose(&process_);
}

}  // namespace pagespeed
