// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Running the worker as a Windows service.
//
// The Service Control Manager starts a service process and expects it to
// connect back within thirty seconds, report its state as it starts, and
// stop when asked. A console program does none of that, so without this the
// worker could only be run by hand or killed. With `--service` on the
// command line the worker hands its whole ordinary startup to the service
// dispatcher, reports RUNNING once it is initialized, and turns the
// manager's Stop (and the system's shutdown notice) into the same graceful
// shutdown SIGINT/SIGTERM start. Every other option means what it means on
// the console.
//
// A service has no console, so `--log-file PATH` (service mode only) sends
// the worker's log to a file, opened for appending once the service has
// connected to the manager: both C streams and the Win32 standard handles.
// A file that cannot be opened stops the service with service-specific exit
// code 90. Without the option the log is discarded.
//
// At system shutdown the service takes the pre-shutdown notice, which gives
// the graceful stop its time; a Stop that arrives while the worker is still
// starting is refused by the manager until the worker reports RUNNING.
//
// Off Windows `--service` and `--log-file` are not options at all.

#ifndef SRC_WORKER_WINDOWS_SERVICE_H_
#define SRC_WORKER_WINDOWS_SERVICE_H_

#include <string>
#include <vector>

namespace pagespeed {

// The two service-mode options, taken out of a command line. Pure, so that
// it can be tested on every platform.
struct ServiceArgs {
  bool service = false;
  std::string log_file;
  // argv without the two options, argv[0] first.
  std::vector<std::string> rest;
  // False when the command line is malformed; `error` says why.
  bool ok = true;
  std::string error;
};
ServiceArgs ParseServiceArgs(int argc, char* argv[]);

// Windows: when argv carries `--service`, runs `body` with the remaining
// arguments under the service dispatcher, sets *exit_code and returns true.
// Otherwise, and on every other platform, returns false and does nothing.
bool RunAsServiceIfRequested(int argc, char* argv[], int (*body)(int, char**),
                             int* exit_code);

// Called by the worker once it has initialized. In service mode this reports
// RUNNING and starts accepting Stop; otherwise it does nothing.
void ServiceReportRunning();

// The function the service's Stop calls. It must be safe to call from
// another thread (the worker's SignalHandler is).
void ServiceSetStopHandler(void (*stop)());

}  // namespace pagespeed

#endif  // SRC_WORKER_WINDOWS_SERVICE_H_
