// Copyright 2026 We-Amp B.V.
// Licensed under the Apache License, Version 2.0 (the "License").
//
// Implementation of the nginx Web-Bot-Auth (RFC 9421) wiring -- observe-only
// unless WebBotAuthBotDetection is on, in which case a verified signature also
// classifies the request as an automated client for PageSpeed's own bot
// detection (see ngx_webbotauth_handler.h). the design record Amendment A1: the FREE
// verifier -- no 401/402, no enforcement, no RSL-CAP, no metering beyond an
// opt-in counter.
//
// A1 v1 scope: signer keys come from an operator-LOCAL JWKS file
// (WebBotAuthKeyDirectoryFile) and are resolved SYNCHRONOUSLY in-memory. The
// network-fetch directory provider is deliberately NOT used here because its
// async fetch cannot complete inline in an nginx phase handler; automatic
// network refresh of the directory is a tracked follow-up.

#include "ngx_webbotauth_handler.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "ngx_rewrite_options.h"
#include "ngx_server_context.h"
#include "pagespeed/kernel/base/file_system.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/kernel/http/content_type.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "pagespeed/kernel/webbotauth/classifier.h"
#include "pagespeed/kernel/webbotauth/key_directory.h"
#include "pagespeed/kernel/webbotauth/rsl_cap_token.h"
#include "pagespeed/kernel/webbotauth/rsl_cap_validator.h"
#include "pagespeed/kernel/webbotauth/static_key_directory.h"
#include "pagespeed/kernel/webbotauth/verifier.h"
#include "pagespeed/kernel/webbotauth/webbotauth_counter_store.h"

namespace net_instaweb {

const char kWebBotAuthVerifiedSignedRequests[] =
    "web_bot_auth_verified_signed_requests";
// Signature material that is NOT web-bot-auth (parseable Signature-Input but
// no member tagged "web-bot-auth" -- e.g. a CDN or other RFC 9421 signing
// scheme). Such requests are classified as if unsigned ($x_verified_bot ==
// "human", never "unknown"); this counter keeps them visible instead of
// silently ignored. Bare/unparseable signature material (e.g. draft-cavage
// `Signature` headers from fediverse/webhook signers) cannot be attributed to
// any scheme, so it fail-closes to "unknown" and is counted NOWHERE -- the
// verdict variable is its only surface. (Upstreamed from the optimizer line.)
const char kWebBotAuthOtherSignatureRequests[] =
    "web_bot_auth_other_signature_requests";

void ps_webbotauth_init_stats(Statistics* statistics) {
  statistics->AddVariable(kWebBotAuthVerifiedSignedRequests);
  statistics->AddVariable(kWebBotAuthOtherSignatureRequests);
}

namespace {

// The index of the $x_verified_bot variable, obtained once at postconfiguration
// (ps_init) so the preaccess handler can store the computed verdict directly,
// making the variable get-handler a no-recompute reader on the common path.
ngx_int_t g_xvb_var_index = NGX_ERROR;

// Maximum size of the local JWKS key-directory file we will read.
const int64_t kMaxKeyDirectoryFileBytes = int64_t{256} * 1024;

// Hardening: cap the Authorization header length BEFORE parsing an
// RSL-CAP token, so an abusive multi-KB header cannot drive parser work. A real
// RSL-CAP token is well under this; operators can also bound it upstream via
// large_client_header_buffers. Oversized -> treated as no valid token (401).
const size_t kMaxRslCapAuthHeaderBytes = size_t{8} * 1024;

// =================================================================
// Web Bot Auth opt-in counter state + doc builder
// =================================================================

// The shared memory-mapped counter store. Mapped once by the master in
// ps_webbotauth_counter_map (before workers fork); the MAP_SHARED region is
// inherited by every forked worker, so all workers increment the same physical
// counters. nullptr until mapped (or if mapping failed) -- every Record* /
// reader treats null as "no counting".
webbotauth::WebBotAuthCounterStats* g_wba_counter = nullptr;

// The SECRET bearer token gating the EXACT counter document. Read ONCE from the
// environment (PAGESPEED_WEB_BOT_AUTH_COUNTER_TOKEN) at worker init -- NEVER via
// a config directive (pagespeed config is world-readable). Empty = no token
// configured (the exact doc is then unreachable; only the coarse/404 surfaces
// exist). NEVER logged, echoed, or exposed on any surface.
GoogleString g_wba_counter_token;

// JSON-escape operator-controlled free text (signer names, keyids): escape the
// two structural characters (" and \), ALSO escape < and > (as </>)
// so a `</script>` in an operator name can never break out of a <script> block
// if this doc is ever embedded in HTML (stored-XSS defense -- names are operator
// free text), DROP control characters entirely, and cap the input length on a
// UTF-8 codepoint boundary (a mid-multibyte cut would emit invalid JSON a
// malicious operator could weaponize as denial-of-parsing downstream). Non-ASCII
// bytes otherwise pass through unchanged (valid UTF-8 in a JSON string needs no
// escaping).
std::string CounterJsonEscape(std::string_view in, size_t max_len) {
  size_t n = in.size() > max_len ? max_len : in.size();
  if (n < in.size()) {
    while (n > 0 && (static_cast<unsigned char>(in[n]) & 0xC0) == 0x80) {
      --n;
    }
  }
  std::string out;
  out.reserve(n + 8);
  for (size_t i = 0; i < n; ++i) {
    unsigned char c = static_cast<unsigned char>(in[i]);
    if (c == '"') {
      out += "\\\"";
    } else if (c == '\\') {
      out += "\\\\";
    } else if (c == '<') {
      out += "\\u003c";
    } else if (c == '>') {
      out += "\\u003e";
    } else if (c < 0x20 || c == 0x7f) {
      continue;  // drop control chars (incl. CR/LF/TAB)
    } else {
      out += static_cast<char>(c);
    }
  }
  return out;
}

// Render a 16-byte boot identity as a canonical RFC-4122 UUID string.
std::string CounterFormatUuid(const uint8_t id[16]) {
  static const char* kHex = "0123456789abcdef";
  std::string s;
  s.reserve(36);
  for (int i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) s += '-';
    s += kHex[(id[i] >> 4) & 0x0F];
    s += kHex[id[i] & 0x0F];
  }
  return s;
}

// 64-bit hash -> 16 lowercase hex chars (reproducible identifier for a verified
// signer whose keyid is not in the operator registry -- the engine stores only
// the unsalted FNV-1a-64 of the keyid, never the raw keyid).
std::string CounterFormatHashHex(uint64_t h) {
  static const char* kHex = "0123456789abcdef";
  std::string s;
  s.reserve(16);
  for (int shift = 60; shift >= 0; shift -= 4) {
    s += kHex[(h >> shift) & 0x0F];
  }
  return s;
}

// Days since the Unix epoch -> "YYYY-MM-DD" (UTC, Hinnant civil-from-days).
std::string CounterFormatSince(uint64_t unix_day) {
  int64_t z = static_cast<int64_t>(unix_day) + 719468;
  int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  uint64_t doe = static_cast<uint64_t>(z - era * 146097);
  uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t y = static_cast<int64_t>(yoe) + era * 400;
  uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  uint64_t mp = (5 * doy + 2) / 153;
  uint64_t d = doy - (153 * mp + 2) / 5 + 1;
  uint64_t m = mp < 10 ? mp + 3 : mp - 9;
  y += (m <= 2);
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%04lld-%02llu-%02llu",
                static_cast<long long>(y), static_cast<unsigned long long>(m),
                static_cast<unsigned long long>(d));
  return std::string(buf);
}

// Coarse count tier (~2 significant figures) for the public/unauthenticated
// doc. Fixed, monotone buckets -- the exact cumulative value is never exposed to
// an unauthenticated caller.
const char* CounterCoarseTier(uint64_t n) {
  if (n == 0) return "0";
  if (n < 100) return "1-99";
  if (n < 1000) return "100-999";
  if (n < 10000) return "1k-10k";
  if (n < 100000) return "10k-100k";
  if (n < 1000000) return "100k-1M";
  return "1M+";
}

inline uint64_t CounterLoad(uint64_t& f) {
  return std::atomic_ref<uint64_t>(f).load(std::memory_order_relaxed);
}

// Build the counter doc. `exact` selects the token-gated EXACT doc vs the public
// COARSE doc. The two docs differ deliberately, to minimise fingerprinting on
// the unauthenticated surface:
//   EXACT : {since, boot_id, raw totals, by_signer:[{kid,name?,count}] for
//            EVERY verified signer (registered + hash-only), other_verified_bots}
//   COARSE: {tiered totals, by_signer:[{name,count-tier}] for REGISTERED signers
//            ONLY, other_verified_bots tier} -- no `since` (instance-age
//            fingerprint), no boot_id, no hash-only entries, no exact distinct-
//            signer count, no raw cumulative.
// Verify latency is NEVER on either doc (first-party statistics only). The doc
// is byte-compatible with ModPageSpeed 2.0's identical schema.
std::string BuildCounterDoc(bool exact,
                            const webbotauth::VerifiedBotRegistry& registry) {
  webbotauth::WebBotAuthCounterStats* s = g_wba_counter;

  uint64_t verified = 0, invalid = 0, other = 0, other_bots = 0, since_day = 0;
  uint8_t boot[16] = {0};
  struct Sig {
    uint64_t hash;
    uint64_t count;
  };
  std::vector<Sig> sigs;
  if (s != nullptr) {
    verified = CounterLoad(s->verified_total);
    invalid = CounterLoad(s->invalid_total);
    other = CounterLoad(s->other_total);
    other_bots = CounterLoad(s->other_verified_bots);
    since_day = s->counting_since_unix_day;  // instance identity, set at create
    std::memcpy(boot, s->boot_id, sizeof(boot));
    for (auto& slot : s->signers) {
      uint64_t h = CounterLoad(slot.kid_hash);
      if (h != 0) sigs.push_back({h, CounterLoad(slot.count)});
    }
  }

  std::string doc;
  doc.reserve(256);
  doc += "{";
  bool first_field = true;
  auto sep = [&]() {
    if (!first_field) doc += ",";
    first_field = false;
  };
  if (exact) {
    // `since` and `boot_id` are exact-doc only (both instance fingerprints).
    sep();
    doc += "\"since\":\"";
    doc += CounterFormatSince(since_day);
    doc += "\"";
    sep();
    doc += "\"boot_id\":\"";
    doc += CounterFormatUuid(boot);
    doc += "\"";
  }
  auto count_field = [&](const char* key, uint64_t n) {
    sep();
    doc += "\"";
    doc += key;
    doc += "\":";
    if (exact) {
      doc += std::to_string(n);
    } else {
      doc += "\"";
      doc += CounterCoarseTier(n);
      doc += "\"";
    }
  };
  count_field("verified_total", verified);
  count_field("invalid_total", invalid);
  count_field("other_total", other);
  sep();
  doc += "\"by_signer\":[";
  bool first_sig = true;
  for (const auto& sig : sigs) {
    std::string kid, name;
    registry.ForEach([&](StringPiece k, StringPiece nm) {
      if (webbotauth::HashWebBotAuthKeyid(
              std::string_view(k.data(), k.size())) == sig.hash) {
        kid.assign(k.data(), k.size());
        name.assign(nm.data(), nm.size());
      }
    });
    const bool registered = !kid.empty();
    if (!exact && !registered) {
      // COARSE doc lists REGISTERED signers only (drop hash-only entries; do not
      // reveal the exact distinct-signer count).
      continue;
    }
    if (!registered) {
      // EXACT doc: verified signer not in the operator registry -- identify by
      // the reproducible (unsalted FNV-1a-64) keyid hash.
      kid = "hash:" + CounterFormatHashHex(sig.hash);
    }
    if (!first_sig) doc += ",";
    first_sig = false;
    doc += "{";
    if (exact) {
      doc += "\"kid\":\"";
      doc += CounterJsonEscape(kid, 128);
      doc += "\"";
      if (!name.empty()) {
        doc += ",\"name\":\"";
        doc += CounterJsonEscape(name, 64);
        doc += "\"";
      }
      doc += ",\"count\":";
      doc += std::to_string(sig.count);
    } else {
      // COARSE: name -> tier only (no kid/hash, no raw count).
      doc += "\"name\":\"";
      doc += CounterJsonEscape(name, 64);
      doc += "\",\"count\":\"";
      doc += CounterCoarseTier(sig.count);
      doc += "\"";
    }
    doc += "}";
  }
  doc += "]";
  count_field("other_verified_bots", other_bots);
  doc += "}";
  return doc;
}

std::mutex g_counter_render_mutex;
struct CounterRenderCache {
  std::string body;
  time_t rendered_at = 0;
  bool valid = false;
};
CounterRenderCache g_counter_exact_cache;
CounterRenderCache g_counter_coarse_cache;

// Render the requested doc into a short-lived per-kind cache under a lock, then
// copy it out. Bounds render cost under load; the returned copy is what the
// caller emits (send_out_headers_and_body then copies it again into the request
// pool, so the served bytes outlive both this lock and the local).
void RenderCounterDoc(bool exact,
                      const webbotauth::VerifiedBotRegistry& registry,
                      std::string* out) {
  std::lock_guard<std::mutex> lk(g_counter_render_mutex);
  CounterRenderCache& c =
      exact ? g_counter_exact_cache : g_counter_coarse_cache;
  time_t now = ngx_time();
  if (!c.valid || now - c.rendered_at >= 1) {
    c.body = BuildCounterDoc(exact, registry);
    c.rendered_at = now;
    c.valid = true;
  }
  *out = c.body;
}

// Case-insensitively find request header `name` (length `name_len`); returns
// its value as a StringPiece backed by the request (valid for the synchronous
// duration of the request), or an empty StringPiece if absent.
StringPiece FindRequestHeader(ngx_http_request_t* r, const char* name,
                              size_t name_len) {
  ngx_list_part_t* part = &r->headers_in.headers.part;
  ngx_table_elt_t* h = static_cast<ngx_table_elt_t*>(part->elts);
  for (ngx_uint_t i = 0;; i++) {
    if (i >= part->nelts) {
      if (part->next == nullptr) {
        break;
      }
      part = part->next;
      h = static_cast<ngx_table_elt_t*>(part->elts);
      i = 0;
    }
    if (h[i].key.len == name_len &&
        ngx_strncasecmp(h[i].key.data,
                        reinterpret_cast<u_char*>(const_cast<char*>(name)),
                        name_len) == 0) {
      return StringPiece(reinterpret_cast<char*>(h[i].value.data),
                         h[i].value.len);
    }
  }
  return StringPiece();
}

// Append one HeaderField (with the canonical lowercase `canonical_name`) for
// EVERY occurrence of request header `name` -- RFC 9421 field
// canonicalization joins repeated field lines, so all instances must be
// wired, not just the first. Values are the raw wire bytes (StringPieces into
// the request pool; valid for the synchronous duration of the request).
void CollectRequestHeaderFields(ngx_http_request_t* r, const char* name,
                                size_t name_len, StringPiece canonical_name,
                                std::vector<webbotauth::HeaderField>* out) {
  ngx_list_part_t* part = &r->headers_in.headers.part;
  ngx_table_elt_t* h = static_cast<ngx_table_elt_t*>(part->elts);
  for (ngx_uint_t i = 0;; i++) {
    if (i >= part->nelts) {
      if (part->next == nullptr) {
        break;
      }
      part = part->next;
      h = static_cast<ngx_table_elt_t*>(part->elts);
      i = 0;
    }
    if (h[i].key.len == name_len &&
        ngx_strncasecmp(h[i].key.data,
                        reinterpret_cast<u_char*>(const_cast<char*>(name)),
                        name_len) == 0) {
      webbotauth::HeaderField field;
      field.name = canonical_name;
      field.value = h[i].value.data == nullptr
                        ? StringPiece()
                        : StringPiece(reinterpret_cast<char*>(h[i].value.data),
                                      h[i].value.len);
      out->push_back(field);
    }
  }
}

// True iff ANY field line of request header `name` has a non-empty value.
// This is the signature-material presence probe: it must look at EVERY line
// (not just the first) so that an empty first field line cannot hide a real
// signature on a second line -- but it never allocates, keeping the common
// unsigned path allocation-free.
bool HasNonEmptyRequestHeader(ngx_http_request_t* r, const char* name,
                              size_t name_len) {
  ngx_list_part_t* part = &r->headers_in.headers.part;
  ngx_table_elt_t* h = static_cast<ngx_table_elt_t*>(part->elts);
  for (ngx_uint_t i = 0;; i++) {
    if (i >= part->nelts) {
      if (part->next == nullptr) {
        break;
      }
      part = part->next;
      h = static_cast<ngx_table_elt_t*>(part->elts);
      i = 0;
    }
    if (h[i].key.len == name_len && h[i].value.len > 0 &&
        ngx_strncasecmp(h[i].key.data,
                        reinterpret_cast<u_char*>(const_cast<char*>(name)),
                        name_len) == 0) {
      return true;
    }
  }
  return false;
}

// KeyDirectoryProvider over the operator-local JWKS file that defers the file
// read (up to kMaxKeyDirectoryFileBytes, synchronous) until the FIRST actual
// key lookup, memoized for the rest of the request. The verifier only calls
// GetKey after the signature material parsed, was tagged web-bot-auth, and
// passed the profile + freshness gates -- so requests that fail closed
// earlier (bare Signature header, untagged/other-tag material, malformed or
// unsupported profiles) never pay the file read.
class LazyFileKeyDirectory : public webbotauth::KeyDirectoryProvider {
 public:
  LazyFileKeyDirectory(NgxServerContext* server_context,
                       const GoogleString& kd_file)
      : server_context_(server_context), kd_file_(kd_file) {}

  webbotauth::KeyLookupResult GetKey(StringPiece host, StringPiece keyid,
                                     int64_t now_unix_sec) override {
    if (directory_ == nullptr) {
      // An empty/unreadable file yields an empty directory -> kNotFound ->
      // Verdict::kUnknown, the honest result when no key is available. The
      // path comes ONLY from operator config, never the request.
      GoogleString jwks_doc;
      if (!kd_file_.empty()) {
        FileSystem* fs = server_context_->file_system();
        if (fs != nullptr) {
          fs->ReadFile(kd_file_.c_str(), kMaxKeyDirectoryFileBytes, &jwks_doc,
                       server_context_->message_handler());
        }
      }
      directory_.reset(new webbotauth::StaticKeyDirectory(jwks_doc));
    }
    return directory_->GetKey(host, keyid, now_unix_sec);
  }

 private:
  NgxServerContext* const server_context_;
  const GoogleString& kd_file_;
  std::unique_ptr<webbotauth::StaticKeyDirectory> directory_;
};

// The comma-joined value of every occurrence of request header `name`
// (RFC 9110: repeated list-typed field lines are semantically the ", "-join).
// RFC 9421 section 4.1 explicitly contemplates an intermediary adding its
// signature as a SEPARATE Signature-Input/Signature field line, so parsing
// only the first line would either hide the bot's tagged signature behind a
// CDN's line or split a label from its signature bytes. Allocates -- call it
// only on the signed-request path.
GoogleString JoinRequestHeaderValues(ngx_http_request_t* r, const char* name,
                                     size_t name_len) {
  std::vector<webbotauth::HeaderField> lines;
  CollectRequestHeaderFields(r, name, name_len, /*canonical_name=*/"", &lines);
  GoogleString joined;
  for (const webbotauth::HeaderField& line : lines) {
    if (!joined.empty()) joined += ", ";
    StrAppend(&joined, line.value);
  }
  return joined;
}

// Parse the operator verified-bot registry: "keyid=name,keyid2=name2".
void ParseVerifiedBots(StringPiece spec, webbotauth::VerifiedBotRegistry* reg) {
  StringPieceVector pairs;
  SplitStringPieceToVector(spec, ",", &pairs, true /* omit_empty */);
  for (size_t i = 0; i < pairs.size(); ++i) {
    StringPiece pair = pairs[i];
    StringPiece::size_type eq = pair.find('=');
    if (eq == StringPiece::npos) {
      continue;
    }
    StringPiece keyid = pair.substr(0, eq);
    StringPiece name = pair.substr(eq + 1);
    if (!keyid.empty()) {
      reg->Register(keyid, name);
    }
  }
}

// Classify the current request. Returns the verdict; sets *bot_name_out and
// *keyid_out when applicable, and *other_signature_out (may be null) when the
// request carried signature material with NO web-bot-auth-tagged member
// (classified as if unsigned -- the verdict is kHuman -- but kept visible via
// the other-signature counter). The common human path (no non-empty
// Signature-Input or Signature field line) short-circuits BEFORE any file
// read or allocation, so THIS FUNCTION is allocation-free for unsigned
// traffic (the caller still allocates the $x_verified_bot variable value once
// per enabled request); the presence probe scans every field line without
// allocating, and the full ", "-join below runs only once material is
// present. The operator-local JWKS key-directory file is read LAZILY -- only
// when the verifier performs an actual key lookup, i.e. after the material
// parsed, was tagged web-bot-auth, and passed the profile + freshness gates
// -- so fail-closed material never costs a file read. No network, ever.
webbotauth::Verdict ClassifyRequest(ngx_http_request_t* r,
                                    NgxServerContext* server_context,
                                    NgxRewriteOptions* options,
                                    GoogleString* bot_name_out,
                                    GoogleString* keyid_out,
                                    bool* other_signature_out,
                                    uint64_t* verify_latency_us_out) {
  if (other_signature_out != nullptr) {
    *other_signature_out = false;
  }
  if (verify_latency_us_out != nullptr) {
    *verify_latency_us_out = 0;
  }
  // No signature material at all -> human. The probe checks EVERY field line
  // (an empty first line must not hide a real signature on a second line) but
  // never allocates. (A bare Signature header WITHOUT Signature-Input
  // proceeds to the verifier and fail-closes to "unknown" -- the core's
  // partial-headers posture.)
  bool has_sig_input = HasNonEmptyRequestHeader(r, "Signature-Input",
                                                sizeof("Signature-Input") - 1);
  bool has_sig =
      HasNonEmptyRequestHeader(r, "Signature", sizeof("Signature") - 1);
  if (!has_sig_input && !has_sig) {
    return webbotauth::Verdict::kHuman;
  }

  // No validated Host header (e.g. a bare HTTP/1.0 request line): there is no
  // authority to bind "@authority" against, and ps_determine_host's fallback
  // returns a view into a function-local buffer (dangling by the time the
  // verifier would read it). No legitimate signer signs "@authority" for a
  // request that carries none -- fail closed.
  if (r->headers_in.server.len == 0) {
    return webbotauth::Verdict::kUnknown;
  }

  // RFC 9110/9421: repeated Signature-Input / Signature field lines are the
  // comma-joined value (an intermediary legitimately adds its signature as a
  // SEPARATE line, RFC 9421 section 4.1). Join ALL lines before parsing so a
  // bot's tagged signature on a second line -- or signature bytes split from
  // their label across lines -- is still seen. The parse bounds
  // (member/component caps) apply to the joined value.
  GoogleString sig_input_joined = JoinRequestHeaderValues(
      r, "Signature-Input", sizeof("Signature-Input") - 1);
  GoogleString sig_joined =
      JoinRequestHeaderValues(r, "Signature", sizeof("Signature") - 1);

  StringPiece user_agent =
      FindRequestHeader(r, "User-Agent", sizeof("User-Agent") - 1);

  // RFC 9421 binds "@path" to the WIRE request-target path: the raw
  // (unparsed) URI minus any query, NOT nginx's decoded/merged r->uri --
  // a signer covering "@path" signs the bytes it sent, so an escaped or
  // normalizable path must verify over those exact bytes.
  StringPiece wire_path = str_to_string_piece(r->unparsed_uri);
  StringPiece::size_type query_pos = wire_path.find('?');
  if (query_pos != StringPiece::npos) {
    wire_path = wire_path.substr(0, query_pos);
  }

  webbotauth::RequestView req;
  req.method = str_to_string_piece(r->method_name);
  req.authority = ps_determine_host(r);
  req.path = wire_path;
  req.signature_input = sig_input_joined;
  req.signature = sig_joined;
  req.user_agent = user_agent;
  // Wire the HTTP fields the verifier may need as RFC 9421 covered field
  // components (raw wire bytes, EVERY field line; canonicalization happens in
  // the core). The Web Bot Auth draft covers ("@authority" "signature-agent");
  // the Signature-Agent value is advisory-only here -- it participates in the
  // signature base but NEVER selects a key directory (directory hosts stay
  // operator-configured). Allocation happens only on this signed-request
  // path, never for plain traffic.
  CollectRequestHeaderFields(r, "Signature-Agent",
                             sizeof("Signature-Agent") - 1, "signature-agent",
                             &req.fields);
  CollectRequestHeaderFields(r, "User-Agent", sizeof("User-Agent") - 1,
                             "user-agent", &req.fields);
  // directory_host is unused by the static provider (single local directory).

  webbotauth::VerifiedBotRegistry registry;
  ParseVerifiedBots(options->web_bot_auth_verified_bots(), &registry);

  // Operator-local key directory (A1 v1: local file, no fetch), read lazily
  // on the first actual key lookup only (see LazyFileKeyDirectory).
  LazyFileKeyDirectory local_provider(
      server_context, options->web_bot_auth_key_directory_file());

  // A2 warm-fetch: when the operator has configured a remote key directory (URL
  // + SSRF allowlist + directory_host all set), resolve keys CACHE-ONLY from the
  // shared blocking cache the background warmer populates, then fall back to the
  // local file. The cache-only read never fetches on the event loop. With no
  // remote directory configured this is byte-identical to A1 v1 (local file).
  webbotauth::KeyDirectoryProvider* provider = &local_provider;
  webbotauth::CachedKeyDirectoryProvider cache_only(
      nullptr, server_context->metadata_cache(), /*realm=*/"wba",
      /*read_through=*/false);
  std::unique_ptr<webbotauth::ChainedKeyDirectoryProvider> chained;
  if (!options->web_bot_auth_key_directory_url().empty() &&
      !options->web_bot_auth_key_directory_allowlist().empty() &&
      !options->web_bot_auth_directory_host().empty() &&
      server_context->metadata_cache() != nullptr) {
    // The cache key is (directory_host, keyid); the warmer writes under the same
    // operator-configured host, so the request lookup must use it too.
    req.directory_host = options->web_bot_auth_directory_host();
    std::vector<webbotauth::KeyDirectoryProvider*> chain;
    chain.push_back(&cache_only);
    chain.push_back(&local_provider);
    chained.reset(new webbotauth::ChainedKeyDirectoryProvider(chain));
    provider = chained.get();
  }

  // Use the engine's injectable Timer (unix seconds) rather than time(), so the
  // clock is the single mockable time source the rest of the engine uses.
  const int64_t now_unix_sec =
      server_context->timer()->NowMs() / Timer::kSecondMs;
  // Measure the verify latency around the (synchronous, store-backed) verify
  // call for the opt-in counter's coarse latency histogram. The
  // cost is a pair of steady_clock reads on the signed-request path only.
  const auto verify_t0 = std::chrono::steady_clock::now();
  webbotauth::VerifyResult result =
      webbotauth::VerifyAndClassify(req, provider, registry, now_unix_sec);
  if (verify_latency_us_out != nullptr) {
    *verify_latency_us_out = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - verify_t0)
            .count());
  }
  if (keyid_out != nullptr) {
    *keyid_out = result.keyid;
  }
  if (bot_name_out != nullptr) {
    *bot_name_out = result.bot_name;
  }
  // Signature material with no web-bot-auth-tagged member: some other
  // RFC 9421 signing scheme. Classified exactly like an unsigned request --
  // $x_verified_bot == "human", never "unknown" -- but kept visible via the
  // other-signature counter. Matched on the verifier's REASON (not just the
  // kHuman verdict) so a future kHuman return path in the core cannot
  // silently miscount (see the classifier.h invariant note).
  if (result.verdict == webbotauth::Verdict::kHuman &&
      result.reason == "no-web-bot-auth-signature" &&
      other_signature_out != nullptr) {
    *other_signature_out = true;
  }
  return result.verdict;
}

// Render the verdict as the $x_verified_bot token. A verified bot is emitted as
// "<bot-name>, ed25519-verified"; otherwise the bare verdict token.
// Suffix FormatVerdict appends for a registered verified bot. Named because
// ps_webbotauth_signature_verified below recognises the stored value by it;
// the two must agree, and this constant is what makes them agree.
const char kVerifiedBotSuffix[] = ", ed25519-verified";

GoogleString FormatVerdict(webbotauth::Verdict verdict,
                           const GoogleString& bot_name) {
  if (verdict == webbotauth::Verdict::kVerifiedBot) {
    return StrCat(bot_name, kVerifiedBotSuffix);
  }
  return webbotauth::VerdictToken(verdict);
}

// Store `out` into an nginx variable value, allocated from the request pool.
void SetNgxVarValue(ngx_http_request_t* r, ngx_http_variable_value_t* v,
                    const GoogleString& out) {
  u_char* data = static_cast<u_char*>(ngx_pnalloc(r->pool, out.size()));
  if (data == nullptr) {
    v->not_found = 1;
    return;
  }
  ngx_memcpy(data, out.data(), out.size());
  v->valid = 1;
  v->no_cacheable = 0;
  v->not_found = 0;
  v->len = static_cast<unsigned>(out.size());
  v->data = data;
}

}  // namespace

void ps_webbotauth_set_var_index(ngx_int_t index) { g_xvb_var_index = index; }

ngx_int_t ps_webbotauth_preaccess_handler(ngx_http_request_t* r) {
  NgxServerContext* server_context = ps_get_server_context(r);
  if (server_context == nullptr) {
    return NGX_DECLINED;
  }
  NgxRewriteOptions* options = server_context->config();
  // Default OFF: zero-cost fast path when the operator has not enabled it.
  if (options == nullptr || !options->web_bot_auth()) {
    return NGX_DECLINED;
  }

  // Classify ONLY the main request's first (external) pass. Internal
  // redirects (index resolution, error_page, try_files fallbacks) and
  // subrequests (SSI, mirror, auth_request) re-enter this phase for the SAME
  // client transaction; without this guard one signed probe would be
  // re-verified and double-counted, and an @path-covering signature would
  // flip the stored verdict to "unknown" after the URI rewrite. The
  // $x_verified_bot slot stored below persists across internal redirects
  // (same request object), and the variable get-handler resolves subrequests
  // against the main request's slot. (Same guard shape as nginx's mirror
  // module: r == r->main, plus !r->internal.)
  if (r != r->main || r->internal) {
    return NGX_DECLINED;
  }

  // Compute the verdict EXACTLY ONCE per client transaction here.
  GoogleString bot_name;
  GoogleString keyid;
  bool other_signature = false;
  uint64_t verify_latency_us = 0;
  webbotauth::Verdict verdict =
      ClassifyRequest(r, server_context, options, &bot_name, &keyid,
                      &other_signature, &verify_latency_us);

  // the design record Bar-A opt-in counter (experimental): record into the shared
  // memory-mapped counter store whenever Web Bot Auth is enabled -- INDEPENDENT
  // of WebBotAuthTelemetry (the first-party statistics counters) and of the
  // counter MODE (which gates only PUBLICATION at the well-known endpoint).
  // Records ALL verified keyids, including our own probe key -- probe-kid /
  // first-party exclusion is an aggregator-side concern; the engine stays
  // neutral. g_wba_counter is null when the counter file could not be mapped, in
  // which case every Record* call is a no-op.
  if (g_wba_counter != nullptr) {
    const bool verified = verdict == webbotauth::Verdict::kSignedAgent ||
                          verdict == webbotauth::Verdict::kVerifiedBot;
    if (verified) {
      webbotauth::RecordWebBotAuthSigned(g_wba_counter, true);
      webbotauth::RecordWebBotAuthVerifiedSigner(
          g_wba_counter, std::string_view(keyid.data(), keyid.size()),
          verify_latency_us);
    } else if (verdict == webbotauth::Verdict::kUnknown) {
      // Signature material was present but fail-closed to unknown (bad key /
      // expired / tampered / unparseable): the "invalid" bucket.
      webbotauth::RecordWebBotAuthSigned(g_wba_counter, false);
    } else if (other_signature) {
      // Parseable signature material with no web-bot-auth-tagged member.
      webbotauth::RecordWebBotAuthOtherSignature(g_wba_counter);
    }
  }

  if (options->web_bot_auth_telemetry()) {
    Statistics* stats = server_context->statistics();
    if (stats != nullptr) {
      if (verdict == webbotauth::Verdict::kSignedAgent ||
          verdict == webbotauth::Verdict::kVerifiedBot) {
        Variable* v = stats->GetVariable(kWebBotAuthVerifiedSignedRequests);
        if (v != nullptr) {
          v->Add(1);
        }
      } else if (other_signature) {
        // Parseable signature material with no web-bot-auth-tagged member:
        // classified as if unsigned (verdict "human", no trust), counted
        // separately so other signing schemes stay visible.
        Variable* v = stats->GetVariable(kWebBotAuthOtherSignatureRequests);
        if (v != nullptr) {
          v->Add(1);
        }
      }
    }
  }

  // Store the verdict into the indexed $x_verified_bot value so the variable
  // get-handler reads it instead of recomputing (counter and variable now
  // reflect the same single computation).
  if (g_xvb_var_index != NGX_ERROR) {
    ngx_http_variable_value_t* vv = &r->variables[g_xvb_var_index];
    SetNgxVarValue(r, vv, FormatVerdict(verdict, bot_name));
  }

  // Observe-only: never block, never alter the request.
  return NGX_DECLINED;
}

ngx_int_t ps_rsl_cap_preaccess_handler(ngx_http_request_t* r) {
  NgxServerContext* server_context = ps_get_server_context(r);
  if (server_context == nullptr) {
    return NGX_DECLINED;
  }
  NgxRewriteOptions* options = server_context->config();
  // Default OFF: zero-cost fast path when the operator has not enabled
  // enforcement. This is the PAID, demand-gated sibling of A1; it stays inert
  // until explicitly turned on.
  if (options == nullptr || !options->rsl_cap_enforcement()) {
    return NGX_DECLINED;
  }

  // Read the capability token from the Authorization header. Absent/oversized
  // are both treated as "no valid token" -> 401 (the size cap runs BEFORE any
  // parse so an abusive header cannot drive parser work).
  StringPiece auth =
      FindRequestHeader(r, "Authorization", sizeof("Authorization") - 1);
  if (auth.size() > kMaxRslCapAuthHeaderBytes) {
    return NGX_HTTP_UNAUTHORIZED;
  }

  // Resolve issuer keys from the operator-local JWKS file (v1: synchronous, no
  // network), mirroring A1. The path comes ONLY from operator config, never the
  // request. The SSRF-guarded NetFetchKeyDirectory
  // implements the same KeyDirectoryProvider interface and drops in here once
  // an async continuation exists -- it is deferred for the SAME reason A1 defers
  // it: an async fetch cannot complete inline in an nginx phase handler.
  GoogleString jwks_doc;
  const GoogleString& kd_file = options->rsl_cap_key_directory_file();
  if (!kd_file.empty()) {
    FileSystem* fs = server_context->file_system();
    if (fs != nullptr) {
      fs->ReadFile(kd_file.c_str(), kMaxKeyDirectoryFileBytes, &jwks_doc,
                   server_context->message_handler());
    }
  }

  webbotauth::StaticKeyDirectory local_provider(jwks_doc);

  // A2 warm-fetch (mirrors A1): prefer cache-only remote keys then the local
  // file when a remote RSL-CAP issuer directory is configured. The validator
  // passes rsl_cap_directory_host() to GetKey, the same host the warmer writes
  // under, so the cache key matches. Default-off => local file only (v1).
  webbotauth::KeyDirectoryProvider* provider = &local_provider;
  webbotauth::CachedKeyDirectoryProvider cache_only(
      nullptr, server_context->metadata_cache(), /*realm=*/"rsl",
      /*read_through=*/false);
  std::unique_ptr<webbotauth::ChainedKeyDirectoryProvider> chained;
  if (!options->rsl_cap_key_directory_url().empty() &&
      !options->rsl_cap_key_directory_allowlist().empty() &&
      !options->rsl_cap_directory_host().empty() &&
      server_context->metadata_cache() != nullptr) {
    std::vector<webbotauth::KeyDirectoryProvider*> chain;
    chain.push_back(&cache_only);
    chain.push_back(&local_provider);
    chained.reset(new webbotauth::ChainedKeyDirectoryProvider(chain));
    provider = chained.get();
  }

  webbotauth::RslCapValidator validator(provider);
  webbotauth::RslCapToken token;
  // Use the engine's injectable Timer (unix seconds) rather than time(), so the
  // clock is the single mockable time source the rest of the engine uses.
  const int64_t now_unix_sec =
      server_context->timer()->NowMs() / Timer::kSecondMs;
  webbotauth::RslCapStatus status = validator.Validate(
      auth, options->rsl_cap_requested_license(),
      options->rsl_cap_requested_scope(), options->rsl_cap_directory_host(),
      now_unix_sec, &token);

  // Issuer pin (hardening): when an issuer is configured, an otherwise-authorized
  // token whose iss does not match is rejected as an unknown issuer (401). The
  // core parses iss but leaves it unbound; this binds it at the policy layer.
  const GoogleString& want_iss = options->rsl_cap_issuer();
  if (status == webbotauth::RslCapStatus::kAuthorized && !want_iss.empty() &&
      token.iss != want_iss) {
    status = webbotauth::RslCapStatus::kUnknownIssuer;
  }

  // The verdict->status mapping is a kernel free function (unit-tested without
  // nginx). 0 means "allow" (NGX_DECLINED); 402/401 are returned verbatim and
  // match NGX_HTTP_UNAUTHORIZED (401) and the prior kHttpPaymentRequired (402).
  const int http_status = webbotauth::RslCapStatusToHttpStatus(status);
  return http_status == 0 ? NGX_DECLINED : http_status;
}

ngx_int_t ps_x_verified_bot_variable(ngx_http_request_t* r,
                                     ngx_http_variable_value_t* v,
                                     uintptr_t /*data*/) {
  NgxServerContext* server_context = ps_get_server_context(r);
  if (server_context == nullptr || server_context->config() == nullptr ||
      !server_context->config()->web_bot_auth()) {
    v->not_found = 1;
    return NGX_OK;
  }

  // On the common path the preaccess handler has already stored the indexed
  // value on the MAIN request, so first resolve against that slot: a
  // subrequest (or a post-internal-redirect evaluation) reuses the verdict
  // computed once for the client transaction instead of re-running the full
  // pipeline (file read + ed25519) per subrequest. The value bytes live in
  // the main request's pool, which outlives every subrequest.
  if (g_xvb_var_index != NGX_ERROR) {
    const ngx_http_variable_value_t* mv = &r->main->variables[g_xvb_var_index];
    if (mv->valid && !mv->not_found && mv->data != nullptr) {
      *v = *mv;
      return NGX_OK;
    }
  }

  // Fallback: the preaccess handler never ran for this transaction (e.g. a
  // phase pipeline that skipped it). Recompute once. (No counter here --
  // only the preaccess handler counts, so a recompute can never
  // double-count.)
  GoogleString bot_name;
  GoogleString keyid;
  webbotauth::Verdict verdict = ClassifyRequest(
      r, server_context, server_context->config(), &bot_name, &keyid,
      /*other_signature_out=*/nullptr, /*verify_latency_us_out=*/nullptr);
  SetNgxVarValue(r, v, FormatVerdict(verdict, bot_name));
  return NGX_OK;
}

bool ps_webbotauth_signature_verified(ngx_http_request_t* r) {
  if (g_xvb_var_index == NGX_ERROR || r == nullptr) {
    return false;
  }
  // Resolve against the MAIN request, exactly as ps_x_verified_bot_variable
  // does: the verdict is computed once per client transaction and a subrequest
  // must inherit it rather than see an empty slot.
  const ngx_http_variable_value_t* mv = &r->main->variables[g_xvb_var_index];
  if (!mv->valid || mv->not_found || mv->data == nullptr) {
    // The preaccess handler never ran (WebBotAuth off, or a phase pipeline
    // that skipped it). No opinion -- never recompute here, since this is
    // called on the content path where the verifier's cost budget does not
    // apply.
    return false;
  }
  const StringPiece value(reinterpret_cast<const char*>(mv->data), mv->len);
  // Positive verdicts only. kSignedAgent formats as its own token;
  // kVerifiedBot formats as "<bot name>" + kVerifiedBotSuffix (FormatVerdict).
  // kHuman and kUnknown format as their own tokens and are not matched here.
  const StringPiece signed_agent_token(
      webbotauth::VerdictToken(webbotauth::Verdict::kSignedAgent));
  return value == signed_agent_token || value.ends_with(kVerifiedBotSuffix);
}

void ps_webbotauth_counter_map(const GoogleString& path) {
  // Mapped once (in the master, before workers fork); the MAP_SHARED region is
  // inherited by every worker. Idempotent: a config reload re-runs the master
  // init but the mapping already exists, so keep it (do NOT re-open / reset).
  if (g_wba_counter != nullptr || path.empty()) {
    return;
  }
  const std::string p(path.data(), path.size());
  g_wba_counter = webbotauth::OpenOrCreateWebBotAuthCounter(p);
  if (g_wba_counter == nullptr) {
    // Mapping ultimately failed -> every Record* is a silent no-op. Surface it
    // so the silent-disable is observable. Path + errno only; NEVER the token or
    // any secret (none is involved here).
    ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, ngx_errno,
                  "pagespeed: web-bot-auth counter file could not be mapped "
                  "(\"%s\"); the opt-in counter will not record",
                  p.c_str());
  }
}

void ps_webbotauth_counter_read_token() {
  // Read the secret bearer token from the environment once per worker. Log
  // PRESENCE only -- never the value.
  const char* tok = getenv("PAGESPEED_WEB_BOT_AUTH_COUNTER_TOKEN");
  if (tok != nullptr && tok[0] != '\0') {
    g_wba_counter_token = tok;
  }
}

bool ps_webbotauth_counter_build_response(ngx_http_request_t* r,
                                          ResponseHeaders* headers,
                                          GoogleString* body) {
  NgxServerContext* server_context = ps_get_server_context(r);
  if (server_context == nullptr || server_context->config() == nullptr) {
    return false;
  }
  NgxRewriteOptions* options = server_context->config();
  const GoogleString& mode = options->web_bot_auth_public_counter();
  const bool public_mode = (mode == "public");
  const bool private_mode = (mode == "private");
  if (!public_mode && !private_mode) {
    // off / unknown: hide (the router should not have reached us, but fail safe).
    return false;
  }

  // Inline bearer-token check against the env-configured secret. The "Bearer "
  // prefix and the length are non-secret (early-exit is fine), but the token
  // bytes are compared in constant time to avoid a timing oracle on the secret.
  StringPiece auth =
      FindRequestHeader(r, "Authorization", sizeof("Authorization") - 1);
  bool token_ok = false;
  if (!g_wba_counter_token.empty() && auth.size() > 0) {
    std::string_view av(auth.data(), auth.size());
    static constexpr std::string_view kBearer = "Bearer ";
    const size_t tok_len = g_wba_counter_token.size();
    if (av.size() == kBearer.size() + tok_len &&
        av.substr(0, kBearer.size()) == kBearer) {
      const char* a = av.data() + kBearer.size();
      const char* b = g_wba_counter_token.data();
      unsigned char diff = 0;
      for (size_t i = 0; i < tok_len; ++i) {
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
      }
      token_ok = (diff == 0);
    }
  }

  bool exact;
  if (token_ok) {
    exact = true;  // valid token (any non-off mode) -> exact doc
  } else if (public_mode) {
    exact = false;  // public + no/invalid token -> coarse doc
  } else {
    // private + no/invalid token -> hide (the caller declines -> normal 404).
    return false;
  }

  webbotauth::VerifiedBotRegistry registry;
  ParseVerifiedBots(options->web_bot_auth_verified_bots(), &registry);
  RenderCounterDoc(exact, registry, body);

  headers->SetStatusAndReason(HttpStatus::kOK);
  headers->set_major_version(1);
  headers->set_minor_version(1);
  headers->Add(HttpAttributes::kContentType, "application/json");
  // Never let a browser sniff this JSON as HTML (stored-XSS defense in depth).
  headers->Add("X-Content-Type-Options", "nosniff");
  if (exact) {
    headers->Add(HttpAttributes::kCacheControl, "no-store");
  } else {
    headers->Add(HttpAttributes::kCacheControl, "public, max-age=300");
  }
  // Vary: Authorization on BOTH docs. The response CONTENT depends on the token
  // (present -> exact, absent -> coarse/404), so a shared cache must key on
  // Authorization either way -- defense-in-depth against a nonconformant CDN
  // that would ignore no-store on the exact response.
  headers->Add(HttpAttributes::kVary, "Authorization");
  headers->SetDate(server_context->timer()->NowMs());
  return true;
}

}  // namespace net_instaweb
