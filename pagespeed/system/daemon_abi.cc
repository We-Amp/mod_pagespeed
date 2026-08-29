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

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

#ifndef _WIN32
#include <dlfcn.h>
#endif

namespace net_instaweb {

const char kDefaultDaemonLibraryName[] = "libpagespeed.so";

DaemonAbi::DaemonAbi() = default;
DaemonAbi::~DaemonAbi() = default;

namespace {

#ifndef _WIN32

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
using ServeStatsCloseFn = void (*)(void*);

class DlopenDaemonAbi : public DaemonAbi {
 public:
  explicit DlopenDaemonAbi(void* handle) : handle_(handle) {}

  ~DlopenDaemonAbi() override {
    if (handle_ != nullptr) {
      dlclose(handle_);
    }
  }

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
           Bind("ps_serve_stats_close", &serve_stats_close_, error);
  }

  // Entry points that may legitimately be absent because the installed daemon
  // predates them.  Absence is recorded, never an error: the adapter decides
  // what each missing capability costs, and for the volume-size reader the
  // answer is "degrade", not "fail to start".
  void BindOptional() {
    dlerror();
    cache_config_init_sized_ = reinterpret_cast<CacheConfigInitSizedFn>(
        dlsym(handle_, "ps_cache_config_init_sized"));
    dlerror();
    shared_config_volume_size_ = reinterpret_cast<SharedConfigVolumeSizeFn>(
        dlsym(handle_, "ps_read_shared_config_volume_size"));
    dlerror();
    shared_config_generation_ = reinterpret_cast<SharedConfigGenerationFn>(
        dlsym(handle_, "ps_read_shared_config_generation"));
    dlerror();
    // The per-failure explanation.  Optional on the same terms as the two
    // above and for a smaller stake: its absence costs one clause in an error
    // line, so refusing a library over it would trade a working degrade for a
    // server that will not start.
    last_error_message_ = reinterpret_cast<LastErrorMessageFn>(
        dlsym(handle_, "ps_last_error_message"));
    dlerror();
  }

 private:
  template <typename Fn>
  bool Bind(const char* symbol, Fn* out, GoogleString* error) {
    dlerror();  // Clear any stale condition before the lookup.
    void* address = dlsym(handle_, symbol);
    if (address == nullptr) {
      const char* why = dlerror();
      StrAppend(error, "the optimizer daemon library does not export ", symbol,
                why == nullptr ? "" : ": ", why == nullptr ? "" : why);
      return false;
    }
    // A data pointer to function pointer conversion is what dlsym's contract
    // is; POSIX blesses it and there is no narrower spelling available.
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

#endif  // !_WIN32

}  // namespace

DaemonAbi* LoadDaemonAbi(StringPiece library_path, GoogleString* error) {
#ifdef _WIN32
  StrAppend(error,
            "run-time binding of the optimizer daemon library is not "
            "implemented on this platform (requested: ",
            library_path, ")");
  return nullptr;
#else
  const GoogleString path(library_path.data(), library_path.size());
  // RTLD_LOCAL: the daemon library's symbols stay private to this handle so
  // they can never satisfy an unrelated lookup elsewhere in the server.
  void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr) {
    const char* why = dlerror();
    StrAppend(error, "cannot load the optimizer daemon library ", path,
              why == nullptr ? "" : ": ", why == nullptr ? "" : why);
    return nullptr;
  }

  std::unique_ptr<DlopenDaemonAbi> abi =
      std::make_unique<DlopenDaemonAbi>(handle);

  // The version gate runs FIRST, before the rest of the surface is bound: a
  // daemon below the floor is missing entry points by definition, and a
  // missing-symbol message about one of them describes the symptom rather
  // than the cause.  See BindVersion.
  if (!abi->BindVersion(error)) {
    return nullptr;
  }
  const int major = abi->VersionMajor();
  const int minor = abi->VersionMinor();
  if (major != kRequiredAbiMajor || minor < kRequiredAbiMinor) {
    StrAppend(error, "the optimizer daemon library at ", path, " reports API ",
              IntegerToString(major), ".", IntegerToString(minor),
              ", which this build cannot use (needs ",
              IntegerToString(kRequiredAbiMajor), ".",
              IntegerToString(kRequiredAbiMinor), " or a later minor)");
    return nullptr;
  }

  if (!abi->BindAll(error)) {
    return nullptr;
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
        return nullptr;
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
    return nullptr;
  }

  // The record arm's parameter structs get the same treatment, once, here --
  // not on the request that first needs them.
  DlopenDaemonAbi* bound = abi.get();
  if (!ParamsHandshakeOk<PsWriteParams>(
          [bound](PsWriteParams* p) { bound->WriteParamsInit(p); },
          "cache-write parameters", path, error) ||
      !ParamsHandshakeOk<PsNotifyParams>(
          [bound](PsNotifyParams* p) { bound->NotifyParamsInit(p); },
          "notification parameters", path, error)) {
    return nullptr;
  }

  return abi.release();
#endif  // _WIN32
}

}  // namespace net_instaweb
