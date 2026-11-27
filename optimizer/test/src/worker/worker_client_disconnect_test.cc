// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// A peer that goes away must cost the daemon one connection, never the
// process.
//
// These tests run the real factory_worker binary as a child process, because
// what they pin is process-level: with SIGPIPE at its default disposition, a
// write to a socket or pipe whose other end has closed terminates the whole
// process.  An in-process Worker would share the test's own signal
// dispositions and prove nothing about the binary.
//
// Every signal this file sends goes through SignalWorker(), which only ever
// targets the live, unreaped child the fixture forked.  Run it in a container
// or another PID namespace of its own when trying it by hand.
//
// The child is started with SIGPIPE explicitly reset to the default, whatever
// the test runner did to it, so the binary's own start-up is what decides the
// outcome.  The ordering "the client is gone before the reply is written" is
// forced rather than raced: the worker is stopped (SIGSTOP), the client
// connects, sends its request and closes, and only then is the worker
// resumed -- the same shape as a health check that gave up on a daemon that
// was briefly stalled.

#ifndef _WIN32

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "src/proto/worker_ipc.h"
#include "test/test_util/tcp_client.h"
#include "test/test_util/temp_dir.h"

namespace pagespeed {
namespace {

using std::chrono::milliseconds;
using std::chrono::steady_clock;

// Generous: a sanitizer-instrumented worker on a loaded machine starts slowly.
constexpr auto kStartupBudget = std::chrono::seconds(120);
constexpr auto kServeBudget = std::chrono::seconds(60);
constexpr auto kExitBudget = std::chrono::seconds(120);

std::string DescribeWaitStatus(int status) {
  std::ostringstream os;
  if (WIFSIGNALED(status)) {
    os << "terminated by signal " << WTERMSIG(status) << " ("
       << strsignal(WTERMSIG(status)) << ")";
  } else if (WIFEXITED(status)) {
    os << "exited with status " << WEXITSTATUS(status);
  } else {
    os << "wait status " << status;
  }
  return os.str();
}

int ConnectUnix(const std::string& path) {
  struct sockaddr_un addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path)) return -1;
  std::memcpy(addr.sun_path, path.c_str(), path.size());
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  struct timeval tv;
  tv.tv_sec = 10;
  tv.tv_usec = 0;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) !=
      0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

std::string ReadAll(int fd) {
  std::string out;
  char buf[4096];
  while (true) {
    ssize_t n = test::SocketRead(fd, buf, sizeof(buf));
    if (n <= 0) break;
    out.append(buf, static_cast<size_t>(n));
  }
  return out;
}

// One request/response exchange over a unix socket.  Empty on any failure.
std::string UnixRoundTrip(const std::string& path, const std::string& request) {
  int fd = ConnectUnix(path);
  if (fd < 0) return "";
  if (!request.empty()) {
    (void)test::SocketWrite(fd, request.data(), request.size());
  }
  std::string reply = ReadAll(fd);
  ::close(fd);
  return reply;
}

int PickFreeTcpPort() {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return 0;
  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  socklen_t len = sizeof(addr);
  int port = 0;
  if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) ==
          0 &&
      ::getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr), &len) == 0) {
    port = ntohs(addr.sin_port);
  }
  ::close(fd);
  return port;
}

const char kHttpHealthRequest[] =
    "GET /v1/health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";

class WorkerClientDisconnectTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // The test process writes to sockets too, and inherits whatever the
    // runner set.  Ignore SIGPIPE here; the child gets the default back
    // explicitly in StartWorker().
    struct sigaction ignore{};
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    ASSERT_EQ(::sigaction(SIGPIPE, &ignore, &saved_sigpipe_), 0);

    // SignalWorker() relies on a terminated child staying a zombie until
    // this fixture reaps it, so its pid cannot be reused between the
    // waitpid() check and the kill().  An inherited SIG_IGN or SA_NOCLDWAIT
    // for SIGCHLD would make the kernel reap it on its own; pin the default
    // for the life of the fixture.
    struct sigaction sigchld_default{};
    sigchld_default.sa_handler = SIG_DFL;
    sigemptyset(&sigchld_default.sa_mask);
    sigchld_default.sa_flags = 0;
    ASSERT_EQ(::sigaction(SIGCHLD, &sigchld_default, &saved_sigchld_), 0);
    sigchld_pinned_ = true;

    const char* binary = std::getenv("FACTORY_WORKER");
    ASSERT_NE(binary, nullptr) << "FACTORY_WORKER is not set";
    binary_ = std::filesystem::absolute(binary).string();
    ASSERT_TRUE(std::filesystem::exists(binary_)) << binary_;

    socket_path_ = dir_.path() + "/w.sock";
    health_path_ = socket_path_ + ".health";
    mgmt_path_ = socket_path_ + ".mgmt";
    log_path_ = dir_.path() + "/worker.log";
    std::filesystem::create_directories(dir_.path() + "/cache");
  }

  void TearDown() override {
    // SIGKILL ends a stopped child too; reap it only if it was signalled.
    if (SignalWorker(SIGKILL)) {
      int status = 0;
      ::waitpid(pid_, &status, 0);
      pid_ = -1;
    }
    if (log_read_fd_ >= 0) ::close(log_read_fd_);
    if (sigchld_pinned_) ::sigaction(SIGCHLD, &saved_sigchld_, nullptr);
    ::sigaction(SIGPIPE, &saved_sigpipe_, nullptr);
    if (HasFailure()) {
      std::ifstream log(log_path_);
      std::stringstream contents;
      contents << log.rdbuf();
      ADD_FAILURE() << "worker output:\n" << contents.str();
    }
  }

  // Starts the binary and waits until its health socket answers.  With
  // `output_to_pipe`, stdout and stderr are a pipe whose read end this test
  // holds (log_read_fd_); otherwise they go to a file.
  void StartWorker(const std::vector<std::string>& extra_args,
                   bool output_to_pipe = false) {
    std::vector<std::string> args = {binary_,
                                     "--socket",
                                     socket_path_,
                                     "--cache-dir",
                                     dir_.path() + "/cache",
                                     "--cache-size",
                                     "67108864"};
    args.insert(args.end(), extra_args.begin(), extra_args.end());
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (std::string& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);

    int out_fd = -1;
    if (output_to_pipe) {
      int fds[2];
      ASSERT_EQ(::pipe(fds), 0);
      log_read_fd_ = fds[0];
      out_fd = fds[1];
    } else {
      out_fd = ::open(log_path_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
      ASSERT_GE(out_fd, 0);
    }

    pid_ = ::fork();
    ASSERT_GE(pid_, 0);
    if (pid_ == 0) {
      // Child.  Async-signal-safe calls only.  Default disposition AND
      // unblocked: a SIGPIPE blocked by an inherited mask would stay pending
      // and let the test pass without the fix.
      ::signal(SIGPIPE, SIG_DFL);
      sigset_t unblock;
      sigemptyset(&unblock);
      sigaddset(&unblock, SIGPIPE);
      ::sigprocmask(SIG_UNBLOCK, &unblock, nullptr);
      ::dup2(out_fd, STDOUT_FILENO);
      ::dup2(out_fd, STDERR_FILENO);
      if (log_read_fd_ >= 0) ::close(log_read_fd_);
      ::close(out_fd);
      ::execv(argv[0], argv.data());
      ::_exit(127);
    }
    ::close(out_fd);

    const auto deadline = steady_clock::now() + kStartupBudget;
    while (steady_clock::now() < deadline) {
      ASSERT_TRUE(StillRunning()) << "worker did not start: " << exit_note_;
      if (HealthAnswers()) return;
      std::this_thread::sleep_for(milliseconds(50));
    }
    FAIL() << "worker health socket did not answer within the start-up budget";
  }

  // The ONLY place this file sends a signal.  It refuses anything but the
  // worker child this fixture forked and has not yet reaped: never pid <= 0
  // (kill(-1, ...) reaches every process of the user, kill(0, ...) the whole
  // process group), never this process, and never a child that has already
  // terminated (its pid may be reused).  Returns true if the signal was sent.
  bool SignalWorker(int sig) {
    if (pid_ <= 0 || pid_ == ::getpid()) return false;
    int status = 0;
    const pid_t r = ::waitpid(pid_, &status, WNOHANG);
    if (r != 0) {
      // Reaped just now (it had exited) or not our child any more.
      if (r == pid_) exit_note_ = DescribeWaitStatus(status);
      pid_ = -1;
      return false;
    }
    return ::kill(pid_, sig) == 0;
  }

  // True while the child has not terminated.  When it has, exit_note_ says
  // how.
  bool StillRunning() {
    if (pid_ <= 0) return false;
    int status = 0;
    pid_t r = ::waitpid(pid_, &status, WNOHANG);
    if (r == 0) return true;
    exit_note_ = DescribeWaitStatus(status);
    pid_ = -1;
    return false;
  }

  bool HealthAnswers() {
    std::string reply = UnixRoundTrip(health_path_, "");
    return reply.starts_with("OK") || reply.starts_with("DEGRADED");
  }

  // Stop the worker and wait until it really is stopped, so nothing it does
  // can interleave with the client that follows.
  void Stall() {
    ASSERT_TRUE(SignalWorker(SIGSTOP)) << "the worker is gone: " << exit_note_;
    int status = 0;
    const pid_t r = ::waitpid(pid_, &status, WUNTRACED);
    if (r != pid_ || !WIFSTOPPED(status)) {
      // It terminated instead (and is reaped now), or is no longer ours:
      // forget the pid so nothing is ever sent to it again.
      if (r == pid_) exit_note_ = DescribeWaitStatus(status);
      pid_ = -1;
      FAIL() << "the worker did not stop: " << exit_note_;
    }
  }

  void Resume() {
    ASSERT_TRUE(SignalWorker(SIGCONT)) << "the worker is gone: " << exit_note_;
  }

  // The assertion every test ends on: the worker is still the same running
  // process and answers its health socket.
  void ExpectStillServing() {
    const auto deadline = steady_clock::now() + kServeBudget;
    while (steady_clock::now() < deadline) {
      ASSERT_TRUE(StillRunning())
          << "the worker did not survive the disconnect: " << exit_note_;
      if (HealthAnswers()) break;
      std::this_thread::sleep_for(milliseconds(20));
    }
    // Answering once is not yet proof: the reply to the vanished client may
    // still be in flight.  Give it time to fail, then look again.
    std::this_thread::sleep_for(milliseconds(300));
    ASSERT_TRUE(StillRunning())
        << "the worker did not survive the disconnect: " << exit_note_;
    EXPECT_TRUE(HealthAnswers());
    ASSERT_TRUE(StillRunning())
        << "the worker did not survive the disconnect: " << exit_note_;
  }

  // SIGTERM, then wait for the process to end.  Returns the wait status, or
  // -1 if it did not end in time.
  int TerminateAndWait() {
    if (!SignalWorker(SIGTERM)) return -1;
    const auto deadline = steady_clock::now() + kExitBudget;
    while (steady_clock::now() < deadline) {
      int status = 0;
      pid_t r = ::waitpid(pid_, &status, WNOHANG);
      if (r == pid_) {
        pid_ = -1;
        return status;
      }
      std::this_thread::sleep_for(milliseconds(20));
    }
    return -1;
  }

  test::TempDir dir_;
  std::string binary_;
  std::string socket_path_;
  std::string health_path_;
  std::string mgmt_path_;
  std::string log_path_;
  std::string exit_note_;
  pid_t pid_ = -1;
  int log_read_fd_ = -1;
  struct sigaction saved_sigpipe_{};
  struct sigaction saved_sigchld_{};
  bool sigchld_pinned_ = false;
};

// The reported case: a health check connects while the worker is stalled,
// gives up and closes; the worker then writes its status line to nobody.
TEST_F(WorkerClientDisconnectTest, HealthClientGoneBeforeReply) {
  StartWorker({});
  if (HasFatalFailure()) return;

  Stall();
  if (HasFatalFailure()) return;
  int fd = ConnectUnix(health_path_);
  ASSERT_GE(fd, 0);
  ::close(fd);
  Resume();
  if (HasFatalFailure()) return;

  ExpectStillServing();
  if (HasFatalFailure()) return;
}

TEST_F(WorkerClientDisconnectTest, ManagementSocketClientGoneBeforeReply) {
  StartWorker({});
  if (HasFatalFailure()) return;

  Stall();
  if (HasFatalFailure()) return;
  int fd = ConnectUnix(mgmt_path_);
  ASSERT_GE(fd, 0);
  const std::string command = "STATS\n";
  ASSERT_EQ(test::SocketWrite(fd, command.data(), command.size()),
            static_cast<ssize_t>(command.size()));
  ::close(fd);
  Resume();
  if (HasFatalFailure()) return;

  // The same listener keeps answering the next client.
  std::string stats = UnixRoundTrip(mgmt_path_, "STATS\n");
  EXPECT_NE(stats.find("\"status\":\"ok\""), std::string::npos) << stats;
  ExpectStillServing();
  if (HasFatalFailure()) return;
}

TEST_F(WorkerClientDisconnectTest, ApiSocketClientGoneBeforeReply) {
  const std::string api_path = dir_.path() + "/api.sock";
  StartWorker({"--api-socket", api_path});
  if (HasFatalFailure()) return;

  Stall();
  if (HasFatalFailure()) return;
  int fd = ConnectUnix(api_path);
  ASSERT_GE(fd, 0);
  ASSERT_EQ(
      test::SocketWrite(fd, kHttpHealthRequest, sizeof(kHttpHealthRequest) - 1),
      static_cast<ssize_t>(sizeof(kHttpHealthRequest) - 1));
  ::close(fd);
  Resume();
  if (HasFatalFailure()) return;

  std::string reply = UnixRoundTrip(api_path, kHttpHealthRequest);
  EXPECT_NE(reply.find("200"), std::string::npos) << reply;
  ExpectStillServing();
  if (HasFatalFailure()) return;
}

// Over TCP a short reply to a closed peer is absorbed by the kernel; it takes
// a reply larger than the socket buffer, so that the worker is still writing
// when the peer's reset arrives.  A console asset of several megabytes is
// such a reply, and needs no credential.
TEST_F(WorkerClientDisconnectTest, ApiTcpClientGoneDuringLargeReply) {
  const std::string console_dir = dir_.path() + "/console";
  std::filesystem::create_directories(console_dir);
  {
    std::ofstream index(console_dir + "/index.html");
    index << "<!doctype html><title>console</title>";
    std::ofstream big(console_dir + "/big.js", std::ios::binary);
    const std::string block(1 << 20, 'x');
    for (int i = 0; i < 16; ++i) big << block;
  }
  const int port = PickFreeTcpPort();
  ASSERT_GT(port, 0);
  StartWorker({"--api-port", std::to_string(port), "--api-no-auth",
               "--console-dir", console_dir});
  if (HasFatalFailure()) return;

  // The asset really is served in full to a client that stays.
  const std::string asset_request =
      "GET /console/big.js HTTP/1.1\r\nHost: localhost\r\n"
      "Connection: close\r\n\r\n";
  ASSERT_GT(test::SendRequest(port, asset_request).size(), size_t{16} << 20);

  Stall();
  if (HasFatalFailure()) return;
  int sock = test::ConnectTcp(port);
  ASSERT_GE(sock, 0);
  ASSERT_EQ(test::SocketWrite(sock, asset_request.data(), asset_request.size()),
            static_cast<ssize_t>(asset_request.size()));
  test::CloseSocket(sock);
  Resume();
  if (HasFatalFailure()) return;

  std::string reply = test::SendRequest(port, kHttpHealthRequest);
  EXPECT_NE(reply.find("200"), std::string::npos) << reply;
  ExpectStillServing();
  if (HasFatalFailure()) return;
}

// stdout and stderr are commonly a pipe to a log collector.  When the
// collector goes away, the next log line is a write to a pipe with no reader:
// that must not end the daemon, at run time or while it shuts down.
TEST_F(WorkerClientDisconnectTest, LogReaderGoneDoesNotEndTheWorker) {
  StartWorker({}, /*output_to_pipe=*/true);
  if (HasFatalFailure()) return;
  ::close(log_read_fd_);
  log_read_fd_ = -1;

  // Make the worker log: an accepted notification is announced at the
  // default log level.
  CacheNotification notification;
  notification.url = "http://example.com/page.html";
  notification.hostname = "example.com";
  notification.scheme = "http";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  const std::vector<char> frame = notification.Serialize();
  ASSERT_FALSE(frame.empty());
  int fd = ConnectUnix(socket_path_);
  ASSERT_GE(fd, 0);
  ASSERT_EQ(test::SocketWrite(fd, frame.data(), frame.size()),
            static_cast<ssize_t>(frame.size()));
  ::close(fd);

  ExpectStillServing();
  if (HasFatalFailure()) return;

  // Shutdown logs as well; it must still be an orderly exit, not a death by
  // signal.
  const int status = TerminateAndWait();
  ASSERT_NE(status, -1) << "worker did not exit after SIGTERM";
  EXPECT_TRUE(WIFEXITED(status)) << DescribeWaitStatus(status);
}

}  // namespace
}  // namespace pagespeed

#else  // _WIN32

#include "gtest/gtest.h"

// Windows has no SIGPIPE: a write to a closed pipe or socket there is an
// ordinary error return.
TEST(WorkerClientDisconnectTest, NotApplicableOnWindows) { SUCCEED(); }

#endif  // _WIN32
