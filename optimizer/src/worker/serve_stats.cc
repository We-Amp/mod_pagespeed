// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "src/worker/serve_stats.h"

#ifdef _WIN32
#include <windows.h>

#include "src/worker/posix_compat.h"
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "lib/base/message_handler.h"
#include "lib/classify/capability_mask.h"

namespace pagespeed {

// Atomically zero every counter field (relaxed), preserving magic+version.
// A separate process (nginx) concurrently increments these same fields via
// std::atomic_ref on the shared mapping, so a plain memset here would be a
// data race (UB: torn / lost writes). Storing each field atomically matches
// the increment side's access.
static void ZeroCounters(ServeStats* s) {
  for (uint64_t* field :
       {&s->html_original_bytes, &s->html_optimized_bytes,
        &s->css_original_bytes, &s->css_optimized_bytes, &s->js_original_bytes,
        &s->js_optimized_bytes, &s->image_original_bytes,
        &s->image_optimized_bytes, &s->html_optimized_hits,
        &s->css_optimized_hits, &s->js_optimized_hits, &s->image_optimized_hits,
        &s->svg_optimized_hits, &s->webbotauth_signed_verified,
        &s->webbotauth_signed_invalid, &s->webbotauth_other_signature,
        &s->rslcap_authorized, &s->rslcap_denied_401, &s->rslcap_denied_402,
        // v6 opt-in counter fields (NOT boot_id, NOT counting_since_unix_day —
        // those are identity/epoch, minted only in the create-fresh path).
        &s->webbotauth_other_verified_bots,
        &s->webbotauth_verify_latency_lt100us,
        &s->webbotauth_verify_latency_lt1ms,
        &s->webbotauth_verify_latency_lt10ms,
        &s->webbotauth_verify_latency_ge10ms,
        // v7 zero-copy serve-barrier + bounded-stale counters.
        &s->zerocopy_torn_aborts, &s->zerocopy_proactive_copyouts,
        &s->zerocopy_copy_then_verify_discards, &s->swr_coalesced_serves,
        &s->stale_if_error_serves,
        // v8 serve-class counters + the saturation accumulator.  NOT
        // worker_pool_threads: it is configuration, re-stamped by the worker
        // after this runs (see the field comment).
        &s->serve_optimized_total, &s->serve_original_cold_total,
        &s->serve_original_pending_total, &s->serve_original_declined_total,
        &s->serve_original_skew_total, &s->notify_suppressed_total,
        &s->serve_class_unrecognized_total, &s->serve_flags_unrecognized_total,
        &s->saturation_sample_accum, &s->saturation_sample_count,
        // v11 response-header sidecar counters.
        &s->headers_sidecar_fall_through, &s->headers_sidecar_never_optimized,
        &s->headers_sidecar_replayed, &s->headers_sidecar_missing,
        &s->headers_sidecar_write_failed, &s->headers_sidecar_stored}) {
    std::atomic_ref<uint64_t>(*field).store(0, std::memory_order_relaxed);
  }
  // v9 by-transfer-encoding serve counters: zeroed with the counters above,
  // same relaxed-atomic discipline.
  for (auto& row : s->serve_hits_by_encoding) {
    for (auto& field : row) {
      std::atomic_ref<uint64_t>(field).store(0, std::memory_order_relaxed);
    }
  }
  for (auto& row : s->serve_bytes_by_encoding) {
    for (auto& field : row) {
      std::atomic_ref<uint64_t>(field).store(0, std::memory_order_relaxed);
    }
  }
  // Per-signer slots: zero the hash FIRST (releases the slot), then the count.
  // Doing it in this order shrinks the restart-reset window in which a
  // concurrent RecordWebBotAuthVerifiedSigner increment could be orphaned: once
  // kid_hash is 0 the slot is free to be re-claimed, so a stray increment lands
  // in a freshly-claimed slot rather than on a stale count we then zero. A
  // single count lost during this restart-reset window is acceptable (the reset
  // is by design and cosmetic at worst — never corruption or a torn read).
  for (auto& slot : s->webbotauth_signers) {
    std::atomic_ref<uint64_t>(slot.kid_hash)
        .store(0, std::memory_order_relaxed);
    std::atomic_ref<uint64_t>(slot.count).store(0, std::memory_order_relaxed);
  }
  // v8 high-water mark: a counter-like max, reset with the counters.
  std::atomic_ref<uint32_t>(s->saturation_hwm)
      .store(0, std::memory_order_relaxed);
  // v10 host slots: unpublish the name (ready) before freeing the slot
  // (key_hash), then the counters and the name; then the other triple.  A
  // claim racing this can leave one slot unnamed or with a torn name (the
  // reader's hash check sends it to other), and a counter add racing it can
  // leave a few counts that the slot's next claimer inherits -- cosmetic
  // until the next restart.
  for (auto& slot : s->serve_hosts) {
    std::atomic_ref<uint64_t>(slot.ready).store(0, std::memory_order_relaxed);
    std::atomic_ref<uint64_t>(slot.key_hash)
        .store(0, std::memory_order_relaxed);
    std::atomic_ref<uint64_t>(slot.hits).store(0, std::memory_order_relaxed);
    std::atomic_ref<uint64_t>(slot.original_bytes)
        .store(0, std::memory_order_relaxed);
    std::atomic_ref<uint64_t>(slot.optimized_bytes)
        .store(0, std::memory_order_relaxed);
    std::memset(slot.name, 0, sizeof(slot.name));
  }
  for (uint64_t* field :
       {&s->serve_hosts_other_hits, &s->serve_hosts_other_original_bytes,
        &s->serve_hosts_other_optimized_bytes}) {
    std::atomic_ref<uint64_t>(*field).store(0, std::memory_order_relaxed);
  }
  // Split block: counters, zeroed with the rest.
  for (ServeStats::SplitCounters* split :
       {&s->split_html, &s->split_css, &s->split_js, &s->split_image}) {
    for (uint64_t* field :
         {&split->original_bytes, &split->compressed_original_bytes,
          &split->optimized_bytes}) {
      std::atomic_ref<uint64_t>(*field).store(0, std::memory_order_relaxed);
    }
  }
  // Busy-read block: counters, zeroed with the rest.
  for (uint64_t* field : {&s->cache_busy_reads, &s->cache_busy_retry_hits}) {
    std::atomic_ref<uint64_t>(*field).store(0, std::memory_order_relaxed);
  }
}

// Read the version word of an existing serve-stats file without mapping it.
// Used only to name the version in the skew warning, so a short/absent/
// unreadable file simply yields no version rather than an error.
static bool PeekFileVersion(const std::string& path, uint32_t* magic_out,
                            uint32_t* version_out) {
  FILE* f = fopen(path.c_str(), "rb");
  if (f == nullptr) return false;
  uint32_t header[2] = {0, 0};
  const size_t read = fread(header, sizeof(header[0]), 2, f);
  fclose(f);
  if (read != 2) return false;
  *magic_out = header[0];
  *version_out = header[1];
  return true;
}

std::string ServeStatsPath(const std::string& cache_path) {
  std::filesystem::path p(cache_path);
  return (p.parent_path() / ".pagespeed-serve-stats").generic_string();
}

ServeStats* CreateServeStats(const std::string& path, MessageHandler* handler) {
  // Try to reuse the existing file so that nginx's mmap stays valid across
  // worker restarts.  If the file exists with correct magic/version, just
  // zero the counters and return it.
  ServeStats* existing = OpenServeStats(path);
  if (existing != nullptr) {
    // Zero only the counter fields, preserving magic+version. Use atomic
    // stores (not memset) because nginx may be incrementing them concurrently.
    ZeroCounters(existing);
    return existing;
  }

  // File missing or invalid — create fresh.  A file that is present but
  // carries a different version is the one case worth announcing: the counters
  // it holds are about to be discarded, and any peer built against that
  // version will stop recording the moment this file is replaced.  Both are
  // silent downstream (flat lines, no error), so say it here, once, naming
  // both versions so the skew window is identifiable in a log.
  if (handler != nullptr) {
    uint32_t existing_magic = 0;
    uint32_t existing_version = 0;
    if (PeekFileVersion(path, &existing_magic, &existing_version) &&
        existing_magic == ServeStats::kMagic &&
        existing_version != ServeStats::kVersion) {
      handler->Warning(
          "serve-stats file %s is version %u, this build writes version %u — "
          "recreating it: its counters restart at zero, and any peer still on "
          "version %u stops recording until it is upgraded",
          path.c_str(), existing_version, ServeStats::kVersion,
          existing_version);
    }
  }
#ifdef _WIN32
  _unlink(path.c_str());
#else
  unlink(path.c_str());
#endif
#ifdef _WIN32
  HANDLE hFile = CreateFileA(
      path.c_str(), GENERIC_READ | GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_NEW,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (hFile == INVALID_HANDLE_VALUE) return nullptr;
  LARGE_INTEGER sz;
  sz.QuadPart = ServeStats::kFileSize;
  if (!SetFilePointerEx(hFile, sz, nullptr, FILE_BEGIN) ||
      !SetEndOfFile(hFile)) {
    CloseHandle(hFile);
    return nullptr;
  }
  HANDLE hMap =
      CreateFileMappingA(hFile, nullptr, PAGE_READWRITE, 0,
                         static_cast<DWORD>(ServeStats::kFileSize), nullptr);
  CloseHandle(hFile);
  if (!hMap) return nullptr;
  void* mem =
      MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, ServeStats::kFileSize);
  CloseHandle(hMap);
  if (!mem) return nullptr;
#else
  int fd = open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW, 0660);
  if (fd < 0) return nullptr;
  // Explicit, umask-independent: owner (daemon) + group (the web-server
  // peers that mmap this file read-write).  Was 0666 before the H1-H3
  // privilege drop; world access is no longer needed — every legitimate
  // peer is a member of the daemon's group.
  fchmod(fd, 0660);
  if (ftruncate(fd, ServeStats::kFileSize) != 0) {
    close(fd);
    return nullptr;
  }
  void* mem = mmap(nullptr, ServeStats::kFileSize, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
  close(fd);
  if (mem == MAP_FAILED) return nullptr;
#endif

  // Zero and write header.
  std::memset(mem, 0, ServeStats::kFileSize);
  auto* stats = static_cast<ServeStats*>(mem);
  stats->magic = ServeStats::kMagic;
  stats->version = ServeStats::kVersion;
  // Mint the opt-in counter's INSTANCE identity and counting-since day.  These
  // are identity/epoch fields, set ONLY here on a fresh file create — they are
  // preserved across same-version reuse (ZeroCounters leaves them alone), so
  // the identity is stable across ordinary worker restarts and changes only
  // when the file is recreated.
  {
    // boot_id is a non-crypto instance identifier, so a weak fallback is fine:
    // never let random_device (which can throw when no entropy source is
    // available) propagate out of worker startup.
    try {
      std::random_device rd;
      for (unsigned char& b : stats->boot_id) {
        b = static_cast<unsigned char>(rd() & 0xFF);
      }
    } catch (...) {
      // Portable weak fallback: mix wall-clock time with this mapping's address
      // (distinct per process) — no crypto strength needed for an instance id.
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
  }
  stats->counting_since_unix_day = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::hours>(
          std::chrono::system_clock::now().time_since_epoch())
          .count() /
      24);
  return stats;
}

ServeStats* OpenServeStats(const std::string& path) {
#ifdef _WIN32
  HANDLE hFile = CreateFileA(
      path.c_str(), GENERIC_READ | GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (hFile == INVALID_HANDLE_VALUE) return nullptr;
  LARGE_INTEGER fileSize;
  if (!GetFileSizeEx(hFile, &fileSize) ||
      static_cast<size_t>(fileSize.QuadPart) < ServeStats::kFileSize) {
    CloseHandle(hFile);
    return nullptr;
  }
  HANDLE hMap =
      CreateFileMappingA(hFile, nullptr, PAGE_READWRITE, 0,
                         static_cast<DWORD>(ServeStats::kFileSize), nullptr);
  CloseHandle(hFile);
  if (!hMap) return nullptr;
  void* mem =
      MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, ServeStats::kFileSize);
  CloseHandle(hMap);
  if (!mem) return nullptr;
#else
  int fd = open(path.c_str(), O_RDWR | O_NOFOLLOW);
  if (fd < 0) return nullptr;
  struct stat st;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
      static_cast<size_t>(st.st_size) < ServeStats::kFileSize) {
    close(fd);
    return nullptr;
  }
  void* mem = mmap(nullptr, ServeStats::kFileSize, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
  close(fd);
  if (mem == MAP_FAILED) return nullptr;
#endif

  auto* stats = static_cast<ServeStats*>(mem);
  if (stats->magic != ServeStats::kMagic ||
      stats->version != ServeStats::kVersion) {
#ifdef _WIN32
    UnmapViewOfFile(mem);
#else
    munmap(mem, ServeStats::kFileSize);
#endif
    return nullptr;
  }
  return stats;
}

void CloseServeStats(ServeStats* stats) {
  if (stats != nullptr) {
#ifdef _WIN32
    UnmapViewOfFile(stats);
#else
    munmap(stats, ServeStats::kFileSize);
#endif
  }
}

namespace {
// Cross-platform relaxed atomic add on a counter living in the shared mmap.
// std::atomic_ref (C++20) compiles to the same lock-free RMW as the GCC/Clang
// __atomic_fetch_add builtin but is also supported by MSVC — serve_stats.cc is
// compiled into factory_worker.exe and the win-x64 libpagespeed.dll, neither of
// which can use __atomic_*. The 64-bit counters are naturally aligned (offset 8
// onward), satisfying atomic_ref's alignment requirement. The reader side uses
// a plain aligned 64-bit load (api_handlers.cc), unchanged.
inline void RelaxedAdd(uint64_t& field, uint64_t value) {
  std::atomic_ref<uint64_t>(field).fetch_add(value, std::memory_order_relaxed);
}

// Loads from the shared mapping through a const view: atomic_ref needs a
// non-const reference, and the mapping is writable.
inline uint64_t LoadRelaxed(const uint64_t& field) {
  return std::atomic_ref<uint64_t>(const_cast<uint64_t&>(field))
      .load(std::memory_order_relaxed);
}
inline uint64_t LoadAcquire(const uint64_t& field) {
  return std::atomic_ref<uint64_t>(const_cast<uint64_t&>(field))
      .load(std::memory_order_acquire);
}

// The per-type totals and the by-encoding rows.  Returns whether the type is
// one of the four counted ones (the serve then also belongs to a host row or
// to the other triple).
bool RecordServeTotals(ServeStats* stats, ContentType content_type,
                       uint64_t original_bytes, uint64_t optimized_bytes,
                       uint32_t mask) {
  switch (content_type) {
    case ContentType::kHtml:
      RelaxedAdd(stats->html_original_bytes, original_bytes);
      RelaxedAdd(stats->html_optimized_bytes, optimized_bytes);
      RelaxedAdd(stats->html_optimized_hits, 1);
      break;
    case ContentType::kCss:
      RelaxedAdd(stats->css_original_bytes, original_bytes);
      RelaxedAdd(stats->css_optimized_bytes, optimized_bytes);
      RelaxedAdd(stats->css_optimized_hits, 1);
      break;
    case ContentType::kJs:
      RelaxedAdd(stats->js_original_bytes, original_bytes);
      RelaxedAdd(stats->js_optimized_bytes, optimized_bytes);
      RelaxedAdd(stats->js_optimized_hits, 1);
      break;
    case ContentType::kImage:
      RelaxedAdd(stats->image_original_bytes, original_bytes);
      RelaxedAdd(stats->image_optimized_bytes, optimized_bytes);
      RelaxedAdd(stats->image_optimized_hits, 1);
      // SVG variants serve as kImage with format bits == kSvg (3). Count them
      // separately so svg.served reflects real serve-time hits.
      // Same `& 0x03 == 3` SVG-detect idiom as cache_handlers.cc.
      if ((mask & 0x03) == 3) {
        RelaxedAdd(stats->svg_optimized_hits, 1);
      }
      break;
    default:
      return false;
  }
  // v9: the same serve, split by the transfer encoding the mask names
  // (bits 6-7).  kReserved is not a servable encoding and writes no row;
  // the per-type totals above already counted the serve either way.
  const auto type_index = static_cast<size_t>(content_type);
  const auto encoding = CapabilityMask::Decode(mask).transfer_encoding();
  if (encoding != CapabilityMask::TransferEncoding::kReserved) {
    const auto e = static_cast<size_t>(encoding);
    RelaxedAdd(stats->serve_hits_by_encoding[type_index][e], 1);
    RelaxedAdd(stats->serve_bytes_by_encoding[type_index][e], optimized_bytes);
  }
  return true;
}

void RecordServeOther(ServeStats* stats, uint64_t original_bytes,
                      uint64_t optimized_bytes) {
  RelaxedAdd(stats->serve_hosts_other_hits, 1);
  RelaxedAdd(stats->serve_hosts_other_original_bytes, original_bytes);
  RelaxedAdd(stats->serve_hosts_other_optimized_bytes, optimized_bytes);
}

void AddToSlot(ServeStats::HostSlot& slot, uint64_t original_bytes,
               uint64_t optimized_bytes) {
  RelaxedAdd(slot.hits, 1);
  RelaxedAdd(slot.original_bytes, original_bytes);
  RelaxedAdd(slot.optimized_bytes, optimized_bytes);
}

// Plain FNV-1a-64 of the normalised name, as for the signer keyids; the one
// name that would hash to 0 (the "free slot" value) is remapped.
uint64_t HashServeHost(std::string_view host) {
  uint64_t h = 0xcbf29ce484222325ULL;
  for (const unsigned char c : host) {
    h ^= c;
    h *= 0x100000001b3ULL;
  }
  return h == 0 ? 0x9e3779b97f4a7c15ULL : h;
}

bool IsPortDigits(std::string_view port) {
  if (port.empty() || port.size() > 5) return false;
  for (const char c : port) {
    if (c < '0' || c > '9') return false;
  }
  return true;
}

bool IsHostNameByte(char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
         c == '-' || c == '_';
}

bool IsIpv6LiteralByte(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || c == ':' ||
         c == '.';
}

bool IsAlnum(char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

// Longest input worth looking at: a full name plus ":" and a 5-digit port.
constexpr size_t kServeHostInputMax = ServeStats::kHostNameCapacity + 6;

// The serve hot path's form of THE HOST-NAME RULE (serve_stats.h,
// NormalizeServeHost): no allocation.  Lowercases `host` into `buf`
// (kServeHostInputMax bytes, on the caller's stack) and returns the length
// of the normalised name, which is always a prefix of `buf`, or 0 when the
// host is refused.
size_t NormalizeServeHostInto(std::string_view host, char* buf) {
  // Longer than any acceptable name plus a port: refuse before copying.
  if (host.empty() || host.size() > kServeHostInputMax) return 0;
  for (size_t i = 0; i < host.size(); ++i) {
    const char c = host[i];
    buf[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
  }
  std::string_view v(buf, host.size());
  bool has_alnum = false;
  if (v.front() == '[') {
    const size_t close = v.find(']');
    if (close == std::string_view::npos || close == 1) return 0;
    const std::string_view rest = v.substr(close + 1);
    if (!rest.empty() &&
        (rest.front() != ':' || !IsPortDigits(rest.substr(1)))) {
      return 0;
    }
    for (const char c : v.substr(1, close - 1)) {
      if (!IsIpv6LiteralByte(c)) return 0;
      has_alnum = has_alnum || IsAlnum(c);
    }
    v = v.substr(0, close + 1);
  } else {
    const size_t colon = v.rfind(':');
    if (colon != std::string_view::npos) {
      if (!IsPortDigits(v.substr(colon + 1))) return 0;
      v = v.substr(0, colon);
    }
    if (!v.empty() && v.back() == '.') v.remove_suffix(1);
    // Exactly one trailing dot is dropped.  A name still ending in one
    // ("www.example.com..") would not survive the reader's second
    // normalisation, so it could never be reported: refuse it here rather
    // than let it take a slot.
    if (v.empty() || v.back() == '.') return 0;
    for (const char c : v) {
      if (!IsHostNameByte(c)) return 0;
      has_alnum = has_alnum || IsAlnum(c);
    }
  }
  // "." -> refused above; "..", "-", "[:]" have no letter or digit.
  if (!has_alnum) return 0;
  if (v.size() > ServeStats::kHostNameCapacity - 1) return 0;
  return v.size();
}
}  // namespace

bool NormalizeServeHost(std::string_view host, std::string* out) {
  out->clear();
  char buf[kServeHostInputMax];
  const size_t len = NormalizeServeHostInto(host, buf);
  if (len == 0) return false;
  out->assign(buf, len);
  return true;
}

void RecordServeHostHit(ServeStats* stats, std::string_view host,
                        uint64_t original_bytes, uint64_t optimized_bytes) {
  if (stats == nullptr) return;
  // Every serve passes here: normalise into a stack buffer, never the heap.
  char buf[kServeHostInputMax];
  const size_t len = NormalizeServeHostInto(host, buf);
  if (len == 0) {
    RecordServeOther(stats, original_bytes, optimized_bytes);
    return;
  }
  const std::string_view name(buf, len);
  // With every slot taken, a serve for a host without a slot probes all 64
  // slots before counting under other: 64 relaxed loads, acceptable on the
  // serve path and bounded.
  const uint64_t key = HashServeHost(name);
  const size_t start = static_cast<size_t>(key % ServeStats::kHostSlots);
  for (size_t i = 0; i < ServeStats::kHostSlots; ++i) {
    ServeStats::HostSlot& slot =
        stats->serve_hosts[(start + i) % ServeStats::kHostSlots];
    std::atomic_ref<uint64_t> slot_key(slot.key_hash);
    uint64_t seen = slot_key.load(std::memory_order_acquire);
    if (seen == 0) {
      uint64_t expected = 0;
      if (slot_key.compare_exchange_strong(expected, key,
                                           std::memory_order_acq_rel,
                                           std::memory_order_acquire)) {
        // Claimed: write the name, then publish it.
        std::memcpy(slot.name, name.data(), name.size());
        slot.name[name.size()] = '\0';
        std::atomic_ref<uint64_t>(slot.ready)
            .store(1, std::memory_order_release);
        AddToSlot(slot, original_bytes, optimized_bytes);
        return;
      }
      seen = expected;  // another process claimed it first
    }
    if (seen == key) {
      AddToSlot(slot, original_bytes, optimized_bytes);
      return;
    }
  }
  RecordServeOther(stats, original_bytes, optimized_bytes);
}

void RecordServeHit(ServeStats* stats, ContentType content_type,
                    uint64_t original_bytes, uint64_t optimized_bytes,
                    uint32_t mask) {
  if (stats == nullptr) return;
  if (RecordServeTotals(stats, content_type, original_bytes, optimized_bytes,
                        mask)) {
    RecordServeOther(stats, original_bytes, optimized_bytes);
  }
}

void RecordServeHit(ServeStats* stats, ContentType content_type,
                    uint64_t original_bytes, uint64_t optimized_bytes,
                    uint32_t mask, std::string_view host) {
  if (stats == nullptr) return;
  if (RecordServeTotals(stats, content_type, original_bytes, optimized_bytes,
                        mask)) {
    RecordServeHostHit(stats, host, original_bytes, optimized_bytes);
  }
}

namespace {
ServeStats::SplitCounters* SplitFor(ServeStats* stats, ContentType type) {
  switch (type) {
    case ContentType::kHtml:
      return &stats->split_html;
    case ContentType::kCss:
      return &stats->split_css;
    case ContentType::kJs:
      return &stats->split_js;
    case ContentType::kImage:
      return &stats->split_image;
    default:
      return nullptr;
  }
}
}  // namespace

void RecordServeHitSplit(ServeStats* stats, ContentType content_type,
                         uint64_t original_bytes,
                         uint64_t compressed_original_bytes,
                         uint64_t optimized_bytes, uint32_t mask,
                         std::string_view host) {
  if (stats == nullptr) return;
  ServeStats::SplitCounters* split = SplitFor(stats, content_type);
  if (split == nullptr) return;
  RecordServeHit(stats, content_type, original_bytes, optimized_bytes, mask,
                 host);
  RelaxedAdd(split->original_bytes, original_bytes);
  RelaxedAdd(split->compressed_original_bytes, compressed_original_bytes);
  RelaxedAdd(split->optimized_bytes, optimized_bytes);
}

ServeStats::SplitCounters ReadServeSplit(const ServeStats* stats,
                                         ContentType content_type) {
  ServeStats::SplitCounters out{};
  if (stats == nullptr) return out;
  const ServeStats::SplitCounters* split =
      SplitFor(const_cast<ServeStats*>(stats), content_type);
  if (split == nullptr) return out;
  out.original_bytes = LoadRelaxed(split->original_bytes);
  out.compressed_original_bytes = LoadRelaxed(split->compressed_original_bytes);
  out.optimized_bytes = LoadRelaxed(split->optimized_bytes);
  return out;
}

ServeSavingsByHost ReadServeSavingsByHost(const ServeStats* stats,
                                          size_t limit) {
  ServeSavingsByHost out;
  if (stats == nullptr) return out;
  out.other.hits = LoadRelaxed(stats->serve_hosts_other_hits);
  out.other.original_bytes =
      LoadRelaxed(stats->serve_hosts_other_original_bytes);
  out.other.optimized_bytes =
      LoadRelaxed(stats->serve_hosts_other_optimized_bytes);
  std::vector<ServeHostRow> rows;
  for (const ServeStats::HostSlot& slot : stats->serve_hosts) {
    const uint64_t key = LoadAcquire(slot.key_hash);
    if (key == 0) continue;
    ServeHostRow row;
    row.hits = LoadRelaxed(slot.hits);
    row.original_bytes = LoadRelaxed(slot.original_bytes);
    row.optimized_bytes = LoadRelaxed(slot.optimized_bytes);
    if (row.hits == 0) continue;
    const bool ready = LoadAcquire(slot.ready) == 1;
    const size_t len =
        ready ? strnlen(slot.name, ServeStats::kHostNameCapacity) : 0;
    const std::string_view stored(slot.name, len);
    std::string normalized;
    // A slot counts as a host only when its name re-validates AND hashes to
    // the key it was claimed under: a name torn by a ZeroCounters racing a
    // claim across a restart ("www.exa") is grammar-valid but fails the hash,
    // so it never becomes a JSON key or a metrics label.
    if (ready && len > 0 && len < ServeStats::kHostNameCapacity &&
        NormalizeServeHost(stored, &normalized) && normalized == stored &&
        HashServeHost(stored) == key) {
      row.host = std::move(normalized);
      rows.push_back(std::move(row));
    } else {
      out.other.hits += row.hits;
      out.other.original_bytes += row.original_bytes;
      out.other.optimized_bytes += row.optimized_bytes;
    }
  }
  // A ZeroCounters racing a probe can leave one host in two slots: report it
  // once, both slots summed, before ranking and applying the limit, so no
  // host is listed twice and no metrics series repeats.
  std::sort(rows.begin(), rows.end(),
            [](const ServeHostRow& a, const ServeHostRow& b) {
              return a.host < b.host;
            });
  size_t kept = 0;
  for (size_t i = 0; i < rows.size(); ++i) {
    if (kept > 0 && rows[kept - 1].host == rows[i].host) {
      rows[kept - 1].hits += rows[i].hits;
      rows[kept - 1].original_bytes += rows[i].original_bytes;
      rows[kept - 1].optimized_bytes += rows[i].optimized_bytes;
    } else {
      if (kept != i) rows[kept] = std::move(rows[i]);
      ++kept;
    }
  }
  rows.resize(kept);
  std::sort(rows.begin(), rows.end(),
            [](const ServeHostRow& a, const ServeHostRow& b) {
              return a.hits != b.hits ? a.hits > b.hits : a.host < b.host;
            });
  for (size_t i = limit; i < rows.size(); ++i) {
    out.other.hits += rows[i].hits;
    out.other.original_bytes += rows[i].original_bytes;
    out.other.optimized_bytes += rows[i].optimized_bytes;
  }
  if (rows.size() > limit) rows.resize(limit);
  out.hosts = std::move(rows);
  return out;
}

void RecordWebBotAuthSigned(ServeStats* stats, bool verified) {
  if (stats == nullptr) return;
  if (verified) {
    RelaxedAdd(stats->webbotauth_signed_verified, 1);
  } else {
    RelaxedAdd(stats->webbotauth_signed_invalid, 1);
  }
}

void RecordWebBotAuthOtherSignature(ServeStats* stats) {
  if (stats == nullptr) return;
  RelaxedAdd(stats->webbotauth_other_signature, 1);
}

uint64_t HashWebBotAuthKeyid(std::string_view keyid) {
  // Plain UNSALTED FNV-1a-64 over the raw keyid bytes.  Intentionally
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

void RecordWebBotAuthVerifiedSigner(ServeStats* stats, std::string_view keyid,
                                    uint64_t latency_us) {
  if (stats == nullptr) return;

  // Bucket the verify latency (each request lands in exactly one bucket).
  if (latency_us < 100) {
    RelaxedAdd(stats->webbotauth_verify_latency_lt100us, 1);
  } else if (latency_us < 1000) {
    RelaxedAdd(stats->webbotauth_verify_latency_lt1ms, 1);
  } else if (latency_us < 10000) {
    RelaxedAdd(stats->webbotauth_verify_latency_lt10ms, 1);
  } else {
    RelaxedAdd(stats->webbotauth_verify_latency_ge10ms, 1);
  }

  const uint64_t h = HashWebBotAuthKeyid(keyid);
  // Find-or-claim a slot lock-free. CAS an empty (kid_hash==0) slot to `h`,
  // then increment. Cross-process-safe: nginx workers + worker all map the
  // same file MAP_SHARED, and every access here is via std::atomic_ref.
  for (auto& slot : stats->webbotauth_signers) {
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
  RelaxedAdd(stats->webbotauth_other_verified_bots, 1);
}

void RecordZerocopyTornAbort(ServeStats* stats) {
  if (stats == nullptr) return;
  RelaxedAdd(stats->zerocopy_torn_aborts, 1);
}

void RecordZerocopyProactiveCopyout(ServeStats* stats) {
  if (stats == nullptr) return;
  RelaxedAdd(stats->zerocopy_proactive_copyouts, 1);
}

void RecordZerocopyCopyThenVerifyDiscard(ServeStats* stats) {
  if (stats == nullptr) return;
  RelaxedAdd(stats->zerocopy_copy_then_verify_discards, 1);
}

void RecordSwrCoalescedServe(ServeStats* stats) {
  if (stats == nullptr) return;
  RelaxedAdd(stats->swr_coalesced_serves, 1);
}

void RecordStaleIfErrorServe(ServeStats* stats) {
  if (stats == nullptr) return;
  RelaxedAdd(stats->stale_if_error_serves, 1);
}

void RecordHeadersSidecarFallThrough(ServeStats* stats) {
  if (stats == nullptr) return;
  RelaxedAdd(stats->headers_sidecar_fall_through, 1);
}

void RecordCacheBusyRead(ServeStats* stats, bool retry_hit) {
  if (stats == nullptr) return;
  RelaxedAdd(stats->cache_busy_reads, 1);
  if (retry_hit) RelaxedAdd(stats->cache_busy_retry_hits, 1);
}

void RecordHeadersSidecarNeverOptimized(ServeStats* stats) {
  if (stats == nullptr) return;
  RelaxedAdd(stats->headers_sidecar_never_optimized, 1);
}

void RecordHeadersSidecarReplayed(ServeStats* stats) {
  if (stats == nullptr) return;
  RelaxedAdd(stats->headers_sidecar_replayed, 1);
}

void RecordHeadersSidecarMissing(ServeStats* stats) {
  if (stats == nullptr) return;
  RelaxedAdd(stats->headers_sidecar_missing, 1);
}

void RecordHeadersSidecarWriteFailed(ServeStats* stats) {
  if (stats == nullptr) return;
  RelaxedAdd(stats->headers_sidecar_write_failed, 1);
}

void RecordHeadersSidecarStored(ServeStats* stats) {
  if (stats == nullptr) return;
  RelaxedAdd(stats->headers_sidecar_stored, 1);
}

void RecordServeClass(ServeStats* stats, ServeClass cls, uint32_t flags) {
  if (stats == nullptr) return;
  // Exactly one class per call, or none.  A value that is not one of the five
  // — including two of them combined, which the disjoint-bit values make an
  // unrecognised value rather than a third class — falls through and counts
  // nothing.
  switch (cls) {
    case ServeClass::kOptimized:
      RelaxedAdd(stats->serve_optimized_total, 1);
      break;
    case ServeClass::kOriginalCold:
      RelaxedAdd(stats->serve_original_cold_total, 1);
      break;
    case ServeClass::kOriginalPending:
      RelaxedAdd(stats->serve_original_pending_total, 1);
      break;
    case ServeClass::kOriginalDeclined:
      RelaxedAdd(stats->serve_original_declined_total, 1);
      break;
    case ServeClass::kOriginalSkew:
      RelaxedAdd(stats->serve_original_skew_total, 1);
      break;
    default:
      // Unrecognised class: drop the write, but leave a trace.  Counting
      // nothing at all was the earlier behaviour and it made a broken
      // partition indistinguishable from a healthy one — the five counters
      // would simply be short, with no way to tell from inside the surface.
      // The flags are not examined: this call was not understood, and
      // reporting its flags too would double-report one bad call.
      RelaxedAdd(stats->serve_class_unrecognized_total, 1);
      return;
  }
  // Orthogonal to the class: a suppressed notify still had exactly one class.
  if ((flags & kServeFlagNotifySuppressed) != 0) {
    RelaxedAdd(stats->notify_suppressed_total, 1);
  }
  // Flag bits this build does not know are ignored by design, so a newer peer
  // can pass one without losing its serve.  Counted separately from the drop
  // above because the meaning is the opposite: the class landed, the partition
  // is intact, and the only thing lost is an annotation.
  if ((flags & ~kServeFlagKnownMask) != 0) {
    RelaxedAdd(stats->serve_flags_unrecognized_total, 1);
  }
}

void RecordSaturationSample(ServeStats* stats, uint32_t in_flight) {
  if (stats == nullptr) return;
  RelaxedAdd(stats->saturation_sample_accum, in_flight);
  RelaxedAdd(stats->saturation_sample_count, 1);
  // Monotone max.  Relaxed CAS loop: only ever raises the mark, and a lost
  // race costs at most this sample's contribution to it.
  std::atomic_ref<uint32_t> hwm(stats->saturation_hwm);
  uint32_t observed = hwm.load(std::memory_order_relaxed);
  while (in_flight > observed &&
         !hwm.compare_exchange_weak(observed, in_flight,
                                    std::memory_order_relaxed)) {
    // compare_exchange_weak refreshed `observed`; retry only while we would
    // still be raising the mark.
  }
}

void SetWorkerPoolThreads(ServeStats* stats, uint32_t threads) {
  if (stats == nullptr) return;
  std::atomic_ref<uint32_t>(stats->worker_pool_threads)
      .store(threads, std::memory_order_relaxed);
}

void RecordRslCapVerdict(ServeStats* stats, int http_status) {
  if (stats == nullptr) return;
  switch (http_status) {
    case 0:
      RelaxedAdd(stats->rslcap_authorized, 1);
      break;
    case 401:
      RelaxedAdd(stats->rslcap_denied_401, 1);
      break;
    case 402:
      RelaxedAdd(stats->rslcap_denied_402, 1);
      break;
    default:
      break;  // handler only emits 0/401/402
  }
}

}  // namespace pagespeed
