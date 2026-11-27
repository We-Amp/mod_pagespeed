// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "src/worker/windows_service.h"

#include <atomic>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#endif

namespace pagespeed {

ServiceArgs ParseServiceArgs(int argc, char* argv[]) {
  ServiceArgs out;
  for (int i = 0; i < argc; ++i) {
    std::string_view arg(argv[i]);
    if (i > 0 && arg == "--service") {
      out.service = true;
    } else if (i > 0 && arg == "--log-file") {
      if (i + 1 >= argc || std::string_view(argv[i + 1]).starts_with("--")) {
        out.ok = false;
        out.error = "--log-file needs a path";
        return out;
      }
      out.log_file = argv[++i];
    } else {
      out.rest.emplace_back(arg);
    }
  }
  if (out.ok && !out.log_file.empty() && !out.service) {
    out.ok = false;
    out.error = "--log-file is only valid with --service";
  }
  return out;
}

#ifdef _WIN32

namespace {

// Service-specific exit codes the manager records (event 7024) when the
// worker cannot start. The worker's own failures keep its exit code.
constexpr DWORD kExitLogFile = 90;

// One service per process (SERVICE_WIN32_OWN_PROCESS), so its state is
// process-wide. The control handler runs on the dispatcher's thread while the
// worker runs on the service's, so everything both touch is atomic or under
// g_status_mutex.
std::atomic<SERVICE_STATUS_HANDLE> g_status_handle{nullptr};
std::mutex g_status_mutex;
std::condition_variable g_status_changed;
SERVICE_STATUS g_status = {};
std::atomic<void (*)()> g_stop{nullptr};
int (*g_body)(int, char**) = nullptr;
std::vector<std::string>* g_args = nullptr;
std::string g_log_file;
int g_exit_code = 0;

// How long the manager waits between checkpoints of a pending state before
// it calls the service hung. The start heartbeat below keeps a slow
// initialization (a large cache being created) inside it.
constexpr DWORD kPendingWaitHintMs = 30000;
constexpr std::chrono::seconds kStartHeartbeat{10};

void ReportLocked(DWORD state, DWORD win32_exit_code, DWORD service_exit_code) {
  SERVICE_STATUS_HANDLE handle = g_status_handle.load();
  if (handle == nullptr) return;
  // Never leave STOPPED once reported: a late Stop must not revive it.
  if (g_status.dwCurrentState == SERVICE_STOPPED) return;
  g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  g_status.dwCurrentState = state;
  g_status.dwWin32ExitCode = win32_exit_code;
  g_status.dwServiceSpecificExitCode = service_exit_code;
  // PRESHUTDOWN rather than SHUTDOWN: at system shutdown it gives the worker
  // the time its graceful stop takes (drain, cache close) instead of the few
  // seconds a SHUTDOWN handler gets before the process is killed.
  g_status.dwControlsAccepted =
      state == SERVICE_RUNNING
          ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_PRESHUTDOWN
          : 0;
  const bool pending =
      state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING;
  g_status.dwWaitHint = pending ? kPendingWaitHintMs : 0;
  g_status.dwCheckPoint = pending ? g_status.dwCheckPoint + 1 : 0;
  SetServiceStatus(handle, &g_status);
  g_status_changed.notify_all();
}

void Report(DWORD state, DWORD win32_exit_code, DWORD service_exit_code) {
  std::lock_guard<std::mutex> lock(g_status_mutex);
  ReportLocked(state, win32_exit_code, service_exit_code);
}

DWORD WINAPI ControlHandler(DWORD control, DWORD /*event_type*/,
                            LPVOID /*event_data*/, LPVOID /*context*/) {
  switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_PRESHUTDOWN: {
      {
        std::lock_guard<std::mutex> lock(g_status_mutex);
        if (g_status.dwCurrentState != SERVICE_RUNNING) return NO_ERROR;
        ReportLocked(SERVICE_STOP_PENDING, NO_ERROR, 0);
      }
      void (*stop)() = g_stop.load();
      if (stop != nullptr) stop();
      return NO_ERROR;
    }
    case SERVICE_CONTROL_INTERROGATE:
      return NO_ERROR;
    default:
      return ERROR_CALL_NOT_IMPLEMENTED;
  }
}

// Sends the service's log to `path`: the C streams AND the process's Win32
// standard handles, so output that bypasses the C runtime (a Rust panic
// message, a child process that inherits them) lands there too. One file,
// opened once, shared for reading so an operator can follow it.
bool RedirectLog(const std::string& path) {
  if (std::freopen(path.c_str(), "a", stderr) == nullptr) return false;
  std::setvbuf(stderr, nullptr, _IONBF, 0);
  // stdout joins it on the same descriptor. A service has no console, so
  // stdout starts with no descriptor at all (fileno -2) and must be given
  // one before it can be pointed at the log; that part is best effort.
  std::fflush(stdout);
  if (_fileno(stdout) < 0) std::freopen("NUL", "w", stdout);
  if (_fileno(stdout) >= 0 && _dup2(_fileno(stderr), _fileno(stdout)) == 0) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
  }
  const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stderr)));
  if (handle != INVALID_HANDLE_VALUE) {
    SetStdHandle(STD_ERROR_HANDLE, handle);
    SetStdHandle(STD_OUTPUT_HANDLE, handle);
  }
  return true;
}

void WINAPI ServiceMain(DWORD /*argc*/, LPWSTR* /*argv*/) {
  // The name is ignored for an own-process service.
  SERVICE_STATUS_HANDLE handle =
      RegisterServiceCtrlHandlerExW(L"", ControlHandler, nullptr);
  if (handle == nullptr) {
    // Nothing can be reported without the handle, and the dispatcher would
    // wait for a STOPPED that never comes.
    ExitProcess(1);
  }
  g_status_handle.store(handle);
  Report(SERVICE_START_PENDING, NO_ERROR, 0);

  // Opened only now, connected to the manager, so that a log file that
  // cannot be opened is a reported stop with its own code rather than a
  // process that never answered.
  if (!g_log_file.empty() && !RedirectLog(g_log_file)) {
    g_exit_code = static_cast<int>(kExitLogFile);
    Report(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR, kExitLogFile);
    return;
  }

  // While the worker initializes, keep the manager's start wait alive.
  bool starting = true;
  std::thread heartbeat([&starting] {
    std::unique_lock<std::mutex> lock(g_status_mutex);
    while (starting && g_status.dwCurrentState == SERVICE_START_PENDING) {
      if (g_status_changed.wait_for(lock, kStartHeartbeat) ==
          std::cv_status::timeout) {
        if (starting && g_status.dwCurrentState == SERVICE_START_PENDING) {
          ReportLocked(SERVICE_START_PENDING, NO_ERROR, 0);
        }
      }
    }
  });

  // The worker's arguments are the process's own command line (the image
  // path the service was registered with), not the start parameters.
  std::vector<char*> argv;
  argv.reserve(g_args->size() + 1);
  for (std::string& arg : *g_args) argv.push_back(arg.data());
  argv.push_back(nullptr);
  g_exit_code = g_body(static_cast<int>(g_args->size()), argv.data());
  g_stop.store(nullptr);

  {
    std::lock_guard<std::mutex> lock(g_status_mutex);
    starting = false;
    g_status_changed.notify_all();
  }
  heartbeat.join();

  if (g_exit_code == 0) {
    Report(SERVICE_STOPPED, NO_ERROR, 0);
  } else {
    Report(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR,
           static_cast<DWORD>(g_exit_code));
  }
}

}  // namespace

bool RunAsServiceIfRequested(int argc, char* argv[], int (*body)(int, char**),
                             int* exit_code) {
  ServiceArgs parsed = ParseServiceArgs(argc, argv);
  if (!parsed.ok) {
    std::fprintf(stderr, "Error: %s\n", parsed.error.c_str());
    *exit_code = 1;
    return true;
  }
  if (!parsed.service) return false;

  g_body = body;
  g_args = &parsed.rest;
  g_log_file = parsed.log_file;
  SERVICE_TABLE_ENTRYW table[] = {
      {const_cast<LPWSTR>(L""), ServiceMain},
      {nullptr, nullptr},
  };
  if (!StartServiceCtrlDispatcherW(table)) {
    const DWORD error = GetLastError();
    if (error == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
      std::fprintf(stderr,
                   "Error: --service is for the Service Control Manager; "
                   "run the worker without it on a console\n");
    } else {
      std::fprintf(stderr, "Error: the service dispatcher failed (%lu)\n",
                   static_cast<unsigned long>(error));
    }
    *exit_code = 1;
    return true;
  }
  *exit_code = g_exit_code;
  return true;
}

void ServiceReportRunning() {
  if (g_status_handle.load() != nullptr) Report(SERVICE_RUNNING, NO_ERROR, 0);
}

void ServiceSetStopHandler(void (*stop)()) { g_stop.store(stop); }

#else  // !_WIN32

bool RunAsServiceIfRequested(int /*argc*/, char* /*argv*/[],
                             int (* /*body*/)(int, char**),
                             int* /*exit_code*/) {
  return false;
}

void ServiceReportRunning() {}

void ServiceSetStopHandler(void (* /*stop*/)()) {}

#endif  // _WIN32

}  // namespace pagespeed
