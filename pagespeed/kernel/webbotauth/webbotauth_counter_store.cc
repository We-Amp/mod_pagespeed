// Copyright 2026 We-Amp B.V.
// Licensed under the Apache License, Version 2.0 (the "License").

#include "pagespeed/kernel/webbotauth/webbotauth_counter_store.h"

#ifdef _WIN32
#include <windows.h>

#include <cstdio>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <cstring>
#include <random>
#include <string>
#include <string_view>

namespace net_instaweb {
namespace webbotauth {

namespace {

// Cross-platform relaxed atomic add on a counter living in the shared mapping.
// std::atomic_ref (C++20) compiles to the same lock-free RMW as the GCC/Clang
// __atomic_fetch_add builtin but is also supported by MSVC. The 64-bit counters
// are naturally aligned, satisfying atomic_ref's alignment requirement.
inline void RelaxedAdd(uint64_t& field, uint64_t value) {
  std::atomic_ref<uint64_t>(field).fetch_add(value, std::memory_order_relaxed);
}

// Fill boot_id with a random 16-byte instance identity + the counting-since day
// on a fresh file. boot_id is a non-crypto instance identifier, so a weak
// fallback is fine: never let random_device (which can throw when no entropy
// source is available) propagate out of startup.
void MintIdentity(WebBotAuthCounterStats* stats) {
  try {
    std::random_device rd;
    for (unsigned char& b : stats->boot_id) {
      b = static_cast<unsigned char>(rd() & 0xFF);
    }
  } catch (...) {
    // Portable weak fallback: mix wall-clock time with this mapping's address
    // (distinct per process) -- no crypto strength needed for an instance id.
    std::mt19937_64 gen(
        static_cast<uint64_t>(
            std::chrono::system_clock::now().time_since_epoch().count()) ^
        reinterpret_cast<uintptr_t>(stats));
    for (unsigned char& b : stats->boot_id) {
      b = static_cast<unsigned char>(gen() & 0xFF);
    }
  }
  // RFC-4122 version-4 variant bits so it renders as a valid random UUID.
  stats->boot_id[6] = (stats->boot_id[6] & 0x0F) | 0x40;
  stats->boot_id[8] = (stats->boot_id[8] & 0x3F) | 0x80;
  stats->counting_since_unix_day = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::hours>(
          std::chrono::system_clock::now().time_since_epoch())
          .count() /
      24);
}

}  // namespace

std::string WebBotAuthCounterPath(const std::string& dir) {
  std::string d = dir;
  if (!d.empty() && (d.back() == '/' || d.back() == '\\')) {
    d.pop_back();
  }
  return d + "/.pagespeed-webbotauth-counter";
}

WebBotAuthCounterStats* OpenWebBotAuthCounter(const std::string& path) {
#ifdef _WIN32
  HANDLE hFile = CreateFileA(
      path.c_str(), GENERIC_READ | GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (hFile == INVALID_HANDLE_VALUE) return nullptr;
  LARGE_INTEGER fileSize;
  if (!GetFileSizeEx(hFile, &fileSize) ||
      static_cast<size_t>(fileSize.QuadPart) <
          WebBotAuthCounterStats::kFileSize) {
    CloseHandle(hFile);
    return nullptr;
  }
  HANDLE hMap = CreateFileMappingA(
      hFile, nullptr, PAGE_READWRITE, 0,
      static_cast<DWORD>(WebBotAuthCounterStats::kFileSize), nullptr);
  CloseHandle(hFile);
  if (!hMap) return nullptr;
  void* mem = MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0,
                            WebBotAuthCounterStats::kFileSize);
  CloseHandle(hMap);
  if (!mem) return nullptr;
#else
  int fd = open(path.c_str(), O_RDWR | O_NOFOLLOW);
  if (fd < 0) return nullptr;
  struct stat st;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
      static_cast<size_t>(st.st_size) < WebBotAuthCounterStats::kFileSize) {
    close(fd);
    return nullptr;
  }
  void* mem = mmap(nullptr, WebBotAuthCounterStats::kFileSize,
                   PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (mem == MAP_FAILED) return nullptr;
#endif

  auto* stats = static_cast<WebBotAuthCounterStats*>(mem);
  // Acquire-load magic (and version) to pair with the create path's release
  // store of magic (published LAST): a reader can never observe magic==kMagic
  // while boot_id / counting_since are still being minted by a racing creator.
  if (std::atomic_ref<uint32_t>(stats->magic).load(std::memory_order_acquire) !=
          WebBotAuthCounterStats::kMagic ||
      std::atomic_ref<uint32_t>(stats->version)
              .load(std::memory_order_acquire) !=
          WebBotAuthCounterStats::kVersion) {
    CloseWebBotAuthCounter(stats);
    return nullptr;
  }
  return stats;
}

WebBotAuthCounterStats* OpenOrCreateWebBotAuthCounter(const std::string& path) {
  // Prefer a valid existing file: PRESERVE its identity and counters (they
  // persist across worker restart / config reload; they reset only when the
  // file is deleted or the version changes).
  WebBotAuthCounterStats* existing = OpenWebBotAuthCounter(path);
  if (existing != nullptr) return existing;

  // Absent, OR present-but-invalid (wrong magic/version/size, or corrupt). In
  // BOTH cases self-heal: unlink any stale file, then create fresh. This is how
  // a VERSION BUMP resets the counter (and re-mints boot_id), honoring the
  // documented "reset on file deletion OR version bump" invariant -- without it
  // a stale/old-version file would make every Record* a silent no-op forever.
  // Best-effort unlink (ignore ENOENT). O_EXCL then fails safely if a concurrent
  // creator raced us (we fall back to Open); O_NOFOLLOW rejects a symlink
  // swapped in for the just-unlinked path.
#ifdef _WIN32
  DeleteFileA(path.c_str());
  HANDLE hFile = CreateFileA(
      path.c_str(), GENERIC_READ | GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_NEW,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (hFile == INVALID_HANDLE_VALUE) {
    // A concurrent creator won (or it already existed): retry Open.
    return OpenWebBotAuthCounter(path);
  }
  LARGE_INTEGER sz;
  sz.QuadPart = WebBotAuthCounterStats::kFileSize;
  if (!SetFilePointerEx(hFile, sz, nullptr, FILE_BEGIN) ||
      !SetEndOfFile(hFile)) {
    CloseHandle(hFile);
    return nullptr;
  }
  HANDLE hMap = CreateFileMappingA(
      hFile, nullptr, PAGE_READWRITE, 0,
      static_cast<DWORD>(WebBotAuthCounterStats::kFileSize), nullptr);
  CloseHandle(hFile);
  if (!hMap) return nullptr;
  void* mem = MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0,
                            WebBotAuthCounterStats::kFileSize);
  CloseHandle(hMap);
  if (!mem) return nullptr;
#else
  unlink(path.c_str());
  // Owner-only 0600 (the create mode is authoritative -- no fchmod widening).
  // mpp is nginx-only: the nginx MASTER creates this file ONCE (ps_init_module,
  // before fork) and workers inherit the MAP_SHARED mapping in the same,
  // same-UID process tree -- they never reopen it. 0600 closes a local hole a
  // world-readable file would open: any local user could read the token-gated
  // exact counts off disk AND inject fake signer slots that surface on the
  // UNAUTHENTICATED coarse badge (aggregator poisoning). (2.0 uses 0666 only
  // because a SEPARATE worker process shares the file with nginx; that rationale
  // does NOT apply here -- do not "restore 2.0 parity" by widening this.)
  int fd = open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
  if (fd < 0) {
    // EEXIST (a concurrent creator won) or a symlink (O_NOFOLLOW): retry Open,
    // which validates magic/version and fails closed on anything unexpected.
    return OpenWebBotAuthCounter(path);
  }
  if (ftruncate(fd, WebBotAuthCounterStats::kFileSize) != 0) {
    close(fd);
    return nullptr;
  }
  void* mem = mmap(nullptr, WebBotAuthCounterStats::kFileSize,
                   PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (mem == MAP_FAILED) return nullptr;
#endif

  std::memset(mem, 0, WebBotAuthCounterStats::kFileSize);
  auto* stats = static_cast<WebBotAuthCounterStats*>(mem);
  MintIdentity(stats);
  // Publish the header LAST: OpenWebBotAuthCounter validates magic+version, so a
  // racing reader either sees the fully-initialized file or (magic still 0)
  // rejects it and retries.
  stats->version = WebBotAuthCounterStats::kVersion;
  std::atomic_ref<uint32_t>(stats->magic)
      .store(WebBotAuthCounterStats::kMagic, std::memory_order_release);
  return stats;
}

void CloseWebBotAuthCounter(WebBotAuthCounterStats* stats) {
  if (stats != nullptr) {
#ifdef _WIN32
    UnmapViewOfFile(stats);
#else
    munmap(stats, WebBotAuthCounterStats::kFileSize);
#endif
  }
}

uint64_t HashWebBotAuthKeyid(std::string_view keyid) {
  // Plain UNSALTED FNV-1a-64 over the raw keyid bytes. Intentionally
  // reproducible (no secret salt) so a downstream consumer can recognise a
  // known keyid; see the header for the rationale.
  uint64_t h = 0xcbf29ce484222325ULL;
  for (unsigned char c : keyid) {
    h ^= c;
    h *= 0x100000001b3ULL;
  }
  // 0-remap (NOT a salt): the one input that would hash to 0 is remapped to a
  // fixed non-zero sentinel so a real keyid never collides with "empty slot".
  if (h == 0) h = 0x9e3779b97f4a7c15ULL;
  return h;
}

void RecordWebBotAuthSigned(WebBotAuthCounterStats* stats, bool verified) {
  if (stats == nullptr) return;
  if (verified) {
    RelaxedAdd(stats->verified_total, 1);
  } else {
    RelaxedAdd(stats->invalid_total, 1);
  }
}

void RecordWebBotAuthOtherSignature(WebBotAuthCounterStats* stats) {
  if (stats == nullptr) return;
  RelaxedAdd(stats->other_total, 1);
}

void RecordWebBotAuthVerifiedSigner(WebBotAuthCounterStats* stats,
                                    std::string_view keyid,
                                    uint64_t latency_us) {
  if (stats == nullptr) return;

  // Bucket the verify latency (each request lands in exactly one bucket).
  if (latency_us < 100) {
    RelaxedAdd(stats->verify_latency_lt100us, 1);
  } else if (latency_us < 1000) {
    RelaxedAdd(stats->verify_latency_lt1ms, 1);
  } else if (latency_us < 10000) {
    RelaxedAdd(stats->verify_latency_lt10ms, 1);
  } else {
    RelaxedAdd(stats->verify_latency_ge10ms, 1);
  }

  const uint64_t h = HashWebBotAuthKeyid(keyid);
  // Find-or-claim a slot lock-free. CAS an empty (kid_hash==0) slot to `h`, then
  // increment. Cross-process-safe: every worker maps the same file MAP_SHARED
  // and every access here is via std::atomic_ref.
  for (auto& slot : stats->signers) {
    std::atomic_ref<uint64_t> kid(slot.kid_hash);
    uint64_t cur = kid.load(std::memory_order_relaxed);
    if (cur == h) {
      RelaxedAdd(slot.count, 1);
      return;
    }
    if (cur == 0) {
      uint64_t expected = 0;
      if (kid.compare_exchange_strong(expected, h, std::memory_order_relaxed)) {
        RelaxedAdd(slot.count, 1);
        return;
      }
      // Lost the race: another writer claimed this slot. If it claimed OUR
      // keyid, count it here; otherwise keep scanning for a later slot.
      if (expected == h) {
        RelaxedAdd(slot.count, 1);
        return;
      }
    }
  }
  // All slots taken by other keyids.
  RelaxedAdd(stats->other_verified_bots, 1);
}

}  // namespace webbotauth
}  // namespace net_instaweb
