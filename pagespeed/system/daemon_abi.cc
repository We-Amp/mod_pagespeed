/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "pagespeed/system/daemon_abi.h"

#include <cstddef>
#include <cstring>
#include <memory>
#include <string>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

#ifndef _WIN32
#include <dlfcn.h>
#else
#include <windows.h>
#endif

namespace net_instaweb {

#ifdef _WIN32
// The FILE name of the Windows artefact.  A NAME, never a path: on this
// platform the loader refuses anything that is not an absolute path (see
// NativeOps::Load), so a caller must join this to a directory -- the IIS
// port resolves it against its own module directory.
const char kDefaultDaemonLibraryName[] = "pagespeed.dll";
#else
const char kDefaultDaemonLibraryName[] = "libpagespeed.so";
#endif

DaemonAbi::DaemonAbi() = default;
DaemonAbi::~DaemonAbi() = default;

namespace {

// The three platform touch points of the run-time binding, in ONE place:
// open the library, look a symbol up, and what "close" means.  Everything
// else -- the bind order, the version gate, the layout handshakes, the
// error texts -- is written once, below, and is the same on every platform.
#ifdef _WIN32

// UTF-8 to wide, on the caller's stack; the loader's path argument stays
// UTF-8 everywhere else.  Returns false for invalid UTF-8 (or an empty
// input, which the caller reports with its own message): the two are errors
// in the path itself, not in anything the loader could answer.
bool Utf8ToWide(StringPiece utf8, std::wstring* out) {
  if (utf8.empty()) {
    return false;
  }
  const int length =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                          static_cast<int>(utf8.size()), nullptr, 0);
  if (length <= 0) {
    return false;
  }
  out->assign(length, L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                      static_cast<int>(utf8.size()), &(*out)[0], length);
  return true;
}

// Absolute in the forms this platform's loader fully qualifies: a
// drive-letter root ("X:\" or "X:/") or a UNC path, in either separator
// style ("\\host\share" or "//host/share").  A bare name and every relative
// or drive-relative form fail this, deliberately.  This is a FORM check, not
// a loadability guarantee: a `\\?\`-prefixed path passes it and fails at the
// API with the path named, which is the same honesty.
bool PathIsFullyQualified(StringPiece path) {
  if (path.size() < 2) {
    return false;  // also keeps the first-byte read below in bounds
  }
  const unsigned char drive = static_cast<unsigned char>(path[0]);
  // Drive letters are ASCII, never locale-dependent: isalpha() on a plain
  // char is undefined for bytes >= 0x80, and a UTF-8 path may start with one.
  const bool is_drive_letter =
      (drive >= 'A' && drive <= 'Z') || (drive >= 'a' && drive <= 'z');
  return (path.size() >= 3 && is_drive_letter && path[1] == ':' &&
          (path[2] == '\\' || path[2] == '/')) ||
         (path.size() >= 2 && (path[0] == '\\' || path[0] == '/') &&
          path[0] == path[1]);
}

// GetLastError() rendered as UTF-8 text, with the trailing line break
// FormatMessage appends trimmed.
GoogleString LastErrorText(DWORD code) {
  wchar_t* message = nullptr;
  const DWORD length = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<LPWSTR>(&message), 0, nullptr);
  GoogleString out;
  if (length != 0 && message != nullptr) {
    std::wstring trimmed(message, length);
    while (!trimmed.empty() &&
           (trimmed.back() == L'\r' || trimmed.back() == L'\n' ||
            trimmed.back() == L' ')) {
      trimmed.pop_back();
    }
    const int utf8_length = WideCharToMultiByte(
        CP_UTF8, 0, trimmed.data(), static_cast<int>(trimmed.size()), nullptr,
        0, nullptr, nullptr);
    if (utf8_length > 0) {
      out.resize(utf8_length);
      WideCharToMultiByte(CP_UTF8, 0, trimmed.data(),
                          static_cast<int>(trimmed.size()), &out[0],
                          utf8_length, nullptr, nullptr);
    }
  }
  if (message != nullptr) {
    LocalFree(message);
  }
  if (out.empty()) {
    out = "error " + Integer64ToString(static_cast<int64>(code));
  }
  return out;
}

struct NativeOps {
  // UTF-8 path in, wide to the API.  The path must be ABSOLUTE, refused
  // before the API is touched: with LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR a
  // relative path fails with an opaque "parameter is incorrect", and a bare
  // name searched anywhere is the hijack surface the flags exist to close.
  // The two flags cover the application directory, System32 and directories
  // added via AddDllDirectory -- never PATH, never the current directory,
  // and never a bare LoadLibraryW.  (LoadLibraryExW is MAX_PATH-bound and
  // takes no \\?\ prefix; an install root past 260 characters is out of
  // scope here.)
  static void* Load(const GoogleString& path, GoogleString* error) {
    if (path.empty()) {
      StrAppend(error, "the optimizer daemon library path is empty");
      return nullptr;
    }
    std::wstring wide;
    if (!Utf8ToWide(path, &wide)) {
      StrAppend(error,
                "the optimizer daemon library path is not valid "
                "UTF-8: ",
                path);
      return nullptr;
    }
    if (!PathIsFullyQualified(path)) {
      StrAppend(error,
                "the optimizer daemon library path must be absolute "
                "on this platform (the loader searches no "
                "directories): ",
                path);
      return nullptr;
    }
    // Thread-scoped and restored after the call: a failed load inside the
    // server process must never surface a dialog box, but flipping the
    // process-wide switch is not this library's to do inside a host
    // process.
    DWORD previous_mode = 0;
    const BOOL mode_changed =
        SetThreadErrorMode(SEM_FAILCRITICALERRORS, &previous_mode);
    HMODULE handle = LoadLibraryExW(
        wide.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
    const DWORD why = handle == nullptr ? GetLastError() : 0;
    if (mode_changed != 0) {
      SetThreadErrorMode(previous_mode, nullptr);
    }
    if (handle == nullptr) {
      StrAppend(error, "cannot load the optimizer daemon library ", path, ": ",
                LastErrorText(why));
    }
    return handle;
  }

  static void* Sym(void* handle, const char* symbol, GoogleString* error) {
    SetLastError(0);
    void* address = reinterpret_cast<void*>(
        GetProcAddress(static_cast<HMODULE>(handle), symbol));
    if (address == nullptr) {
      const DWORD why = GetLastError();
      StrAppend(error, "the optimizer daemon library does not export ", symbol,
                ": ", LastErrorText(why));
    }
    return address;
  }

  static void Close(void* handle) {
    // NOT FreeLibrary, deliberately: the teardown order around
    // DLL_PROCESS_DETACH has a recycle-leak history in this module, and the
    // module already keeps process-lifetime singletons for the same reason.
    // The place a reader would look for the FreeLibrary is here: a
    // successfully returned binding is the process's, for the process's
    // lifetime.  A REJECTED library is a different operation, one call
    // below.
  }

  static void CloseUnused(void* handle) {
    // A REJECTED library was never accepted into use, so the never-free
    // rationale above does not cover it: a file that failed the version
    // gate, a required symbol or a layout handshake is freed here, so a bad
    // file does not stay mapped -- and, on Windows, locked against
    // replacement -- for the process's life.
    FreeLibrary(static_cast<HMODULE>(handle));
  }
};

#else

struct NativeOps {
  // RTLD_LOCAL: the daemon library's symbols stay private to this handle so
  // they can never satisfy an unrelated lookup elsewhere in the server.
  static void* Load(const GoogleString& path, GoogleString* error) {
    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
      const char* why = dlerror();
      StrAppend(error, "cannot load the optimizer daemon library ", path,
                why == nullptr ? "" : ": ", why == nullptr ? "" : why);
    }
    return handle;
  }

  static void* Sym(void* handle, const char* symbol, GoogleString* error) {
    dlerror();  // Clear any stale condition before the lookup.
    void* address = dlsym(handle, symbol);
    if (address == nullptr) {
      const char* why = dlerror();
      StrAppend(error, "the optimizer daemon library does not export ", symbol,
                why == nullptr ? "" : ": ", why == nullptr ? "" : why);
    }
    return address;
  }

  static void Close(void* handle) { dlclose(handle); }

  // Same operation as Close on this platform: a rejected library is freed,
  // exactly as the destructor has always freed it.
  static void CloseUnused(void* handle) { dlclose(handle); }
};

#endif  // _WIN32

// Signatures of the entry points bound below.  Spelled out here rather than
// in the header so the only thing the rest of the tree sees is the C++
// interface, never a raw function pointer into the peer.
using VersionFn = int (*)();
using CacheConfigInitFn = void (*)(PsCacheConfig*);
using CacheConfigInitSizedFn = void (*)(PsCacheConfig*, size_t);
using CacheOpenFn = int (*)(const PsCacheConfig*, void**);
using CacheCloseFn = void (*)(void*);
using StrErrorFn = const char* (*)(int);
using LastErrorMessageFn = const char* (*)();
using SharedConfigVolumeSizeFn = uint64_t (*)(const char*);
using SharedConfigGenerationFn = uint32_t (*)(const char*);
using WriteParamsInitSizedFn = void (*)(PsWriteParams*, size_t);
using NotifyParamsInitSizedFn = void (*)(PsNotifyParams*, size_t);
using CacheWriteOriginalFn = int (*)(void*, const char*, const char*,
                                     const char*, const PsWriteParams*, void**);
using WriteDataFn = int (*)(void*, const void*, size_t);
using WriteCloseFn = int (*)(void*);
using WriteAbortFn = void (*)(void*);
using VaryUncacheableFn = int (*)(const char*);
using VaryVariesAcceptFn = int (*)(const char*);
using ParseCacheControlFn = int (*)(const char*, PsCacheControl*);
using AgeAdjustedInsertTimeFn = uint32_t (*)(uint32_t, uint32_t);
using ClassifyFn = uint32_t (*)(const char*, const char*, const char*,
                                const char*);
using ClassifyContentTypeFn = int (*)(const char*);
using OptionContextSignatureFn = int (*)(const char*, size_t, char*, size_t);
using NotifyWorkerExFn = int (*)(const char*, const PsNotifyParams*);
using CacheReadBestFn = int (*)(void*, const char*, const char*, const char*,
                                uint32_t, void**);
using CacheReadAlternateFn = int (*)(void*, const char*, const char*,
                                     const char*, uint8_t, void**);
using ReadContentFn = int (*)(const void*, const uint8_t**, size_t*);
using ReadUint32Fn = uint32_t (*)(const void*);
using ReadUint16Fn = uint16_t (*)(const void*);
using ReadUint8Fn = uint8_t (*)(const void*);
using ReadIntFn = int (*)(const void*);
using ReadStringFn = const char* (*)(const void*);
using ReadBytesFn = const uint8_t* (*)(const void*);
using ReadFreeFn = void (*)(void*);
using EvaluateFreshnessFn = int (*)(const PsFreshnessInput*,
                                    const PsFreshnessConfig*,
                                    PsFreshnessResult*);
using BuildCacheControlFn = int (*)(const PsCacheControlInput*, char*, size_t,
                                    size_t*, uint32_t*);
using ServeStatsOpenFn = int (*)(const char*, void**);
using ServeStatsRecordServeClassFn = void (*)(void*, int, uint32_t);
using ServeStatsRecordHitFn = void (*)(void*, int, uint64_t, uint64_t,
                                       uint32_t);
using ServeStatsRecordHitHostFn = void (*)(void*, int, uint64_t, uint64_t,
                                           uint32_t, const char*, size_t);
using ServeStatsCloseFn = void (*)(void*);

class LibraryDaemonAbi : public DaemonAbi {
 public:
  explicit LibraryDaemonAbi(void* handle) : handle_(handle) {}

  ~LibraryDaemonAbi() override {
    if (handle_ != nullptr) {
      NativeOps::Close(handle_);
    }
  }

  // The loader calls this when it REJECTS the library after a successful
  // open: the handle is freed by the caller (CloseUnused), and the
  // destructor must not free it a second time.
  void ForgetHandle() { handle_ = nullptr; }

  int VersionMajor() const override { return version_major_(); }
  int VersionMinor() const override { return version_minor_(); }

  void CacheConfigInit(PsCacheConfig* config) const override {
    if (cache_config_init_sized_ != nullptr) {
      // sizeof(PsCacheConfig), NOT the size of the buffer behind it.  The
      // sized spelling records the number it is given as the struct's size,
      // so handing it the over-allocation would have it describe a struct
      // this build does not have -- and the caller would then be telling
      // ps_cache_open to read fields that are not there.
      cache_config_init_sized_(config, sizeof(PsCacheConfig));
      return;
    }
    // Older daemon: the initializer writes its own struct's worth regardless
    // of what we own.  The caller's over-allocation is what makes that safe;
    // there is nothing to pass and nothing to check first.
    cache_config_init_(config);
  }

  bool HasSizedCacheConfigInit() const override {
    return cache_config_init_sized_ != nullptr;
  }

  uint64_t SharedConfigVolumeSize(const char* volume_path) const override {
    if (shared_config_volume_size_ == nullptr) {
      return 0;
    }
    return shared_config_volume_size_(volume_path);
  }

  bool PublishesVolumeSize() const override {
    return shared_config_volume_size_ != nullptr;
  }

  uint32_t SharedConfigGeneration(const char* volume_path) const override {
    if (shared_config_generation_ == nullptr) {
      return 0;
    }
    return shared_config_generation_(volume_path);
  }

  bool PublishesGeneration() const override {
    return shared_config_generation_ != nullptr;
  }

  bool PublishesLastErrorMessage() const override {
    return last_error_message_ != nullptr;
  }

  int CacheOpen(const PsCacheConfig* config, void** out_cache) const override {
    return cache_open_(config, out_cache);
  }

  void CacheClose(void* cache) const override { cache_close_(cache); }

  const char* StrError(int error) const override { return str_error_(error); }

  const char* LastErrorMessage() const override {
    if (last_error_message_ == nullptr) {
      return nullptr;
    }
    return last_error_message_();
  }

  void WriteParamsInit(PsWriteParams* params) const override {
    write_params_init_sized_(params, sizeof(PsWriteParams));
  }

  void NotifyParamsInit(PsNotifyParams* params) const override {
    notify_params_init_sized_(params, sizeof(PsNotifyParams));
  }

  int CacheWriteOriginal(void* cache, const char* url, const char* hostname,
                         const char* scheme, const PsWriteParams* params,
                         void** out_handle) const override {
    return cache_write_original_(cache, url, hostname, scheme, params,
                                 out_handle);
  }

  int WriteData(void* handle, const void* data, size_t length) const override {
    return write_data_(handle, data, length);
  }

  int WriteClose(void* handle) const override { return write_close_(handle); }

  void WriteAbort(void* handle) const override { write_abort_(handle); }

  int VaryUncacheable(const char* vary) const override {
    return vary_uncacheable_(vary);
  }

  int VaryVariesAccept(const char* vary) const override {
    return vary_varies_accept_(vary);
  }

  int ParseCacheControl(const char* header_value,
                        PsCacheControl* out) const override {
    return parse_cache_control_(header_value, out);
  }

  uint32_t AgeAdjustedInsertTime(uint32_t now_seconds,
                                 uint32_t age_seconds) const override {
    return age_adjusted_insert_time_(now_seconds, age_seconds);
  }

  uint32_t Classify(const char* accept, const char* user_agent,
                    const char* save_data,
                    const char* accept_encoding) const override {
    return classify_(accept, user_agent, save_data, accept_encoding);
  }

  int ClassifyContentType(const char* content_type_header) const override {
    return classify_content_type_(content_type_header);
  }

  int OptionContextSignature(const char* payload, size_t payload_length,
                             char* out, size_t out_size) const override {
    return option_context_signature_(payload, payload_length, out, out_size);
  }

  int NotifyWorker(const char* socket_path,
                   const PsNotifyParams* params) const override {
    return notify_worker_ex_(socket_path, params);
  }

  int CacheReadBest(void* cache, const char* url, const char* hostname,
                    const char* scheme, uint32_t mask,
                    void** out_result) const override {
    return cache_read_best_(cache, url, hostname, scheme, mask, out_result);
  }

  int CacheReadAlternate(void* cache, const char* url, const char* hostname,
                         const char* scheme, uint8_t alternate_id,
                         void** out_result) const override {
    return cache_read_alternate_(cache, url, hostname, scheme, alternate_id,
                                 out_result);
  }

  int ReadContent(const void* result, const uint8_t** out_data,
                  size_t* out_length) const override {
    return read_content_(result, out_data, out_length);
  }

  uint32_t ReadMask(const void* result) const override {
    return read_mask_(result);
  }

  uint8_t ReadFlags(const void* result) const override {
    return read_flags_(result);
  }

  int ReadContentType(const void* result) const override {
    return read_content_type_(result);
  }

  const char* ReadOriginContentType(const void* result) const override {
    return read_origin_content_type_(result);
  }

  uint32_t ReadCacheInsertedAt(const void* result) const override {
    return read_cache_inserted_at_(result);
  }

  uint32_t ReadOriginMaxAge(const void* result) const override {
    return read_origin_max_age_(result);
  }

  uint32_t ReadOriginSMaxAge(const void* result) const override {
    return read_origin_s_maxage_(result);
  }

  uint16_t ReadOriginCcFlags(const void* result) const override {
    return read_origin_cc_flags_(result);
  }

  uint32_t ReadOriginLastModified(const void* result) const override {
    return read_origin_last_modified_(result);
  }

  const char* ReadOriginEtag(const void* result) const override {
    return read_origin_etag_(result);
  }

  const uint8_t* ReadOriginHtmlHash(const void* result) const override {
    return read_origin_html_hash_(result);
  }

  int ReadIsWorkerProcessed(const void* result) const override {
    return read_is_worker_processed_(result);
  }

  uint32_t ReadOriginContentLength(const void* result) const override {
    return read_origin_content_length_(result);
  }

  void ReadFree(void* result) const override { read_free_(result); }

  int EvaluateFreshness(const PsFreshnessInput* input,
                        const PsFreshnessConfig* config,
                        PsFreshnessResult* out) const override {
    return evaluate_freshness_(input, config, out);
  }

  int BuildCacheControl(const PsCacheControlInput* input, char* buf,
                        size_t capacity, size_t* out_len,
                        uint32_t* out_final_max_age) const override {
    return build_cache_control_(input, buf, capacity, out_len,
                                out_final_max_age);
  }

  int ServeStatsOpen(const char* cache_path, void** out_handle) const override {
    return serve_stats_open_(cache_path, out_handle);
  }

  void ServeStatsRecordServeClass(void* handle, int serve_class,
                                  uint32_t flags) const override {
    serve_stats_record_serve_class_(handle, serve_class, flags);
  }

  void ServeStatsRecordHit(void* handle, int content_type,
                           uint64_t original_bytes, uint64_t optimized_bytes,
                           uint32_t mask) const override {
    serve_stats_record_hit_(handle, content_type, original_bytes,
                            optimized_bytes, mask);
  }

  bool ServeStatsRecordHitForHost(void* handle, int content_type,
                                  uint64_t original_bytes,
                                  uint64_t optimized_bytes, uint32_t mask,
                                  StringPiece host) const override {
    if (serve_stats_record_hit_host_ == nullptr) {
      return false;
    }
    serve_stats_record_hit_host_(handle, content_type, original_bytes,
                                 optimized_bytes, mask, host.data(),
                                 host.size());
    return true;
  }

  void ServeStatsClose(void* handle) const override {
    serve_stats_close_(handle);
  }

  // Binds every symbol, naming the first one that is missing.  Returns false
  // and leaves the object unusable if any is absent -- a partially bound ABI
  // is never handed out.
  // The two version accessors, bound BEFORE anything else so the version gate
  // can run first.
  //
  // ORDER IS THE WHOLE POINT HERE.  Every symbol below the floor's minor is
  // bound unconditionally, so a daemon a minor too old fails on whichever
  // entry point it happens to be missing -- "the optimizer daemon library does
  // not export ps_vary_varies_accept" -- which reads to an operator like a
  // broken package rather than an old one.  Asking the version first turns the
  // same refusal into the sentence that names the version it has, the version
  // this build needs, and therefore what to do about it.  These two have been
  // at 1.0 since the ABI existed, so a library that cannot answer them is not
  // an old daemon at all.
  bool BindVersion(GoogleString* error) {
    return Bind("ps_version_major", &version_major_, error) &&
           Bind("ps_version_minor", &version_minor_, error);
  }

  bool BindAll(GoogleString* error) {
    return Bind("ps_cache_config_init", &cache_config_init_, error) &&
           Bind("ps_cache_open", &cache_open_, error) &&
           Bind("ps_cache_close", &cache_close_, error) &&
           Bind("ps_strerror", &str_error_, error) &&
           Bind("ps_write_params_init_sized", &write_params_init_sized_,
                error) &&
           Bind("ps_notify_params_init_sized", &notify_params_init_sized_,
                error) &&
           Bind("ps_cache_write_original", &cache_write_original_, error) &&
           Bind("ps_write_data", &write_data_, error) &&
           Bind("ps_write_close", &write_close_, error) &&
           Bind("ps_write_abort", &write_abort_, error) &&
           Bind("ps_vary_uncacheable", &vary_uncacheable_, error) &&
           Bind("ps_vary_varies_accept", &vary_varies_accept_, error) &&
           Bind("ps_parse_cache_control", &parse_cache_control_, error) &&
           Bind("ps_age_adjusted_insert_time", &age_adjusted_insert_time_,
                error) &&
           Bind("ps_classify", &classify_, error) &&
           Bind("ps_classify_content_type", &classify_content_type_, error) &&
           Bind("ps_option_context_signature", &option_context_signature_,
                error) &&
           Bind("ps_notify_worker_ex", &notify_worker_ex_, error) &&
           BindServeArm(error);
  }

  // The serve arm's surface, split out for readability only -- it is bound on
  // exactly the same terms as everything above it.  Every symbol here was
  // published at or before 1.2, so any floor this module has ever had already
  // guarantees all of them and none of this has ever moved the floor.
  bool BindServeArm(GoogleString* error) {
    return Bind("ps_cache_read_best", &cache_read_best_, error) &&
           Bind("ps_cache_read_alternate", &cache_read_alternate_, error) &&
           Bind("ps_read_content", &read_content_, error) &&
           Bind("ps_read_mask", &read_mask_, error) &&
           Bind("ps_read_flags", &read_flags_, error) &&
           Bind("ps_read_content_type", &read_content_type_, error) &&
           Bind("ps_read_origin_content_type", &read_origin_content_type_,
                error) &&
           Bind("ps_read_cache_inserted_at", &read_cache_inserted_at_, error) &&
           Bind("ps_read_origin_max_age", &read_origin_max_age_, error) &&
           Bind("ps_read_origin_s_maxage", &read_origin_s_maxage_, error) &&
           Bind("ps_read_origin_cc_flags", &read_origin_cc_flags_, error) &&
           Bind("ps_read_origin_last_modified", &read_origin_last_modified_,
                error) &&
           Bind("ps_read_origin_etag", &read_origin_etag_, error) &&
           Bind("ps_read_origin_html_hash", &read_origin_html_hash_, error) &&
           Bind("ps_read_is_worker_processed", &read_is_worker_processed_,
                error) &&
           Bind("ps_read_origin_content_length", &read_origin_content_length_,
                error) &&
           Bind("ps_read_free", &read_free_, error) &&
           Bind("ps_evaluate_freshness", &evaluate_freshness_, error) &&
           Bind("ps_build_cache_control", &build_cache_control_, error) &&
           Bind("ps_serve_stats_open", &serve_stats_open_, error) &&
           Bind("ps_serve_stats_record_serve_class",
                &serve_stats_record_serve_class_, error) &&
           Bind("ps_serve_stats_record_hit", &serve_stats_record_hit_, error) &&
           Bind("ps_serve_stats_close", &serve_stats_close_, error);
  }

  // Entry points that may legitimately be absent because the installed daemon
  // predates them.  Absence is recorded, never an error: the adapter decides
  // what each missing capability costs, and for the volume-size reader the
  // answer is "degrade", not "fail to start".
  void BindOptional() {
    GoogleString unused_error;
    cache_config_init_sized_ = reinterpret_cast<CacheConfigInitSizedFn>(
        NativeOps::Sym(handle_, "ps_cache_config_init_sized", &unused_error));
    shared_config_volume_size_ =
        reinterpret_cast<SharedConfigVolumeSizeFn>(NativeOps::Sym(
            handle_, "ps_read_shared_config_volume_size", &unused_error));
    shared_config_generation_ =
        reinterpret_cast<SharedConfigGenerationFn>(NativeOps::Sym(
            handle_, "ps_read_shared_config_generation", &unused_error));
    // The host-aware serve recorder (1.11).  Optional for the same reason:
    // without it a serve is recorded exactly as before, without the host.
    serve_stats_record_hit_host_ =
        reinterpret_cast<ServeStatsRecordHitHostFn>(NativeOps::Sym(
            handle_, "ps_serve_stats_record_hit_host", &unused_error));
    // The per-failure explanation.  Optional on the same terms as the two
    // above and for a smaller stake: its absence costs one clause in an error
    // line, so refusing a library over it would trade a working degrade for a
    // server that will not start.
    last_error_message_ = reinterpret_cast<LastErrorMessageFn>(
        NativeOps::Sym(handle_, "ps_last_error_message", &unused_error));
  }

 private:
  template <typename Fn>
  bool Bind(const char* symbol, Fn* out, GoogleString* error) {
    void* address = NativeOps::Sym(handle_, symbol, error);
    if (address == nullptr) {
      return false;
    }
    // A data pointer to function pointer conversion is what the platform's
    // symbol lookup contracts; there is no narrower spelling available.
    *out = reinterpret_cast<Fn>(address);
    return true;
  }

  void* handle_ = nullptr;
  VersionFn version_major_ = nullptr;
  VersionFn version_minor_ = nullptr;
  CacheConfigInitFn cache_config_init_ = nullptr;
  CacheConfigInitSizedFn cache_config_init_sized_ = nullptr;
  SharedConfigVolumeSizeFn shared_config_volume_size_ = nullptr;
  SharedConfigGenerationFn shared_config_generation_ = nullptr;
  CacheOpenFn cache_open_ = nullptr;
  CacheCloseFn cache_close_ = nullptr;
  StrErrorFn str_error_ = nullptr;
  LastErrorMessageFn last_error_message_ = nullptr;
  WriteParamsInitSizedFn write_params_init_sized_ = nullptr;
  NotifyParamsInitSizedFn notify_params_init_sized_ = nullptr;
  CacheWriteOriginalFn cache_write_original_ = nullptr;
  WriteDataFn write_data_ = nullptr;
  WriteCloseFn write_close_ = nullptr;
  WriteAbortFn write_abort_ = nullptr;
  VaryUncacheableFn vary_uncacheable_ = nullptr;
  VaryVariesAcceptFn vary_varies_accept_ = nullptr;
  ParseCacheControlFn parse_cache_control_ = nullptr;
  AgeAdjustedInsertTimeFn age_adjusted_insert_time_ = nullptr;
  ClassifyFn classify_ = nullptr;
  ClassifyContentTypeFn classify_content_type_ = nullptr;
  OptionContextSignatureFn option_context_signature_ = nullptr;
  NotifyWorkerExFn notify_worker_ex_ = nullptr;
  CacheReadBestFn cache_read_best_ = nullptr;
  CacheReadAlternateFn cache_read_alternate_ = nullptr;
  ReadContentFn read_content_ = nullptr;
  ReadUint32Fn read_mask_ = nullptr;
  ReadUint8Fn read_flags_ = nullptr;
  ReadIntFn read_content_type_ = nullptr;
  ReadStringFn read_origin_content_type_ = nullptr;
  ReadUint32Fn read_cache_inserted_at_ = nullptr;
  ReadUint32Fn read_origin_max_age_ = nullptr;
  ReadUint32Fn read_origin_s_maxage_ = nullptr;
  ReadUint16Fn read_origin_cc_flags_ = nullptr;
  ReadUint32Fn read_origin_last_modified_ = nullptr;
  ReadStringFn read_origin_etag_ = nullptr;
  ReadBytesFn read_origin_html_hash_ = nullptr;
  ReadIntFn read_is_worker_processed_ = nullptr;
  ReadUint32Fn read_origin_content_length_ = nullptr;
  ReadFreeFn read_free_ = nullptr;
  EvaluateFreshnessFn evaluate_freshness_ = nullptr;
  BuildCacheControlFn build_cache_control_ = nullptr;
  ServeStatsOpenFn serve_stats_open_ = nullptr;
  ServeStatsRecordServeClassFn serve_stats_record_serve_class_ = nullptr;
  ServeStatsRecordHitFn serve_stats_record_hit_ = nullptr;
  ServeStatsRecordHitHostFn serve_stats_record_hit_host_ = nullptr;
  ServeStatsCloseFn serve_stats_close_ = nullptr;
};

// Runs one parameter struct's size handshake.
//
// WHAT IS BEING CHECKED, and why it is not the same check as the cache
// config's.  These initializers ARE size-aware: they are told this build's
// sizeof and promise to write exactly that many bytes and stamp it back.  So
// there are two independent, cheap properties worth proving once, at load,
// rather than discovering on a request:
//
//   1. The stamped size comes back as the number we passed.  A peer that
//      stamped its own sizeof instead would have every subsequent call read a
//      different number of bytes than this build wrote.
//   2. Nothing past that size is touched.  This is the promise that makes the
//      whole append-only convention safe, and a canary is the only way to see
//      it.
//
// A failure of either means the peer's idea of the struct is not this
// build's, and the correct response is to refuse the library rather than to
// hand it a struct it will misread -- the values in question end up in the
// stored entry's origin state and on the notify wire.
template <typename Params, typename InitFn>
bool ParamsHandshakeOk(const InitFn& init, const char* what,
                       StringPiece library_path, GoogleString* error) {
  // Over-allocated so the canary has somewhere to live past the struct, and
  // so a peer that writes past the stated size lands in storage this function
  // owns instead of on whatever follows it.
  constexpr size_t kProbeBytes = sizeof(Params) * 4;
  alignas(std::max_align_t) unsigned char storage[kProbeBytes];
  memset(storage, kCacheConfigProbeCanary, sizeof(storage));
  Params* probe = reinterpret_cast<Params*>(storage);
  init(probe);

  if (probe->struct_size != sizeof(Params)) {
    StrAppend(error, "the optimizer daemon library at ", library_path,
              " does not honour the size it is given for ", what,
              " (it reports ",
              Integer64ToString(static_cast<int64>(probe->struct_size)),
              " for a struct of ",
              Integer64ToString(static_cast<int64>(sizeof(Params))),
              " bytes); refusing to use it");
    return false;
  }
  for (size_t i = sizeof(Params); i < sizeof(storage); ++i) {
    if (storage[i] != kCacheConfigProbeCanary) {
      StrAppend(error, "the optimizer daemon library at ", library_path,
                " wrote past the size it was given while initialising ", what,
                "; refusing to use it");
      return false;
    }
  }
  return true;
}

}  // namespace

DaemonAbi* LoadDaemonAbi(StringPiece library_path, GoogleString* error) {
#if defined(_WIN32) && !defined(_WIN64)
  // The peer publishes a 64-bit layout only: in a 32-bit process every
  // size_t and pointer in those structs is 4 bytes, and the layout guard is
  // compiled out there, so binding would mis-marshal silently.  Refuse
  // instead, with the same sentence this platform has always had.
  StrAppend(error,
            "run-time binding of the optimizer daemon library is not "
            "implemented on this platform (a 32-bit process: the peer's "
            "layout is 64-bit only; requested: ",
            library_path, ")");
  return nullptr;
#else
  const GoogleString path(library_path.data(), library_path.size());
  void* handle = NativeOps::Load(path, error);
  if (handle == nullptr) {
    return nullptr;
  }

  std::unique_ptr<LibraryDaemonAbi> abi =
      std::make_unique<LibraryDaemonAbi>(handle);
  // Every refusal below frees the handle: a REJECTED library was never
  // accepted into use, so it must not stay mapped (and, on Windows, locked)
  // for the process's life.  The two operations are deliberately distinct:
  // a successfully returned binding is never freed anywhere (Close), a
  // rejected one is freed here (CloseUnused).  POSIX has always freed on
  // this path, through the destructor; this is the same effect, named.
  const auto reject = [&abi, handle]() -> DaemonAbi* {
    // Forget first: the object must never name a freed module, even
    // transiently.
    abi->ForgetHandle();
    NativeOps::CloseUnused(handle);
    return nullptr;
  };

  // The version gate runs FIRST, before the rest of the surface is bound: a
  // daemon below the floor is missing entry points by definition, and a
  // missing-symbol message about one of them describes the symptom rather
  // than the cause.  See BindVersion.
  if (!abi->BindVersion(error)) {
    return reject();
  }
  const int major = abi->VersionMajor();
  const int minor = abi->VersionMinor();
  if (major != kRequiredAbiMajor || minor < kRequiredAbiMinor) {
    StrAppend(error, "the optimizer daemon library at ", path, " reports API ",
              IntegerToString(major), ".", IntegerToString(minor),
              ", which this build cannot use (needs ",
              IntegerToString(kRequiredAbiMajor), ".",
              IntegerToString(kRequiredAbiMinor), " or a later minor)");
    return reject();
  }

  if (!abi->BindAll(error)) {
    return reject();
  }
  abi->BindOptional();

  // The layout handshake.  Run against an OVER-ALLOCATED, canary-filled
  // buffer, because the size-unaware initializer writes its own struct's
  // worth before anything here can look at the result -- see the note in
  // daemon_abi.h.  Checking on a bare PsCacheConfig would be checking for an
  // overrun that had already happened, in the server's parent process.
  alignas(std::max_align_t) unsigned char probe_storage[kCacheConfigProbeBytes];
  memset(probe_storage, kCacheConfigProbeCanary, sizeof(probe_storage));
  PsCacheConfig* probe = reinterpret_cast<PsCacheConfig*>(probe_storage);
  abi->CacheConfigInit(probe);

  if (abi->HasSizedCacheConfigInit()) {
    // On this path `struct_size` cannot tell us anything: the sized spelling
    // records the number IT WAS GIVEN, and we gave it sizeof(PsCacheConfig),
    // so comparing the two only asks whether the peer can echo. What IS worth
    // checking is the promise that made this path preferable in the first
    // place -- that it writes nothing past the size it was told.
    for (size_t i = sizeof(PsCacheConfig); i < sizeof(probe_storage); ++i) {
      if (probe_storage[i] != kCacheConfigProbeCanary) {
        StrAppend(error, "the optimizer daemon library at ", path,
                  " wrote past the size it was given while initialising a "
                  "cache configuration; refusing to use it");
        return reject();
      }
    }
  } else if (probe->struct_size != sizeof(PsCacheConfig)) {
    // Size-unaware path: the peer stamped ITS OWN sizeof, so a mismatch is a
    // real layout disagreement rather than an echo.
    StrAppend(error, "the optimizer daemon library at ", path,
              " uses a cache-configuration layout this build does not know "
              "(peer reports ",
              Integer64ToString(static_cast<int64>(probe->struct_size)),
              " bytes, this build expects ",
              Integer64ToString(static_cast<int64>(sizeof(PsCacheConfig))),
              " bytes)");
    return reject();
  }

  // The record arm's parameter structs get the same treatment, once, here --
  // not on the request that first needs them.
  LibraryDaemonAbi* bound = abi.get();
  if (!ParamsHandshakeOk<PsWriteParams>(
          [bound](PsWriteParams* p) { bound->WriteParamsInit(p); },
          "cache-write parameters", path, error) ||
      !ParamsHandshakeOk<PsNotifyParams>(
          [bound](PsNotifyParams* p) { bound->NotifyParamsInit(p); },
          "notification parameters", path, error)) {
    return reject();
  }

  return abi.release();
#endif
}

}  // namespace net_instaweb
