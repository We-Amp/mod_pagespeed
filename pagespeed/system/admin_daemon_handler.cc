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

#include "pagespeed/system/admin_daemon_handler.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

#include "net/instaweb/http/public/async_fetch.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/http/content_type.h"
#include "pagespeed/kernel/http/google_url.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/query_params.h"
#include "pagespeed/kernel/http/request_headers.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "pagespeed/system/daemon_reader.h"
#include "pagespeed/system/daemon_site_filter.h"

namespace net_instaweb {

namespace {

// What a per-virtual-host console may see of a leaf it is served: the
// answer as it is, or the answer rebuilt for this site by
// daemon_site_filter.h.  The whole-server console is never narrowed.
enum SiteFilter : uint8_t {
  kNoSiteFilter = 0,
  kServeSavingsSiteFilter = 1,  // stats: only this site's per-host row
  kCooldownsSiteFilter = 2,     // cooldowns: only this site's entries
};

// The endpoint table: the ONLY source of upstream daemon paths.  The
// request leaf must match `leaf` exactly (two-segment leaves such as
// "cache/urls" included); the upstream request is then built from
// scratch -- fixed method GET/HEAD, fixed path, and a query string
// rebuilt from validated, re-encoded values for the allow-listed
// parameters only.  Adding a path or a parameter here is a deliberate,
// reviewed act -- the daemon's socket transport is unauthenticated, so
// anything reachable through this table is reachable by anyone who can
// reach the admin console.
struct DaemonEndpoint {
  const char* leaf;           // exact request leaf, e.g. "health"
  const char* upstream_path;  // daemon API path, e.g. "/v1/health"
  uint32_t allowed_params;    // bitmask of the parameters this leaf takes
  uint32_t required_params;   // subset of allowed_params, must be present
  int max_in_flight;          // concurrent upstream reads admitted
  bool whole_server_only;     // 403 on a per-virtual-host console
  bool not_found_is_answer;   // upstream 404 -> 404 "not_in_index"
  bool serves_content;        // response bytes pass the content-type gate
  bool distinct_too_large;    // over-cap read -> 502 "response_too_large"
                              // (other JSON leaves: "daemon_unreachable")
  uint8_t site_filter;        // per-virtual-host console: a SiteFilter
};

// Every query parameter the daemon proxy knows about, as a bit.  An
// endpoint allow-lists the subset it accepts; anything else is rejected
// before any upstream request is built.  Names match exactly and
// case-sensitively on their escaped form.
enum ParamBit : uint8_t {
  kParamUrl = 1u << 0,
  kParamOffset = 1u << 1,
  kParamLimit = 1u << 2,
  kParamHostname = 1u << 3,
  kParamScheme = 1u << 4,
  kParamAlternateId = 1u << 5,
  kParamSince = 1u << 6,
};

struct ParamName {
  const char* name;
  uint32_t bit;
};
constexpr ParamName kParamNames[] = {
    {"url", kParamUrl},       {"offset", kParamOffset},
    {"limit", kParamLimit},   {"hostname", kParamHostname},
    {"scheme", kParamScheme}, {"alternate_id", kParamAlternateId},
    {"since", kParamSince},
};

constexpr DaemonEndpoint kDaemonEndpoints[] = {
    {"health", "/v1/health", 0, 0, 1, false, false, false, false,
     kNoSiteFilter},
    // Served on every console; a per-virtual-host console receives only its
    // own site's row of the serve savings per host.
    {"stats", "/v1/stats", 0, 0, 1, false, false, false, false,
     kServeSavingsSiteFilter},
    // Served on every console (the status page shows it); a
    // per-virtual-host console receives only its own site's entries.
    {"cooldowns", "/v1/cache/cooldowns", 0, 0, 1, false, false, false, false,
     kCooldownsSiteFilter},
    {"cache/urls", "/v1/cache/urls",
     kParamOffset | kParamLimit | kParamHostname, 0 /* required */,
     1 /* max_in_flight */, true /* whole_server_only */,
     false /* not_found_is_answer */, false /* serves_content */,
     false /* distinct_too_large */, kNoSiteFilter},
    {"cache/alternates", "/v1/cache/alternates",
     kParamUrl | kParamHostname | kParamScheme,
     kParamUrl | kParamHostname | kParamScheme, 1 /* max_in_flight */,
     true /* whole_server_only */, true /* not_found_is_answer */,
     false /* serves_content */, false /* distinct_too_large */, kNoSiteFilter},
    {"cache/content", "/v1/cache/content",
     kParamUrl | kParamHostname | kParamScheme | kParamAlternateId,
     kParamUrl | kParamHostname | kParamScheme | kParamAlternateId,
     2 /* max_in_flight: parallel image loads */, true /* whole_server_only */,
     true /* not_found_is_answer */, true /* serves_content */,
     false /* distinct_too_large: content_too_large has its own branch */,
     kNoSiteFilter},
    // The optimizer's recent log ring: whole-server data (one ring for
    // every host the daemon serves), so the same gates as the cache index.
    // Its pages are bounded by the optimizer, so an over-cap answer is a
    // malformed peer, not an unreachable one -- it gets its own code.
    {"logs", "/v1/logs", kParamSince | kParamLimit, 0 /* required */,
     1 /* max_in_flight */, true /* whole_server_only */,
     false /* not_found_is_answer */, false /* serves_content */,
     true /* distinct_too_large */, kNoSiteFilter},
};

static_assert(sizeof(kDaemonEndpoints) / sizeof(kDaemonEndpoints[0]) ==
              AdminDaemonHandler::kNumEndpoints);

// Every daemon-proxy response -- success, upstream-unreachable, and
// error alike -- is not storable, its content type is pinned against
// sniffing (matching every other admin JSON response, see
// WriteJsonResponse/WriteJsonError in admin_site.cc), and it is
// same-origin only: a cross-origin page can neither load it nor learn
// from a load's success or failure whether the data exists.
void AddNoStoreHeaders(ResponseHeaders* headers) {
  headers->Add(HttpAttributes::kCacheControl, "no-store, private");
  headers->Add("X-Content-Type-Options", "nosniff");
  headers->Add("Cross-Origin-Resource-Policy", "same-origin");
}

// A client-supplied parameter name, made safe to echo in an error
// body: at most 64 bytes, every byte outside [A-Za-z0-9_%.-] replaced
// by '_'.
GoogleString SanitizedParamName(StringPiece name) {
  const size_t n = std::min<size_t>(name.size(), 64);
  GoogleString out;
  out.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    const char c = name[i];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '%' ||
                    c == '.' || c == '-';
    out.push_back(ok ? c : '_');
  }
  return out;
}

// offset/limit/alternate_id: decimal digits only, within [min, max].
// (StringToInt64 alone would accept "45x" as 45 -- check the digits.)
bool ParseDecimalParam(StringPiece value, int64 min, int64 max, int64* out) {
  if (value.empty() || value.size() > 10) {
    return false;
  }
  for (char c : value) {
    if (c < '0' || c > '9') {
      return false;
    }
  }
  int64 v = 0;
  if (!StringToInt64(value, &v) || v < min || v > max) {
    return false;
  }
  *out = v;
  return true;
}

// since: decimal digits only, 0..2^63-1 (up to 19 digits -- a sequence
// cursor, not a page-size number, so ParseDecimalParam's 10-character cap
// does not apply; StringToInt64's overflow check rejects a 19-digit value
// above INT64_MAX).
bool ParseSeqParam(StringPiece value, int64* out) {
  if (value.empty() || value.size() > 19) {
    return false;
  }
  for (char c : value) {
    if (c < '0' || c > '9') {
      return false;
    }
  }
  return StringToInt64(value, out);
}

// hostname: 1..253 bytes of a conservative host character set (plus
// the brackets of an IPv6 literal, as GoogleUrl::Host() records one);
// the value is re-encoded by the caller before it goes upstream.
bool IsValidHostnameParam(StringPiece value) {
  if (value.empty() || value.size() > 253) {
    return false;
  }
  for (char c : value) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                    c == ':' || c == '-' || c == '[' || c == ']';
    if (!ok) {
      return false;
    }
  }
  return true;
}

// url: the origin-form path + query the module records for a cached
// entry (GoogleUrl::PathAndLeaf(), e.g. "/hero.png?v=2"): a leading
// '/', at most 2048 bytes after one percent-decode, printable ASCII
// only (0x21-0x7E: no space, control byte, DEL or byte >= 0x80) and no
// '#'.  The recorded form is canonical ASCII, so this rejects nothing
// the daemon can hold.  The caller re-encodes the value, so a '?', '&',
// '=', '%' or '+' inside it reaches the daemon escaped.
bool IsValidUrlParam(StringPiece value) {
  if (value.empty() || value.size() > 2048 || value[0] != '/') {
    return false;
  }
  for (const char c : value) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x21 || u > 0x7e || c == '#') {
      return false;
    }
  }
  return true;
}

// scheme: the daemon's own vocabulary, exactly "http" or "https".
bool IsValidSchemeParam(StringPiece value) {
  return value == "http" || value == "https";
}

// Extracts the bare media type from a Content-Type value: the token
// before ';', whitespace-trimmed, ASCII-lowercased.  A missing (or, via
// Lookup1, duplicated) header yields "".
GoogleString NormalizeMediaType(const char* content_type) {
  GoogleString media_type(content_type == nullptr ? "" : content_type);
  const size_t semicolon = media_type.find(';');
  if (semicolon != GoogleString::npos) {
    media_type.resize(semicolon);
  }
  GoogleString trimmed;
  TrimWhitespace(StringPiece(media_type), &trimmed);
  for (char& c : trimmed) {
    if (c >= 'A' && c <= 'Z') {
      c += 'a' - 'A';
    }
  }
  return trimmed;
}

// The five inert image media types the content leaf serves, and the
// filename extension for each; nullptr for anything else.
const char* ContentExtension(StringPiece media_type) {
  if (media_type == "image/png") {
    return "png";
  } else if (media_type == "image/jpeg") {
    return "jpg";
  } else if (media_type == "image/gif") {
    return "gif";
  } else if (media_type == "image/webp") {
    return "webp";
  } else if (media_type == "image/avif") {
    return "avif";
  }
  return nullptr;
}

// The byte-serving leaf's extra hardening, on every one of its
// responses: the bytes can never run as a document or be framed.
// (nosniff, no-store and same-origin come from AddNoStoreHeaders.)
void AddContentHardeningHeaders(ResponseHeaders* headers) {
  headers->Add("Content-Security-Policy",
               "sandbox; default-src 'none'; frame-ancestors 'none'");
}

// cache/content reads admitted across EVERY handler in this server
// worker process.  There is one AdminSite, and so one AdminDaemonHandler,
// per server context (one per virtual host); the per-handler counters
// alone would multiply the buffered-bytes bound by the number of hosts.
// With this counter the proxy holds at most max_in_flight x the
// content cap of upstream body (32 MiB) per server worker process.
std::atomic<int> g_content_reads_in_flight{0};

// Admits one more concurrent read on `counter` if fewer than
// `max_in_flight` are admitted; false (nothing changed) otherwise.
bool TryAcquireSlot(std::atomic<int>* counter, int max_in_flight) {
  int in_flight = counter->load(std::memory_order_acquire);
  do {
    if (in_flight >= max_in_flight) {
      return false;
    }
  } while (!counter->compare_exchange_weak(in_flight, in_flight + 1,
                                           std::memory_order_acq_rel,
                                           std::memory_order_acquire));
  return true;
}

}  // namespace

// DaemonProxyFetch captures the upstream daemon response and forwards status,
// content-type, and body (only those) to the original client fetch.
// Self-deletes on completion.  Named at namespace scope (not the anonymous
// namespace above) so AdminDaemonHandler can befriend it.  For the one
// byte-serving endpoint it instead applies the media-type gate and the
// hardening headers described in HandleDone, on every response.
class DaemonProxyFetch : public StringAsyncFetch {
 public:
  DaemonProxyFetch(AsyncFetch* client_fetch, AdminDaemonHandler* handler,
                   int endpoint_index, bool head_request,
                   const GoogleString& content_filename_stem,
                   uint8_t site_filter, StringPiece own_serve_host,
                   MessageHandler* message_handler)
      : StringAsyncFetch(client_fetch->request_context()),
        client_fetch_(client_fetch),
        handler_(handler),
        endpoint_index_(endpoint_index),
        head_request_(head_request),
        content_filename_stem_(content_filename_stem),
        site_filter_(site_filter),
        own_serve_host_(own_serve_host.as_string()),
        message_handler_(message_handler) {}

  void HandleDone(bool success) override {
    set_success(success);
    set_done(true);

    const DaemonEndpoint& endpoint = kDaemonEndpoints[endpoint_index_];
    ResponseHeaders* client_headers = client_fetch_->response_headers();
    const int status = response_headers()->status_code();

    // Every JSON branch pins the client Content-Type to JSON and marks the
    // response not storable/unsniffable/same-origin; the byte-serving leaf
    // adds its sandboxing CSP to every response.
    auto set_client_status = [&](int client_status) {
      client_headers->set_status_code(client_status);
      client_headers->Add(HttpAttributes::kContentType,
                          kContentTypeJson.mime_type());
      AddNoStoreHeaders(client_headers);
      if (endpoint.serves_content) {
        AddContentHardeningHeaders(client_headers);
      }
    };

    if (!success || status < 100) {
      // The reader says why the read failed.  Only an observed over-cap
      // body is "too large" (never a truncated image or page); a timeout,
      // a drop, a disabled or unreachable transport is "unreachable".  On
      // the JSON leaves every failure is "unreachable", except where the
      // table gives an over-cap answer its own code.
      set_client_status(HttpStatus::kBadGateway);
      if (endpoint.serves_content &&
          read_failure_ == DaemonReadFailure::kTooLarge) {
        client_fetch_->Write("{\"error\":\"content_too_large\"}",
                             message_handler_);
      } else if (endpoint.distinct_too_large &&
                 read_failure_ == DaemonReadFailure::kTooLarge) {
        client_fetch_->Write("{\"error\":\"response_too_large\"}",
                             message_handler_);
      } else {
        client_fetch_->Write("{\"error\":\"daemon_unreachable\"}",
                             message_handler_);
      }
    } else if (status == HttpStatus::kNotFound) {
      if (endpoint.not_found_is_answer) {
        // Every supported optimizer serves this route, so its 404 is
        // the per-URL answer: the entry is not in the index.  The
        // module writes its own reason code and drops the daemon's
        // body -- the console never decides a state from text an
        // untrusted socket peer controls.
        set_client_status(HttpStatus::kNotFound);
        client_fetch_->Write("{\"error\":\"not_in_index\"}", message_handler_);
      } else {
        // The endpoint table only names paths this module knows about;
        // on the other endpoints a 404 upstream means the running
        // optimizer predates this endpoint.
        set_client_status(HttpStatus::kNotImplemented);
        client_fetch_->Write("{\"error\":\"endpoint_unsupported_by_daemon\"}",
                             message_handler_);
      }
    } else if (endpoint.serves_content) {
      if (status != HttpStatus::kOK) {
        // Only a gated 200 serves upstream bytes; any other upstream
        // status is answered here, with none of the daemon's bytes.
        set_client_status(HttpStatus::kBadGateway);
        client_fetch_->Write("{\"error\":\"daemon_error\"}", message_handler_);
      } else {
        // The upstream Content-Type decides: exactly five inert image
        // types pass (parameters dropped); anything else is a 415 JSON
        // error with NO upstream bytes.
        const GoogleString media_type = NormalizeMediaType(
            response_headers()->Lookup1(HttpAttributes::kContentType));
        const char* ext = ContentExtension(media_type);
        if (ext == nullptr) {
          set_client_status(HttpStatus::kUnsupportedMediaType);
          client_fetch_->Write("{\"error\":\"unsupported_content_type\"}",
                               message_handler_);
        } else {
          client_headers->set_status_code(HttpStatus::kOK);
          client_headers->Add(HttpAttributes::kContentType, media_type);
          AddNoStoreHeaders(client_headers);
          AddContentHardeningHeaders(client_headers);
          client_headers->Add("Content-Disposition",
                              StrCat("inline; filename=\"",
                                     content_filename_stem_, ".", ext, "\""));
          if (!head_request_) {
            client_fetch_->Write(buffer(), message_handler_);
          }
        }
      }
    } else if (site_filter_ != kNoSiteFilter) {
      // A per-virtual-host console on a leaf whose answer names other
      // sites: the module rebuilds what this site may see and sends none of
      // the optimizer's bytes it did not read.  An answer it cannot read is
      // "unreachable"; an upstream error status keeps its status without the
      // optimizer's body.
      if (head_request_) {
        set_client_status(status);
      } else if (status != HttpStatus::kOK) {
        set_client_status(status);
        client_fetch_->Write("{\"error\":\"daemon_error\"}", message_handler_);
      } else {
        GoogleString site_body;
        const bool readable =
            site_filter_ == kServeSavingsSiteFilter
                ? FilterStatsForSite(buffer(), own_serve_host_, &site_body)
                : FilterCooldownsForSite(buffer(), own_serve_host_, &site_body);
        if (readable) {
          set_client_status(HttpStatus::kOK);
          client_fetch_->Write(site_body, message_handler_);
        } else {
          set_client_status(HttpStatus::kBadGateway);
          client_fetch_->Write("{\"error\":\"daemon_unreachable\"}",
                               message_handler_);
        }
      }
    } else {
      // JSON leaves: forward ONLY status + body; no upstream response
      // headers are copied.  The Content-Type is pinned to JSON rather
      // than taken from upstream (the socket peer is untrusted).
      set_client_status(status);
      if (!head_request_) {
        client_fetch_->Write(buffer(), message_handler_);
      }
    }
    client_fetch_->Done(true);

    // Release the in-flight slot as the LAST handler access before
    // self-delete, so the handler's shutdown drain cannot observe the slot
    // released while we still touch handler state.
    handler_->ReleaseSlot(endpoint_index_);
    delete this;
  }

 public:
  // Where the reader records why the read failed (DaemonReader::Get).
  DaemonReadFailure* read_failure() { return &read_failure_; }

 private:
  AsyncFetch* client_fetch_;
  AdminDaemonHandler* handler_;
  int endpoint_index_;
  bool head_request_;
  GoogleString content_filename_stem_;
  uint8_t site_filter_;          // kNoSiteFilter on the whole-server console
  GoogleString own_serve_host_;  // the per-virtual-host console's own host
  MessageHandler* message_handler_;
  DaemonReadFailure read_failure_ = DaemonReadFailure::kNone;

  DaemonProxyFetch(const DaemonProxyFetch&) = delete;
  DaemonProxyFetch& operator=(const DaemonProxyFetch&) = delete;
};

AdminDaemonHandler::AdminDaemonHandler(DaemonReader* reader,
                                       MessageHandler* message_handler)
    : reader_(reader), message_handler_(message_handler) {}

AdminDaemonHandler::~AdminDaemonHandler() {
  // Wait for in-flight proxy fetches to complete so their HandleDone() never
  // touches a destroyed handler.  Bounded: the transport's upstream timeout
  // (5s) guarantees every fetch completes, so this is a short drain, not a
  // shutdown hang.  (Same pattern as ~AdminLicenseHandler.)
  static constexpr int64_t kDestructorTimeoutMs = 10000;
  static constexpr int64_t kPollIntervalMs = 10;
  int64_t waited_ms = 0;
  for (;;) {
    bool any_in_flight = false;
    for (int i = 0; i < kNumEndpoints; ++i) {
      if (in_flight_[i].load(std::memory_order_acquire) != 0) {
        any_in_flight = true;
        break;
      }
    }
    if (!any_in_flight) {
      return;
    }
    if (waited_ms >= kDestructorTimeoutMs) {
      // Use fprintf instead of LOG() -- the logging sink may already be torn
      // down during process shutdown, causing a use-after-free crash.
      fprintf(stderr,
              "AdminDaemonHandler: timed out waiting for in-flight "
              "daemon fetches to complete during shutdown\n");
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
    waited_ms += kPollIntervalMs;
  }
}

bool AdminDaemonHandler::HandleRequest(StringPiece leaf,
                                       const QueryParams& query_params,
                                       bool is_global,
                                       StringPiece own_serve_host,
                                       AsyncFetch* fetch) {
  int index = -1;
  for (int i = 0; i < kNumEndpoints; ++i) {
    if (leaf == kDaemonEndpoints[i].leaf) {
      index = i;
      break;
    }
  }
  if (index < 0) {
    return false;  // Caller responds 404.
  }
  const DaemonEndpoint& endpoint = kDaemonEndpoints[index];

  // GET/HEAD only: the daemon's socket transport is unauthenticated for
  // every method, so this proxy must never originate a state-changing
  // request.
  const RequestHeaders* request_headers = fetch->request_headers();
  const RequestHeaders::Method method = request_headers != nullptr
                                            ? request_headers->method()
                                            : RequestHeaders::kGet;
  if (method != RequestHeaders::kGet && method != RequestHeaders::kHead) {
    fetch->response_headers()->Add(HttpAttributes::kAllow, "GET, HEAD");
    WriteError(fetch, HttpStatus::kMethodNotAllowed,
               "method not allowed; daemon endpoints are read-only",
               endpoint.serves_content);
    return true;
  }

  // Leaves whose data spans every virtual host are served on the
  // whole-server console only: on a per-vhost console they would show
  // one host's operator another host's data.  Checked before any
  // parameter is looked at.
  if (endpoint.whole_server_only && !is_global) {
    WriteError(fetch, HttpStatus::kForbidden, "whole_server_console_only",
               endpoint.serves_content);
    return true;
  }

  // Those same leaves also refuse a cross-site browser request outright.
  // Cross-Origin-Resource-Policy already stops a cross-site page from
  // reading the response, but the request would still reach the daemon
  // with the admin's own access, so a page that never reads the response
  // could still use load timing or slot exhaustion.  Checked before any
  // parameter is looked at or the daemon is contacted.  A request without
  // the header (a non-browser client, e.g. monitoring) is unaffected.
  if (endpoint.whole_server_only) {
    const char* sec_fetch_site =
        request_headers != nullptr ? request_headers->Lookup1("Sec-Fetch-Site")
                                   : nullptr;
    if (sec_fetch_site != nullptr) {
      const StringPiece site(sec_fetch_site);
      if (site != "same-origin" && site != "none") {
        WriteError(fetch, HttpStatus::kForbidden, "cross_site_request",
                   endpoint.serves_content);
        return true;
      }
    }
  }

  // Every query parameter must be known (exact, case-sensitive name),
  // allow-listed for this leaf and appear at most once.  The upstream
  // query is rebuilt from validated values, so the raw client query
  // string never leaves this process.
  uint32_t seen = 0;
  for (int i = 0; i < query_params.size(); ++i) {
    const StringPiece name = query_params.name(i);
    uint32_t bit = 0;
    for (const ParamName& p : kParamNames) {
      if (name == p.name) {
        bit = p.bit;
        break;
      }
    }
    if (bit == 0) {
      WriteParamError(fetch, "unknown_parameter", name,
                      endpoint.serves_content);
      return true;
    }
    if ((endpoint.allowed_params & bit) == 0) {
      WriteParamError(fetch, "parameter_not_allowed", name,
                      endpoint.serves_content);
      return true;
    }
    if ((seen & bit) != 0) {
      WriteParamError(fetch, "invalid_parameter", name,
                      endpoint.serves_content);
      return true;
    }
    seen |= bit;
  }

  // Required parameters must be present before any value is validated.
  const uint32_t missing = endpoint.required_params & ~seen;
  if (missing != 0) {
    for (const ParamName& p : kParamNames) {
      if ((missing & p.bit) != 0) {
        WriteParamError(fetch, "missing_parameter", p.name,
                        endpoint.serves_content);
        return true;
      }
    }
  }

  // Validate the allow-listed values (each decoded exactly once, and
  // each must carry one) and rebuild the upstream query in a fixed
  // order from the re-encoded values.  Nothing the client sent is
  // copied verbatim.
  GoogleString hostname_enc;
  bool has_hostname = false;
  int64 offset_value = 0;
  bool has_offset = false;
  int64 limit_value = 0;
  bool has_limit = false;
  GoogleString url_enc;
  bool has_url = false;
  GoogleString scheme_value;
  bool has_scheme = false;
  int64 alternate_id_value = 0;
  bool has_alternate_id = false;
  int64 since_value = 0;
  bool has_since = false;
  for (int i = 0; i < query_params.size(); ++i) {
    const StringPiece name = query_params.name(i);
    GoogleString value;
    if (!query_params.UnescapedValue(i, &value)) {
      // "?offset" with no '=': a name without a value.
      WriteParamError(fetch, "invalid_parameter", name,
                      endpoint.serves_content);
      return true;
    }
    if (name == "offset") {
      if (!ParseDecimalParam(value, 0, 1000000000, &offset_value)) {
        WriteParamError(fetch, "invalid_parameter", name,
                        endpoint.serves_content);
        return true;
      }
      has_offset = true;
    } else if (name == "since") {
      if (!ParseSeqParam(value, &since_value)) {
        WriteParamError(fetch, "invalid_parameter", name,
                        endpoint.serves_content);
        return true;
      }
      has_since = true;
    } else if (name == "limit") {
      if (!ParseDecimalParam(value, 1, 500, &limit_value)) {
        WriteParamError(fetch, "invalid_parameter", name,
                        endpoint.serves_content);
        return true;
      }
      has_limit = true;
    } else if (name == "hostname") {
      if (!IsValidHostnameParam(value)) {
        WriteParamError(fetch, "invalid_parameter", name,
                        endpoint.serves_content);
        return true;
      }
      hostname_enc = GoogleUrl::EscapeQueryParam(value);
      has_hostname = true;
    } else if (name == "url") {
      if (!IsValidUrlParam(value)) {
        WriteParamError(fetch, "invalid_parameter", name,
                        endpoint.serves_content);
        return true;
      }
      url_enc = GoogleUrl::EscapeQueryParam(value);
      has_url = true;
    } else if (name == "scheme") {
      if (!IsValidSchemeParam(value)) {
        WriteParamError(fetch, "invalid_parameter", name,
                        endpoint.serves_content);
        return true;
      }
      scheme_value = value;
      has_scheme = true;
    } else if (name == "alternate_id") {
      if (!ParseDecimalParam(value, 0, 255, &alternate_id_value)) {
        WriteParamError(fetch, "invalid_parameter", name,
                        endpoint.serves_content);
        return true;
      }
      has_alternate_id = true;
    }
    // A registry name reaches this loop only if its leaf allow-lists
    // it, and a name is allow-listed only together with its branch
    // here: no value passes unvalidated.
  }

  GoogleString upstream_path(endpoint.upstream_path);
  const char* sep = "?";
  if (has_url) {
    StrAppend(&upstream_path, sep, "url=", url_enc);
    sep = "&";
  }
  if (has_hostname) {
    StrAppend(&upstream_path, sep, "hostname=", hostname_enc);
    sep = "&";
  }
  if (has_scheme) {
    StrAppend(&upstream_path, sep, "scheme=", scheme_value);
    sep = "&";
  }
  if (has_offset) {
    StrAppend(&upstream_path, sep, "offset=", Integer64ToString(offset_value));
    sep = "&";
  }
  if (has_since) {
    StrAppend(&upstream_path, sep, "since=", Integer64ToString(since_value));
    sep = "&";
  }
  if (has_limit) {
    StrAppend(&upstream_path, sep, "limit=", Integer64ToString(limit_value));
    sep = "&";
  }
  if (has_alternate_id) {
    StrAppend(&upstream_path, sep,
              "alternate_id=", Integer64ToString(alternate_id_value));
    sep = "&";
  }

  // Per-endpoint in-flight slots: polling panels on different endpoints
  // must not 429 each other, and one endpoint admits at most
  // `max_in_flight` concurrent upstream reads.  The byte-serving leaf's
  // reads are ALSO admitted process-wide (see g_content_reads_in_flight);
  // the per-handler counter stays, for the destructor's drain.
  if (!TryAcquireSlot(&in_flight_[index], endpoint.max_in_flight)) {
    WriteError(fetch, 429,
               "a request for this daemon endpoint is already in flight",
               endpoint.serves_content);
    return true;
  }
  if (endpoint.serves_content &&
      !TryAcquireSlot(&g_content_reads_in_flight, endpoint.max_in_flight)) {
    in_flight_[index].fetch_sub(1, std::memory_order_release);
    WriteError(fetch, 429,
               "a request for this daemon endpoint is already in flight",
               endpoint.serves_content);
    return true;
  }

  if (reader_ == nullptr) {
    // No daemon transport: the port has none, or its DaemonApiSocketPath is
    // empty (the ports' NewDaemonReader() then returns no reader).  There is
    // nothing to proxy to, which is a configuration state, not a transport
    // failure -- distinct from an upstream that fails to answer.
    ReleaseSlot(index);
    WriteError(fetch, HttpStatus::kUnavailable, "daemon_not_configured",
               endpoint.serves_content);
    return true;
  }

  GoogleString content_filename_stem;
  if (endpoint.serves_content) {
    // The download filename is built from the validated id -- never from
    // raw client input or upstream headers.
    content_filename_stem =
        StrCat("variant-", Integer64ToString(alternate_id_value));
  }
  // The whole-server console is never narrowed; a per-virtual-host console
  // gets the leaf's site filter (most leaves have none).
  const uint8_t site_filter = is_global ? kNoSiteFilter : endpoint.site_filter;
  DaemonProxyFetch* proxy_fetch = new DaemonProxyFetch(
      fetch, this, index, method == RequestHeaders::kHead,
      content_filename_stem, site_filter, own_serve_host, message_handler_);
  // The upstream request is built from scratch: fixed method, fixed path
  // from the table, and only the validated, re-encoded query above.
  proxy_fetch->request_headers()->set_method(method);
  reader_->Get(upstream_path,
               endpoint.serves_content ? kMaxContentResponseBytes
                                       : kMaxJsonResponseBytes,
               proxy_fetch, proxy_fetch->read_failure());
  return true;
}

void AdminDaemonHandler::AddProxyResponseHeaders(ResponseHeaders* headers) {
  AddNoStoreHeaders(headers);
}

void AdminDaemonHandler::WriteError(AsyncFetch* fetch, int status_code,
                                    StringPiece error, bool content_leaf) {
  ResponseHeaders* headers = fetch->response_headers();
  headers->set_status_code(status_code);
  headers->Add(HttpAttributes::kContentType, kContentTypeJson.mime_type());
  AddNoStoreHeaders(headers);
  if (content_leaf) {
    AddContentHardeningHeaders(headers);
  }
  GoogleString json = StrCat("{\"error\":\"", JsonEscape(error), "\"}");
  fetch->Write(json, message_handler_);
  fetch->Done(true);
}

void AdminDaemonHandler::WriteParamError(AsyncFetch* fetch, StringPiece code,
                                         StringPiece param, bool content_leaf) {
  ResponseHeaders* headers = fetch->response_headers();
  headers->set_status_code(HttpStatus::kBadRequest);
  headers->Add(HttpAttributes::kContentType, kContentTypeJson.mime_type());
  AddNoStoreHeaders(headers);
  if (content_leaf) {
    AddContentHardeningHeaders(headers);
  }
  GoogleString json =
      StrCat("{\"error\":\"", JsonEscape(code), "\",\"parameter\":\"",
             JsonEscape(SanitizedParamName(param)), "\"}");
  fetch->Write(json, message_handler_);
  fetch->Done(true);
}

void AdminDaemonHandler::ReleaseSlot(int index) {
  if (kDaemonEndpoints[index].serves_content) {
    g_content_reads_in_flight.fetch_sub(1, std::memory_order_release);
  }
  // The per-handler slot last: the shutdown drain watches it.
  in_flight_[index].fetch_sub(1, std::memory_order_release);
}

int AdminDaemonHandler::ContentReadsInFlightForTesting() {
  return g_content_reads_in_flight.load(std::memory_order_acquire);
}

}  // namespace net_instaweb
