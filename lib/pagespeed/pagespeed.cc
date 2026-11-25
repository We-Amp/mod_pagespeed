// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// pagespeed.cc -- C API implementation for libpagespeed.so
//
// Every public extern "C" function wraps its body in try/catch(...)
// to prevent C++ exceptions from crossing the C ABI boundary.

#include "lib/pagespeed/pagespeed.h"

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "lib/base/message_handler.h"
#include "lib/base/string_writer.h"
#include "lib/cache/cache.h"
#include "lib/cache/cache_control_header.h"
#include "lib/cache/freshness.h"
#include "lib/cache/headers_sidecar.h"
#include "lib/cache/html_cache_decision.h"
#include "lib/cache/html_cache_policy.h"
#include "lib/cache/html_withheld_marker.h"
#include "lib/cache/origin_cache_control.h"
#include "lib/cache/vary_storability.h"
#include "lib/classify/alternate_id.h"
#include "lib/classify/alternate_metadata.h"
#include "lib/classify/capability_mask.h"
#include "lib/classify/content_type.h"
#include "lib/classify/hostname.h"
#include "lib/classify/option_context.h"
#include "lib/classify/pagespeed_selector.h"
#include "lib/css/css_import_flattener.h"
#include "lib/css/css_minify.h"
#include "lib/html/compat/message_handler.h"
#include "lib/html/html_keywords.h"
#include "lib/html/html_parse.h"
#include "lib/html/html_writer_filter.h"
#include "lib/pagespeed/pagespeed_internal.h"
#include "sha256.hpp"
#include "src/product_version/build_commit.h"
#include "src/product_version/version.h"
#include "src/proto/notification_sender.h"
#include "src/proto/worker_ipc.h"
#include "src/worker/async_css_loader.h"
#include "src/worker/critical_css_extractor.h"
#include "src/worker/html_css_injector.h"
#include "src/worker/html_mode.h"
#include "src/worker/html_scanner.h"
#include "src/worker/html_transform_filter.h"
#include "src/worker/serve_stats.h"
#include "src/worker/shared_config.h"

using pagespeed::AlternateId;
using pagespeed::AlternateMetadata;
using pagespeed::CapabilityMask;
using pagespeed::ContentType;
using pagespeed::HeaderField;
using pagespeed::SentinelId;

// ================================================================
// PS_API 1.8 struct-size pins for legacy initializers
// ================================================================
//
// These constants freeze the PS_API 1.8 prefix length so that the legacy forms
// stay behavior-preserving at this release, while a future struct append forces
// a developer decision (the static_assert fires) rather than silently breaking
// the contract.

constexpr size_t kCacheConfigSizeV1_8 = 48;        // ps_cache_config_t
constexpr size_t kHtmlConfigSizeV1_8 = 128;        // ps_html_config_t
constexpr size_t kCriticalCssConfigSizeV1_8 = 88;  // ps_critical_css_config_t
static_assert(
    kCacheConfigSizeV1_8 == sizeof(ps_cache_config_t),
    "ps_cache_config_t's 1.8 size is 48 bytes. This equality is the "
    "this-release proof; if it fires after you APPENDED a field, keep "
    "the constant at 48 and relax to the >= form (kWriteParamsSizeV1_1 "
    "is the pattern); if you did not intend growth, a field changed "
    "size or was inserted.");
// ps_html_config_t has grown past its 1.8 size (scheme, PS_API 1.12), so the
// legacy initializer keeps writing and stamping the 128-byte prefix and the
// new field is read only when struct_size covers it.
static_assert(kHtmlConfigSizeV1_8 <= sizeof(ps_html_config_t),
              "ps_html_config_t's 1.8 size is 128 bytes; fields are only ever "
              "appended after it.");
static_assert(offsetof(ps_html_config_t, scheme) >= kHtmlConfigSizeV1_8,
              "ps_html_config_t.scheme must lie past the 1.8 prefix, or a "
              "legacy caller's struct_size would claim it.");
static_assert(
    kCriticalCssConfigSizeV1_8 == sizeof(ps_critical_css_config_t),
    "ps_critical_css_config_t's 1.8 size is 88 bytes. This equality is the "
    "this-release proof; if it fires after you APPENDED a field, keep "
    "the constant at 88 and relax to the >= form (kWriteParamsSizeV1_1 "
    "is the pattern); if you did not intend growth, a field changed "
    "size or was inserted.");

// ================================================================
// Version
// ================================================================

extern "C" int ps_version_major(void) { return PS_API_VERSION_MAJOR; }
extern "C" int ps_version_minor(void) { return PS_API_VERSION_MINOR; }
extern "C" int ps_version_patch(void) { return PS_API_VERSION_PATCH; }
extern "C" const char* ps_git_commit(void) {
  return pagespeed::kBuildCommitShort;
}
extern "C" const char* ps_product_version(void) {
  return pagespeed::kPageSpeedVersion;
}
extern "C" const char* ps_async_css_loader_path(void) {
  // kAsyncCssLoaderPath is a string_view over a null-terminated literal.
  return pagespeed::kAsyncCssLoaderPath.data();
}
extern "C" const char* ps_async_css_loader_js(void) {
  return pagespeed::kAsyncCssLoaderJs.data();
}

// ================================================================
// Error codes
// ================================================================

extern "C" const char* ps_error_name(ps_error_t err) {
  switch (err) {
    case PS_OK:
      return "PS_OK";
    case PS_ERR_NOT_FOUND:
      return "PS_ERR_NOT_FOUND";
    case PS_ERR_IO:
      return "PS_ERR_IO";
    case PS_ERR_CORRUPTED:
      return "PS_ERR_CORRUPTED";
    case PS_ERR_NO_SPACE:
      return "PS_ERR_NO_SPACE";
    case PS_ERR_INVALID_ARG:
      return "PS_ERR_INVALID_ARG";
    case PS_ERR_BUSY:
      return "PS_ERR_BUSY";
    case PS_ERR_CLOSED:
      return "PS_ERR_CLOSED";
    case PS_ERR_TOO_MANY_ALTERNATES:
      return "PS_ERR_TOO_MANY_ALTERNATES";
    case PS_ERR_EXISTS:
      return "PS_ERR_EXISTS";
    case PS_ERR_NOT_OWNED:
      return "PS_ERR_NOT_OWNED";
    case PS_ERR_VERSION_MISMATCH:
      return "PS_ERR_VERSION_MISMATCH";
    case PS_ERR_INTERNAL:
      return "PS_ERR_INTERNAL";
    default:
      return "PS_ERR_UNKNOWN";
  }
}

extern "C" const char* ps_strerror(ps_error_t err) {
  switch (err) {
    case PS_OK:
      return "Success";
    case PS_ERR_NOT_FOUND:
      return "Not found";
    case PS_ERR_IO:
      return "Input/output error";
    case PS_ERR_CORRUPTED:
      return "Data corrupted";
    case PS_ERR_NO_SPACE:
      return "No space available";
    case PS_ERR_INVALID_ARG:
      return "Invalid argument";
    case PS_ERR_BUSY:
      return "Resource busy";
    case PS_ERR_CLOSED:
      return "Handle closed";
    case PS_ERR_TOO_MANY_ALTERNATES:
      return "Too many alternates";
    case PS_ERR_EXISTS:
      return "Already exists";
    case PS_ERR_NOT_OWNED:
      return "Not owned by this process";
    case PS_ERR_VERSION_MISMATCH:
      return "Version mismatch";
    case PS_ERR_INTERNAL:
      return "Internal error";
    default:
      return "Unknown error";
  }
}

extern "C" const char* ps_last_error_message(void) {
  return g_last_error_message.c_str();
}

// ================================================================
// Content types
// ================================================================

extern "C" ps_content_type_t ps_classify_content_type(
    const char* content_type_header) {
  ClearLastError();
  if (content_type_header == nullptr) return PS_CONTENT_OTHER;
  auto ct = pagespeed::ClassifyContentType(content_type_header);
  return static_cast<ps_content_type_t>(ct);
}

extern "C" const char* ps_content_type_mime(ps_content_type_t type) {
  auto ct = static_cast<ContentType>(type);
  auto sv = pagespeed::ContentTypeMime(ct);
  // ContentTypeMime returns static string_views pointing to literals.
  return sv.data();
}

// ================================================================
// Classification
// ================================================================

extern "C" uint32_t ps_classify(const char* accept, const char* user_agent,
                                const char* save_data,
                                const char* accept_encoding) {
  ClearLastError();
  try {
    auto mask = CapabilityMask::FromHeaders(
        (accept != nullptr) ? accept : "",
        (user_agent != nullptr) ? user_agent : "",
        (save_data != nullptr) ? save_data : "",
        (accept_encoding != nullptr) ? accept_encoding : "");
    return mask.Encode();
  } catch (...) {
    return CapabilityMask().Encode();
  }
}

extern "C" int ps_wants_agent_markdown(const char* accept) {
  return pagespeed::WantsAgentMarkdown(accept != nullptr ? accept : "") ? 1 : 0;
}

extern "C" uint32_t ps_mask_set_viewport_from_width(uint32_t mask,
                                                    uint16_t width_px) {
  auto cap = CapabilityMask::Decode(mask);
  // Boundaries mirror CapabilityMask::ViewportWidthRange (Tablet 480-1279,
  // Desktop 1280-65535); keep them in lockstep so the C ABI and the engine's
  // UA-based classifier agree on the 1024-1279px band (e.g. iPad-Pro portrait).
  if (width_px < 480)
    cap.set_viewport(CapabilityMask::Viewport::kMobile);
  else if (width_px < 1280)
    cap.set_viewport(CapabilityMask::Viewport::kTablet);
  else
    cap.set_viewport(CapabilityMask::Viewport::kDesktop);
  return cap.Encode();
}

extern "C" int ps_score_alternate(uint32_t client_mask, uint32_t stored_mask) {
  return pagespeed::ScoreAlternate(client_mask, stored_mask);
}

extern "C" int ps_normalize_hostname(const char* hostname, char* out_buf,
                                     size_t buf_size) {
  ClearLastError();
  if ((hostname == nullptr) || (out_buf == nullptr) || buf_size == 0) return -1;
  try {
    std::string normalized = pagespeed::NormalizeHostname(hostname);
    size_t copy_len = std::min(normalized.size(), buf_size - 1);
    std::memcpy(out_buf, normalized.data(), copy_len);
    out_buf[copy_len] = '\0';
    return static_cast<int>(normalized.size());
  } catch (...) {
    out_buf[0] = '\0';
    return -1;
  }
}

// ================================================================
// Cache config
// ================================================================

extern "C" void ps_cache_config_init_sized(ps_cache_config_t* config,
                                           size_t size) {
  if (config == nullptr) return;
  // Same rule as ps_write_params_init_sized / ps_notify_params_init_sized:
  // below the width of struct_size there is nowhere to record the size, and
  // stamping it would be the overrun this entry point exists to prevent.
  if (size < sizeof(size_t)) return;
  // Fill a local of the size THIS build knows, then copy back only as many
  // bytes as the caller said they own.  Writing the fields directly through
  // `config` would overrun a caller whose struct is shorter, which is the
  // whole defect being fixed.
  ps_cache_config_t full;
  // Zeroed FIRST, and this matters beyond tidiness: ps_cache_config_init
  // assigns fields one by one and never touches the padding between them, so
  // a local it filled would carry whatever the stack held in those bytes.
  // Copying that to the caller would hand out indeterminate padding that
  // differs run to run. Zeroing makes what this writes a pure function of the
  // library's defaults, which is what the _sized contract has to be.
  std::memset(&full, 0, sizeof(full));
  ps_cache_config_init(&full);
  // The caller's struct_size is theirs, not ours: it describes the buffer
  // they actually allocated. Note what that buys and what it does not: it
  // bounds what THIS INITIALIZER writes, and nothing else. ps_cache_open
  // does not consult struct_size — it reads every field it knows about — so
  // a caller whose struct is shorter than this library's must still own
  // enough buffer for the open call, not just for the init.
  full.struct_size = size;
  const size_t copy = size < sizeof(full) ? size : sizeof(full);
  std::memcpy(config, &full, copy);
  if (size > sizeof(full)) {
    // The caller's struct is LONGER than this library's: zero the tail rather
    // than leave fields we know nothing about holding stack garbage.
    std::memset(reinterpret_cast<unsigned char*>(config) + sizeof(full), 0,
                size - sizeof(full));
  }
}

extern "C" void ps_html_config_init_sized(ps_html_config_t* config,
                                          size_t size) {
  if (config == nullptr) return;
  // Same rule as ps_write_params_init_sized / ps_notify_params_init_sized:
  // below the width of struct_size there is nowhere to record the size, and
  // stamping it would be the overrun this entry point exists to prevent.
  if (size < sizeof(size_t)) return;
  // Fill a local of the size THIS build knows, then copy back only as many
  // bytes as the caller said they own.  Writing the fields directly through
  // `config` would overrun a caller whose struct is shorter, which is the
  // whole defect being fixed.
  ps_html_config_t full;
  // Zeroed FIRST, and this matters beyond tidiness: ps_html_config_init
  // does memset + field assignments, so a local it filled would carry
  // whatever the stack held in padding bytes. Copying that to the caller
  // would hand out indeterminate padding that differs run to run. Zeroing
  // makes what this writes a pure function of the library's defaults, which
  // is what the _sized contract has to be.
  std::memset(&full, 0, sizeof(full));
  ps_html_config_init(&full);
  // The caller's struct_size is theirs, not ours: it describes the buffer
  // they actually allocated. Note what that buys and what it does not: it
  // bounds what THIS INITIALIZER writes, and nothing else. ps_html_process
  // reads struct_size only to decide whether `scheme` (PS_API 1.12) is
  // present, and reads every earlier field it knows about, so a caller whose
  // struct is shorter must still own the 128-byte prefix for the process
  // call, not just for the init.
  full.struct_size = size;
  const size_t copy = size < sizeof(full) ? size : sizeof(full);
  std::memcpy(config, &full, copy);
  if (size > sizeof(full)) {
    // The caller's struct is LONGER than this library's: zero the tail rather
    // than leave fields we know nothing about holding stack garbage.
    std::memset(reinterpret_cast<unsigned char*>(config) + sizeof(full), 0,
                size - sizeof(full));
  }
}

extern "C" void ps_critical_css_config_init_sized(
    ps_critical_css_config_t* config, size_t size) {
  if (config == nullptr) return;
  // Same rule as ps_write_params_init_sized / ps_notify_params_init_sized:
  // below the width of struct_size there is nowhere to record the size, and
  // stamping it would be the overrun this entry point exists to prevent.
  if (size < sizeof(size_t)) return;
  // Fill a local of the size THIS build knows, then copy back only as many
  // bytes as the caller said they own.  Writing the fields directly through
  // `config` would overrun a caller whose struct is shorter, which is the
  // whole defect being fixed.
  ps_critical_css_config_t full;
  // Zeroed FIRST, and this matters beyond tidiness: ps_critical_css_config_init
  // does memset + field assignments, so a local it filled would carry
  // whatever the stack held in padding bytes. Copying that to the caller
  // would hand out indeterminate padding that differs run to run. Zeroing
  // makes what this writes a pure function of the library's defaults, which
  // is what the _sized contract has to be.
  std::memset(&full, 0, sizeof(full));
  ps_critical_css_config_init(&full);
  // The caller's struct_size is theirs, not ours: it describes the buffer
  // they actually allocated. Note what that buys and what it does not: it
  // bounds what THIS INITIALIZER writes, and nothing else. ps_css_extract_critical
  // does not consult struct_size — it reads every field it knows about — so
  // a caller whose struct is shorter than this library's must still own
  // enough buffer for the extraction call, not just for the init.
  full.struct_size = size;
  const size_t copy = size < sizeof(full) ? size : sizeof(full);
  std::memcpy(config, &full, copy);
  if (size > sizeof(full)) {
    // The caller's struct is LONGER than this library's: zero the tail rather
    // than leave fields we know nothing about holding stack garbage.
    std::memset(reinterpret_cast<unsigned char*>(config) + sizeof(full), 0,
                size - sizeof(full));
  }
}

extern "C" void ps_cache_config_init(ps_cache_config_t* config) {
  if (config == nullptr) return;
  config->struct_size = kCacheConfigSizeV1_8;
  config->volume_path = nullptr;
  config->volume_size = 1ULL * 1024 * 1024 * 1024;  // 1GB
  config->enable_checksum = 1;
  // Default the per-process RAM tier OFF for serve surfaces (issue #652):
  // it is keyed (CacheKey, AlternateId) with no validation against the
  // on-disk document and is only evicted by the *removing* process, so a
  // worker-side purge followed by a re-write at the same slot would keep
  // serving pre-purge bytes+metadata from RAM indefinitely.  Mirrors the
  // nginx module's MakeNginxCacheConfig().  Callers may still opt in
  // explicitly; re-enable the default once Cyclone validates RAM hits
  // against the directory entry (a Cyclone follow-up).
  config->ram_cache_size = 0;
  // Derived from the metadata format's worst case; see the prefix-budget
  // block in lib/classify/alternate_metadata.h.  Not an ABI change — the
  // field's offset and width are unchanged, only the value this initializer
  // writes into the caller's buffer.
  config->max_metadata_size =
      pagespeed::AlternateMetadata::kRecommendedMaxMetadataSize;
}

// ================================================================
// Cache open / close
// ================================================================

namespace {

// Remembers the most recent warning or error the cache logs while it opens, so
// ps_cache_open can hand the real reason (for example a refused volume file)
// to the caller through ps_last_error_message(). It lives on the stack of
// ps_cache_open and is detached from the cache before that returns.
class OpenReasonHandler final : public pagespeed::MessageHandler {
 public:
  OpenReasonHandler() { SetMinLogLevel(pagespeed::LogLevel::kWarning); }

  void Message(pagespeed::MessageType type, const char* format, ...) override {
    va_list args;
    va_start(args, format);
    MessageV(type, format, args);
    va_end(args);
  }

  const std::string& last_reason() const { return last_reason_; }

 protected:
  void MessageV(pagespeed::MessageType type, const char* format,
                va_list args) override {
    if (MessageTypeToLogLevel(type) < min_log_level()) return;
    last_reason_ = FormatMessage(format, args);
  }

 private:
  std::string last_reason_;
};

// Builds the text ps_last_error_message() reports when opening the cache
// fails: the cache library's own status message, plus the last warning or
// error it logged while opening, when there was one.
std::string DescribeCacheOpenFailure(cyclone::CacheError err,
                                     const std::string& logged_reason) {
  std::string msg = "Failed to create cache: ";
  msg += make_error_code(err).message();
  if (!logged_reason.empty()) {
    msg += " (";
    msg += logged_reason;
    msg += ")";
  }
  return msg;
}

}  // namespace

extern "C" ps_error_t ps_cache_open(const ps_cache_config_t* config,
                                    ps_cache_t** out_cache) {
  ClearLastError();
  if ((config == nullptr) || (out_cache == nullptr)) return PS_ERR_INVALID_ARG;
  if (config->volume_path == nullptr) {
    SetLastError("volume_path is NULL");
    return PS_ERR_INVALID_ARG;
  }

  try {
    pagespeed::PageSpeedCacheConfig cpp_config;
    cpp_config.volume_path = config->volume_path;
    cpp_config.volume_size = config->volume_size;
    cpp_config.enable_checksum = (config->enable_checksum != 0);
    cpp_config.ram_cache_size = config->ram_cache_size;
    cpp_config.max_metadata_size = config->max_metadata_size;
    // Always enable multi-process sharing.
    cpp_config.multi_process.enabled = true;

    OpenReasonHandler open_reason;
    cpp_config.handler = &open_reason;

    auto result = pagespeed::PageSpeedCache::Create(cpp_config);
    if (!result.has_value()) {
      SetLastError(
          DescribeCacheOpenFailure(result.error(), open_reason.last_reason()));
      return MapError(result.error());
    }
    // The logger is stack-local: the cache must not keep a pointer to it.
    (*result)->set_handler(nullptr);

    auto* c = new ps_cache_s();
    c->cache = std::move(*result);
    *out_cache = c;
    return PS_OK;
  } catch (const std::exception& e) {
    SetLastError(e.what());
    return PS_ERR_INTERNAL;
  } catch (...) {
    SetLastError("Unknown exception in ps_cache_open");
    return PS_ERR_INTERNAL;
  }
}

extern "C" void ps_cache_close(ps_cache_t* cache) { delete cache; }

// ================================================================
// Cache reads
// ================================================================

namespace {

// A site argument of the _site entry points: NULL or "" is the unsited
// keyspace (the same call, same key bytes, as the unsited entry point); 64
// lowercase hex characters is that site's keyspace; anything else is refused.
ps_error_t ParseSiteArg(const char* site, std::string_view* out) {
  *out = {};
  if (site == nullptr || site[0] == '\0') return PS_OK;
  const std::string_view view(site);
  if (!pagespeed::IsSiteId(view)) {
    SetLastError("site must be NULL, empty, or PS_SITE_ID_CHARS lowercase hex");
    return PS_ERR_INVALID_ARG;
  }
  *out = view;
  return PS_OK;
}

ps_error_t CacheReadBest(ps_cache_t* cache, const char* url,
                         const char* hostname, const char* scheme,
                         std::string_view site, uint32_t mask,
                         ps_read_result_t** out) {
  if ((cache == nullptr) || (out == nullptr)) return PS_ERR_INVALID_ARG;
  ps_error_t err = ValidateUrlHostname(url, hostname);
  if (err != PS_OK) return err;

  try {
    auto cap = CapabilityMask::Decode(mask);
    auto result = cache->cache->ReadBestAlternate(
        url, hostname, scheme, cap, /*agent_request_entitled=*/false, site);
    if (!result.has_value()) {
      return MapError(result.error());
    }

    auto* r = new ps_read_result_s();
    r->result = std::move(result.value());
    r->origin_ct_cstr = r->result.metadata.origin_content_type;
    *out = r;
    return PS_OK;
  } catch (const std::exception& e) {
    SetLastError(e.what());
    return PS_ERR_INTERNAL;
  } catch (...) {
    SetLastError("Unknown exception in ps_cache_read_best");
    return PS_ERR_INTERNAL;
  }
}

}  // namespace

extern "C" ps_error_t ps_cache_read_best(ps_cache_t* cache, const char* url,
                                         const char* hostname,
                                         const char* scheme, uint32_t mask,
                                         ps_read_result_t** out) {
  ClearLastError();
  return CacheReadBest(cache, url, hostname, scheme, {}, mask, out);
}

extern "C" ps_error_t ps_cache_read_best_site(ps_cache_t* cache,
                                              const char* url,
                                              const char* hostname,
                                              const char* scheme,
                                              const char* site, uint32_t mask,
                                              ps_read_result_t** out) {
  ClearLastError();
  std::string_view site_view;
  ps_error_t err = ParseSiteArg(site, &site_view);
  if (err != PS_OK) return err;
  return CacheReadBest(cache, url, hostname, scheme, site_view, mask, out);
}

extern "C" ps_error_t ps_cache_read_best_agent(
    ps_cache_t* cache, const char* url, const char* hostname,
    const char* scheme, uint32_t mask, int agent_entitled,
    ps_read_result_t** out) {
  ClearLastError();
  if ((cache == nullptr) || (out == nullptr)) return PS_ERR_INVALID_ARG;
  ps_error_t err = ValidateUrlHostname(url, hostname);
  if (err != PS_OK) return err;

  try {
    auto cap = CapabilityMask::Decode(mask);
    // Routes through the SINGLE audited agent serve gate (P3.1): a stale/unbound
    // markdown is refused (NotFound), so a native caller can never serve
    // superseded markdown.
    auto result = cache->cache->ReadBestAlternateAgent(
        url, hostname, (scheme != nullptr) ? scheme : "", cap,
        agent_entitled != 0);
    if (!result.has_value()) {
      return MapError(result.error());
    }

    auto* r = new ps_read_result_s();
    r->result = std::move(result.value());
    r->origin_ct_cstr = r->result.metadata.origin_content_type;
    *out = r;
    return PS_OK;
  } catch (const std::exception& e) {
    SetLastError(e.what());
    return PS_ERR_INTERNAL;
  } catch (...) {
    SetLastError("Unknown exception in ps_cache_read_best_agent");
    return PS_ERR_INTERNAL;
  }
}

extern "C" int ps_read_shared_config_agent_entitled(const char* cache_path) {
  ClearLastError();
  if (cache_path == nullptr) return -1;
  try {
    std::string path = pagespeed::SharedConfigFilePath(cache_path);
    if (path.empty()) return -1;
    pagespeed::SharedConfig sc = pagespeed::ReadSharedConfigFile(path);
    return sc.agent_optimize_entitled ? 1 : 0;
  } catch (const std::exception& e) {
    SetLastError(e.what());
    return -1;
  } catch (...) {
    return -1;
  }
}

extern "C" uint64_t ps_read_shared_config_volume_size(const char* cache_path) {
  ClearLastError();
  if (cache_path == nullptr) return 0;
  try {
    std::string path = pagespeed::SharedConfigFilePath(cache_path);
    if (path.empty()) return 0;
    pagespeed::SharedConfig sc = pagespeed::ReadSharedConfigFile(path);
    // 0 flows straight through and means "not known" — a missing file, an
    // unreadable one, a schema version this build refuses to parse, or a
    // worker predating the field.  Every one of those has to stay
    // distinguishable from a real size, because the documented contract is
    // that a caller who gets 0 declines to open rather than picking a number.
    return sc.volume_size;
  } catch (const std::exception& e) {
    SetLastError(e.what());
    return 0;
  } catch (...) {
    return 0;
  }
}

static_assert(PS_HTML_MODE_OFF == static_cast<int>(pagespeed::HtmlMode::kOff),
              "the published HTML mode values are the worker's");
static_assert(PS_HTML_MODE_MARKDOWN_STATIC ==
                  static_cast<int>(pagespeed::HtmlMode::kMarkdownStatic),
              "the published HTML mode values are the worker's");
static_assert(PS_HTML_MODE_MARKDOWN_RENDERED ==
                  static_cast<int>(pagespeed::HtmlMode::kMarkdownRendered),
              "the published HTML mode values are the worker's");
static_assert(PS_HTML_MODE_FULL == static_cast<int>(pagespeed::HtmlMode::kFull),
              "the published HTML mode values are the worker's");

extern "C" int ps_read_shared_config_html_mode(const char* cache_path) {
  ClearLastError();
  if (cache_path == nullptr) return -1;
  try {
    std::string path = pagespeed::SharedConfigFilePath(cache_path);
    if (path.empty()) return PS_HTML_MODE_FULL;
    // The shared-config reader applies the worker's own precedence rule
    // (ResolveHtmlMode): a valid html_mode wins, otherwise disable_html
    // decides, and a missing or unreadable file reads as the default, full.
    const pagespeed::SharedConfig sc = pagespeed::ReadSharedConfigFile(path);
    return static_cast<int>(sc.html_mode);
  } catch (const std::exception& e) {
    SetLastError(e.what());
    return PS_HTML_MODE_FULL;
  } catch (...) {
    return PS_HTML_MODE_FULL;
  }
}

extern "C" uint32_t ps_read_shared_config_generation(const char* cache_path) {
  ClearLastError();
  if (cache_path == nullptr) return 0;
  try {
    std::string path = pagespeed::SharedConfigFilePath(cache_path);
    if (path.empty()) return 0;
    pagespeed::SharedConfig sc = pagespeed::ReadSharedConfigFile(path);
    // 0 flows straight through and means "not known" — same contract and
    // same rationale as ps_read_shared_config_volume_size above: a caller
    // who gets 0 must not substitute a generation of its own.
    return sc.cache_dir_generation > 0
               ? static_cast<uint32_t>(sc.cache_dir_generation)
               : 0u;
  } catch (const std::exception& e) {
    SetLastError(e.what());
    return 0;
  } catch (...) {
    return 0;
  }
}

namespace {

ps_error_t CacheReadAlternate(ps_cache_t* cache, const char* url,
                              const char* hostname, const char* scheme,
                              std::string_view site, uint8_t alternate_id,
                              ps_read_result_t** out) {
  if ((cache == nullptr) || (out == nullptr)) return PS_ERR_INVALID_ARG;
  ps_error_t err = ValidateUrlHostname(url, hostname);
  if (err != PS_OK) return err;

  try {
    auto result = cache->cache->ReadAlternate(
        url, hostname, scheme, static_cast<AlternateId>(alternate_id), site);
    if (!result.has_value()) {
      return MapError(result.error());
    }

    auto* r = new ps_read_result_s();
    r->result = std::move(result.value());
    r->origin_ct_cstr = r->result.metadata.origin_content_type;
    *out = r;
    return PS_OK;
  } catch (const std::exception& e) {
    SetLastError(e.what());
    return PS_ERR_INTERNAL;
  } catch (...) {
    SetLastError("Unknown exception in ps_cache_read_alternate");
    return PS_ERR_INTERNAL;
  }
}

}  // namespace

extern "C" ps_error_t ps_cache_read_alternate(
    ps_cache_t* cache, const char* url, const char* hostname,
    const char* scheme, uint8_t alternate_id, ps_read_result_t** out) {
  ClearLastError();
  return CacheReadAlternate(cache, url, hostname, scheme, {}, alternate_id,
                            out);
}

extern "C" ps_error_t ps_cache_read_alternate_site(
    ps_cache_t* cache, const char* url, const char* hostname,
    const char* scheme, const char* site, uint8_t alternate_id,
    ps_read_result_t** out) {
  ClearLastError();
  std::string_view site_view;
  ps_error_t err = ParseSiteArg(site, &site_view);
  if (err != PS_OK) return err;
  return CacheReadAlternate(cache, url, hostname, scheme, site_view,
                            alternate_id, out);
}

extern "C" ps_error_t ps_cache_read_early_hints(ps_cache_t* cache,
                                                const char* url,
                                                const char* hostname,
                                                const char* scheme,
                                                ps_read_result_t** out) {
  return ps_cache_read_alternate(cache, url, hostname, scheme,
                                 PS_SENTINEL_EARLY_HINTS, out);
}

extern "C" ps_error_t ps_read_content(const ps_read_result_t* result,
                                      const uint8_t** out_data,
                                      size_t* out_length) {
  ClearLastError();
  if ((result == nullptr) || (out_data == nullptr) || (out_length == nullptr))
    return PS_ERR_INVALID_ARG;
  auto content = result->result.content();
  *out_data = reinterpret_cast<const uint8_t*>(content.data());
  *out_length = content.size();
  return PS_OK;
}

extern "C" ps_error_t ps_read_copy(const ps_read_result_t* result, void* buf,
                                   size_t buf_size, size_t* out_copied) {
  ClearLastError();
  if ((result == nullptr) || (buf == nullptr) || (out_copied == nullptr))
    return PS_ERR_INVALID_ARG;
  auto content = result->result.content();
  size_t copy_len = std::min(content.size(), buf_size);
  std::memcpy(buf, content.data(), copy_len);
  *out_copied = copy_len;
  return PS_OK;
}

extern "C" uint32_t ps_read_mask(const ps_read_result_t* result) {
  if (result == nullptr) return 0;
  return result->result.metadata.full_mask;
}

extern "C" ps_content_type_t ps_read_content_type(
    const ps_read_result_t* result) {
  if (result == nullptr) return PS_CONTENT_OTHER;
  return static_cast<ps_content_type_t>(result->result.metadata.content_type);
}

extern "C" const char* ps_read_origin_content_type(
    const ps_read_result_t* result) {
  if (result == nullptr) return nullptr;
  if (result->origin_ct_cstr.empty()) return nullptr;
  return result->origin_ct_cstr.c_str();
}

extern "C" const uint8_t* ps_read_origin_html_hash(
    const ps_read_result_t* result) {
  if (result == nullptr) return nullptr;
  const auto& h = result->result.metadata.origin_html_hash;
  // All-zero = unbound (pre-v7 metadata or a variant written without a binding).
  bool all_zero = std::all_of(h.begin(), h.end(),
                              [](std::byte b) { return b == std::byte{0}; });
  if (all_zero) return nullptr;
  return reinterpret_cast<const uint8_t*>(h.data());
}

extern "C" uint8_t ps_read_flags(const ps_read_result_t* result) {
  if (result == nullptr) return 0;
  return result->result.metadata.flags;
}

extern "C" int ps_read_is_ram_hit(const ps_read_result_t* result) {
  // Cyclone's ReadHandle does not expose RAM hit info.
  (void)result;
  return 0;
}

extern "C" uint32_t ps_read_cache_inserted_at(const ps_read_result_t* result) {
  if (result == nullptr) return 0;
  return result->result.metadata.cache_inserted_at;
}

extern "C" uint32_t ps_read_origin_content_length(
    const ps_read_result_t* result) {
  if (result == nullptr) return 0;
  return result->result.metadata.origin_content_length;
}

extern "C" int ps_read_encoded_origin_length(const ps_read_result_t* result,
                                             uint64_t* out) {
  if (result == nullptr || out == nullptr) return 0;
  const auto encoded = result->result.metadata.EncodedOriginLength();
  if (!encoded.has_value()) return 0;
  *out = *encoded;
  return 1;
}

extern "C" uint32_t ps_read_origin_max_age(const ps_read_result_t* result) {
  if (result == nullptr) return 0;
  return result->result.metadata.origin_max_age;
}

extern "C" uint32_t ps_read_origin_s_maxage(const ps_read_result_t* result) {
  if (result == nullptr) return 0;
  return result->result.metadata.origin_s_maxage;
}

extern "C" uint16_t ps_read_origin_cc_flags(const ps_read_result_t* result) {
  if (result == nullptr) return 0;
  return result->result.metadata.origin_cc_flags;
}

extern "C" uint32_t ps_read_origin_last_modified(
    const ps_read_result_t* result) {
  if (result == nullptr) return 0;
  return result->result.metadata.origin_last_modified;
}

extern "C" const char* ps_read_origin_etag(const ps_read_result_t* result) {
  if (result == nullptr) return nullptr;
  // Owned by the metadata inside `result`, so the pointer stays valid for the
  // result's lifetime — the same contract ps_read_origin_content_type has.
  const std::string& etag = result->result.metadata.origin_etag;
  if (etag.empty()) return nullptr;
  return etag.c_str();
}

extern "C" int ps_read_is_worker_processed(const ps_read_result_t* result) {
  if (result == nullptr) return 0;
  return (result->result.metadata.flags &
          AlternateMetadata::kFlagWorkerProcessed) != 0
             ? 1
             : 0;
}

extern "C" int ps_read_renew_lease(ps_read_result_t* result) {
  if (result == nullptr) return 0;
  return result->result.renew_lease() ? 1 : 0;
}

extern "C" void ps_read_free(ps_read_result_t* result) { delete result; }

// ================================================================
// Serve stats
// ================================================================

struct ps_serve_stats_s {
  pagespeed::ServeStats* stats;
};

extern "C" ps_error_t ps_serve_stats_open(const char* cache_path,
                                          ps_serve_stats_t** out) {
  ClearLastError();
  if ((cache_path == nullptr) || (out == nullptr)) return PS_ERR_INVALID_ARG;
  *out = nullptr;

  const std::string path = pagespeed::ServeStatsPath(cache_path);
  pagespeed::ServeStats* stats = pagespeed::OpenServeStats(path);
  if (stats == nullptr) {
    // The worker is the sole creator; until it has created the file, signal
    // not-found so callers can open lazily / retry (mirrors nginx's lazy
    // g_serve_stats open). OpenServeStats also returns nullptr for a present
    // but corrupt/wrong-version file, but the dominant case for a front-end
    // is "worker not started yet," which .NET maps to PageSpeedError.NotFound.
    SetLastError("serve-stats file not available (worker not started?)");
    return PS_ERR_NOT_FOUND;
  }

  auto* handle = new ps_serve_stats_s{stats};
  *out = handle;
  return PS_OK;
}

extern "C" void ps_serve_stats_record_hit(ps_serve_stats_t* h,
                                          ps_content_type_t content_type,
                                          uint64_t original_bytes,
                                          uint64_t optimized_bytes,
                                          uint32_t mask) {
  if (h == nullptr) return;
  pagespeed::RecordServeHit(h->stats, static_cast<ContentType>(content_type),
                            original_bytes, optimized_bytes, mask);
}

extern "C" void ps_serve_stats_record_hit_host(ps_serve_stats_t* h,
                                               ps_content_type_t content_type,
                                               uint64_t original_bytes,
                                               uint64_t optimized_bytes,
                                               uint32_t mask, const char* host,
                                               size_t host_len) {
  if (h == nullptr) return;
  const std::string_view host_view =
      host == nullptr ? std::string_view() : std::string_view(host, host_len);
  pagespeed::RecordServeHit(h->stats, static_cast<ContentType>(content_type),
                            original_bytes, optimized_bytes, mask, host_view);
}

extern "C" void ps_serve_stats_record_hit_split(ps_serve_stats_t* h,
                                                ps_content_type_t content_type,
                                                uint64_t original_bytes,
                                                uint64_t encoded_origin_bytes,
                                                uint64_t optimized_bytes,
                                                uint32_t mask, const char* host,
                                                size_t host_len) {
  if (h == nullptr) return;
  const std::string_view host_view =
      host == nullptr ? std::string_view() : std::string_view(host, host_len);
  pagespeed::RecordServeHitSplit(
      h->stats, static_cast<ContentType>(content_type), original_bytes,
      encoded_origin_bytes, optimized_bytes, mask, host_view);
}

extern "C" void ps_serve_stats_record_serve_class(ps_serve_stats_t* h,
                                                  ps_serve_class_t cls,
                                                  uint32_t flags) {
  if (h == nullptr) return;
  // The C enum and the C++ one carry the same disjoint-bit values; an
  // unrecognised value is dropped by RecordServeClass, so no validation here.
  pagespeed::RecordServeClass(
      h->stats, static_cast<pagespeed::ServeClass>(static_cast<uint32_t>(cls)),
      flags);
}

extern "C" ps_error_t ps_serve_stats_snapshot(const ps_serve_stats_t* h,
                                              ps_serve_stats_snapshot_t* out) {
  ClearLastError();
  if ((h == nullptr) || (out == nullptr)) return PS_ERR_INVALID_ARG;
  const size_t caller_size = out->struct_size;
  if (caller_size < sizeof(size_t)) {
    SetLastError("ps_serve_stats_snapshot: struct_size not initialized");
    return PS_ERR_INVALID_ARG;
  }
  const pagespeed::ServeStats* s = h->stats;
  if (s == nullptr) return PS_ERR_INVALID_ARG;

  // Fill a local of this build's shape, then copy back only as much as the
  // caller's build knows about, so an older caller linked against a newer
  // library reads the prefix it understands and nothing past it.
  ps_serve_stats_snapshot_t full;
  std::memset(&full, 0, sizeof(full));
  full.struct_size = std::min(caller_size, sizeof(full));

  // Plain aligned 64-bit loads, matching the reader idiom the worker's stats
  // API already uses on this mapping.
  const auto load64 = [](const uint64_t& field) -> uint64_t {
    return *reinterpret_cast<const volatile uint64_t*>(&field);
  };
  const auto load32 = [](const uint32_t& field) -> uint32_t {
    return *reinterpret_cast<const volatile uint32_t*>(&field);
  };

  full.serve_optimized_total = load64(s->serve_optimized_total);
  full.serve_original_cold_total = load64(s->serve_original_cold_total);
  full.serve_original_pending_total = load64(s->serve_original_pending_total);
  full.serve_original_declined_total = load64(s->serve_original_declined_total);
  full.serve_original_skew_total = load64(s->serve_original_skew_total);
  full.notify_suppressed_total = load64(s->notify_suppressed_total);
  full.serve_class_unrecognized_total =
      load64(s->serve_class_unrecognized_total);
  full.serve_flags_unrecognized_total =
      load64(s->serve_flags_unrecognized_total);
  // Count BEFORE accum: the pair has no cross-field atomicity, and this order
  // bounds the error at one sample in the direction that overstates the mean,
  // which is the safe direction for a saturation reading.
  full.saturation_sample_count = load64(s->saturation_sample_count);
  full.saturation_sample_accum = load64(s->saturation_sample_accum);
  full.worker_pool_threads = load32(s->worker_pool_threads);
  full.saturation_hwm = load32(s->saturation_hwm);

  std::memcpy(out, &full, full.struct_size);
  return PS_OK;
}

extern "C" void ps_serve_stats_close(ps_serve_stats_t* h) {
  if (h == nullptr) return;
  pagespeed::CloseServeStats(h->stats);
  delete h;
}

// ================================================================
// Cache writes
// ================================================================

namespace {

// The size ps_write_params_t had at PS_API 1.1 — everything up to and
// including origin_ct. It is what ps_write_params_init may touch: a caller
// compiled before 1.2 passes a struct exactly this big, and clearing the
// library's (larger) idea of the struct would run off the end of it.
constexpr size_t kWriteParamsSizeV1_1 =
    offsetof(ps_write_params_t, origin_ct) + sizeof(const char*);
static_assert(kWriteParamsSizeV1_1 == 48,
              "ps_write_params_t's 1.1 prefix is part of the shipped ABI: "
              "48 bytes, origin_ct at offset 40. If this fires, a field was "
              "inserted rather than appended.");
static_assert(sizeof(ps_write_params_t) >= kWriteParamsSizeV1_1,
              "ps_write_params_t may only grow");

// The size ps_write_params_t had from PS_API 1.2 through 1.12, ending with the
// _reserved tail. A caller built against those headers passes exactly this
// much, and gets the 1.13 origin_html_hash as NULL (unbound).
constexpr size_t kWriteParamsSizeV1_2 =
    offsetof(ps_write_params_t, origin_html_hash);
static_assert(sizeof(void*) != 8 || kWriteParamsSizeV1_2 == 80,
              "ps_write_params_t's 1.2 size is part of the shipped ABI: 80 "
              "bytes on LP64. origin_html_hash is appended after it.");
static_assert(kWriteParamsSizeV1_2 > kWriteParamsSizeV1_1);

// Snap a caller-declared struct_size DOWN to the largest published size that
// does not exceed it.
//
// The caller's number is not a length we may take on trust: it selects how many
// bytes of their buffer we read. A value that lands between two published sizes
// is not a shape this library ever defined, and copying that many bytes tears
// whatever field straddles the boundary — for ps_write_params_t that is a
// POINTER, which the write path then dereferences. Clamping to a rung of the
// published ladder means every read is of a shape that genuinely existed, so a
// partially-copied field cannot occur at all.
//
// `ladder` is ascending and its first element is the smallest published size.
// Returns 0 when the declared size is below even that — not a shape we ever
// published, so the caller is telling us something we cannot act on.
size_t ClampToPublishedSize(size_t declared, const size_t* ladder,
                            size_t ladder_len) {
  size_t chosen = 0;
  for (size_t i = 0; i < ladder_len; ++i) {
    if (ladder[i] <= declared) {
      chosen = ladder[i];
    } else {
      break;
    }
  }
  return chosen;
}

// Copy a caller's ps_write_params_t through its OWN declared size into a
// zeroed local, snapped to a published rung of the size ladder.  Shared by
// ps_cache_write_begin and ps_cache_write_original so the two cannot drift
// apart on the one piece of this API that is a live overread if it is wrong.
// Returns false when struct_size names no shape this library ever published.
bool CopyWriteParams(const ps_write_params_t* params, ps_write_params_t* out) {
  const size_t kPublishedSizes[] = {kWriteParamsSizeV1_1, kWriteParamsSizeV1_2,
                                    sizeof(ps_write_params_t)};
  const size_t usable = ClampToPublishedSize(
      params->struct_size, kPublishedSizes,
      sizeof(kPublishedSizes) / sizeof(kPublishedSizes[0]));
  if (usable == 0) return false;
  std::memset(out, 0, sizeof(*out));
  std::memcpy(out, params, std::min(usable, sizeof(*out)));
  out->struct_size = sizeof(*out);
  return true;
}

// Fill an AlternateMetadata from already-copied params.  The insertion-time
// fallback is part of it: 0 means the writer supplied none, so the local clock
// is stamped UNADJUSTED — the pre-1.2 behaviour, correct only for a response
// that came straight from the origin.  See ps_write_params_t.
AlternateMetadata MetadataFromParams(const ps_write_params_t& p) {
  AlternateMetadata meta;
  meta.full_mask = p.full_mask;
  meta.content_type = static_cast<ContentType>(p.content_type);
  meta.flags = p.flags;
  if (p.origin_ct != nullptr) meta.origin_content_type = p.origin_ct;
  if (p.origin_etag != nullptr) meta.origin_etag = p.origin_etag;
  meta.origin_max_age = p.origin_max_age;
  meta.origin_s_maxage = p.origin_s_maxage;
  meta.origin_cc_flags = p.origin_cc_flags;
  meta.origin_last_modified = p.origin_last_modified;
  meta.cache_inserted_at = p.cache_inserted_at != 0
                               ? p.cache_inserted_at
                               : static_cast<uint32_t>(std::time(nullptr));
  // 1.13: the content binding. NULL leaves the stamp all-zero (unbound), and
  // 32 zero bytes are the same value.
  if (p.origin_html_hash != nullptr) {
    std::memcpy(meta.origin_html_hash.data(), p.origin_html_hash,
                AlternateMetadata::kHashSize);
  }
  return meta;
}

}  // namespace

extern "C" void ps_write_params_init_sized(ps_write_params_t* params,
                                           size_t size) {
  if (params == nullptr) return;
  // Below the width of struct_size itself there is nowhere to record anything:
  // stamping the field would be the very overrun this entry point exists to
  // avoid. The struct is left untouched, and ps_cache_write_begin rejects it —
  // loudly, rather than acting on a size nobody set.
  if (size < sizeof(size_t)) return;
  std::memset(params, 0, size);
  params->struct_size = size;
}

extern "C" void ps_write_params_init(ps_write_params_t* params) {
  if (params == nullptr) return;
  // Deliberately NOT sizeof(*params): this entry point is what pre-1.2
  // callers link against, and their struct is only the 1.1 prefix. Writing
  // the current size would corrupt whatever follows it on their stack.
  std::memset(params, 0, kWriteParamsSizeV1_1);
  params->struct_size = kWriteParamsSizeV1_1;
}

extern "C" uint32_t ps_age_adjusted_insert_time(uint32_t now_seconds,
                                                uint32_t age_seconds) {
  if (age_seconds > 0 && age_seconds <= now_seconds) {
    return now_seconds - age_seconds;
  }
  return now_seconds;
}

extern "C" ps_error_t ps_cache_write_begin(ps_cache_t* cache, const char* url,
                                           const char* hostname,
                                           const char* scheme,
                                           const ps_write_params_t* params,
                                           ps_write_handle_t** out) {
  ClearLastError();
  if ((cache == nullptr) || (params == nullptr) || (out == nullptr))
    return PS_ERR_INVALID_ARG;

  // Take the caller's struct through its OWN declared size, snapped to a
  // published rung of the size ladder — see CopyWriteParams.
  ps_write_params_t p;
  if (!CopyWriteParams(params, &p)) {
    SetLastError(
        "ps_cache_write_begin: params->struct_size is not a published size of "
        "ps_write_params_t (uninitialized, or smaller than the 1.1 struct)");
    return PS_ERR_INVALID_ARG;
  }

  ps_error_t err = ValidateUrlHostname(url, hostname);
  if (err != PS_OK) return err;

  // Reject sentinel IDs.
  if (pagespeed::IsSentinel(p.alternate_id)) {
    SetLastError("Use ps_cache_write_sentinel for sentinel IDs");
    return PS_ERR_INVALID_ARG;
  }

  try {
    AlternateMetadata meta = MetadataFromParams(p);

    auto wh = cache->cache->WriteAlternate(
        url, hostname, scheme, static_cast<AlternateId>(p.alternate_id),
        p.content_length, meta);
    if (!wh.has_value()) {
      SetLastError("WriteAlternate failed");
      return MapError(wh.error());
    }

    auto* h = new ps_write_handle_s();
    h->write_result = std::move(*wh);
    *out = h;
    return PS_OK;
  } catch (const std::exception& e) {
    SetLastError(e.what());
    return PS_ERR_INTERNAL;
  } catch (...) {
    SetLastError("Unknown exception in ps_cache_write_begin");
    return PS_ERR_INTERNAL;
  }
}

namespace {

ps_error_t CacheWriteOriginal(ps_cache_t* cache, const char* url,
                              const char* hostname, const char* scheme,
                              std::string_view site,
                              const ps_write_params_t* params,
                              ps_write_handle_t** out) {
  if ((cache == nullptr) || (params == nullptr) || (out == nullptr))
    return PS_ERR_INVALID_ARG;

  ps_write_params_t p;
  if (!CopyWriteParams(params, &p)) {
    SetLastError(
        "ps_cache_write_original: params->struct_size is not a published size "
        "of ps_write_params_t (uninitialized, or smaller than the 1.1 struct)");
    return PS_ERR_INVALID_ARG;
  }

  ps_error_t err = ValidateUrlHostname(url, hostname);
  if (err != PS_OK) return err;

  constexpr uint8_t kOriginalId =
      static_cast<uint8_t>(SentinelId::kOriginalContent);

  // The id is the class's, not the caller's.  Zero is "not supplied" — the
  // value a caller that never set the field passes — and naming the class
  // explicitly is allowed; anything else is a caller who believes they are
  // writing something other than a durable original.
  if (p.alternate_id != 0 && p.alternate_id != kOriginalId) {
    SetLastError(
        "ps_cache_write_original writes PS_SENTINEL_ORIGINAL; leave "
        "params->alternate_id 0");
    return PS_ERR_INVALID_ARG;
  }
  // Same rule for the stored mask's low byte, which is what the entry
  // self-describes as: 0 means not supplied and is stamped for the caller.
  if ((p.full_mask & 0xFFu) != 0 && (p.full_mask & 0xFFu) != kOriginalId) {
    SetLastError(
        "ps_cache_write_original: params->full_mask's low byte must be 0 or "
        "PS_SENTINEL_ORIGINAL");
    return PS_ERR_INVALID_ARG;
  }

  try {
    AlternateMetadata meta = MetadataFromParams(p);
    meta.full_mask = (p.full_mask & ~0xFFu) | kOriginalId;

    auto wh = cache->cache->WriteOriginalAlternate(
        url, hostname, scheme, p.content_length, meta, site);
    if (!wh.has_value()) {
      // Name the cap when the cap is why. PS_ERR_NO_SPACE is shared with the
      // substrate's genuine out-of-space, and the two call for OPPOSITE
      // handling: over-cap is permanent for this response (do not retry, do
      // not alarm), out-of-space is transient. The error code cannot carry
      // that distinction without breaking every exhaustive switch downstream,
      // so the message does.
      if (wh.error() == cyclone::CacheError::NoSpace) {
        SetLastError(
            "response is larger than the durable-original content cap (" +
            std::to_string(cache->cache->MaxOriginalContentLength()) +
            " bytes); the original was not stored, which is a normal outcome "
            "and not a retryable error");
      } else {
        SetLastError("WriteOriginalAlternate failed");
      }
      return MapError(wh.error());
    }

    auto* h = new ps_write_handle_s();
    h->write_result = std::move(*wh);
    *out = h;
    return PS_OK;
  } catch (const std::exception& e) {
    SetLastError(e.what());
    return PS_ERR_INTERNAL;
  } catch (...) {
    SetLastError("Unknown exception in ps_cache_write_original");
    return PS_ERR_INTERNAL;
  }
}

}  // namespace

extern "C" ps_error_t ps_cache_write_original(ps_cache_t* cache,
                                              const char* url,
                                              const char* hostname,
                                              const char* scheme,
                                              const ps_write_params_t* params,
                                              ps_write_handle_t** out) {
  ClearLastError();
  return CacheWriteOriginal(cache, url, hostname, scheme, {}, params, out);
}

extern "C" ps_error_t ps_cache_write_original_site(
    ps_cache_t* cache, const char* url, const char* hostname,
    const char* scheme, const char* site, const ps_write_params_t* params,
    ps_write_handle_t** out) {
  ClearLastError();
  std::string_view site_view;
  ps_error_t err = ParseSiteArg(site, &site_view);
  if (err != PS_OK) return err;
  return CacheWriteOriginal(cache, url, hostname, scheme, site_view, params,
                            out);
}

namespace {

// Build the C caller's parallel name/value arrays into the header list the
// gate takes.  Returns false when the arrays are not usable — the views point
// straight at the caller's memory, so a NULL element is a crash waiting one
// call later, not a value to be lenient about.
bool CollectHeaderFields(const char* const* names, const char* const* values,
                         size_t count, std::vector<HeaderField>* out) {
  if (count == 0) return true;
  if ((names == nullptr) || (values == nullptr)) return false;
  out->reserve(count);
  for (size_t i = 0; i < count; ++i) {
    if ((names[i] == nullptr) || (values[i] == nullptr)) return false;
    out->push_back(
        HeaderField{std::string_view(names[i]), std::string_view(values[i])});
  }
  return true;
}

}  // namespace

extern "C" int ps_headers_sidecar_classify(const char* const* names,
                                           const char* const* values,
                                           size_t count) {
  std::vector<HeaderField> fields;
  if (!CollectHeaderFields(names, values, count, &fields)) return -1;
  return static_cast<int>(pagespeed::ClassifyForHeadersSidecar(fields).verdict);
}

// ---- HTML cacheability policy (PS_API 1.12) ----
//
// Thin adapters: the scalar and array arguments become the C++ request,
// response and policy structs, and the verdict comes back through the
// out-parameters.  Nothing here holds state, so nothing here touches the
// last-error message.

extern "C" int ps_html_store_classify(
    int mode, const char* method, const char* url_path, uint32_t request_flags,
    int status, const char* const* names, const char* const* values,
    size_t count, uint64_t body_bytes, int body_complete,
    const char* body_prefix, size_t body_prefix_len, uint64_t max_body_bytes,
    uint32_t now_seconds, int* out_reason, uint32_t* out_lifetime_s,
    uint32_t* out_flags, uint16_t* out_cc_flags, int* out_sidecar,
    char* detail_buf, size_t detail_capacity, size_t* out_detail_len) {
  try {
    if (mode != PS_HTML_CACHE_OFF && mode != PS_HTML_CACHE_REPORT_ONLY &&
        mode != PS_HTML_CACHE_SERVE_VARIANTS &&
        mode != PS_HTML_CACHE_SERVE_CACHED_HTML) {
      return -1;
    }
    if (method == nullptr || url_path == nullptr || out_reason == nullptr ||
        out_lifetime_s == nullptr || out_flags == nullptr ||
        out_cc_flags == nullptr || out_sidecar == nullptr ||
        out_detail_len == nullptr) {
      return -1;
    }
    if (body_prefix == nullptr && body_prefix_len != 0) return -1;
    if (detail_buf == nullptr && detail_capacity != 0) return -1;
    std::vector<HeaderField> fields;
    if (!CollectHeaderFields(names, values, count, &fields)) return -1;

    pagespeed::HtmlStoreRequest request;
    request.method = method;
    request.url_path = url_path;
    request.has_authorization =
        (request_flags & PS_HTML_STORE_REQUEST_HAS_AUTHORIZATION) != 0;
    request.authenticated_user =
        (request_flags & PS_HTML_STORE_REQUEST_AUTHENTICATED_USER) != 0;
    request.has_cookie =
        (request_flags & PS_HTML_STORE_REQUEST_HAS_COOKIE) != 0;
    request.request_no_store =
        (request_flags & PS_HTML_STORE_REQUEST_NO_STORE) != 0;
    pagespeed::HtmlStoreResponse response;
    response.status = status;
    response.headers = fields;
    response.body_bytes = body_bytes;
    response.body_complete = body_complete != 0;
    if (body_prefix != nullptr) {
      response.body_prefix = std::string_view(body_prefix, body_prefix_len);
    }
    pagespeed::HtmlStorePolicy policy;
    policy.mode = static_cast<pagespeed::HtmlCacheMode>(mode);
    policy.max_body_bytes = max_body_bytes;
    policy.now_seconds = now_seconds;

    const pagespeed::HtmlStoreVerdict v =
        pagespeed::ClassifyHtmlForStore(request, response, policy);

    *out_reason = static_cast<int>(v.reason);
    *out_lifetime_s = v.lifetime_s;
    uint32_t flags = 0;
    if (v.lifetime_explicit) flags |= PS_HTML_STORE_FLAG_LIFETIME_EXPLICIT;
    if (v.needs_validation) flags |= PS_HTML_STORE_FLAG_NEEDS_VALIDATION;
    if (v.has_validators) flags |= PS_HTML_STORE_FLAG_HAS_VALIDATORS;
    if (v.explicit_shared) flags |= PS_HTML_STORE_FLAG_EXPLICIT_SHARED;
    if (v.sidecar_evaluated) flags |= PS_HTML_STORE_FLAG_SIDECAR_EVALUATED;
    if (v.cc_evaluated) flags |= PS_HTML_STORE_FLAG_CC_EVALUATED;
    *out_flags = flags;
    *out_cc_flags = v.cc_flags;
    *out_sidecar =
        v.sidecar_evaluated ? static_cast<int>(v.sidecar.verdict) : 0;
    *out_detail_len = v.detail.size();
    if (detail_capacity != 0) {
      const size_t n = std::min(v.detail.size(), detail_capacity - 1);
      std::memcpy(detail_buf, v.detail.data(), n);
      detail_buf[n] = '\0';
    }
    return static_cast<int>(v.action);
  } catch (...) {
    return -1;
  }
}

extern "C" int ps_html_decision_format(const char* const* keys,
                                       const char* const* values, size_t count,
                                       char* buf, size_t capacity,
                                       size_t* out_len) {
  try {
    if (out_len == nullptr) return -1;
    if (buf == nullptr && capacity != 0) return -1;
    std::vector<pagespeed::HtmlCacheDecisionField> fields;
    if (count != 0) {
      if (keys == nullptr || values == nullptr) return -1;
      fields.reserve(count);
      for (size_t i = 0; i < count; ++i) {
        if (keys[i] == nullptr || values[i] == nullptr) return -1;
        fields.push_back(
            {std::string_view(keys[i]), std::string_view(values[i])});
      }
    }
    *out_len = 0;
    if (!pagespeed::HtmlCacheDecisionCApiAdmits(fields)) return -2;
    const std::optional<std::string> line =
        pagespeed::FormatHtmlCacheDecisionLine(fields);
    if (!line.has_value()) return -2;
    *out_len = line->size();
    if (line->size() + 1 > capacity) {
      if (capacity != 0) buf[0] = '\0';
      return -3;
    }
    std::memcpy(buf, line->data(), line->size());
    buf[line->size()] = '\0';
    return 0;
  } catch (...) {
    return -1;
  }
}

// The names are string literals held in static tables, so the view's data
// pointer is NUL-terminated and outlives the call.
extern "C" const char* ps_html_store_reason_name(int reason) {
  if (reason < 0 || reason >= pagespeed::kHtmlStoreReasonCount) return nullptr;
  return pagespeed::HtmlStoreReasonName(
             static_cast<pagespeed::HtmlStoreReason>(reason))
      .data();
}

// ---- HTML serve policy (PS_API 1.14) ----
//
// Thin adapters again: the C structs become the C++ request and facts, and
// the verdict comes back through the out struct.  Nothing here holds state or
// touches the last-error message.

namespace {

// The PS_API 1.14 sizes: the end of the last field, rounded up to the
// struct's alignment.  Pinned so that a later append keeps accepting callers
// built against 1.14 (the >= rule), and so that the sizes are stated, not
// assumed: they are also pinned in the ABI golden.
template <typename T>
constexpr size_t RoundUpToAlign(size_t n) {
  return (n + alignof(T) - 1) / alignof(T) * alignof(T);
}
constexpr size_t kServeRequestSizeV1_13 =
    RoundUpToAlign<ps_html_serve_request_t>(
        offsetof(ps_html_serve_request_t, now_seconds) + sizeof(uint32_t));
constexpr size_t kStoredFactsSizeV1_13 = RoundUpToAlign<ps_html_stored_facts_t>(
    offsetof(ps_html_stored_facts_t, markdown_hash) + PS_HTML_HASH_BYTES);
static_assert(kServeRequestSizeV1_13 == sizeof(ps_html_serve_request_t),
              "ps_html_serve_request_t grew: keep this constant at its 1.14 "
              "value and relax to the >= form");
static_assert(kStoredFactsSizeV1_13 == sizeof(ps_html_stored_facts_t),
              "ps_html_stored_facts_t grew: keep this constant at its 1.14 "
              "value and relax to the >= form");
static_assert(sizeof(void*) != 8 || sizeof(ps_html_serve_request_t) == 64,
              "ps_html_serve_request_t is 64 bytes on 64-bit targets");
static_assert(sizeof(ps_html_stored_facts_t) == 96 || sizeof(size_t) != 8,
              "ps_html_stored_facts_t is 96 bytes on 64-bit targets");
static_assert(sizeof(ps_html_serve_verdict_t) == 64 || sizeof(size_t) != 8,
              "ps_html_serve_verdict_t is 64 bytes on 64-bit targets");

static_assert(PS_HTML_HASH_BYTES == pagespeed::kHtmlParentHashBytes,
              "the published hash length and the marker's must agree");
static_assert(PS_HTML_WITHHELD_BELOW_FLOOR ==
                  static_cast<int>(pagespeed::HtmlWithheldReason::kBelowFloor),
              "withheld reason values are shared with the stored marker");
static_assert(
    PS_HTML_WITHHELD_EXTRACT_FAILED ==
        static_cast<int>(pagespeed::HtmlWithheldReason::kExtractFailed),
    "withheld reason values are shared with the stored marker");
static_assert(PS_HTML_SERVE_ACTION_SERVE_MARKDOWN ==
                  static_cast<int>(pagespeed::HtmlServeAction::kServeMarkdown),
              "serve action values are the policy's");
static_assert(PS_HTML_SERVE_ACTION_VALIDATE ==
                  static_cast<int>(pagespeed::HtmlServeAction::kValidate),
              "serve action values are the policy's");
static_assert(PS_HTML_STORE_ACTION_STORE ==
                  static_cast<int>(pagespeed::HtmlStoreAction::kStore),
              "store action values are the policy's");
static_assert(PS_HTML_STORE_ACTION_STORE_NO_NOTIFY ==
                  static_cast<int>(pagespeed::HtmlStoreAction::kStoreNoNotify),
              "store action values are the policy's");

// The copy's default lifetime bound, for a caller that leaves render_ttl_s 0.
// Inside the policy 0 is a real bound (it refuses every copy), so the
// translation is made here, where 0 means "not supplied".
constexpr uint32_t kDefaultRenderTtlSeconds = 86400;

bool IsHtmlContentType(std::string_view ct) {
  constexpr std::string_view kHtml = "text/html";
  if (ct.size() < kHtml.size()) return false;
  for (size_t i = 0; i < kHtml.size(); ++i) {
    char c = ct[i];
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c | 0x20);
    if (c != kHtml[i]) return false;
  }
  if (ct.size() == kHtml.size()) return true;
  const char next = ct[kHtml.size()];
  return next == ';' || next == ' ' || next == '\t';
}

bool AllZeroBytes(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    if (p[i] != 0) return false;
  }
  return true;
}

// The Markdown entity tag into `out` (PS_HTML_MARKDOWN_ETAG_CHARS bytes plus
// a NUL).  Both hashes are known to be non-zero.
void MakeMarkdownEtag(const uint8_t* binding_hash, const uint8_t* markdown_hash,
                      char* out) {
  std::array<std::byte, 2 * PS_HTML_HASH_BYTES> input{};
  std::memcpy(input.data(), binding_hash, PS_HTML_HASH_BYTES);
  std::memcpy(input.data() + PS_HTML_HASH_BYTES, markdown_hash,
              PS_HTML_HASH_BYTES);
  const auto digest = cyclone::crypto::SHA256::hash(
      std::span<const std::byte>(input.data(), input.size()));
  static constexpr char kHex[] = "0123456789abcdef";
  std::memcpy(out, "W/\"psm-", 7);
  for (size_t i = 0; i < 8; ++i) {
    const auto b = static_cast<uint8_t>(digest[i]);
    out[7 + 2 * i] = kHex[b >> 4];
    out[8 + 2 * i] = kHex[b & 0x0F];
  }
  out[23] = '"';
  out[24] = '\0';
}

// RFC 9110 section 8.8.3: entity-tag = [ "W/" ] DQUOTE *etagc DQUOTE, with
// etagc = %x21 / %x23-7E / obs-text.  Consumes one tag from the front of `s`
// and returns its opaque-tag (the quotes included), or nullopt.
std::optional<std::string_view> TakeEntityTag(std::string_view* s) {
  std::string_view v = *s;
  if (v.substr(0, 2) == "W/") v.remove_prefix(2);
  if (v.empty() || v.front() != '"') return std::nullopt;
  size_t i = 1;
  while (i < v.size() && v[i] != '"') {
    const auto u = static_cast<unsigned char>(v[i]);
    if (u < 0x21 || u == 0x7F) return std::nullopt;
    ++i;
  }
  if (i >= v.size()) return std::nullopt;
  const std::string_view opaque = v.substr(0, i + 1);
  *s = v.substr(i + 1);
  return opaque;
}

std::string_view TrimOwsView(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
    s.remove_prefix(1);
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
    s.remove_suffix(1);
  }
  return s;
}

// -1 malformed etag, 0 no match, 1 match.
int IfNoneMatchMatchesWeak(std::string_view inm, std::string_view etag) {
  std::string_view tag_view = etag;
  const std::optional<std::string_view> wanted = TakeEntityTag(&tag_view);
  if (!wanted.has_value() || !tag_view.empty()) return -1;
  // RFC 9110 section 13.1.2: the field is "*" or a list of entity-tags. `*`
  // matches any current representation; the caller asks only about a copy it
  // would send, so that is this one.
  if (TrimOwsView(inm) == "*") return 1;
  // RFC 9110 section 5.6.1: empty list members are allowed and ignored.
  bool matched = false;
  std::string_view rest = inm;
  while (true) {
    rest = TrimOwsView(rest);
    if (rest.empty()) break;
    if (rest.front() == ',') {
      rest.remove_prefix(1);
      continue;
    }
    // `*` is a list of its own (RFC 9110 section 13.1.2: "*" /
    // #entity-tag), handled above; inside a list it is malformed.
    if (rest.front() == '*') return 0;
    const std::optional<std::string_view> tag = TakeEntityTag(&rest);
    if (!tag.has_value())
      return 0;  // a list that does not parse matches nothing
    if (*tag == *wanted) matched = true;
    rest = TrimOwsView(rest);
    if (!rest.empty() && rest.front() != ',') return 0;
  }
  return matched ? 1 : 0;
}

}  // namespace

extern "C" int ps_html_serve_classify(const ps_html_serve_request_t* request,
                                      const ps_html_stored_facts_t* facts,
                                      ps_html_serve_verdict_t* out) {
  try {
    if (request == nullptr || facts == nullptr || out == nullptr) return -1;
    if (request->struct_size < kServeRequestSizeV1_13 ||
        facts->struct_size < kStoredFactsSizeV1_13 ||
        out->struct_size < sizeof(size_t)) {
      return -1;
    }
    if (request->mode < PS_HTML_CACHE_OFF ||
        request->mode > PS_HTML_CACHE_SERVE_CACHED_HTML) {
      return -1;
    }
    if (request->method == nullptr) return -1;

    const std::string_view method(request->method);
    pagespeed::HtmlServeRequest r;
    r.mode = static_cast<pagespeed::HtmlCacheMode>(request->mode);
    r.wants_markdown = request->accept != nullptr &&
                       pagespeed::WantsAgentMarkdown(request->accept);
    r.method_cacheable = method == "GET" || method == "HEAD";
    const uint32_t rf = request->request_flags;
    r.has_range = (rf & (PS_HTML_SERVE_REQUEST_HAS_RANGE |
                         PS_HTML_SERVE_REQUEST_HAS_IF_RANGE)) != 0;
    r.has_origin = (rf & PS_HTML_SERVE_REQUEST_HAS_ORIGIN) != 0;
    r.has_authorization = (rf & PS_HTML_SERVE_REQUEST_HAS_AUTHORIZATION) != 0;
    r.authenticated_user = (rf & PS_HTML_SERVE_REQUEST_AUTHENTICATED_USER) != 0;
    r.has_cookie = (rf & PS_HTML_SERVE_REQUEST_HAS_COOKIE) != 0;
    if (request->cache_control != nullptr) {
      pagespeed::ApplyHtmlRequestCacheControl(request->cache_control, &r);
    }
    if (request->pragma != nullptr) {
      // RFC 9111 section 5.4: Pragma: no-cache, consulted only when the
      // request carries no Cache-Control at all (the policy applies that).
      std::string_view rest(request->pragma);
      while (!rest.empty()) {
        const size_t comma = rest.find(',');
        std::string_view item = TrimOwsView(rest.substr(0, comma));
        if (item.size() == 8) {
          std::string lower(item);
          for (char& c : lower) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c | 0x20);
          }
          if (lower == "no-cache") r.pragma_no_cache = true;
        }
        if (comma == std::string_view::npos) break;
        rest.remove_prefix(comma + 1);
      }
    }
    r.now_seconds = request->now_seconds;

    const uint32_t ff = facts->flags;
    pagespeed::StoredHtmlFacts f;
    f.present = (ff & PS_HTML_FACTS_PRESENT) != 0;
    f.stored_at = facts->stored_at;
    f.age_at_store = facts->age_at_store;
    f.lifetime_s = facts->lifetime_s;
    f.needs_validation = (ff & PS_HTML_FACTS_NEEDS_VALIDATION) != 0;
    f.explicit_shared = (ff & PS_HTML_FACTS_EXPLICIT_SHARED) != 0;
    f.headers_not_reproducible =
        (ff & PS_HTML_FACTS_HEADERS_NOT_REPRODUCIBLE) != 0;
    f.sidecar_required = (ff & PS_HTML_FACTS_SIDECAR_REQUIRED) != 0;
    f.sidecar_present = (ff & PS_HTML_FACTS_SIDECAR_PRESENT) != 0;
    f.purged = (ff & PS_HTML_FACTS_PURGED) != 0;
    f.validated_this_request = (ff & PS_HTML_FACTS_VALIDATED_THIS_REQUEST) != 0;
    f.variant_present = (ff & PS_HTML_FACTS_VARIANT_PRESENT) != 0;
    f.binding_live = (ff & PS_HTML_FACTS_BINDING_LIVE) != 0;
    f.no_transform = (ff & PS_HTML_FACTS_NO_TRANSFORM) != 0;
    f.withheld = (ff & PS_HTML_FACTS_WITHHELD) != 0;
    f.render_ttl_s = facts->render_ttl_s == 0 ? kDefaultRenderTtlSeconds
                                              : facts->render_ttl_s;

    const pagespeed::HtmlServeVerdict v = pagespeed::ClassifyHtmlForServe(r, f);

    ps_html_serve_verdict_t result{};
    result.struct_size = out->struct_size;
    result.action = static_cast<int>(v.action);
    result.reason = static_cast<int>(v.reason);
    result.enforce = v.enforce ? 1 : 0;
    result.wants_markdown = r.wants_markdown ? 1 : 0;
    result.age_s = v.age_s;
    if (v.action == pagespeed::HtmlServeAction::kServeMarkdown &&
        !AllZeroBytes(facts->binding_hash, PS_HTML_HASH_BYTES) &&
        !AllZeroBytes(facts->markdown_hash, PS_HTML_HASH_BYTES)) {
      MakeMarkdownEtag(facts->binding_hash, facts->markdown_hash, result.etag);
      if (request->if_none_match != nullptr &&
          IfNoneMatchMatchesWeak(request->if_none_match, result.etag) == 1) {
        result.not_modified = 1;
      }
    }
    // The tag is written whole or not at all: a caller whose struct ends
    // inside it gets only NUL bytes there, never a cut-off tag.
    if (out->struct_size <
        offsetof(ps_html_serve_verdict_t, etag) + sizeof(result.etag)) {
      std::memset(result.etag, 0, sizeof(result.etag));
    }
    std::memcpy(out, &result, std::min(out->struct_size, sizeof(result)));
    return 0;
  } catch (...) {
    return -1;
  }
}

extern "C" const char* ps_html_serve_reason_name(int reason) {
  if (reason < 0 || reason >= pagespeed::kHtmlServeReasonCount) return nullptr;
  return pagespeed::HtmlServeReasonName(
             static_cast<pagespeed::HtmlServeReason>(reason))
      .data();
}

namespace {

// The inbound Age, by the rules on ps_html_store_write_params: nullopt when
// it must refuse the store, 0 when there is none.
std::optional<uint32_t> InboundAgeSeconds(const char* const* lines,
                                          size_t count) {
  std::optional<uint32_t> age;
  for (size_t i = 0; i < count; ++i) {
    if (lines[i] == nullptr) return std::nullopt;
    const std::string_view v = TrimOwsView(lines[i]);
    if (v.empty() || v.size() > 10) return std::nullopt;
    uint64_t n = 0;
    for (char c : v) {
      if (c < '0' || c > '9') return std::nullopt;
      n = n * 10 + static_cast<uint64_t>(c - '0');
    }
    if (n > 2147483647u) return std::nullopt;
    if (age.has_value() && *age != n) return std::nullopt;
    age = static_cast<uint32_t>(n);
  }
  return age.value_or(0);
}

}  // namespace

extern "C" int ps_html_store_write_params(
    int action, uint32_t lifetime_s, uint32_t store_flags, uint16_t cc_flags,
    int sidecar_stored, int64_t capture_start_ms, const char* const* age_lines,
    size_t age_count, uint64_t content_length, const char* origin_content_type,
    ps_write_params_t* params) {
  if (action != PS_HTML_STORE_ACTION_STORE &&
      action != PS_HTML_STORE_ACTION_STORE_NO_NOTIFY) {
    return -1;
  }
  if (capture_start_ms < 1000 || params == nullptr ||
      origin_content_type == nullptr ||
      (age_lines == nullptr && age_count != 0)) {
    return -1;
  }
  const int64_t capture_s = capture_start_ms / 1000;  // floored: ms >= 1000
  if (capture_s > static_cast<int64_t>(UINT32_MAX)) return -1;
  const std::optional<uint32_t> age = InboundAgeSeconds(age_lines, age_count);
  // An Age at or above the capture time says the page is older than the
  // epoch: a value no origin produces, so nothing is stored.
  if (!age.has_value() || static_cast<int64_t>(*age) >= capture_s) return -1;
  // The origin-state fields arrived at 1.2; a struct without them cannot
  // carry a page this policy may serve.
  constexpr size_t kWithOriginState =
      offsetof(ps_write_params_t, origin_cc_flags) + sizeof(uint16_t);
  if (params->struct_size < kWithOriginState) return -1;

  params->alternate_id = PS_SENTINEL_ORIGINAL;
  params->content_length = content_length;
  params->full_mask = PS_SENTINEL_ORIGINAL;
  params->content_type = PS_CONTENT_HTML;
  uint8_t flags = PS_FLAG_HEADERS_CLASSIFIED;
  if (sidecar_stored != 0) flags |= PS_FLAG_HEADERS_SIDECAR_REQUIRED;
  params->flags = flags;
  params->origin_ct = origin_content_type;
  params->origin_etag = nullptr;
  // Age < capture_s, so the result is at least 1: never the 0 the write
  // path reads as "not supplied".
  params->cache_inserted_at = static_cast<uint32_t>(capture_s - *age);
  // The lifetime the policy computed (s-maxage, else max-age, else Expires
  // minus Date) is what a later serve compares against, so it is what is
  // stored; s-maxage is kept only when it is what made the page shareable.
  params->origin_max_age = lifetime_s;
  const bool shared_by_s_maxage =
      (store_flags & PS_HTML_STORE_FLAG_EXPLICIT_SHARED) != 0 &&
      (cc_flags & PS_CC_ORIGIN_PUBLIC) == 0;
  params->origin_s_maxage = shared_by_s_maxage ? lifetime_s : 0;
  params->origin_last_modified = 0;
  params->origin_cc_flags = cc_flags;
  return 0;
}

extern "C" int ps_html_stored_facts_from_reads(
    const ps_read_result_t* original, const ps_read_result_t* markdown,
    const ps_read_result_t* withheld_marker, int sidecar_present,
    ps_html_stored_facts_t* out) {
  try {
    if (out == nullptr || out->struct_size < kStoredFactsSizeV1_13) return -1;
    ps_html_stored_facts_t facts{};
    facts.struct_size = out->struct_size;

    const AlternateMetadata* meta =
        original != nullptr ? &original->result.metadata : nullptr;
    const bool is_html_store =
        meta != nullptr && IsHtmlContentType(meta->origin_content_type) &&
        (meta->flags & AlternateMetadata::kFlagHeadersClassified) != 0;
    if (is_html_store) {
      uint32_t flags = PS_HTML_FACTS_PRESENT;
      facts.stored_at = meta->cache_inserted_at;
      facts.age_at_store = 0;  // the insert time is already Age-adjusted
      facts.lifetime_s = meta->origin_max_age;
      const uint16_t cc = meta->origin_cc_flags;
      if (facts.lifetime_s == 0 ||
          (cc & (PS_CC_ORIGIN_NO_CACHE | PS_CC_ORIGIN_NO_CACHE_BARE |
                 PS_CC_ORIGIN_NO_CACHE_QUALIFIED)) != 0) {
        flags |= PS_HTML_FACTS_NEEDS_VALIDATION;
      }
      if (pagespeed::HtmlExplicitlyShared(cc, meta->origin_s_maxage)) {
        flags |= PS_HTML_FACTS_EXPLICIT_SHARED;
      }
      if ((meta->flags &
           AlternateMetadata::kFlagOriginHeadersNotReproducible) != 0) {
        flags |= PS_HTML_FACTS_HEADERS_NOT_REPRODUCIBLE;
      }
      if ((meta->flags & AlternateMetadata::kFlagHeadersSidecarRequired) != 0) {
        flags |= PS_HTML_FACTS_SIDECAR_REQUIRED;
      }
      if (sidecar_present != 0) flags |= PS_HTML_FACTS_SIDECAR_PRESENT;
      if ((cc & PS_CC_ORIGIN_NO_TRANSFORM) != 0) {
        flags |= PS_HTML_FACTS_NO_TRANSFORM;
      }
      std::memcpy(facts.binding_hash, meta->origin_html_hash.data(),
                  PS_HTML_HASH_BYTES);
      const bool bound = !AllZeroBytes(facts.binding_hash, PS_HTML_HASH_BYTES);

      if (markdown != nullptr) flags |= PS_HTML_FACTS_VARIANT_PRESENT;
      // The withheld record first: a copy that may not be served is never
      // called live, whatever else is stored beside it.
      bool withheld = false;
      if (withheld_marker != nullptr && bound) {
        const std::span<const std::byte> payload =
            withheld_marker->result.content();
        const std::span<const uint8_t> bytes(
            reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
        if (pagespeed::HtmlWithheldMarkerApplies(
                bytes, std::span<const uint8_t>(facts.binding_hash,
                                                PS_HTML_HASH_BYTES))) {
          withheld = true;
          flags |= PS_HTML_FACTS_WITHHELD;
          facts.withheld_reason = static_cast<int>(
              pagespeed::DecodeHtmlWithheldMarker(bytes)->reason);
        }
      }
      if (!withheld && markdown != nullptr && bound) {
        const auto& copy_hash = markdown->result.metadata.origin_html_hash;
        if (std::memcmp(copy_hash.data(), facts.binding_hash,
                        PS_HTML_HASH_BYTES) == 0) {
          flags |= PS_HTML_FACTS_BINDING_LIVE;
          // The copy's own content hash, stamped by the optimizer when it
          // wrote the copy, so the entity tag costs no pass over the copy.
          // A copy without one leaves it zero: no tag, no 304.
          std::memcpy(facts.markdown_hash,
                      markdown->result.metadata.render_source_hash.data(),
                      PS_HTML_HASH_BYTES);
        }
      }
      facts.flags = flags;
    }
    std::memcpy(out, &facts, std::min(out->struct_size, sizeof(facts)));
    return 0;
  } catch (...) {
    return -1;
  }
}

extern "C" int ps_html_markdown_etag(const uint8_t* binding_hash,
                                     const uint8_t* markdown_hash, char* buf,
                                     size_t capacity, size_t* out_len) {
  if (binding_hash == nullptr || markdown_hash == nullptr ||
      out_len == nullptr || (buf == nullptr && capacity != 0)) {
    return -1;
  }
  if (AllZeroBytes(binding_hash, PS_HTML_HASH_BYTES) ||
      AllZeroBytes(markdown_hash, PS_HTML_HASH_BYTES)) {
    return -1;
  }
  *out_len = PS_HTML_MARKDOWN_ETAG_CHARS;
  if (capacity < PS_HTML_MARKDOWN_ETAG_CHARS + 1) {
    if (capacity != 0) buf[0] = '\0';
    return -3;
  }
  MakeMarkdownEtag(binding_hash, markdown_hash, buf);
  return 0;
}

extern "C" int ps_if_none_match_matches_weak(const char* if_none_match,
                                             const char* etag) {
  if (etag == nullptr) return -1;
  try {
    if (if_none_match == nullptr) {
      // Still refuse a malformed etag, so the answer does not depend on
      // whether the client sent the field.
      std::string_view tag(etag);
      const std::optional<std::string_view> t = TakeEntityTag(&tag);
      return (t.has_value() && tag.empty()) ? 0 : -1;
    }
    return IfNoneMatchMatchesWeak(if_none_match, etag);
  } catch (...) {
    return -1;
  }
}

extern "C" ps_error_t ps_cache_write_headers_sidecar(
    ps_cache_t* cache, const char* url, const char* hostname,
    const char* scheme, const char* const* names, const char* const* values,
    size_t count, int* out_verdict, int* out_stored) {
  ClearLastError();
  if (cache == nullptr) return PS_ERR_INVALID_ARG;
  ps_error_t err = ValidateUrlHostname(url, hostname);
  if (err != PS_OK) return err;

  std::vector<HeaderField> fields;
  if (!CollectHeaderFields(names, values, count, &fields)) {
    SetLastError(
        "ps_cache_write_headers_sidecar: names/values must be non-NULL arrays "
        "of non-NULL strings when count is non-zero");
    return PS_ERR_INVALID_ARG;
  }

  try {
    auto result =
        cache->cache->WriteHeadersSidecar(url, hostname, scheme, fields);
    if (!result.has_value()) {
      SetLastError("WriteHeadersSidecar failed");
      return MapError(result.error());
    }
    // A refusal is a normal outcome and comes back as PS_OK: the response is
    // served plain rather than optimized in place, which is not an error the
    // caller has anything to handle.
    if (out_verdict != nullptr) {
      *out_verdict = static_cast<int>(result->verdict);
    }
    if (out_stored != nullptr) *out_stored = result->stored ? 1 : 0;
    return PS_OK;
  } catch (const std::exception& e) {
    SetLastError(e.what());
    return PS_ERR_INTERNAL;
  } catch (...) {
    SetLastError("Unknown exception in ps_cache_write_headers_sidecar");
    return PS_ERR_INTERNAL;
  }
}

// ---- Site-scoped headers sidecar (PS_API 1.13) ----

namespace {

constexpr size_t kSidecarWriteParamsSizeV1_13 =
    offsetof(ps_sidecar_write_params_t, origin_html_hash) +
    sizeof(const uint8_t*);
static_assert(kSidecarWriteParamsSizeV1_13 == sizeof(ps_sidecar_write_params_t),
              "ps_sidecar_write_params_t's 1.13 size is the whole struct; when "
              "a field is appended keep this at the 1.13 size and read the new "
              "field only when struct_size covers it.");

}  // namespace

extern "C" ps_error_t ps_cache_write_headers_sidecar_site(
    ps_cache_t* cache, const ps_sidecar_write_params_t* params,
    const char* const* names, const char* const* values, size_t count,
    int* out_verdict, int* out_stored) {
  ClearLastError();
  if (cache == nullptr || params == nullptr) return PS_ERR_INVALID_ARG;
  if (params->struct_size < kSidecarWriteParamsSizeV1_13) {
    SetLastError(
        "ps_cache_write_headers_sidecar_site: params->struct_size is smaller "
        "than the 1.13 ps_sidecar_write_params_t");
    return PS_ERR_INVALID_ARG;
  }
  ps_sidecar_write_params_t p;
  std::memset(&p, 0, sizeof(p));
  std::memcpy(&p, params, sizeof(p));

  ps_error_t err = ValidateUrlHostname(p.url, p.hostname);
  if (err != PS_OK) return err;
  std::string_view site_view;
  err = ParseSiteArg(p.site, &site_view);
  if (err != PS_OK) return err;

  std::span<const std::byte> pairing;
  if (p.origin_html_hash != nullptr) {
    pairing = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(p.origin_html_hash),
        AlternateMetadata::kHashSize);
    if (std::all_of(pairing.begin(), pairing.end(),
                    [](std::byte b) { return b == std::byte{0}; })) {
      SetLastError(
          "ps_cache_write_headers_sidecar_site: origin_html_hash is all zero, "
          "the unbound value; pass NULL for an unpaired block");
      return PS_ERR_INVALID_ARG;
    }
  }

  std::vector<HeaderField> fields;
  if (!CollectHeaderFields(names, values, count, &fields)) {
    SetLastError(
        "ps_cache_write_headers_sidecar_site: names/values must be non-NULL "
        "arrays of non-NULL strings when count is non-zero");
    return PS_ERR_INVALID_ARG;
  }

  try {
    auto result = cache->cache->WriteHeadersSidecar(
        p.url, p.hostname, p.scheme != nullptr ? p.scheme : "", fields,
        site_view, pairing);
    if (!result.has_value()) {
      SetLastError("WriteHeadersSidecar failed");
      return MapError(result.error());
    }
    if (out_verdict != nullptr) {
      *out_verdict = static_cast<int>(result->verdict);
    }
    if (out_stored != nullptr) *out_stored = result->stored ? 1 : 0;
    return PS_OK;
  } catch (const std::exception& e) {
    SetLastError(e.what());
    return PS_ERR_INTERNAL;
  } catch (...) {
    SetLastError("Unknown exception in ps_cache_write_headers_sidecar_site");
    return PS_ERR_INTERNAL;
  }
}

// The parsed block owns its bytes: the payload is copied out of the read
// result, so the block holds no lease on the volume and outlives the read.
struct ps_header_block_s {
  std::vector<std::string> names;
  std::vector<std::string> values;
  bool paired = false;
  std::array<uint8_t, AlternateMetadata::kHashSize> pairing_hash{};
};

extern "C" ps_error_t ps_headers_sidecar_read(
    ps_cache_t* cache, const char* url, const char* hostname,
    const char* scheme, const char* site, ps_header_block_t** out) {
  ClearLastError();
  if (out != nullptr) *out = nullptr;
  if (cache == nullptr || out == nullptr) return PS_ERR_INVALID_ARG;
  ps_error_t err = ValidateUrlHostname(url, hostname);
  if (err != PS_OK) return err;
  std::string_view site_view;
  err = ParseSiteArg(site, &site_view);
  if (err != PS_OK) return err;

  try {
    auto read = cache->cache->ReadHeadersSidecar(
        url, hostname, scheme != nullptr ? scheme : "", site_view);
    if (!read.has_value()) return MapError(read.error());
    const std::string payload(
        pagespeed::PageSpeedCache::HeadersSidecarPayload(*read));
    read->release();
    auto parsed = pagespeed::ParseHeadersSidecarBlock(payload);
    // ReadHeadersSidecar already refuses a block it cannot parse; this is
    // the same check on the bytes now owned.
    if (!parsed.has_value()) return PS_ERR_NOT_FOUND;
    auto block = std::make_unique<ps_header_block_s>();
    block->names.reserve(parsed->fields.size());
    block->values.reserve(parsed->fields.size());
    for (const HeaderField& f : parsed->fields) {
      block->names.emplace_back(f.name);
      block->values.emplace_back(f.value);
    }
    if (parsed->pairing_hash.has_value()) {
      block->paired = true;
      std::memcpy(block->pairing_hash.data(), parsed->pairing_hash->data(),
                  block->pairing_hash.size());
    }
    *out = block.release();
    return PS_OK;
  } catch (const std::exception& e) {
    SetLastError(e.what());
    return PS_ERR_INTERNAL;
  } catch (...) {
    SetLastError("Unknown exception in ps_headers_sidecar_read");
    return PS_ERR_INTERNAL;
  }
}

extern "C" size_t ps_header_block_count(const ps_header_block_t* block) {
  return block == nullptr ? 0 : block->names.size();
}

extern "C" ps_error_t ps_header_block_get(const ps_header_block_t* block,
                                          size_t index, const char** out_name,
                                          size_t* out_name_len,
                                          const char** out_value,
                                          size_t* out_value_len) {
  if (block == nullptr || index >= block->names.size()) {
    return PS_ERR_INVALID_ARG;
  }
  if (out_name != nullptr) *out_name = block->names[index].c_str();
  if (out_name_len != nullptr) *out_name_len = block->names[index].size();
  if (out_value != nullptr) *out_value = block->values[index].c_str();
  if (out_value_len != nullptr) *out_value_len = block->values[index].size();
  return PS_OK;
}

extern "C" const uint8_t* ps_header_block_pairing_hash(
    const ps_header_block_t* block) {
  if (block == nullptr || !block->paired) return nullptr;
  return block->pairing_hash.data();
}

extern "C" int ps_header_block_pairing(const ps_header_block_t* block,
                                       const uint8_t* entry_hash) {
  if (block == nullptr) return -1;
  if (!block->paired || entry_hash == nullptr) return PS_SIDECAR_UNPAIRED;
  return std::memcmp(block->pairing_hash.data(), entry_hash,
                     block->pairing_hash.size()) == 0
             ? PS_SIDECAR_PAIRED
             : PS_SIDECAR_UNPAIRED;
}

extern "C" void ps_header_block_free(ps_header_block_t* block) { delete block; }

extern "C" ps_error_t ps_cache_write_sentinel(
    ps_cache_t* cache, const char* url, const char* hostname,
    const char* scheme, uint8_t sentinel_id, uint64_t content_length,
    ps_write_handle_t** out) {
  ClearLastError();
  if ((cache == nullptr) || (out == nullptr)) return PS_ERR_INVALID_ARG;
  ps_error_t err = ValidateUrlHostname(url, hostname);
  if (err != PS_OK) return err;

  if (!pagespeed::IsSentinel(sentinel_id)) {
    SetLastError("sentinel_id is not a valid sentinel");
    return PS_ERR_INVALID_ARG;
  }

  // Admission control at the one surface a third party can reach.  Two kinds
  // of id are refused here, for the same underlying reason — the class's bytes
  // must only ever be written by something that knows the class's rules:
  //
  //   RESERVED  — the format is fixed but no reader or writer exists yet.  An
  //               embedder occupying the slot means the class arrives to find
  //               foreign bytes under its own id, unversioned and
  //               indistinguishable from its own.
  //   DEDICATED — the class exists and has its own entry point, which enforces
  //               invariants a raw sentinel write would skip (the durable
  //               original's metadata prefix and its content cap).  "The class
  //               shipped" is therefore not a reason to open this door;
  //               ps_cache_write_original is the door.
  //
  // Shape-only ids that name no class stay writable: an embedder may use a
  // free sentinel slot, it just may not take a claimed one.
  if (!pagespeed::IsEmbedderWritableSentinel(sentinel_id)) {
    SetLastError(
        "sentinel_id names an entry class that is reserved, or that has its "
        "own write entry point");
    return PS_ERR_INVALID_ARG;
  }

  try {
    auto wh = cache->cache->WriteSentinel(url, hostname, scheme,
                                          static_cast<SentinelId>(sentinel_id),
                                          content_length);
    if (!wh.has_value()) {
      SetLastError("WriteSentinel failed");
      return MapError(wh.error());
    }

    auto* h = new ps_write_handle_s();
    h->write_result = std::move(*wh);
    *out = h;
    return PS_OK;
  } catch (const std::exception& e) {
    SetLastError(e.what());
    return PS_ERR_INTERNAL;
  } catch (...) {
    SetLastError("Unknown exception in ps_cache_write_sentinel");
    return PS_ERR_INTERNAL;
  }
}

extern "C" ps_error_t ps_write_data(ps_write_handle_t* handle, const void* data,
                                    size_t length) {
  ClearLastError();
  if (handle == nullptr) return PS_ERR_INVALID_ARG;
  if (handle->closed) return PS_ERR_CLOSED;
  if (handle->errored) return PS_ERR_CLOSED;

  if (length > 0 && (data == nullptr)) return PS_ERR_INVALID_ARG;

  try {
    auto bytes =
        std::span<const std::byte>(static_cast<const std::byte*>(data), length);
    auto result = handle->write_result.write_sync(bytes);
    if (!result.has_value()) {
      handle->errored = true;
      if (handle->write_result.over_content_cap()) {
        // The durable-original cap, hit mid-stream: the response turned out
        // larger than it declared. The entry is abandoned — nothing partial
        // is stored — and this is a skipped store, not a failure to handle.
        SetLastError(
            "write exceeds the durable-original content cap (" +
            std::to_string(handle->write_result.content_cap()) +
            " bytes); the entry was abandoned and nothing partial was stored");
      } else {
        SetLastError("write_sync failed");
      }
      return MapError(result.error());
    }
    return PS_OK;
  } catch (const std::exception& e) {
    handle->errored = true;
    SetLastError(e.what());
    return PS_ERR_INTERNAL;
  } catch (...) {
    handle->errored = true;
    SetLastError("Unknown exception in ps_write_data");
    return PS_ERR_INTERNAL;
  }
}

extern "C" ps_error_t ps_write_close(ps_write_handle_t* handle) {
  ClearLastError();
  if (handle == nullptr) return PS_OK;
  if (handle->closed) {
    delete handle;
    return PS_ERR_CLOSED;
  }
  // A handle abandoned by the content cap has no write left to commit: its
  // underlying handle was aborted at the crossing write. Closing it would
  // reach the substrate and come back "invalid argument", which reads like a
  // caller mistake in an embedder's cleanup log when in fact nothing went
  // wrong beyond a store being skipped. Report it as what it is.
  if (handle->write_result.over_content_cap()) {
    handle->closed = true;
    SetLastError(
        "the entry was abandoned at the durable-original content cap; there "
        "was nothing to commit");
    delete handle;
    return PS_ERR_CLOSED;
  }
  handle->closed = true;

  try {
    auto result = handle->write_result.close_sync();
    ps_error_t err = PS_OK;
    if (!result.has_value()) {
      err = MapError(result.error());
    }
    delete handle;
    return err;
  } catch (const std::exception& e) {
    SetLastError(e.what());
    delete handle;
    return PS_ERR_INTERNAL;
  } catch (...) {
    delete handle;
    return PS_ERR_INTERNAL;
  }
}

extern "C" void ps_write_abort(ps_write_handle_t* handle) {
  // WriteResult destructor aborts if close_sync() was not called.
  delete handle;
}

// ================================================================
// Cache management
// ================================================================

extern "C" int ps_cache_alternate_exists(ps_cache_t* cache, const char* url,
                                         const char* hostname,
                                         const char* scheme,
                                         uint8_t alternate_id) {
  ClearLastError();
  if (cache == nullptr) return 0;
  if (ValidateUrlHostname(url, hostname) != PS_OK) return 0;

  try {
    return cache->cache->AlternateExists(url, hostname, scheme,
                                         static_cast<AlternateId>(alternate_id))
               ? 1
               : 0;
  } catch (...) {
    return 0;
  }
}

extern "C" ps_error_t ps_cache_remove(ps_cache_t* cache, const char* url,
                                      const char* hostname,
                                      const char* scheme) {
  ClearLastError();
  if (cache == nullptr) return PS_ERR_INVALID_ARG;
  ps_error_t err = ValidateUrlHostname(url, hostname);
  if (err != PS_OK) return err;

  try {
    auto result = cache->cache->Remove(url, hostname, scheme);
    if (!result.has_value()) {
      return MapError(result.error());
    }
    return PS_OK;
  } catch (const std::exception& e) {
    SetLastError(e.what());
    return PS_ERR_INTERNAL;
  } catch (...) {
    return PS_ERR_INTERNAL;
  }
}

// ---- Site-scoped removal and the per-site purge epoch (PS_API 1.13) ----

extern "C" ps_error_t ps_cache_remove_site(ps_cache_t* cache, const char* url,
                                           const char* hostname,
                                           const char* scheme,
                                           const char* site) {
  ClearLastError();
  if (cache == nullptr) return PS_ERR_INVALID_ARG;
  ps_error_t err = ValidateUrlHostname(url, hostname);
  if (err != PS_OK) return err;
  std::string_view site_view;
  err = ParseSiteArg(site, &site_view);
  if (err != PS_OK) return err;

  try {
    auto result = cache->cache->Remove(
        url, hostname, scheme != nullptr ? scheme : "", site_view);
    if (!result.has_value()) return MapError(result.error());
    return PS_OK;
  } catch (const std::exception& e) {
    SetLastError(e.what());
    return PS_ERR_INTERNAL;
  } catch (...) {
    SetLastError("Unknown exception in ps_cache_remove_site");
    return PS_ERR_INTERNAL;
  }
}

namespace {

// The epoch entry points take a site and nothing else, and the unsited
// keyspace has no epoch: NULL and "" are refused here, unlike ParseSiteArg.
ps_error_t ParseEpochSiteArg(const char* site, std::string_view* out) {
  if (site == nullptr || site[0] == '\0') {
    SetLastError("site must be PS_SITE_ID_CHARS lowercase hex");
    return PS_ERR_INVALID_ARG;
  }
  return ParseSiteArg(site, out);
}

}  // namespace

extern "C" ps_error_t ps_cache_write_site_purge_epoch(ps_cache_t* cache,
                                                      const char* site,
                                                      uint64_t epoch_s) {
  ClearLastError();
  if (cache == nullptr) return PS_ERR_INVALID_ARG;
  std::string_view site_view;
  ps_error_t err = ParseEpochSiteArg(site, &site_view);
  if (err != PS_OK) return err;
  try {
    const auto now = static_cast<uint64_t>(std::time(nullptr));
    auto result = cache->cache->WriteSitePurgeEpoch(site_view, epoch_s, now);
    if (!result.has_value()) {
      if (result.error() == cyclone::CacheError::InvalidArgument) {
        SetLastError(
            "purge epoch is later than the current time plus the allowed "
            "skew; pass min(observed, now)");
      } else {
        SetLastError("the purge epoch file could not be read or written");
      }
      return MapError(result.error());
    }
    return PS_OK;
  } catch (const std::exception& e) {
    SetLastError(e.what());
    return PS_ERR_INTERNAL;
  } catch (...) {
    SetLastError("Unknown exception in ps_cache_write_site_purge_epoch");
    return PS_ERR_INTERNAL;
  }
}

extern "C" ps_error_t ps_cache_read_site_purge_epoch(ps_cache_t* cache,
                                                     const char* site,
                                                     uint64_t* out_epoch_s) {
  ClearLastError();
  if (cache == nullptr || out_epoch_s == nullptr) return PS_ERR_INVALID_ARG;
  std::string_view site_view;
  ps_error_t err = ParseEpochSiteArg(site, &site_view);
  if (err != PS_OK) return err;
  try {
    auto result = cache->cache->ReadSitePurgeEpoch(site_view);
    if (!result.has_value()) {
      if (result.error() == cyclone::CacheError::Corrupted) {
        SetLastError(
            "the purge epoch file is unreadable; treat every entry of the "
            "site as purged");
      }
      return MapError(result.error());
    }
    *out_epoch_s = *result;
    return PS_OK;
  } catch (const std::exception& e) {
    SetLastError(e.what());
    return PS_ERR_INTERNAL;
  } catch (...) {
    SetLastError("Unknown exception in ps_cache_read_site_purge_epoch");
    return PS_ERR_INTERNAL;
  }
}

extern "C" ps_error_t ps_cache_list_alternates(
    ps_cache_t* cache, const char* url, const char* hostname,
    const char* scheme, ps_alternate_info_t** out_alternates,
    size_t* out_count) {
  ClearLastError();
  if ((cache == nullptr) || (out_alternates == nullptr) ||
      (out_count == nullptr))
    return PS_ERR_INVALID_ARG;
  ps_error_t err = ValidateUrlHostname(url, hostname);
  if (err != PS_OK) return err;

  try {
    auto result = cache->cache->ListAlternates(url, hostname, scheme);
    if (!result.has_value()) {
      return MapError(result.error());
    }

    auto& alts = *result;
    size_t count = alts.size();
    if (count == 0) {
      *out_alternates = nullptr;
      *out_count = 0;
      return PS_OK;
    }

    auto* arr = new ps_alternate_info_t[count];
    for (size_t i = 0; i < count; ++i) {
      std::memset(&arr[i], 0, sizeof(ps_alternate_info_t));
      arr[i].struct_size = sizeof(ps_alternate_info_t);
      arr[i].content_length = alts[i].content_length;
      arr[i].hit_count = alts[i].hit_count;
      arr[i].alternate_id = static_cast<uint8_t>(alts[i].id);
    }
    *out_alternates = arr;
    *out_count = count;
    return PS_OK;
  } catch (const std::exception& e) {
    SetLastError(e.what());
    return PS_ERR_INTERNAL;
  } catch (...) {
    return PS_ERR_INTERNAL;
  }
}

extern "C" void ps_alternates_free(ps_alternate_info_t* alternates) {
  delete[] alternates;
}

// ================================================================
// Cache stats
// ================================================================

extern "C" ps_error_t ps_cache_stats(ps_cache_t* cache, ps_cache_stats_t* out) {
  ClearLastError();
  if ((cache == nullptr) || (out == nullptr)) return PS_ERR_INVALID_ARG;
  const size_t caller_size = out->struct_size;
  if (caller_size < sizeof(size_t)) {
    SetLastError("ps_cache_stats: out->struct_size not initialized");
    return PS_ERR_INVALID_ARG;
  }

  try {
    auto stats = cache->cache->Stats();
    auto volume_bytes = cache->cache->VolumeCapacityBytes();

    // Fill a local of this build's shape, then copy back only as much as the
    // caller's build knows about, so an older caller linked against a newer
    // library reads the prefix it understands and nothing past it.
    ps_cache_stats_t full;
    std::memset(&full, 0, sizeof(full));
    full.struct_size = std::min(caller_size, sizeof(full));
    full.ram_cache_hits = stats.ram_cache_hits;
    full.ram_cache_misses = stats.ram_cache_misses;
    full.disk_cache_hits = stats.disk_cache_hits;
    full.disk_cache_misses = stats.disk_cache_misses;
    full.bytes_read = stats.bytes_read;
    full.bytes_written = stats.bytes_written;
    full.evictions = stats.evictions;
    full.current_entries = stats.current_entries;
    full.current_size_bytes = stats.current_bytes;
    full.volume_capacity_bytes = volume_bytes;
    full.ram_cache_bytes = stats.ram_cache_bytes;
    full.total_hits = stats.ram_cache_hits + stats.disk_cache_hits;
    full.total_misses = stats.ram_cache_misses + stats.disk_cache_misses;

    std::memcpy(out, &full, full.struct_size);
    return PS_OK;
  } catch (const std::exception& e) {
    SetLastError(e.what());
    return PS_ERR_INTERNAL;
  } catch (...) {
    return PS_ERR_INTERNAL;
  }
}

// ================================================================
// HTTP cache policy (RFC 9111)
// ================================================================

namespace {

// The C enums are declared to carry the same values as their C++ twins.
// Pinned here rather than trusted: a reordered C++ enumerator would otherwise
// silently re-map every verdict crossing the boundary.
static_assert(static_cast<int>(pagespeed::FreshnessVerdict::kFresh) ==
                  PS_FRESHNESS_FRESH,
              "FreshnessVerdict::kFresh must match PS_FRESHNESS_FRESH");
static_assert(static_cast<int>(pagespeed::FreshnessVerdict::kStaleServe) ==
                  PS_FRESHNESS_STALE_SERVE,
              "FreshnessVerdict::kStaleServe must match "
              "PS_FRESHNESS_STALE_SERVE");
static_assert(static_cast<int>(pagespeed::FreshnessVerdict::kServeNoCache) ==
                  PS_FRESHNESS_SERVE_NO_CACHE,
              "FreshnessVerdict::kServeNoCache must match "
              "PS_FRESHNESS_SERVE_NO_CACHE");
static_assert(static_cast<int>(pagespeed::FreshnessVerdict::kRevalidate) ==
                  PS_FRESHNESS_REVALIDATE,
              "FreshnessVerdict::kRevalidate must match "
              "PS_FRESHNESS_REVALIDATE");
static_assert(static_cast<int>(pagespeed::CacheMode::kSafe) ==
                  PS_CACHE_MODE_SAFE,
              "CacheMode::kSafe must match PS_CACHE_MODE_SAFE");
static_assert(static_cast<int>(pagespeed::CacheMode::kAggressive) ==
                  PS_CACHE_MODE_AGGRESSIVE,
              "CacheMode::kAggressive must match PS_CACHE_MODE_AGGRESSIVE");

// The published PS_CC_ORIGIN_* constants ARE the stored flag bits. If one of
// these fires, a stored entry and every embedder reading it have stopped
// agreeing on what a directive means.
static_assert(PS_CC_ORIGIN_NO_CACHE == AlternateMetadata::kCCOriginNoCache);
static_assert(PS_CC_ORIGIN_MUST_REVALIDATE ==
              AlternateMetadata::kCCOriginMustRevalidate);
static_assert(PS_CC_ORIGIN_NO_STORE == AlternateMetadata::kCCOriginNoStore);
static_assert(PS_CC_ORIGIN_PRIVATE == AlternateMetadata::kCCOriginPrivate);
static_assert(PS_CC_ORIGIN_PUBLIC == AlternateMetadata::kCCOriginPublic);
static_assert(PS_CC_ORIGIN_IMMUTABLE == AlternateMetadata::kCCOriginImmutable);
static_assert(PS_CC_ORIGIN_S_MAXAGE_PRESENT ==
              AlternateMetadata::kCCOriginSMaxagePresent);
static_assert(PS_CC_ORIGIN_PROXY_REVALIDATE ==
              AlternateMetadata::kCCOriginProxyRevalidate);
static_assert(PS_CC_ORIGIN_NO_TRANSFORM ==
              AlternateMetadata::kCCOriginNoTransform);
static_assert(PS_CC_ORIGIN_HEADER_PRESENT ==
              AlternateMetadata::kCCOriginHeaderPresent);
static_assert(PS_CC_ORIGIN_PRIVATE_QUALIFIED ==
              AlternateMetadata::kCCOriginPrivateQualified);
static_assert(PS_CC_ORIGIN_PRIVATE_BARE ==
              AlternateMetadata::kCCOriginPrivateBare);
static_assert(PS_CC_ORIGIN_NO_CACHE_QUALIFIED ==
              AlternateMetadata::kCCOriginNoCacheQualified);
static_assert(PS_CC_ORIGIN_NO_CACHE_BARE ==
              AlternateMetadata::kCCOriginNoCacheBare);
static_assert(PS_CC_ORIGIN_S_MAXAGE_REPEATED ==
              AlternateMetadata::kCCOriginSMaxageRepeated);
static_assert(PS_FLAG_NEEDS_REVALIDATION ==
              AlternateMetadata::kFlagNeedsRevalidation);
static_assert(PS_FLAG_WORKER_PROCESSED ==
              AlternateMetadata::kFlagWorkerProcessed);
// The bit an embedder sets to say "the origin negotiates on Accept" and the
// bit the optimizer refuses to derive variants from must be ONE bit. They are
// set and read in different layers, by different processes, from different
// builds — nothing at runtime would notice them drifting apart, and the
// symptom would be a variant family derived from one requester's negotiation.
static_assert(PS_FLAG_ORIGIN_VARIES_ACCEPT ==
              AlternateMetadata::kFlagOriginVariesAccept);
// And it must not collide with a bit that already means something else.
static_assert((PS_FLAG_ORIGIN_VARIES_ACCEPT &
               (PS_FLAG_NEEDS_REVALIDATION | PS_FLAG_WORKER_PROCESSED)) == 0);
// The header-fidelity marker is the same kind of two-layer bit: the writer
// sets the published one, a serve path reads the internal one back off the
// stored entry, and no runtime check would notice them drifting apart —
// the symptom would be a serve path silently dropping origin headers again,
// which is the gap the bit exists to close.
static_assert(PS_FLAG_ORIGIN_HEADERS_NOT_REPRODUCIBLE ==
              AlternateMetadata::kFlagOriginHeadersNotReproducible);
static_assert((PS_FLAG_ORIGIN_HEADERS_NOT_REPRODUCIBLE &
               (PS_FLAG_NEEDS_REVALIDATION | PS_FLAG_WORKER_PROCESSED |
                PS_FLAG_ORIGIN_VARIES_ACCEPT)) == 0);
// 1.13 publishes the two headers-sidecar bits a front end stamps and reads
// back; the same two-layer argument applies.
static_assert(PS_FLAG_HEADERS_CLASSIFIED ==
              AlternateMetadata::kFlagHeadersClassified);
static_assert(PS_FLAG_HEADERS_SIDECAR_REQUIRED ==
              AlternateMetadata::kFlagHeadersSidecarRequired);
static_assert(PS_SENTINEL_MARKDOWN_WITHHELD ==
              static_cast<int>(SentinelId::kAgentMarkdownWithheld));
static_assert(pagespeed::IsSentinel(PS_SENTINEL_MARKDOWN_WITHHELD),
              "the withheld marker must never be selectable");
static_assert(
    !pagespeed::IsEmbedderWritableSentinel(PS_SENTINEL_MARKDOWN_WITHHELD),
    "ps_cache_write_sentinel must refuse the withheld marker");

// Copy the caller's prefix of an input struct into a zeroed local of this
// build's shape. Absent fields read as zero; fields this build does not know
// are ignored. Returns false when struct_size was never set, or names a shape
// smaller than anything this ABI has published.
//
// These structs are all new at 1.2, so their ladder is a single rung today.
// When one of them grows, its previous size joins the ladder and an older
// caller keeps working — that is the whole point of the convention.
template <typename T>
bool NormalizeInputStruct(const T* src, T* dst) {
  const size_t ladder[] = {sizeof(T)};
  const size_t usable = ClampToPublishedSize(
      src->struct_size, ladder, sizeof(ladder) / sizeof(ladder[0]));
  if (usable == 0) return false;
  std::memset(dst, 0, sizeof(*dst));
  std::memcpy(dst, src, std::min(usable, sizeof(*dst)));
  dst->struct_size = sizeof(*dst);
  return true;
}

// Shared/private is carried as an enum whose ZERO is shared, because zero is
// what a caller that never heard of the field passes. Anything that is not
// exactly PRIVATE is read as shared, so an unrecognised value from a newer
// header also lands on the cautious side rather than silently unlocking the
// private-cache relaxations.
bool ScopeIsShared(ps_cache_scope_t scope) {
  return scope != PS_CACHE_SCOPE_PRIVATE;
}

}  // namespace

extern "C" void ps_freshness_config_init_sized(ps_freshness_config_t* config,
                                               size_t size) {
  if (config == nullptr) return;
  // Never sizeof(*config): the caller's buffer is `size` bytes, and on a build
  // older than this library that is smaller than this struct. Clearing our own
  // idea of the size is how an initializer corrupts the caller's stack the
  // first time the struct grows.
  if (size < sizeof(size_t)) return;
  std::memset(config, 0, size);
  config->struct_size = size;

  // Fill only the fields that fit in what the caller actually owns.
  const pagespeed::FreshnessConfig defaults;
  const auto fits = [size](size_t end_offset) { return size >= end_offset; };
#define PS_FILL_IF_FITS(field, value)                                         \
  if (fits(offsetof(ps_freshness_config_t, field) + sizeof(config->field))) { \
    config->field = (value);                                                  \
  }
  PS_FILL_IF_FITS(max_age_cap, defaults.max_age_cap)
  PS_FILL_IF_FITS(immutable_max_age_cap, defaults.immutable_max_age_cap)
  PS_FILL_IF_FITS(html_max_age, defaults.html_max_age)
  PS_FILL_IF_FITS(css_max_age, defaults.css_max_age)
  PS_FILL_IF_FITS(image_max_age, defaults.image_max_age)
#undef PS_FILL_IF_FITS
}

extern "C" ps_error_t ps_evaluate_freshness(const ps_freshness_input_t* input,
                                            const ps_freshness_config_t* config,
                                            ps_freshness_result_t* out) {
  ClearLastError();
  if ((input == nullptr) || (out == nullptr)) return PS_ERR_INVALID_ARG;

  ps_freshness_input_t in;
  if (!NormalizeInputStruct(input, &in)) {
    SetLastError("ps_evaluate_freshness: input->struct_size not initialized");
    return PS_ERR_INVALID_ARG;
  }

  pagespeed::FreshnessConfig cfg;
  if (config != nullptr) {
    ps_freshness_config_t c;
    if (!NormalizeInputStruct(config, &c)) {
      SetLastError(
          "ps_evaluate_freshness: config->struct_size not initialized");
      return PS_ERR_INVALID_ARG;
    }
    cfg.max_age_cap = c.max_age_cap;
    cfg.immutable_max_age_cap = c.immutable_max_age_cap;
    cfg.html_max_age = c.html_max_age;
    cfg.css_max_age = c.css_max_age;
    cfg.image_max_age = c.image_max_age;
  }

  pagespeed::FreshnessInput fi;
  fi.now_seconds = in.now_seconds;
  fi.cache_inserted_at = in.cache_inserted_at;
  fi.origin_max_age = in.origin_max_age;
  fi.origin_s_maxage = in.origin_s_maxage;
  fi.origin_cc_flags = in.origin_cc_flags;
  fi.content_type = static_cast<ContentType>(in.content_type);
  fi.is_shared_cache = ScopeIsShared(in.cache_scope);
  fi.force_revalidate = in.force_revalidate != 0;

  const pagespeed::FreshnessResult r = pagespeed::EvaluateFreshness(fi, cfg);

  const size_t caller_size = out->struct_size;
  if (caller_size < sizeof(size_t)) {
    SetLastError("ps_evaluate_freshness: out->struct_size not initialized");
    return PS_ERR_INVALID_ARG;
  }
  ps_freshness_result_t full;
  std::memset(&full, 0, sizeof(full));
  full.struct_size = std::min(caller_size, sizeof(full));
  full.verdict = static_cast<ps_freshness_verdict_t>(r.verdict);
  full.age_seconds = r.age_seconds;
  full.effective_max_age = r.effective_max_age;
  full.remaining_ttl = r.remaining_ttl;
  full.is_stale = r.is_stale ? 1 : 0;
  full.expired_by_age = r.expired_by_age ? 1 : 0;
  std::memcpy(out, &full, full.struct_size);
  return PS_OK;
}

extern "C" ps_error_t ps_build_cache_control(
    const ps_cache_control_input_t* input, char* buf, size_t capacity,
    size_t* out_len, uint32_t* out_final_max_age) {
  ClearLastError();
  if ((input == nullptr) || (buf == nullptr) || (out_len == nullptr) ||
      (capacity == 0)) {
    return PS_ERR_INVALID_ARG;
  }

  ps_cache_control_input_t in;
  if (!NormalizeInputStruct(input, &in)) {
    SetLastError("ps_build_cache_control: input->struct_size not initialized");
    return PS_ERR_INVALID_ARG;
  }

  pagespeed::CacheControlInput cci;
  cci.mode = static_cast<pagespeed::CacheMode>(in.mode);
  cci.origin_cc_flags = in.origin_cc_flags;
  cci.effective_max_age = in.effective_max_age;
  cci.origin_max_age = in.origin_max_age;
  cci.synthesize_swr = in.synthesize_swr != 0;
  cci.is_shared_cache = ScopeIsShared(in.cache_scope);
  cci.content_type = static_cast<ContentType>(in.content_type);
  cci.relay_origin_no_cache = in.relay_origin_no_cache != 0;
  cci.forward_origin_restrictions = in.forward_origin_restrictions != 0;

  const pagespeed::CacheControlOutput r =
      pagespeed::BuildCacheControlHeader(cci, buf, capacity);
  *out_len = r.len;
  if (out_final_max_age != nullptr) *out_final_max_age = r.final_max_age;
  return PS_OK;
}

extern "C" int ps_vary_uncacheable(const char* vary) {
  // No Vary at all: nothing to refuse.
  if (vary == nullptr) return 0;
  return pagespeed::VaryUncacheable(std::string_view(vary)) ? 1 : 0;
}

extern "C" int ps_vary_varies_accept(const char* vary) {
  // No Vary at all: the origin negotiates on nothing.
  if (vary == nullptr) return 0;
  // Straight off the single classifier, exactly as the refusal predicate above
  // is.  Two exported halves of one verdict, both wrappers over one parse of
  // one value — which is what keeps them from becoming two decisions that
  // agree today.  ClassifyVaryForStore already guarantees the pairing this
  // header promises (varies_accept implies storable).
  return pagespeed::ClassifyVaryForStore(std::string_view(vary)).varies_accept
             ? 1
             : 0;
}

extern "C" ps_error_t ps_parse_cache_control(const char* header_value,
                                             ps_cache_control_t* out) {
  ClearLastError();
  if ((header_value == nullptr) || (out == nullptr)) return PS_ERR_INVALID_ARG;
  const size_t caller_size = out->struct_size;
  if (caller_size < sizeof(size_t)) {
    SetLastError("ps_parse_cache_control: out->struct_size not initialized");
    return PS_ERR_INVALID_ARG;
  }

  // Accumulating, so seed from whatever the caller already has. Fields this
  // build knows but the caller does not are zero, and stay zero on the way
  // back out.
  ps_cache_control_t full;
  std::memset(&full, 0, sizeof(full));
  std::memcpy(&full, out, std::min(caller_size, sizeof(full)));

  pagespeed::OriginCacheControl occ;
  occ.cc_flags = full.cc_flags;
  occ.max_age = full.max_age;
  occ.s_maxage = full.s_maxage;
  pagespeed::AccumulateOriginCacheControl(std::string_view(header_value), &occ);

  full.cc_flags = occ.cc_flags;
  full.max_age = occ.max_age;
  full.s_maxage = occ.s_maxage;
  full.struct_size = std::min(caller_size, sizeof(full));
  std::memcpy(out, &full, full.struct_size);
  return PS_OK;
}

// ================================================================
// Worker notification
// ================================================================

extern "C" ps_error_t ps_notify_worker(const char* socket_path, const char* url,
                                       const char* hostname, const char* scheme,
                                       ps_content_type_t content_type,
                                       uint32_t mask) {
  ClearLastError();
  if (socket_path == nullptr) {
    SetLastError("socket_path is NULL");
    return PS_ERR_INVALID_ARG;
  }
  ps_error_t err = ValidateUrlHostname(url, hostname);
  if (err != PS_OK) return err;
  err = ValidateContentType(content_type);
  if (err != PS_OK) return err;

  // Scheme is validated here exactly as in ps_notify_worker_ex, so both entry
  // points agree: a NULL scheme must not reach a std::string constructor, and
  // an unknown one fails with the caller's error instead of later inside
  // serialization.
  if (scheme == nullptr) {
    SetLastError("scheme is NULL");
    return PS_ERR_INVALID_ARG;
  }
  if (!pagespeed::ValidateScheme(scheme)) {
    SetLastError("scheme must be \"http\" or \"https\"");
    return PS_ERR_INVALID_ARG;
  }

  try {
    pagespeed::CacheNotification notif;
    notif.url = url;
    notif.hostname = hostname;
    notif.scheme = scheme;
    notif.content_type = static_cast<ContentType>(content_type);
    notif.capability_mask = mask;

    auto result = pagespeed::SendNotificationWithRetry(socket_path, notif);
    if (!result.success) {
      SetLastError(result.error_message);
      // A notification that refused serialization is a bad argument, not a
      // delivery failure.
      return result.invalid_argument ? PS_ERR_INVALID_ARG : PS_ERR_IO;
    }
    return PS_OK;
  } catch (const std::exception& e) {
    SetLastError(e.what());
    return PS_ERR_INTERNAL;
  } catch (...) {
    SetLastError("Unknown exception in ps_notify_worker");
    return PS_ERR_INTERNAL;
  }
}

// The C macros and the C++ constants are two spellings of one contract, and
// nothing links them: pagespeed.h is what an embedder compiles against, and
// lib/classify/option_context.h is what this library enforces. If they drifted,
// an embedder would build a payload against one ceiling and have it refused
// against another -- silently, as an unoptimized response. Bound here so that
// drift is a compile error in the same change that causes it.
static_assert(static_cast<size_t>(PS_MAX_OPTION_CONTEXT_BYTES) ==
                  pagespeed::kMaxOptionContextBytes,
              "PS_MAX_OPTION_CONTEXT_BYTES and kMaxOptionContextBytes must "
              "agree; the bound is also mirrored in the shared golden-vector "
              "file's `# bound:` line and in the peer component");
static_assert(static_cast<size_t>(PS_OPTION_CONTEXT_SIGNATURE_CHARS) ==
                  pagespeed::kOptionContextSignatureChars,
              "PS_OPTION_CONTEXT_SIGNATURE_CHARS and "
              "kOptionContextSignatureChars must agree");

extern "C" void ps_notify_params_init_sized(ps_notify_params_t* params,
                                            size_t size) {
  if (params == nullptr) return;
  // Same reasoning as ps_write_params_init_sized: below the width of
  // struct_size there is nowhere to record the size, and stamping it would be
  // the overrun this entry point exists to prevent. Leave the struct alone and
  // let ps_notify_worker_ex refuse it.
  if (size < sizeof(size_t)) return;
  std::memset(params, 0, size);
  params->struct_size = size;
}

extern "C" ps_error_t ps_option_context_signature(const char* payload,
                                                  size_t payload_length,
                                                  char* out, size_t out_size) {
  ClearLastError();
  if (payload == nullptr && payload_length != 0) {
    SetLastError("payload is NULL but payload_length is non-zero");
    return PS_ERR_INVALID_ARG;
  }
  if (out == nullptr ||
      out_size < static_cast<size_t>(PS_OPTION_CONTEXT_SIGNATURE_CHARS) + 1) {
    SetLastError("output buffer is NULL or too small for the signature");
    return PS_ERR_INVALID_ARG;
  }
  if (payload_length > static_cast<size_t>(PS_MAX_OPTION_CONTEXT_BYTES)) {
    SetLastError("payload exceeds PS_MAX_OPTION_CONTEXT_BYTES");
    return PS_ERR_INVALID_ARG;
  }
  try {
    const std::string signature = pagespeed::OptionContextSignature(
        std::string_view(payload == nullptr ? "" : payload, payload_length));
    std::memcpy(out, signature.data(), signature.size());
    out[signature.size()] = '\0';
    return PS_OK;
  } catch (const std::exception& e) {
    SetLastError(e.what());
    return PS_ERR_INTERNAL;
  } catch (...) {
    SetLastError("Unknown exception in ps_option_context_signature");
    return PS_ERR_INTERNAL;
  }
}

static_assert(static_cast<size_t>(PS_SITE_ID_CHARS) == pagespeed::kSiteIdChars,
              "PS_SITE_ID_CHARS and kSiteIdChars must agree");
static_assert(PS_NOTIFY_FLAG_HOST_VOUCHED == pagespeed::kNotifyFlagHostVouched,
              "the published flag and the wire bit must agree");
static_assert(PS_NOTIFY_FLAG_FROM_MODULE == pagespeed::kNotifyFlagFromModule,
              "the published flag and the wire bit must agree");
static_assert(PS_NOTIFY_FLAG_HTML_STORED == pagespeed::kNotifyFlagHtmlStored,
              "the published flag and the wire bit must agree");

namespace {

// The size of ps_notify_params_t at PS_API 1.12, where `site` and
// `notify_flags` were appended: the end of `notify_flags`, rounded up to the
// struct's alignment (88 on LP64 and LLP64). A caller whose struct_size is
// below this was built before the site fields existed.
constexpr size_t kNotifyParamsSiteSize =
    (offsetof(ps_notify_params_t, notify_flags) + sizeof(uint32_t) +
     alignof(ps_notify_params_t) - 1) /
    alignof(ps_notify_params_t) * alignof(ps_notify_params_t);
static_assert(sizeof(void*) != 8 || kNotifyParamsSiteSize == 88,
              "ps_notify_params_t at 1.12 is 88 bytes on 64-bit targets");

// The flags a caller may set. Bit 0x4 is the library's own (the wire's
// legacy-caller bit); every other bit is reserved.
constexpr uint32_t kCallerNotifyFlags = PS_NOTIFY_FLAG_HOST_VOUCHED |
                                        PS_NOTIFY_FLAG_FROM_MODULE |
                                        PS_NOTIFY_FLAG_HTML_STORED;

// Once per process: a caller of ps_notify_worker_ex whose parameters predate
// the site fields. The notification is still sent, unsited and unvouched,
// and the frame carries the legacy-caller bit so the worker counts it.
void WarnLegacyNotifyCallerOnce() {
  static std::once_flag once;
  std::call_once(once, [] {
    std::fprintf(stderr,
                 "pagespeed: notification caller predates site identity; "
                 "upgrade the module and reload\n");
  });
}

}  // namespace

extern "C" ps_error_t ps_notify_worker_ex(const char* socket_path,
                                          const ps_notify_params_t* params) {
  ClearLastError();
  if (socket_path == nullptr) {
    SetLastError("socket_path is NULL");
    return PS_ERR_INVALID_ARG;
  }
  if (params == nullptr) {
    SetLastError("params is NULL");
    return PS_ERR_INVALID_ARG;
  }
  if (params->struct_size < sizeof(size_t)) {
    SetLastError("params->struct_size is unset");
    return PS_ERR_INVALID_ARG;
  }

  // Read each field only if the caller's struct is large enough to contain it.
  // This is the whole point of struct_size: a caller compiled against an
  // earlier header has a shorter struct, and reading past it would be reading
  // its stack.
  auto has = [params](size_t end_offset) {
    return params->struct_size >= end_offset;
  };
  const bool has_core =
      has(offsetof(ps_notify_params_t, mask) + sizeof(params->mask));
  if (!has_core) {
    SetLastError(
        "params->struct_size is too small to contain the notification");
    return PS_ERR_INVALID_ARG;
  }

  ps_error_t err = ValidateUrlHostname(params->url, params->hostname);
  if (err != PS_OK) return err;
  err = ValidateContentType(params->content_type);
  if (err != PS_OK) return err;

  // Scheme is validated HERE, and ps_notify_worker validates it the same way,
  // so both entry points agree instead of one failing later inside
  // serialization with no useful message.
  if (params->scheme == nullptr) {
    SetLastError("scheme is NULL");
    return PS_ERR_INVALID_ARG;
  }
  if (!pagespeed::ValidateScheme(params->scheme)) {
    SetLastError("scheme must be \"http\" or \"https\"");
    return PS_ERR_INVALID_ARG;
  }

  const bool has_agent_request =
      has(offsetof(ps_notify_params_t, agent_request) +
          sizeof(params->agent_request));
  const bool has_option_context =
      has(offsetof(ps_notify_params_t, option_signature) +
          sizeof(params->option_signature));

  const char* context = has_option_context ? params->option_context : nullptr;
  const size_t context_length =
      has_option_context ? params->option_context_length : 0;
  const char* signature =
      has_option_context ? params->option_signature : nullptr;

  if ((context == nullptr) != (signature == nullptr)) {
    SetLastError(
        "option_context and option_signature must be supplied together: a "
        "payload without its signature has no name, and a signature without "
        "its payload names a context the receiver never saw");
    return PS_ERR_INVALID_ARG;
  }
  if (context != nullptr &&
      context_length > static_cast<size_t>(PS_MAX_OPTION_CONTEXT_BYTES)) {
    SetLastError("option_context exceeds PS_MAX_OPTION_CONTEXT_BYTES");
    return PS_ERR_INVALID_ARG;
  }

  // Site and flags (1.12). A struct below the 1.12 size reads as no site and
  // no flags: unsited, unvouched, not from the module.
  const bool legacy_caller = params->struct_size < kNotifyParamsSiteSize;
  const char* site = legacy_caller ? nullptr : params->site;
  const uint32_t flags = legacy_caller ? 0u : params->notify_flags;
  const bool site_empty = (site == nullptr || site[0] == '\0');
  if (!site_empty && !pagespeed::IsSiteId(site)) {
    SetLastError("site must be NULL, empty, or PS_SITE_ID_CHARS lowercase hex");
    return PS_ERR_INVALID_ARG;
  }
  if ((flags & ~kCallerNotifyFlags) != 0) {
    SetLastError("notify_flags carries a reserved bit");
    return PS_ERR_INVALID_ARG;
  }
  const bool agent_request = has_agent_request && params->agent_request != 0;
  const bool from_module = (flags & PS_NOTIFY_FLAG_FROM_MODULE) != 0;
  const bool vouched = (flags & PS_NOTIFY_FLAG_HOST_VOUCHED) != 0;
  const bool html_stored = (flags & PS_NOTIFY_FLAG_HTML_STORED) != 0;
  const bool is_html = params->content_type == PS_CONTENT_HTML;
  // The HTML-stored flag names a page the module's HTML store wrote, so it
  // means nothing on any other notification.
  if (html_stored && (!from_module || !is_html)) {
    SetLastError(
        "the HTML-stored flag is valid only on an HTML notification from the "
        "module");
    return PS_ERR_INVALID_ARG;
  }
  if (from_module) {
    // The module's notifications name a site and never ask for llms.txt.
    if (site_empty) {
      SetLastError("a notification from the module must carry a site");
      return PS_ERR_INVALID_ARG;
    }
    if (params->mask == pagespeed::kLlmsTxtSentinel) {
      SetLastError(
          "a notification from the module may not carry the "
          "llms.txt mask");
      return PS_ERR_INVALID_ARG;
    }
    // HTML, and with it an agent request, only for a page the module stored
    // itself, for a host it vouched for. Every other module notification
    // keeps the rule it had before: no HTML, no agent request, no vouching.
    const bool stored_page = is_html && vouched && html_stored;
    if (!stored_page && (is_html || agent_request || vouched)) {
      SetLastError(
          "a notification from the module may carry HTML or an agent request "
          "only with both the host-vouched and the HTML-stored flags, and "
          "may carry the host-vouched flag only then");
      return PS_ERR_INVALID_ARG;
    }
  }

  try {
    pagespeed::CacheNotification notif;
    notif.url = params->url;
    notif.hostname = params->hostname;
    notif.scheme = params->scheme;
    notif.content_type = static_cast<ContentType>(params->content_type);
    notif.capability_mask = params->mask;
    notif.agent_request = agent_request;
    if (!site_empty) notif.site = site;
    notif.host_vouched = (flags & PS_NOTIFY_FLAG_HOST_VOUCHED) != 0;
    notif.from_module = (flags & PS_NOTIFY_FLAG_FROM_MODULE) != 0;
    notif.html_stored = html_stored;
    notif.legacy_ex_caller = legacy_caller;
    if (context != nullptr) {
      notif.option_context.assign(context, context_length);
      notif.option_signature.assign(signature);
      // Checked, not trusted, and checked HERE so the caller gets an error
      // return rather than a notification the receiver will drop. A caller that
      // computed the signature over something other than the payload it is
      // sending has drifted from this format, and the receiver refuses such a
      // notification too — failing at the call site is the same verdict,
      // delivered where it is actionable.
      const pagespeed::OptionContextStatus status =
          pagespeed::ValidateOptionContext(notif.option_context,
                                           notif.option_signature);
      if (status != pagespeed::OptionContextStatus::kOk) {
        SetLastError(std::string("option context refused: ") +
                     std::string(pagespeed::OptionContextStatusName(status)));
        return PS_ERR_INVALID_ARG;
      }
    }

    if (legacy_caller) WarnLegacyNotifyCallerOnce();

    auto result = pagespeed::SendNotificationWithRetry(socket_path, notif);
    if (!result.success) {
      SetLastError(result.error_message);
      return result.invalid_argument ? PS_ERR_INVALID_ARG : PS_ERR_IO;
    }
    return PS_OK;
  } catch (const std::exception& e) {
    SetLastError(e.what());
    return PS_ERR_INTERNAL;
  } catch (...) {
    SetLastError("Unknown exception in ps_notify_worker_ex");
    return PS_ERR_INTERNAL;
  }
}

// ================================================================
// Helper: ensure HtmlKeywords is initialized
// ================================================================

static void EnsureHtmlKeywordsInit() {
  std::call_once(g_html_keywords_init,
                 [] { net_instaweb::HtmlKeywords::Init(); });
}

// ================================================================
// Helper: fill CriticalCssConfig selector arrays from null-terminated C arrays.
// ================================================================

static void FillStringArray(const char** arr, std::vector<std::string>& out) {
  if (arr == nullptr) return;
  out.clear();
  for (const char** p = arr; *p != nullptr; ++p) {
    out.emplace_back(*p);
  }
}

static void FillCriticalCssSelectors(
    pagespeed::CriticalCssConfig& cpp, const char** always_include,
    const char** include_tag, const char** include_class,
    const char** include_id, const char** exclude_class,
    const char** exclude_id, const char** exclude_tag) {
  FillStringArray(always_include, cpp.always_include_selectors);
  FillStringArray(include_tag, cpp.include_tag_patterns);
  FillStringArray(include_class, cpp.include_class_patterns);
  FillStringArray(include_id, cpp.include_id_patterns);
  FillStringArray(exclude_class, cpp.exclude_class_patterns);
  FillStringArray(exclude_id, cpp.exclude_id_patterns);
  FillStringArray(exclude_tag, cpp.exclude_tag_patterns);
}

static pagespeed::CriticalCssConfig BuildCriticalCssConfig(
    const ps_html_config_t* config) {
  pagespeed::CriticalCssConfig cpp;
  if (config == nullptr) return cpp;
  cpp.max_elements = config->critical_css_max_elements;
  cpp.max_depth = config->critical_css_max_depth;
  FillCriticalCssSelectors(
      cpp, config->always_include_selectors, config->include_tag_patterns,
      config->include_class_patterns, config->include_id_patterns,
      config->exclude_class_patterns, config->exclude_id_patterns,
      config->exclude_tag_patterns);
  return cpp;
}

static pagespeed::CriticalCssConfig BuildCriticalCssConfigFromCss(
    const ps_critical_css_config_t* config) {
  pagespeed::CriticalCssConfig cpp;
  if (config == nullptr) return cpp;
  cpp.max_elements = config->max_elements;
  cpp.max_depth = config->max_depth;
  FillCriticalCssSelectors(
      cpp, config->always_include_selectors, config->include_tag_patterns,
      config->include_class_patterns, config->include_id_patterns,
      config->exclude_class_patterns, config->exclude_id_patterns,
      config->exclude_tag_patterns);
  return cpp;
}

// ================================================================
// Helper: map ps_viewport_t to CapabilityMask::Viewport
// ================================================================

static CapabilityMask::Viewport MapViewport(ps_viewport_t vp) {
  switch (vp) {
    case PS_VIEWPORT_MOBILE:
      return CapabilityMask::Viewport::kMobile;
    case PS_VIEWPORT_TABLET:
      return CapabilityMask::Viewport::kTablet;
    default:
      return CapabilityMask::Viewport::kDesktop;
  }
}

// ================================================================
// Helper: validate CSS for XSS
// ================================================================

// True when the CSS holds, outside its strings and comments, a `<` that could
// open markup: `<` then an ASCII letter, `/`, `!` or `?`. A bare `<` (range
// media queries) is allowed, and so is one in a string or comment, which the
// critical block writes as the escape `\3c ` (EscapeLessThanForStyleElement,
// html_css_injector.h).
static bool CssOpensMarkup(const char* css, size_t len) {
  return pagespeed::CssOpensMarkupOutsideStrings(std::string_view(css, len));
}

// True when the CSS holds a `<` that could open markup ANYWHERE, inside CSS
// strings and comments too: `<` then an ASCII letter, `/`, `!` or `?` (which
// covers `</style` in any letter case). This is ps_css_validate's contract,
// stricter than the critical block's: its callers write the CSS into a page
// as it is, with no escaping, and possibly where a `</noscript` or an SVG
// `<style>` end would matter. A bare `<` (range media queries) is allowed.
static bool CssContainsMarkupOpenerAnywhere(const char* css, size_t len) {
  for (size_t i = 0; i + 1 < len; ++i) {
    if (css[i] != '<') continue;
    const unsigned char next = static_cast<unsigned char>(css[i + 1]);
    if ((next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z') ||
        next == '/' || next == '!' || next == '?') {
      return true;
    }
  }
  return false;
}

static bool CssContainsNullBytes(const char* css, size_t len) {
  return std::memchr(css, '\0', len) != nullptr;
}

// ================================================================
// HTML Scanner
// ================================================================

static void PrecomputeClassPtrs(ps_scan_result* sr) {
  sr->class_ptrs.resize(sr->scan.elements.size());
  for (size_t i = 0; i < sr->scan.elements.size(); ++i) {
    auto& classes = sr->scan.elements[i].classes;
    auto& ptrs = sr->class_ptrs[i];
    ptrs.resize(classes.size());
    for (size_t j = 0; j < classes.size(); ++j) {
      ptrs[j] = classes[j].c_str();
    }
  }
}

extern "C" ps_error_t ps_html_scan(const char* html, size_t html_len,
                                   const char* url, ps_scan_result_t** out) {
  ClearLastError();
  if ((html == nullptr) || (url == nullptr) || (out == nullptr))
    return PS_ERR_INVALID_ARG;
  if (std::strlen(url) > kMaxUrlLength) {
    SetLastError("URL exceeds maximum length");
    return PS_ERR_INVALID_ARG;
  }

  try {
    EnsureHtmlKeywordsInit();

    if (html_len == 0) {
      auto* sr = new ps_scan_result();
      sr->scan.success = true;
      *out = sr;
      return PS_OK;
    }

    pagespeed::HtmlScanner scanner;
    auto result = scanner.Scan(url, std::string_view(html, html_len));

    auto* sr = new ps_scan_result();
    sr->scan = std::move(result);
    PrecomputeClassPtrs(sr);
    *out = sr;
    return PS_OK;
  } catch (const std::exception& e) {
    SetLastError(e.what());
    return PS_ERR_INTERNAL;
  } catch (...) {
    SetLastError("Unknown exception in ps_html_scan");
    return PS_ERR_INTERNAL;
  }
}

extern "C" size_t ps_scan_element_count(const ps_scan_result_t* result) {
  if (result == nullptr) return 0;
  return result->scan.elements.size();
}

extern "C" ps_error_t ps_scan_element(const ps_scan_result_t* result,
                                      size_t index, const char** out_tag,
                                      const char** out_id, int* out_depth,
                                      int* out_element_index) {
  if ((result == nullptr) || index >= result->scan.elements.size())
    return PS_ERR_INVALID_ARG;
  const auto& elem = result->scan.elements[index];
  if (out_tag != nullptr) *out_tag = elem.tag_name.c_str();
  if (out_id != nullptr) *out_id = elem.id.c_str();
  if (out_depth != nullptr) *out_depth = elem.depth;
  if (out_element_index != nullptr) *out_element_index = elem.element_index;
  return PS_OK;
}

extern "C" size_t ps_scan_element_classes(const ps_scan_result_t* result,
                                          size_t index,
                                          const char*** out_classes) {
  if ((result == nullptr) || index >= result->scan.elements.size()) {
    if (out_classes != nullptr) *out_classes = nullptr;
    return 0;
  }
  auto& ptrs = result->class_ptrs[index];
  if (out_classes != nullptr) {
    *out_classes =
        ptrs.empty() ? nullptr : const_cast<const char**>(ptrs.data());
  }
  return ptrs.size();
}

extern "C" size_t ps_scan_stylesheet_count(const ps_scan_result_t* result) {
  if (result == nullptr) return 0;
  return result->scan.stylesheets.size();
}

extern "C" ps_error_t ps_scan_stylesheet(const ps_scan_result_t* result,
                                         size_t index, const char** out_href,
                                         const char** out_media) {
  if ((result == nullptr) || index >= result->scan.stylesheets.size())
    return PS_ERR_INVALID_ARG;
  const auto& ss = result->scan.stylesheets[index];
  if (out_href != nullptr) *out_href = ss.href.c_str();
  if (out_media != nullptr) *out_media = ss.media.c_str();
  return PS_OK;
}

extern "C" const char* ps_scan_inline_css(const ps_scan_result_t* result,
                                          size_t* out_len) {
  if ((result == nullptr) || result->scan.inline_css.empty()) {
    if (out_len != nullptr) *out_len = 0;
    return nullptr;
  }
  if (out_len != nullptr) *out_len = result->scan.inline_css.size();
  return result->scan.inline_css.c_str();
}

extern "C" const char* ps_scan_lcp_candidate(const ps_scan_result_t* result,
                                             const char** out_srcset,
                                             const char** out_sizes,
                                             int* out_element_index) {
  if ((result == nullptr) || result->scan.lcp_candidate.src.empty()) {
    if (out_srcset != nullptr) *out_srcset = nullptr;
    if (out_sizes != nullptr) *out_sizes = nullptr;
    if (out_element_index != nullptr) *out_element_index = -1;
    return nullptr;
  }
  const auto& lcp = result->scan.lcp_candidate;
  if (out_srcset != nullptr) {
    *out_srcset = lcp.srcset.empty() ? nullptr : lcp.srcset.c_str();
  }
  if (out_sizes != nullptr) {
    *out_sizes = lcp.sizes.empty() ? nullptr : lcp.sizes.c_str();
  }
  if (out_element_index != nullptr) *out_element_index = lcp.element_index;
  return lcp.src.c_str();
}

extern "C" size_t ps_scan_origin_count(const ps_scan_result_t* result) {
  if (result == nullptr) return 0;
  return result->scan.third_party_origins.size();
}

extern "C" const char* ps_scan_origin(const ps_scan_result_t* result,
                                      size_t index) {
  if ((result == nullptr) || index >= result->scan.third_party_origins.size())
    return nullptr;
  return result->scan.third_party_origins[index].origin.c_str();
}

extern "C" void ps_scan_result_free(ps_scan_result_t* result) { delete result; }

// ================================================================
// Critical CSS
// ================================================================

extern "C" void ps_critical_css_config_init(ps_critical_css_config_t* config) {
  if (config == nullptr) return;
  std::memset(config, 0, kCriticalCssConfigSizeV1_8);
  config->struct_size = kCriticalCssConfigSizeV1_8;
  config->max_elements = 300;
  config->max_depth = 10;
  config->viewport = PS_VIEWPORT_DESKTOP;
  config->max_css_size = static_cast<size_t>(2 * 1024 * 1024);  // 2MB
}

extern "C" ps_error_t ps_css_extract_critical(
    const ps_scan_result_t* scan_result, const char* css, size_t css_len,
    const ps_critical_css_config_t* config, ps_critical_css_result_t** out) {
  ClearLastError();
  if ((scan_result == nullptr) || (css == nullptr) || (out == nullptr))
    return PS_ERR_INVALID_ARG;

  size_t max_css = ((config != nullptr) && config->max_css_size > 0)
                       ? config->max_css_size
                       : (static_cast<size_t>(2 * 1024 * 1024));
  if (css_len > max_css) {
    SetLastError("CSS exceeds maximum size");
    return PS_ERR_INVALID_ARG;
  }

  try {
    pagespeed::CriticalCssConfig cpp_config =
        BuildCriticalCssConfigFromCss(config);

    auto viewport = (config != nullptr) ? MapViewport(config->viewport)
                                        : CapabilityMask::Viewport::kDesktop;

    pagespeed::CriticalCssExtractor extractor(cpp_config);
    auto result = extractor.Extract(scan_result->scan.elements,
                                    std::string_view(css, css_len), viewport);

    auto* cr = new ps_critical_css_result();
    cr->critical_css = std::move(result.critical_css);
    cr->total_rules = result.total_rules;
    cr->critical_rules = result.critical_rules;
    *out = cr;
    return PS_OK;
  } catch (const std::exception& e) {
    SetLastError(e.what());
    return PS_ERR_INTERNAL;
  } catch (...) {
    SetLastError("Unknown exception in ps_css_extract_critical");
    return PS_ERR_INTERNAL;
  }
}

extern "C" const char* ps_critical_css_output(
    const ps_critical_css_result_t* result, size_t* out_len) {
  if ((result == nullptr) || result->critical_css.empty()) {
    if (out_len != nullptr) *out_len = 0;
    return nullptr;
  }
  if (out_len != nullptr) *out_len = result->critical_css.size();
  return result->critical_css.c_str();
}

extern "C" void ps_critical_css_stats(const ps_critical_css_result_t* result,
                                      int* out_total_rules,
                                      int* out_critical_rules) {
  if (result == nullptr) {
    if (out_total_rules != nullptr) *out_total_rules = 0;
    if (out_critical_rules != nullptr) *out_critical_rules = 0;
    return;
  }
  if (out_total_rules != nullptr) *out_total_rules = result->total_rules;
  if (out_critical_rules != nullptr)
    *out_critical_rules = result->critical_rules;
}

extern "C" void ps_critical_css_result_free(ps_critical_css_result_t* result) {
  delete result;
}

// ================================================================
// CSS Processing
// ================================================================

extern "C" ps_error_t ps_css_validate(const char* css, size_t css_len) {
  ClearLastError();
  if (css == nullptr) return PS_ERR_INVALID_ARG;
  if (CssContainsNullBytes(css, css_len)) {
    SetLastError("CSS contains null bytes");
    return PS_ERR_INVALID_ARG;
  }
  if (CssContainsMarkupOpenerAnywhere(css, css_len)) {
    SetLastError("CSS contains a sequence that could open markup");
    return PS_ERR_INVALID_ARG;
  }
  return PS_OK;
}

extern "C" ps_error_t ps_css_flatten_imports(
    const char* css, size_t css_len, const char* css_url,
    ps_css_lookup_fn lookup, void* user_data, int max_depth, char** out_css,
    size_t* out_len, int* out_resolved, int* out_unresolved) {
  ClearLastError();
  if ((css == nullptr) || (css_url == nullptr) || (out_css == nullptr) ||
      (out_len == nullptr))
    return PS_ERR_INVALID_ARG;

  *out_css = nullptr;
  *out_len = 0;
  if (out_resolved != nullptr) *out_resolved = 0;
  if (out_unresolved != nullptr) *out_unresolved = 0;

  try {
    // Adapt C callback to C++ CssLookupFn.
    pagespeed::css::CssLookupFn cpp_lookup =
        [lookup,
         user_data](std::string_view url) -> std::optional<std::string> {
      if (!lookup) return std::nullopt;
      std::string url_str(url);
      size_t len = 0;
      const char* result = lookup(url_str.c_str(), &len, user_data);
      if (!result) return std::nullopt;
      return std::string(result, len);
    };

    auto result = pagespeed::css::FlattenImports(std::string_view(css, css_len),
                                                 css_url, cpp_lookup,
                                                 max_depth > 0 ? max_depth : 5);

    if (out_resolved != nullptr) *out_resolved = result.imports_resolved;
    if (out_unresolved != nullptr) *out_unresolved = result.imports_unresolved;

    // Allocate output buffer using new[].
    size_t output_len = result.css.size();
    char* buf = new char[output_len + 1];
    std::memcpy(buf, result.css.data(), output_len);
    buf[output_len] = '\0';
    *out_css = buf;
    *out_len = output_len;
    return PS_OK;
  } catch (const std::exception& e) {
    SetLastError(e.what());
    return PS_ERR_INTERNAL;
  } catch (...) {
    SetLastError("Unknown exception in ps_css_flatten_imports");
    return PS_ERR_INTERNAL;
  }
}

extern "C" ps_error_t ps_css_minify(const char* css, size_t css_len,
                                    char** out_css, size_t* out_len) {
  ClearLastError();
  if ((css == nullptr) || (out_css == nullptr) || (out_len == nullptr))
    return PS_ERR_INVALID_ARG;
  *out_css = nullptr;
  *out_len = 0;

  try {
    std::string output;
    bool ok =
        pagespeed::css::MinifyCss(std::string_view(css, css_len), &output);
    if (!ok) {
      SetLastError("CSS minification failed");
      return PS_ERR_INTERNAL;
    }

    char* buf = new char[output.size() + 1];
    std::memcpy(buf, output.data(), output.size());
    buf[output.size()] = '\0';
    *out_css = buf;
    *out_len = output.size();
    return PS_OK;
  } catch (const std::exception& e) {
    SetLastError(e.what());
    return PS_ERR_INTERNAL;
  } catch (...) {
    SetLastError("Unknown exception in ps_css_minify");
    return PS_ERR_INTERNAL;
  }
}

extern "C" void ps_free(void* ptr) { delete[] static_cast<char*>(ptr); }

// ================================================================
// HTML Transform
// ================================================================

extern "C" ps_error_t ps_html_transform_create(
    const ps_scan_result_t* scan_result, const ps_html_config_t* config,
    const char* critical_css, size_t critical_css_len, ps_cache_t* cache,
    const char* hostname, const char* speculation_urls,
    size_t speculation_urls_len, ps_html_transform_t** out) {
  ClearLastError();
  if ((scan_result == nullptr) || (out == nullptr)) return PS_ERR_INVALID_ARG;

  try {
    EnsureHtmlKeywordsInit();

    auto* t = new ps_html_transform();

    // Store config.
    if (config != nullptr) {
      t->transform_config.enable_critical_css =
          (config->enable_critical_css != 0);
      t->transform_config.enable_lazy_load = (config->enable_lazy_load != 0);
      t->transform_config.enable_image_dimensions =
          (config->enable_image_dimensions != 0);
      t->transform_config.enable_lcp_preload =
          (config->enable_lcp_preload != 0);
      t->transform_config.enable_preconnect_injection =
          (config->enable_preconnect != 0);
      t->transform_config.enable_speculation_rules =
          (config->enable_speculation_rules != 0);
      // enable_async_css is INERT on this entry point, deliberately.
      //
      // Deferring a stylesheet is only safe once something has established
      // that the inlined critical CSS covers the fold. This builder has no
      // scan of the page's stylesheets, no cache to resolve them from, and no
      // browser — so it can run neither the byte-ratio floor and cold-cache
      // fail-safe that ps_html_process applies, nor the worker's empirical
      // validation gate. Its documentation used to say callers "MUST pre-gate",
      // which the ABI has no way to verify; an embedder that set the flag got
      // unconditional deferral with none of the safety, which is a flash of
      // unstyled content waiting to happen.
      //
      // The critical CSS the caller supplied is still inlined, so only the
      // unsafe half is dropped. Embedders wanting deferral should use
      // ps_html_process, which gates it. Re-enabling this path needs an
      // embedder-supplied validation signal — evidence that the fold renders
      // identically with the stylesheet deferred — not a config flag.
      t->transform_config.enable_async_css = false;
    }

    // Store critical CSS.
    if ((critical_css != nullptr) && critical_css_len > 0) {
      t->critical_css.assign(critical_css, critical_css_len);
    }

    // Store cache pointer.
    t->cache_ptr = (cache != nullptr) ? cache->cache.get() : nullptr;
    if (hostname != nullptr) t->hostname = hostname;

    // Copy LCP candidate and preconnect from scan result.
    t->lcp_candidate = scan_result->scan.lcp_candidate;
    t->preconnect_origins = scan_result->scan.third_party_origins;

    // Parse speculation URLs.
    if ((speculation_urls != nullptr) && speculation_urls_len > 0) {
      std::string_view sv(speculation_urls, speculation_urls_len);
      size_t pos = 0;
      while (pos < sv.size()) {
        auto nl = sv.find('\n', pos);
        if (nl == std::string_view::npos) nl = sv.size();
        auto line = sv.substr(pos, nl - pos);
        if (!line.empty()) {
          t->speculation_urls.emplace_back(line);
        }
        pos = nl + 1;
      }
    }

    *out = t;
    return PS_OK;
  } catch (const std::exception& e) {
    SetLastError(e.what());
    return PS_ERR_INTERNAL;
  } catch (...) {
    SetLastError("Unknown exception in ps_html_transform_create");
    return PS_ERR_INTERNAL;
  }
}

extern "C" ps_error_t ps_html_transform_run(ps_html_transform_t* transform,
                                            const char* html, size_t html_len,
                                            const char* url, char** out_html,
                                            size_t* out_len) {
  ClearLastError();
  if ((transform == nullptr) || (html == nullptr) || (url == nullptr) ||
      (out_html == nullptr) || (out_len == nullptr))
    return PS_ERR_INVALID_ARG;
  *out_html = nullptr;
  *out_len = 0;

  try {
    EnsureHtmlKeywordsInit();

    // Create a fresh HtmlParse + filters for each run.
    net_instaweb::NullMessageHandler message_handler;
    net_instaweb::HtmlParse parser(&message_handler);
    pagespeed::HtmlTransformFilter filter(
        &parser, transform->transform_config, transform->critical_css,
        transform->cache_ptr, transform->hostname, "https",
        transform->lcp_candidate, transform->preconnect_origins,
        transform->speculation_urls);
    // The page is text/html by this entry point's contract (pagespeed.h).
    filter.set_document_is_text_html(true);
    parser.AddFilter(&filter);

    std::string output;
    net_instaweb::StringWriter writer(&output);
    net_instaweb::HtmlWriterFilter writer_filter(&parser);
    writer_filter.set_writer(&writer);
    parser.AddFilter(&writer_filter);

    parser.StartParse(url);
    parser.ParseText(std::string_view(html, html_len));
    parser.FinishParse();

    transform->last_modified = filter.modified();

    if (!filter.modified()) {
      return PS_OK;
    }

    char* buf = new char[output.size() + 1];
    std::memcpy(buf, output.data(), output.size());
    buf[output.size()] = '\0';
    *out_html = buf;
    *out_len = output.size();
    return PS_OK;
  } catch (const std::exception& e) {
    SetLastError(e.what());
    return PS_ERR_INTERNAL;
  } catch (...) {
    SetLastError("Unknown exception in ps_html_transform_run");
    return PS_ERR_INTERNAL;
  }
}

extern "C" int ps_html_transform_modified(
    const ps_html_transform_t* transform) {
  if (transform == nullptr) return 0;
  return transform->last_modified ? 1 : 0;
}

extern "C" void ps_html_transform_free(ps_html_transform_t* transform) {
  delete transform;
}

// ================================================================
// HTML Config Init
// ================================================================

// The in-process .NET middleware mirrors ps_html_config_t with a hand-written
// [StructLayout(Explicit)] NativeHtmlConfig that hard-codes EnableAsyncCss at
// FieldOffset(120) and Size=128. Pin those two numbers here so any future
// layout drift fails the shared-lib build that every front-end links, instead
// of silently corrupting the managed marshaling at runtime.
static_assert(offsetof(ps_html_config_t, enable_async_css) == 120,
              ".NET NativeHtmlConfig FieldOffset(120) must match");
// The mirror may stop at the 128-byte prefix (struct_size 128, scheme read as
// "https") or carry the whole struct, with scheme at offset 128.
static_assert(offsetof(ps_html_config_t, scheme) == 128,
              ".NET NativeHtmlConfig scheme offset must match");
static_assert(sizeof(ps_html_config_t) == 136,
              ".NET NativeHtmlConfig full size must match");

// The scheme ps_html_process reads the page's cached stylesheets under. A
// config whose struct_size predates the field reads as "https", as every
// caller did before it existed; so does NULL or "". Returns false for any
// other value than "http" or "https".
static bool HtmlConfigScheme(const ps_html_config_t* config,
                             std::string_view* out) {
  *out = "https";
  constexpr size_t kSchemeEnd =
      offsetof(ps_html_config_t, scheme) + sizeof(config->scheme);
  if (config->struct_size < kSchemeEnd) return true;
  const char* scheme = config->scheme;
  if ((scheme == nullptr) || (scheme[0] == '\0')) return true;
  if (!pagespeed::ValidateScheme(scheme)) return false;
  *out = scheme;
  return true;
}

extern "C" void ps_html_config_init(ps_html_config_t* config) {
  if (config == nullptr) return;
  std::memset(config, 0, kHtmlConfigSizeV1_8);
  config->struct_size = kHtmlConfigSizeV1_8;
  config->enable_critical_css = 1;
  config->enable_lazy_load = 1;
  config->enable_image_dimensions = 1;
  config->enable_lcp_preload = 1;
  config->enable_preconnect = 1;
  config->enable_speculation_rules = 0;
  config->enable_async_css = 0;  // opt-in, requires critical CSS
  config->critical_css_max_elements = 300;
  config->critical_css_max_depth = 10;
  config->css_import_max_depth = 5;
  config->viewport = PS_VIEWPORT_DESKTOP;
  config->max_html_size = static_cast<size_t>(5 * 1024 * 1024);  // 5MB
  config->max_css_size = static_cast<size_t>(2 * 1024 * 1024);   // 2MB
  // All pattern arrays NULL = use built-in defaults.
}

// ================================================================
// High-level HTML processing
// ================================================================

extern "C" ps_error_t ps_html_process(const char* html, size_t html_len,
                                      const char* url, const char* hostname,
                                      const ps_html_config_t* config,
                                      ps_cache_t* cache,
                                      ps_html_result_t** out) {
  ClearLastError();
  if ((html == nullptr) || (url == nullptr) || (out == nullptr))
    return PS_ERR_INVALID_ARG;
  *out = nullptr;

  // Use default config if not provided.
  ps_html_config_t default_config;
  if (config == nullptr) {
    ps_html_config_init(&default_config);
    config = &default_config;
  }

  if (html_len > config->max_html_size) {
    SetLastError("HTML exceeds maximum size");
    return PS_ERR_INVALID_ARG;
  }
  if (std::strlen(url) > kMaxUrlLength) {
    SetLastError("URL exceeds maximum length");
    return PS_ERR_INVALID_ARG;
  }
  std::string_view scheme;
  if (!HtmlConfigScheme(config, &scheme)) {
    SetLastError("config->scheme must be \"http\" or \"https\"");
    return PS_ERR_INVALID_ARG;
  }

  try {
    EnsureHtmlKeywordsInit();

    // Step 1: Scan HTML.
    pagespeed::HtmlScanner scanner;
    auto scan_result = scanner.Scan(url, std::string_view(html, html_len));

    // Step 2: Gather external CSS from cache.
    std::string combined_css = scan_result.inline_css;
    bool external_css_missing = false;
    size_t max_css = config->max_css_size;

    if ((cache != nullptr) && (hostname != nullptr)) {
      for (const auto& ss : scan_result.stylesheets) {
        // Skip bare hrefs (matches the worker gather loop); an empty href is
        // not a real external sheet and must not poison external_css_missing.
        if (ss.href.empty()) continue;
        // The sheet's rules join the combined sheet under its media, so a
        // print sheet's rules stay print rules and never reach the screen
        // block as unconditional ones. A media value that cannot be written
        // safely as an @media prelude leaves the sheet out of the combined
        // sheet and counts it as missing, so it stays render-blocking.
        const std::optional<std::string_view> scope =
            pagespeed::css::CombinedSheetMediaScope(
                pagespeed::CombinedSheetMediaOf(ss));
        if (!scope.has_value()) {
          external_css_missing = true;
          continue;
        }
        auto read_result = cache->cache->ReadBestAlternate(
            ss.href, hostname, scheme, CapabilityMask());
        if (read_result.has_value()) {
          auto content = read_result->content();
          if (combined_css.size() +
                  pagespeed::css::MediaScopedCssSize(*scope, content.size()) >
              max_css) {
            external_css_missing = true;
            break;
          }
          std::string_view css_sv(reinterpret_cast<const char*>(content.data()),
                                  content.size());
          // A sheet that cannot be wrapped in its media safely counts as
          // missing, like one with an unusable media value.
          if (!pagespeed::css::AppendMediaScopedCss(*scope, css_sv,
                                                    &combined_css)) {
            external_css_missing = true;
          }
        } else {
          external_css_missing = true;
        }
      }
    } else if (!scan_result.stylesheets.empty()) {
      external_css_missing = true;
    }

    // Step 3: Extract critical CSS.
    std::string critical_css;
    bool critical_css_injected = false;

    if ((config->enable_critical_css != 0) && !combined_css.empty()) {
      pagespeed::CriticalCssConfig css_config = BuildCriticalCssConfig(config);
      auto viewport = MapViewport(config->viewport);

      pagespeed::CriticalCssExtractor extractor(css_config);
      auto css_result =
          extractor.Extract(scan_result.elements, combined_css, viewport);

      if (css_result.success && !css_result.critical_css.empty()) {
        // Step 4: Sanitize critical CSS.
        std::string& css = css_result.critical_css;
        // Strip null bytes.
        css.erase(std::remove(css.begin(), css.end(), '\0'), css.end());
        // Reject if it holds, outside its strings and comments, a sequence
        // that could open markup; the transform escapes the rest.
        if (!CssOpensMarkup(css.c_str(), css.size())) {
          critical_css = std::move(css);
        }
      }
    }

    // Step 5: Transform HTML.
    pagespeed::HtmlTransformConfig transform_config;
    transform_config.enable_critical_css = (config->enable_critical_css != 0);
    transform_config.enable_lazy_load = (config->enable_lazy_load != 0);
    transform_config.enable_image_dimensions =
        (config->enable_image_dimensions != 0);
    transform_config.enable_lcp_preload = (config->enable_lcp_preload != 0);
    transform_config.enable_preconnect_injection =
        (config->enable_preconnect != 0);
    transform_config.enable_speculation_rules =
        (config->enable_speculation_rules != 0);
    // Mirror the worker's async-CSS FOUC sufficiency gate so the in-process
    // .NET path is equally fail-safe (it otherwise deferred unconditionally —
    // strictly worse than the worker). No browser profile here, so coverage is
    // unknown (-1) and derived from byte sizes. external_css_missing is the
    // dominant fail-safe: a declared external sheet not resolved from cache
    // forces the stylesheet render-blocking (combined_css is inline-only and
    // the critical CSS was derived without the sheet). Thresholds are the shared
    // defaults — the .NET ABI does not surface them. needs_revalidation is set
    // from external_css_missing below, and the middleware reprocesses per
    // request, so async re-enables once the sheet caches.
    const bool async_css_sufficient = pagespeed::CriticalCssIsSufficient(
        -1.0f, critical_css.size(), combined_css.size(), external_css_missing,
        pagespeed::AsyncCssSufficiencyConfig{});
    transform_config.enable_async_css = (config->enable_async_css != 0) &&
                                        !critical_css.empty() &&
                                        async_css_sufficient;

    net_instaweb::NullMessageHandler message_handler;
    net_instaweb::HtmlParse parser(&message_handler);
    pagespeed::HtmlTransformFilter filter(
        &parser, transform_config, critical_css,
        (cache != nullptr) ? cache->cache.get() : nullptr,
        (hostname != nullptr) ? hostname : "", scheme,
        scan_result.lcp_candidate, scan_result.third_party_origins);
    // The page is text/html by this entry point's contract (pagespeed.h).
    filter.set_document_is_text_html(true);
    parser.AddFilter(&filter);

    std::string output;
    net_instaweb::StringWriter string_writer(&output);
    net_instaweb::HtmlWriterFilter writer_filter(&parser);
    writer_filter.set_writer(&string_writer);
    parser.AddFilter(&writer_filter);

    parser.StartParse(url);
    parser.ParseText(std::string_view(html, html_len));
    parser.FinishParse();

    critical_css_injected = filter.critical_css_injected();

    // Step 6: Build early hints.
    // Sanitize values: strip CR/LF to prevent header injection since early
    // hints are newline-delimited and eventually emitted as HTTP headers.
    auto sanitize_hint = [](const std::string& s) {
      std::string out;
      out.reserve(s.size());
      for (char c : s) {
        if (c != '\n' && c != '\r') out += c;
      }
      return out;
    };

    std::string early_hints;
    // Stylesheets are hinted whether or not async-CSS defers them: the
    // deferral primitive is itself `rel=preload as=style`, so the hint
    // announces exactly the request the transform announces, only earlier.
    // (Mirror of the worker's emitter -- the two paths must agree, since a
    // page can be served by either.)  Print sheets are never render-blocking
    // for screen, so they get no hint either way.
    for (const auto& ss : scan_result.stylesheets) {
      if (!ss.href.empty() && !pagespeed::StylesheetMediaIsPrint(ss.media)) {
        if (!early_hints.empty()) early_hints += '\n';
        early_hints += sanitize_hint(ss.href);
      }
    }
    // Skip the image hint for an <img> inside <picture>: a <source> sibling
    // may win selection and the preload would double-download.
    if (!scan_result.lcp_candidate.src.empty() &&
        !scan_result.lcp_candidate.in_picture) {
      if (!early_hints.empty()) early_hints += '\n';
      early_hints += "image:";
      early_hints += sanitize_hint(scan_result.lcp_candidate.src);
    }
    // CORS-mode origins carry a distinct prefix so the Link/103 emitter warms
    // the same connection pool as the injected HTML preconnect.
    for (const auto& entry : scan_result.third_party_origins) {
      if (!early_hints.empty()) early_hints += '\n';
      early_hints += entry.crossorigin ? "preconnect-cors:" : "preconnect:";
      early_hints += sanitize_hint(entry.origin);
    }

    // Step 7: Package result.
    auto* r = new ps_html_result();
    r->modified = filter.modified();
    r->critical_css_injected = critical_css_injected;
    r->needs_revalidation = external_css_missing;
    if (filter.modified()) {
      r->output_html = std::move(output);
    }
    if (!early_hints.empty()) {
      r->early_hints = std::move(early_hints);
    }
    *out = r;
    return PS_OK;
  } catch (const std::exception& e) {
    SetLastError(e.what());
    return PS_ERR_INTERNAL;
  } catch (...) {
    SetLastError("Unknown exception in ps_html_process");
    return PS_ERR_INTERNAL;
  }
}

// ================================================================
// HTML Result Accessors
// ================================================================

extern "C" const char* ps_html_result_output(const ps_html_result_t* result,
                                             size_t* out_len) {
  if ((result == nullptr) || !result->modified || result->output_html.empty()) {
    if (out_len != nullptr) *out_len = 0;
    return nullptr;
  }
  if (out_len != nullptr) *out_len = result->output_html.size();
  return result->output_html.c_str();
}

extern "C" int ps_html_result_modified(const ps_html_result_t* result) {
  if (result == nullptr) return 0;
  return result->modified ? 1 : 0;
}

extern "C" int ps_html_result_has_critical_css(const ps_html_result_t* result) {
  if (result == nullptr) return 0;
  return result->critical_css_injected ? 1 : 0;
}

extern "C" const char* ps_html_result_early_hints(
    const ps_html_result_t* result, size_t* out_len) {
  if ((result == nullptr) || result->early_hints.empty()) {
    if (out_len != nullptr) *out_len = 0;
    return nullptr;
  }
  if (out_len != nullptr) *out_len = result->early_hints.size();
  return result->early_hints.c_str();
}

extern "C" int ps_html_result_needs_revalidation(
    const ps_html_result_t* result) {
  if (result == nullptr) return 0;
  return result->needs_revalidation ? 1 : 0;
}

extern "C" void ps_html_result_free(ps_html_result_t* result) { delete result; }
