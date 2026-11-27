// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Worker IPC Protocol Implementation

#include "src/proto/worker_ipc.h"

#include <cstring>

namespace pagespeed {

namespace {

// Write a 32-bit value in big-endian format
void WriteBE32(uint32_t value, char* out) {
  out[0] = static_cast<char>((value >> 24) & 0xFF);
  out[1] = static_cast<char>((value >> 16) & 0xFF);
  out[2] = static_cast<char>((value >> 8) & 0xFF);
  out[3] = static_cast<char>(value & 0xFF);
}

// Read a 32-bit value in big-endian format
uint32_t ReadBE32(const char* data) {
  return (static_cast<uint32_t>(static_cast<unsigned char>(data[0])) << 24) |
         (static_cast<uint32_t>(static_cast<unsigned char>(data[1])) << 16) |
         (static_cast<uint32_t>(static_cast<unsigned char>(data[2])) << 8) |
         static_cast<uint32_t>(static_cast<unsigned char>(data[3]));
}

}  // namespace

bool ValidateScheme(std::string_view scheme) {
  return scheme == "http" || scheme == "https";
}

IpcScheme SchemeToWire(std::string_view scheme) {
  // Precondition: caller must validate via ValidateScheme() first.
  if (scheme == "http") return IpcScheme::kHttp;
  if (scheme == "https") return IpcScheme::kHttps;
  // Unreachable after validation. Return https as safe fallback.
  return IpcScheme::kHttps;
}

bool IsSiteId(std::string_view site) {
  if (site.size() != kSiteIdChars) return false;
  for (char c : site) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

std::string_view SchemeFromWire(IpcScheme wire) {
  switch (wire) {
    case IpcScheme::kHttp:
      return "http";
    case IpcScheme::kHttps:
      return "https";
  }
  return "https";  // Unreachable if wire byte was validated.
}

std::vector<char> CacheNotification::Serialize() const {
  size_t url_len = url.size();
  size_t host_len = hostname.size();
  // Sender-side validation matching receiver limits.
  if (url_len > kMaxUrlLength || host_len > kMaxHostnameLength) {
    return {};
  }
  // Validate scheme.
  if (!ValidateScheme(scheme)) {
    return {};
  }

  // Option context (v5).  Three refusals, all of them the same shape as the
  // url/hostname refusals above: the sender declines to build a frame the
  // receiver would have to reject, so the failure lands where the bad value
  // came from instead of one hop away.
  //
  // Refusing rather than dropping the field matters, and the reason is
  // integrity rather than namespacing.  The signature is only meaningful
  // against the payload it was computed over: the receiver re-derives it and
  // compares, which is how two implementations of this format find out they
  // have drifted.  A frame that silently shipped half a context, or a payload
  // this side never bounded, would carry something that cannot be checked at
  // all — the one thing the field exists to make impossible.  Better no
  // notification at all: the resource goes unoptimized this time round.
  const size_t ctx_len = option_context.size();
  const size_t sig_len = option_signature.size();
  if (ctx_len > kMaxOptionContextBytes) {
    return {};
  }
  if (option_context.empty() != option_signature.empty()) {
    return {};  // Half a context cannot be checked against anything.
  }
  if (sig_len != 0 && sig_len != kOptionContextSignatureChars) {
    return {};
  }
  // Site (v6): empty or a site id, nothing else.
  const size_t site_len = site.size();
  if (site_len != 0 && !IsSiteId(site)) {
    return {};
  }
  if (!HtmlStoredShapeValid(html_stored, from_module, host_vouched,
                            site_len != 0,
                            content_type == ContentType::kHtml)) {
    return {};
  }

  // total_length covers everything after itself: version + fields.
  size_t total_size = 1 +         // version
                      4 +         // url_length
                      url_len +   // url
                      4 +         // hostname_length
                      host_len +  // hostname
                      1 +         // content_type
                      4 +         // capability_mask
                      1 +         // scheme
                      1 +         // agent_request (v4)
                      4 +         // option_context_length (v5)
                      ctx_len +   // option_context (v5)
                      1 +         // option_signature_length (v5)
                      sig_len +   // option_signature (v5)
                      1 +         // site_length (v6)
                      site_len +  // site (v6)
                      1;          // flags (v6)

  std::vector<char> result;
  result.resize(4 + total_size);  // 4-byte total_length header + payload

  char* ptr = result.data();

  // Total length (not including this 4-byte field itself)
  WriteBE32(static_cast<uint32_t>(total_size), ptr);
  ptr += 4;

  // Version byte
  *ptr++ = static_cast<char>(kIpcVersion);

  // URL
  WriteBE32(static_cast<uint32_t>(url_len), ptr);
  ptr += 4;
  std::memcpy(ptr, url.data(), url_len);
  ptr += url_len;

  // Hostname
  WriteBE32(static_cast<uint32_t>(host_len), ptr);
  ptr += 4;
  if (host_len > 0) {
    std::memcpy(ptr, hostname.data(), host_len);
    ptr += host_len;
  }

  // Content type
  *ptr++ = static_cast<char>(content_type);

  // Capability mask
  WriteBE32(capability_mask, ptr);
  ptr += 4;

  // Scheme
  *ptr++ = static_cast<char>(SchemeToWire(scheme));

  // Agent-request intent (v4).
  *ptr++ = static_cast<char>(agent_request ? 1 : 0);

  // Option context (v5).
  WriteBE32(static_cast<uint32_t>(ctx_len), ptr);
  ptr += 4;
  if (ctx_len > 0) {
    std::memcpy(ptr, option_context.data(), ctx_len);
    ptr += ctx_len;
  }
  // The signature is 0 or 64 bytes, so a single length byte covers it and
  // makes an over-long signature unrepresentable on the wire rather than
  // merely rejected.
  *ptr++ = static_cast<char>(sig_len);
  if (sig_len > 0) {
    std::memcpy(ptr, option_signature.data(), sig_len);
    ptr += sig_len;
  }

  // Site and flags (v6).
  *ptr++ = static_cast<char>(site_len);
  if (site_len > 0) {
    std::memcpy(ptr, site.data(), site_len);
    ptr += site_len;
  }
  uint8_t flags = 0;
  if (host_vouched) flags |= kNotifyFlagHostVouched;
  if (from_module) flags |= kNotifyFlagFromModule;
  if (legacy_ex_caller) flags |= kNotifyFlagLegacyExCaller;
  if (html_stored) flags |= kNotifyFlagHtmlStored;
  *ptr = static_cast<char>(flags);

  return result;
}

bool CacheNotification::Deserialize(std::string_view data,
                                    CacheNotification* notification,
                                    IpcRejection* rejection) {
  // Every failure path routes through here, so a caller that asked for the
  // reason always gets one and "returned false with reason kNone" is not a
  // state this function can produce.
  auto reject = [rejection](IpcRejectReason reason,
                            uint8_t peer_version) -> bool {
    if (rejection != nullptr) {
      rejection->reason = reason;
      rejection->peer_version = peer_version;
    }
    return false;
  };
  if (rejection != nullptr) {
    *rejection = IpcRejection{};
  }

  // Minimum: 4 (total_length) + 1 (version) = 5 bytes
  if (data.size() < 5) {
    return reject(IpcRejectReason::kTruncated, 0);
  }

  const char* ptr = data.data();

  // Read total_length (framing header, also validated by socket layer)
  uint32_t total_len = ReadBE32(ptr);
  ptr += 4;

  if (data.size() < 4 + static_cast<size_t>(total_len)) {
    return reject(IpcRejectReason::kTruncated, 0);
  }

  // Every bounds check below is against the end of THIS FRAME, not the end of
  // whatever buffer the caller handed over.  The two coincide for the receive
  // path, which slices an exactly-sized view — but only because it does, and a
  // reader whose field reads can run past the length its own header declares is
  // relying on its caller for correctness.  With the trailing length-prefixed
  // fields the frame now carries, that would mean reading the START OF THE NEXT
  // NOTIFICATION as this one's option context out of a buffer holding two.  The
  // clamp can only ever shrink the bound: the check above already established
  // that 4 + total_len fits.
  const char* end = data.data() + 4 + static_cast<size_t>(total_len);

  // Version gate.  Checked BEFORE any field is read, and it is an equality
  // test in both directions: a lower version is an old peer, a higher one is a
  // newer peer, and neither may have its bytes interpreted by this build.  The
  // peer's version is handed back so the receive path can say which peer is
  // out of step instead of only that something was wrong.
  auto version = static_cast<uint8_t>(*ptr++);
  if (version != kIpcVersion) {
    return reject(IpcRejectReason::kVersionMismatch, version);
  }

  // Past this point the version matched, so anything that fails is a damaged
  // or hostile frame from a peer that agrees on the format — a different
  // problem with a different fix, and reported as such.

  // URL (cap at 16 KB to reject oversized input)
  if (ptr + 4 > end) return reject(IpcRejectReason::kMalformed, version);
  uint32_t url_len = ReadBE32(ptr);
  ptr += 4;

  if (url_len > kMaxUrlLength) {
    return reject(IpcRejectReason::kMalformed, version);
  }
  if (ptr + url_len > end) return reject(IpcRejectReason::kMalformed, version);
  notification->url.assign(ptr, url_len);
  ptr += url_len;

  // Hostname (cap at 512 bytes to reject oversized input)
  if (ptr + 4 > end) return reject(IpcRejectReason::kMalformed, version);
  uint32_t host_len = ReadBE32(ptr);
  ptr += 4;

  if (host_len > kMaxHostnameLength) {
    return reject(IpcRejectReason::kMalformed, version);
  }
  if (ptr + host_len > end) return reject(IpcRejectReason::kMalformed, version);
  notification->hostname.assign(ptr, host_len);
  ptr += host_len;

  // Content type (reject values outside the known enum range)
  if (ptr + 1 > end) return reject(IpcRejectReason::kMalformed, version);
  auto ct_byte = static_cast<uint8_t>(*ptr++);
  if (ct_byte > static_cast<uint8_t>(ContentType::kOther)) {
    return reject(IpcRejectReason::kMalformed, version);
  }
  notification->content_type = static_cast<ContentType>(ct_byte);

  // Capability mask
  if (ptr + 4 > end) return reject(IpcRejectReason::kMalformed, version);
  notification->capability_mask = ReadBE32(ptr);
  ptr += 4;

  // Scheme
  if (ptr + 1 > end) return reject(IpcRejectReason::kMalformed, version);
  auto scheme_byte = static_cast<uint8_t>(*ptr);
  ptr += 1;
  if (scheme_byte != static_cast<uint8_t>(IpcScheme::kHttp) &&
      scheme_byte != static_cast<uint8_t>(IpcScheme::kHttps)) {
    return reject(IpcRejectReason::kMalformed, version);
  }
  notification->scheme =
      std::string(SchemeFromWire(static_cast<IpcScheme>(scheme_byte)));

  // Agent-request intent (v4).
  if (ptr + 1 > end) return reject(IpcRejectReason::kMalformed, version);
  notification->agent_request = (*ptr != 0);
  ptr += 1;

  // Option context (v5).
  //
  // The payload is NOT parsed here and never will be.  This engine does not
  // resolve configuration; it is handed a resolved value and the name of that
  // value.  What is checked is only what a carrier must check: that the frame
  // actually contains the bytes it claims, that they are within the bound both
  // sides agree on, and that a payload and a signature arrive together.
  // Whether the signature actually matches its payload is a separate question
  // with a separate answer — see ValidateOptionContext, applied at dispatch,
  // where a mismatch can be counted as the contract divergence it is rather
  // than lumped in with a corrupt frame.
  if (ptr + 4 > end) return reject(IpcRejectReason::kMalformed, version);
  uint32_t ctx_len = ReadBE32(ptr);
  ptr += 4;
  if (ctx_len > kMaxOptionContextBytes) {
    return reject(IpcRejectReason::kMalformed, version);
  }
  if (ptr + ctx_len > end) return reject(IpcRejectReason::kMalformed, version);
  notification->option_context.assign(ptr, ctx_len);
  ptr += ctx_len;

  if (ptr + 1 > end) return reject(IpcRejectReason::kMalformed, version);
  auto sig_len = static_cast<uint8_t>(*ptr);
  ptr += 1;
  if (sig_len != 0 && sig_len != kOptionContextSignatureChars) {
    return reject(IpcRejectReason::kMalformed, version);
  }
  if (ptr + sig_len > end) return reject(IpcRejectReason::kMalformed, version);
  notification->option_signature.assign(ptr, sig_len);
  ptr += sig_len;

  // Site (v6): 0 or kSiteIdChars bytes of lowercase hex.  Any other length,
  // or any other byte, is a malformed frame.
  if (ptr + 1 > end) return reject(IpcRejectReason::kMalformed, version);
  auto site_len = static_cast<uint8_t>(*ptr);
  ptr += 1;
  if (site_len != 0 && site_len != kSiteIdChars) {
    return reject(IpcRejectReason::kMalformed, version);
  }
  if (ptr + site_len > end) return reject(IpcRejectReason::kMalformed, version);
  notification->site.assign(ptr, site_len);
  ptr += site_len;
  if (site_len != 0 && !IsSiteId(notification->site)) {
    return reject(IpcRejectReason::kMalformed, version);
  }

  // Flags (v6).  A reserved bit is a malformed frame.  The flags byte is the
  // last field; nothing reads past it.
  if (ptr + 1 > end) return reject(IpcRejectReason::kMalformed, version);
  auto flags = static_cast<uint8_t>(*ptr);
  if ((flags & ~kNotifyFlagsKnown) != 0) {
    return reject(IpcRejectReason::kMalformed, version);
  }
  notification->host_vouched = (flags & kNotifyFlagHostVouched) != 0;
  notification->from_module = (flags & kNotifyFlagFromModule) != 0;
  notification->legacy_ex_caller = (flags & kNotifyFlagLegacyExCaller) != 0;
  notification->html_stored = (flags & kNotifyFlagHtmlStored) != 0;
  // Fail closed: the stored-page bit means nothing outside its one shape, so
  // a frame that carries it in any other is refused whole.
  if (!HtmlStoredShapeValid(
          notification->html_stored, notification->from_module,
          notification->host_vouched, !notification->site.empty(),
          notification->content_type == ContentType::kHtml)) {
    return reject(IpcRejectReason::kMalformed, version);
  }

  // Both or neither, checked on the way in as well as on the way out: a sender
  // that never emits a half context does not make a receiver that would accept
  // one safe.
  if (notification->option_context.empty() !=
      notification->option_signature.empty()) {
    return reject(IpcRejectReason::kMalformed, version);
  }

  return true;
}

}  // namespace pagespeed
