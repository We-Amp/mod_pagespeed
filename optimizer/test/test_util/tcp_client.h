// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef TEST_TEST_UTIL_TCP_CLIENT_H_
#define TEST_TEST_UTIL_TCP_CLIENT_H_

#include <cerrno>
#include <cstring>
#include <string>

#include "lib/base/string_util.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using ssize_t = intptr_t;
#define SHUT_WR SD_SEND
#define SHUT_RDWR SD_BOTH
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace pagespeed::test {

#ifdef _WIN32
// RAII Winsock initializer. Create one instance per test suite.
struct WinsockInit {
  WinsockInit() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
  }
  ~WinsockInit() { WSACleanup(); }
};

inline void CloseSocket(int sock) { closesocket(sock); }
#else
inline void CloseSocket(int sock) { close(sock); }
#endif

// A signal delivered to the test process interrupts a blocking socket call
// with EINTR before any byte moves; that is not an answer from the server, so
// the POSIX calls below retry it.
inline ssize_t SocketWrite(int sock, const void* data, size_t len) {
#ifdef _WIN32
  return send(sock, static_cast<const char*>(data), static_cast<int>(len), 0);
#else
  ssize_t n;
  do {
    n = write(sock, data, len);
  } while (n < 0 && errno == EINTR);
  return n;
#endif
}

inline ssize_t SocketRead(int sock, void* buf, size_t len) {
#ifdef _WIN32
  return recv(sock, static_cast<char*>(buf), static_cast<int>(len), 0);
#else
  ssize_t n;
  do {
    n = read(sock, buf, len);
  } while (n < 0 && errno == EINTR);
  return n;
#endif
}

// Connect to localhost:port via TCP, send request, read full response.
// Returns the response body, or empty string on failure.
// timeout_sec sets SO_RCVTIMEO (0 = no timeout).
inline std::string SendRequest(int port, const std::string& request,
                               int timeout_sec = 10) {
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) return "";

  if (timeout_sec > 0) {
#ifdef _WIN32
    DWORD tv = timeout_sec * 1000;  // milliseconds
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    struct timeval tv;
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
  }

  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  if (connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) <
      0) {
    CloseSocket(sock);
    return "";
  }

  (void)SocketWrite(sock, request.data(), request.size());
#ifdef _WIN32
  shutdown(sock, SD_SEND);
#else
  shutdown(sock, SHUT_WR);
#endif

  std::string response;
  char buf[4096];
  while (true) {
    ssize_t n = SocketRead(sock, buf, sizeof(buf));
    if (n <= 0) break;
    response.append(buf, static_cast<size_t>(n));
  }

  CloseSocket(sock);
  return response;
}

// Variant that keeps connection open for reading (no shutdown after send).
inline std::string SendRequestKeepOpen(int port, const std::string& request,
                                       int timeout_sec = 10) {
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) return "";

  if (timeout_sec > 0) {
#ifdef _WIN32
    DWORD tv = timeout_sec * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    struct timeval tv;
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
  }

  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  if (connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) <
      0) {
    CloseSocket(sock);
    return "";
  }

  (void)SocketWrite(sock, request.data(), request.size());

  std::string response;
  char buf[4096];
  while (true) {
    ssize_t n = SocketRead(sock, buf, sizeof(buf));
    if (n <= 0) break;
    response.append(buf, static_cast<size_t>(n));
  }

  CloseSocket(sock);
  return response;
}

// Raw socket handle for tests that need more control (e.g., partial reads).
// Caller is responsible for calling CloseSocket().
inline int ConnectTcp(int port, int timeout_sec = 10) {
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) return -1;

  if (timeout_sec > 0) {
#ifdef _WIN32
    DWORD tv = timeout_sec * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    struct timeval tv;
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
  }

  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  if (connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) <
      0) {
    CloseSocket(sock);
    return -1;
  }

  return sock;
}

inline void SocketShutdown(int sock, int how) { shutdown(sock, how); }

// Inject "Connection: close" header into an HTTP request string if no
// Connection header is already present.  This ensures test helpers that
// read until EOF work correctly with keep-alive-capable servers.
inline std::string InjectConnectionClose(const std::string& request) {
  // Check if a Connection header is already present (case-insensitive).
  std::string lower = request;
  net_instaweb::AsciiToLowerInPlace(lower);
  if (lower.find("\nconnection:") != std::string::npos) {
    return request;  // Already has a Connection header.
  }
  // Find the end-of-headers marker and inject before it.
  auto pos = request.find("\r\n\r\n");
  if (pos == std::string::npos) {
    return request;  // Malformed request, return as-is.
  }
  return request.substr(0, pos) + "\r\nConnection: close\r\n\r\n" +
         request.substr(pos + 4);
}

// Inject "X-Requested-With: XMLHttpRequest" into a POST/PATCH request to a
// /v1 path if no X-Requested-With header is already present.  The
// HTTP server CSRF-gates state-changing /v1 requests when no API token is
// configured (the default in tests), requiring this header — which the
// workbench SPA always sends.  Test helpers inject it so handler-behavior
// tests reach the handler past the CSRF gate, mirroring the real caller.
inline std::string InjectCsrfHeaderForV1Mutations(const std::string& request) {
  bool is_mutation =
      request.starts_with("POST ") || request.starts_with("PATCH ");
  if (!is_mutation) return request;

  // Only /v1/ paths.
  if (request.find("/v1/") == std::string::npos) return request;

  std::string lower = request;
  net_instaweb::AsciiToLowerInPlace(lower);
  if (lower.find("\nx-requested-with:") != std::string::npos) {
    return request;  // Already has the header.
  }
  auto pos = request.find("\r\n\r\n");
  if (pos == std::string::npos) {
    return request;  // Malformed request, return as-is.
  }
  return request.substr(0, pos) +
         "\r\nX-Requested-With: XMLHttpRequest\r\n\r\n" +
         request.substr(pos + 4);
}

}  // namespace pagespeed::test

#endif  // TEST_TEST_UTIL_TCP_CLIENT_H_
