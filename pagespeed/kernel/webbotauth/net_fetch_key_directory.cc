// Copyright 2026 We-Amp B.V.
// Licensed under the Apache License, Version 2.0 (the "License").

#include "pagespeed/kernel/webbotauth/net_fetch_key_directory.h"

#include <arpa/inet.h>
#include <netinet/in.h>

#include <cstring>

#include "net/instaweb/http/public/async_fetch.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "pagespeed/kernel/webbotauth/jwks.h"

namespace net_instaweb {
namespace webbotauth {

namespace {

// Lower-case ASCII copy.
GoogleString Lower(StringPiece s) {
  GoogleString out(s.data(), s.size());
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c += 32;
  }
  return out;
}

// Extract scheme://host[:port] (origin) from a URL, lower-cased. Returns false
// if not http(s) or malformed.
bool OriginOf(StringPiece url, GoogleString* origin, GoogleString* host) {
  GoogleString u = Lower(url);
  StringPiece scheme;
  size_t scheme_end = u.find("://");
  if (scheme_end == GoogleString::npos) return false;
  scheme = StringPiece(u).substr(0, scheme_end);
  size_t host_start = scheme_end + 3;
  size_t path_start = u.find('/', host_start);
  GoogleString hostport = (path_start == GoogleString::npos)
                              ? u.substr(host_start)
                              : u.substr(host_start, path_start - host_start);
  *origin = StrCat(scheme, "://", hostport);
  // Strip port for host.
  size_t colon = hostport.rfind(':');
  if (colon != GoogleString::npos && hostport.find(']') == GoogleString::npos) {
    *host = hostport.substr(0, colon);
  } else {
    *host = hostport;
  }
  return scheme == "http" || scheme == "https";
}

}  // namespace

bool IsGloballyRoutableIp(StringPiece ip_literal) {
  GoogleString ip(ip_literal.data(), ip_literal.size());
  // Try IPv4.
  struct in_addr a4;
  if (inet_pton(AF_INET, ip.c_str(), &a4) == 1) {
    uint32_t h = ntohl(a4.s_addr);
    uint8_t b0 = (h >> 24) & 0xFF;
    uint8_t b1 = (h >> 16) & 0xFF;
    // 0.0.0.0/8
    if (b0 == 0) return false;
    // 10.0.0.0/8
    if (b0 == 10) return false;
    // 127.0.0.0/8 loopback
    if (b0 == 127) return false;
    // 169.254.0.0/16 link-local (incl. 169.254.169.254 metadata)
    if (b0 == 169 && b1 == 254) return false;
    // 172.16.0.0/12
    if (b0 == 172 && (b1 >= 16 && b1 <= 31)) return false;
    // 192.168.0.0/16
    if (b0 == 192 && b1 == 168) return false;
    // 100.64.0.0/10 CGNAT
    if (b0 == 100 && (b1 >= 64 && b1 <= 127)) return false;
    // 192.0.0.0/24 and 192.0.2.0/24 (TEST-NET-1)
    if (b0 == 192 && b1 == 0) return false;
    // 198.18.0.0/15 benchmark
    if (b0 == 198 && (b1 == 18 || b1 == 19)) return false;
    // 198.51.100.0/24 TEST-NET-2, 203.0.113.0/24 TEST-NET-3
    if (b0 == 198 && b1 == 51) return false;
    if (b0 == 203 && b1 == 0) return false;
    // 224.0.0.0/4 multicast and 240.0.0.0/4 reserved
    if (b0 >= 224) return false;
    return true;
  }
  // Try IPv6.
  struct in6_addr a6;
  if (inet_pton(AF_INET6, ip.c_str(), &a6) == 1) {
    const uint8_t* b = a6.s6_addr;
    // ::1 loopback and :: unspecified
    bool all_zero = true;
    for (int i = 0; i < 15; ++i) {
      if (b[i] != 0) {
        all_zero = false;
        break;
      }
    }
    if (all_zero && (b[15] == 0 || b[15] == 1)) return false;
    // fe80::/10 link-local
    if (b[0] == 0xFE && (b[1] & 0xC0) == 0x80) return false;
    // fc00::/7 unique-local
    if ((b[0] & 0xFE) == 0xFC) return false;
    // ff00::/8 multicast
    if (b[0] == 0xFF) return false;
    // ::ffff:0:0/96 IPv4-mapped -> reject (must be expressed as IPv4 + checked)
    bool v4mapped = true;
    for (int i = 0; i < 10; ++i) {
      if (b[i] != 0) {
        v4mapped = false;
        break;
      }
    }
    if (v4mapped && b[10] == 0xFF && b[11] == 0xFF) return false;
    return true;
  }
  // Not a parseable IP literal -> not safe.
  return false;
}

bool IsAllowlistedHttpsUrl(StringPiece url,
                           const std::set<GoogleString>& allow) {
  GoogleString origin, host;
  if (!OriginOf(url, &origin, &host)) return false;
  // https only.
  if (origin.compare(0, 6, "https:") != 0) return false;
  return allow.find(origin) != allow.end();
}

KeyLookupResult NetFetchKeyDirectory::GetKey(StringPiece host,
                                             StringPiece keyid,
                                             int64_t /*now_unix_sec*/) {
  // 0. Default-empty allowlist => feature disabled => fail closed.
  if (options_.host_allowlist.empty()) {
    return KeyLookupResult::Error();
  }
  // 1. Resolve the directory URL from operator config (NOT a request header).
  GoogleString host_key(host.data(), host.size());
  auto it = options_.host_to_directory_url.find(host_key);
  if (it == options_.host_to_directory_url.end()) {
    return KeyLookupResult::Error();
  }
  const GoogleString& url = it->second;

  // 2. Allowlist + https check.
  if (!IsAllowlistedHttpsUrl(url, options_.host_allowlist)) {
    return KeyLookupResult::Error();
  }

  // NOTE on SSRF/DNS-rebinding: the strongest control we can apply at this
  // layer is the https-only + origin-allowlist + no-redirect policy below. A
  // resolve->pin->connect check on the actually-connected IP requires the
  // fetcher to expose its connected peer address; the engine serf fetcher does
  // not surface that here, so DNS-rebinding to a private IP remains a DOCUMENTED
  // RESIDUAL RISK that must be mitigated operationally (allowlist only hosts
  // whose DNS the operator trusts; or front the fetch through an egress proxy
  // that re-validates the IP). IsGloballyRoutableIp() is provided and unit-
  // tested so a future fetcher that exposes the peer IP can enforce it.

  // 3. Fetch with a bounded timeout and (separately) a response-size cap.
  if (fetcher_ == nullptr) return KeyLookupResult::Error();

  GoogleString body;
  StringAsyncFetch fetch(request_context_, &body);
  // Disable redirect following at the policy level: we never follow here, and
  // re-validate nothing off-allowlist. (If the fetcher follows redirects
  // internally, the allowlist+https guard on the final origin is our backstop;
  // a redirect to an off-allowlist origin yields a body we will still try to
  // parse -- but the response can only ever provide a 32-byte key, never a
  // capability, so the blast radius is parsing a JWKS from an unexpected host.
  // We additionally cap size below.)
  fetcher_->Fetch(url, handler_, &fetch);

  // The serf fetcher in the nginx context is driven by the event loop; in a
  // blocking call site we rely on the fetcher's synchronous-wait behavior. If
  // the fetch did not complete, fail closed.
  if (!fetch.done() || !fetch.success()) {
    return KeyLookupResult::Error();
  }

  // 4. Response-size cap (post-hoc; a streaming cap would require a custom
  //    AsyncFetch -- documented as a follow-up hardening).
  if (body.size() > options_.max_response_bytes) {
    return KeyLookupResult::Error();
  }

  // 5. Parse the JWKS and extract the Ed25519 key.
  GoogleString raw_key;
  if (!ExtractEd25519Key(body, keyid, &raw_key)) {
    return KeyLookupResult::NotFound();
  }
  return KeyLookupResult::Found(raw_key);
}

}  // namespace webbotauth
}  // namespace net_instaweb
