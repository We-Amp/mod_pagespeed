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

#include "pagespeed/system/daemon_serve_arm.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "pagespeed/kernel/base/atomic_int32.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/time_util.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/system/daemon_adapter.h"
#include "pagespeed/system/daemon_health.h"
#include "pagespeed/system/daemon_record_arm.h"

namespace net_instaweb {

namespace {

// Monotonic count of constructed readers; see DaemonServeReadersConstructed.
AtomicInt32 g_serve_readers_constructed(0);

// The optimizer's own content-identity mixer, reproduced so the derived tag
// is byte-identical to the one its front stage emits for the same entry.
// FNV-1a, 64-bit, with a separator between the two origin validators so that
// an absent ETag plus a timestamp can never alias an ETag whose raw bytes
// happen to spell that timestamp.
constexpr uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;
constexpr uint8_t kFnvDomainSeparator = 0x1F;

// Bytes of the stamped source binding the identity is taken from, and the
// full length of that binding.  The high 8 bytes are used, big-endian, which
// is the optimizer's choice and not a free one.
constexpr size_t kOriginHashBytes = 32;
constexpr size_t kIdentityBytes = 8;

uint64_t FnvMix(uint64_t hash, uint8_t byte) {
  hash ^= byte;
  hash *= kFnvPrime;
  return hash;
}

// The identity discriminator, or false when the entry carries no identity
// signal at all and the legacy tag shape is owed.
bool ContentIdentity64(const uint8_t* origin_html_hash, StringPiece origin_etag,
                       uint32_t origin_last_modified, uint64_t* out) {
  // Tier 1: the stamped source binding.  All-zero is the reserved "not
  // bound" value, never a real hash output.
  if (origin_html_hash != nullptr) {
    bool all_zero = true;
    for (size_t i = 0; i < kOriginHashBytes; ++i) {
      if (origin_html_hash[i] != 0) {
        all_zero = false;
        break;
      }
    }
    if (!all_zero) {
      uint64_t value = 0;
      for (size_t i = 0; i < kIdentityBytes; ++i) {
        value = (value << 8) | static_cast<uint64_t>(origin_html_hash[i]);
      }
      *out = value;
      return true;
    }
  }

  // Tier 2: the origin's own validators, as stored.
  if (!origin_etag.empty() || origin_last_modified != 0) {
    uint64_t hash = kFnvOffset;
    for (char c : origin_etag) {
      hash = FnvMix(hash, static_cast<uint8_t>(c));
    }
    hash = FnvMix(hash, kFnvDomainSeparator);
    for (int i = 0; i < 4; ++i) {
      hash = FnvMix(
          hash, static_cast<uint8_t>((origin_last_modified >> (8 * i)) & 0xFF));
    }
    *out = hash;
    return true;
  }

  // Tier 3: nothing to discriminate with.
  return false;
}

// Returns `piece` as a NUL-terminated C string in `storage`, or nullptr for
// an empty piece.  Same helper, and the same reason, as the record arm's.
const char* CStrOrNull(StringPiece piece, GoogleString* storage) {
  if (piece.empty()) {
    return nullptr;
  }
  piece.CopyToString(storage);
  return storage->c_str();
}

uint32_t ClampToUint32(int64 value) {
  if (value <= 0) {
    return 0;
  }
  if (value > static_cast<int64>(UINT32_MAX)) {
    return UINT32_MAX;
  }
  return static_cast<uint32_t>(value);
}

// Strips one `W/` prefix and the surrounding quotes from an entity tag,
// leaving the opaque tag.  The weak comparison RFC 9110 requires for GET
// ignores the weakness marker on both sides.
StringPiece OpaqueTag(StringPiece tag) {
  TrimWhitespace(&tag);
  if (tag.size() >= 2 && (tag[0] == 'W' || tag[0] == 'w') && tag[1] == '/') {
    tag.remove_prefix(2);
  }
  if (tag.size() >= 2 && tag[0] == '"' && tag[tag.size() - 1] == '"') {
    tag.remove_prefix(1);
    tag.remove_suffix(1);
  }
  return tag;
}

}  // namespace

uint32_t ServeImageFormat(uint32_t mask) {
  return (mask >> kPsMaskImageFormatShift) & kPsMaskImageFormatBits;
}

uint32_t ServeTransferEncoding(uint32_t mask) {
  return (mask >> kPsMaskTransferEncodingShift) & kPsMaskTransferEncodingBits;
}

uint8_t ServeAlternateIdForMask(uint32_t mask) {
  return static_cast<uint8_t>(mask & 0xFFu);
}

bool ServeAlternateIdIsSentinel(uint8_t alternate_id) {
  return ((alternate_id >> kPsMaskViewportShift) & kPsMaskViewportBits) ==
         kPsViewportSentinel;
}

uint32_t ServeMaskWithOriginalFormat(uint32_t mask) {
  return (mask & ~(kPsMaskImageFormatBits << kPsMaskImageFormatShift)) |
         (kPsImageFormatOriginal << kPsMaskImageFormatShift);
}

uint32_t ServeMaskWithIdentityEncoding(uint32_t mask) {
  return (mask &
          ~(kPsMaskTransferEncodingBits << kPsMaskTransferEncodingShift)) |
         (kPsTransferEncodingIdentity << kPsMaskTransferEncodingShift);
}

bool ServeFormatIsAdvertised(uint32_t client_mask, uint32_t stored_mask) {
  const uint32_t stored = ServeImageFormat(stored_mask);
  if (stored == kPsImageFormatOriginal) {
    return true;
  }
  if (stored == kPsImageFormatSvg) {
    // The peer never derives kSvg from a client's headers, so no request can
    // advertise it and there is no mask value this could compare equal to.
    // Refused explicitly rather than left to the equality below, so that a
    // future mask layout in which kSvg IS derivable does not make this
    // silently start serving.
    return false;
  }
  return stored == ServeImageFormat(client_mask);
}

StringPiece ServeMediaTypeForServedBytes(int ps_content_type,
                                         StringPiece body) {
  if (ps_content_type != kPsContentImage) {
    return StringPiece();
  }
  const char* p = body.data();
  const size_t n = body.size();
  if (p == nullptr) {
    return StringPiece();
  }
  if (n >= 8 && memcmp(p, "\x89PNG\r\n\x1a\n", 8) == 0) {
    return StringPiece("image/png");
  }
  if (n >= 3 && memcmp(p, "\xff\xd8\xff", 3) == 0) {
    return StringPiece("image/jpeg");
  }
  if (n >= 6 && (memcmp(p, "GIF87a", 6) == 0 || memcmp(p, "GIF89a", 6) == 0)) {
    return StringPiece("image/gif");
  }
  if (n >= 12 && memcmp(p, "RIFF", 4) == 0 && memcmp(p + 8, "WEBP", 4) == 0) {
    return StringPiece("image/webp");
  }
  // The ISO base-media `ftyp` box, whose major brand is the format.  `avis`
  // is the animated sequence brand and is the same media type.
  if (n >= 12 && memcmp(p + 4, "ftyp", 4) == 0 &&
      (memcmp(p + 8, "avif", 4) == 0 || memcmp(p + 8, "avis", 4) == 0)) {
    return StringPiece("image/avif");
  }
  // Not a format this module can name.  The origin's field stands, which is
  // the right answer for an SVG, an ICO, or anything else the optimizer
  // stored without converting.
  return StringPiece();
}

bool ServeEncodingIsServable(uint32_t stored_mask) {
  return ServeTransferEncoding(stored_mask) == kPsTransferEncodingIdentity;
}

const char* ServeCompressorMediaType(const DaemonServeDecision& decision,
                                     const char* request_media_type) {
  return decision.content_type.empty() ? request_media_type
                                       : decision.content_type.c_str();
}

GoogleString ServeVaryFieldValue(const DaemonServeDecision& decision,
                                 bool handed_to_compressor) {
  GoogleString value;
  if (decision.emit_vary_accept) {
    value.assign(HttpAttributes::kAccept);
  }
  // THE 304 LEG ONLY; on the 200 the compressor is the emitter.  The
  // predicate is the compressor's own: a media type it is given the body
  // for, and a body big enough that it does not take its pass-through
  // shortcut before stating the axis.
  if (decision.not_modified && handed_to_compressor &&
      decision.body.size() > kServeCompressorPassThroughBytes) {
    if (!value.empty()) {
      StrAppend(&value, ", ");
    }
    StrAppend(&value, HttpAttributes::kAcceptEncoding);
  }
  return value;
}

GoogleString ServeDerivedETag(uint32_t full_mask, uint8_t flags,
                              const uint8_t* origin_html_hash,
                              StringPiece origin_etag,
                              uint32_t origin_last_modified,
                              size_t body_length) {
  uint64_t identity = 0;
  const bool has_identity = ContentIdentity64(origin_html_hash, origin_etag,
                                              origin_last_modified, &identity);
  // Sized from the widest form: W/"ps- (6) + mask+flags (10) + '-' +
  // identity (16) + '-' + length (16) + closing quote + NUL.
  char buffer[52];
  int written;
  if (has_identity) {
    written =
        snprintf(buffer, sizeof(buffer), "W/\"ps-%08x%02x-%016llx-%016llx\"",
                 static_cast<unsigned>(full_mask), static_cast<unsigned>(flags),
                 static_cast<unsigned long long>(identity),
                 static_cast<unsigned long long>(body_length));
  } else {
    // The identity-free form, emitted byte for byte as the optimizer emits
    // it, so a client holding a tag for an entry that carries no identity
    // signal keeps getting its 304 whichever front end answers.
    written =
        snprintf(buffer, sizeof(buffer), "W/\"ps-%08x%02x-%016llx\"",
                 static_cast<unsigned>(full_mask), static_cast<unsigned>(flags),
                 static_cast<unsigned long long>(body_length));
  }
  if (written <= 0 || static_cast<size_t>(written) >= sizeof(buffer)) {
    // Suppressed, never truncated: half a validator is a validator for a
    // representation that does not exist.
    return GoogleString();
  }
  return GoogleString(buffer, static_cast<size_t>(written));
}

bool ServeDerivedValidatorMatches(StringPiece if_none_match,
                                  StringPiece derived) {
  if (if_none_match.empty() || derived.empty()) {
    return false;
  }
  const StringPiece want = OpaqueTag(derived);
  if (want.empty()) {
    return false;
  }
  // The field is a comma-separated list.  An entity tag cannot contain a
  // comma (RFC 9110 defines the opaque tag as etagc, which excludes it), so
  // splitting on commas cannot cut one in half.
  StringPieceVector entries;
  SplitStringPieceToVector(if_none_match, ",", &entries,
                           true /* omit_empty_strings */);
  for (StringPiece entry : entries) {
    TrimWhitespace(&entry);
    if (entry == "*") {
      // Not a validator on any plane; see the header.
      continue;
    }
    if (OpaqueTag(entry) == want) {
      return true;
    }
  }
  return false;
}

bool ServeOriginDateIsUnmodified(StringPiece if_modified_since,
                                 uint32_t origin_last_modified) {
  if (if_modified_since.empty() || origin_last_modified == 0) {
    return false;
  }
  int64 client_ms = 0;
  if (!ConvertStringToTime(if_modified_since, &client_ms)) {
    // RFC 9110: an unparseable `If-Modified-Since` is IGNORED, and ignoring
    // it means serving the response.  Never a 304 on a date we could not
    // read.
    return false;
  }
  // The stored stamp has one-second resolution and HTTP dates have no
  // sub-second component, so this is a whole-second comparison on both
  // sides.  Not-modified means the origin's last modification is at or
  // before what the client already has.
  return static_cast<int64>(origin_last_modified) <= client_ms / 1000;
}

DaemonServeReader::DaemonServeReader(const DaemonAbi* abi, void* cache)
    : abi_(abi), cache_(cache) {
  g_serve_readers_constructed.BarrierIncrement(1);
}

DaemonServeReader::~DaemonServeReader() { Release(); }

void DaemonServeReader::Release() {
  if (result_ != nullptr) {
    abi_->ReadFree(result_);
    result_ = nullptr;
  }
}

int DaemonServeReader::ReadBest(const DaemonServeRequest& request,
                                uint32_t mask) {
  Release();
  GoogleString url, hostname, scheme;
  request.url.CopyToString(&url);
  request.hostname.CopyToString(&hostname);
  request.scheme.CopyToString(&scheme);
  const int status = abi_->CacheReadBest(cache_, url.c_str(), hostname.c_str(),
                                         scheme.c_str(), mask, &result_);
  if (status != kPsOk) {
    // The peer does not hand back a result it did not produce; make sure of
    // it rather than inherit the assumption.
    result_ = nullptr;
  }
  return status;
}

int DaemonServeReader::ReadById(const DaemonServeRequest& request,
                                uint8_t alternate_id) {
  Release();
  GoogleString url, hostname, scheme;
  request.url.CopyToString(&url);
  request.hostname.CopyToString(&hostname);
  request.scheme.CopyToString(&scheme);
  const int status =
      abi_->CacheReadAlternate(cache_, url.c_str(), hostname.c_str(),
                               scheme.c_str(), alternate_id, &result_);
  if (status != kPsOk) {
    result_ = nullptr;
  }
  return status;
}

int DaemonServeReader::ProbeOriginalFlags(const DaemonServeRequest& request,
                                          uint8_t* flags) {
  *flags = 0;
  const int status = ReadById(request, kPsSentinelOriginal);
  if (status == kPsOk) {
    *flags = abi_->ReadFlags(result_);
  }
  // Released either way.  On the declined path nothing is served; otherwise
  // the selection below replaces the held result anyway, so holding it here
  // would only add a second lifetime rule to one member.  The byte is copied
  // out first precisely so that nothing later needs the result to still be
  // held to answer a question about the ORIGINAL.
  Release();
  return status;
}

int DaemonServeReader::RetryByExactId(const DaemonServeRequest& request,
                                      DaemonServeDecision* decision) {
  // Two candidates, in preference order, both named from the SELECTION mask
  // so neither can land on a pre-compressed entry:
  //
  //   1. the request's own id -- the variant in the format this client
  //      advertised.  This is what an SVG variant outranked.
  //   2. the format-normalized id -- the original-format variant at this
  //      client's other axes, which an SVG variant also outranks.
  //
  // Each is checked against the same two disqualifies as the selection's
  // result, because a by-id read bypasses the peer's scoring entirely and is
  // therefore the one place an unservable entry could still arrive.
  //
  // AND NEITHER MAY BE A SENTINEL.  The selection read cannot hand one back
  // -- the peer's SELECTOR skips the sentinel space outright and scores a
  // stored sentinel id at zero, which is the guard ReadBest gets for free --
  // but this retry names ids locally and bypasses the selector entirely, so
  // the guard is ours: a selection mask carrying the reserved viewport
  // (bits 2-3 at 3, which no classifier produces today) would fold the
  // format-normalized candidate onto 0x0C, the durable original's id, and
  // the original would be served as kServeOptimized with `Vary: Accept` and
  // the wrong serve class.  Both candidates share the viewport field, so one
  // guard covers both: when it fires the retry names nothing and returns
  // not-found, and the request can still reach the durable original only
  // through the origin-mode read that names it honestly.
  const uint8_t candidates[] = {
      ServeAlternateIdForMask(decision->selection_mask),
      ServeAlternateIdForMask(
          ServeMaskWithOriginalFormat(decision->selection_mask)),
  };
  for (uint8_t id : candidates) {
    if (ServeAlternateIdIsSentinel(id)) {
      continue;
    }
    if (ReadById(request, id) != kPsOk) {
      continue;
    }
    const uint32_t stored_mask = abi_->ReadMask(result_);
    if (ServeEncodingIsServable(stored_mask) &&
        ServeFormatIsAdvertised(decision->client_mask, stored_mask)) {
      return kPsOk;
    }
    Release();
  }
  return kPsErrNotFound;
}

void DaemonServeReader::Compose(const DaemonServeRequest& request,
                                int64 now_seconds, bool negotiated,
                                uint8_t original_flags,
                                DaemonServeDecision* decision) {
  const uint32_t stored_mask = abi_->ReadMask(result_);
  const uint8_t stored_flags = abi_->ReadFlags(result_);
  const int content_type = abi_->ReadContentType(result_);

  decision->selected = true;
  decision->stored_mask = stored_mask;
  decision->stored_flags = stored_flags;
  decision->alternate_id = ServeAlternateIdForMask(stored_mask);
  decision->worker_processed = abi_->ReadIsWorkerProcessed(result_) != 0;
  decision->origin_content_length = abi_->ReadOriginContentLength(result_);
  decision->ps_content_type = content_type;

  const char* origin_ct = abi_->ReadOriginContentType(result_);
  if (origin_ct != nullptr) {
    decision->content_type.assign(origin_ct);
  }
  decision->origin_last_modified = abi_->ReadOriginLastModified(result_);

  const uint8_t* data = nullptr;
  size_t length = 0;
  if (abi_->ReadContent(result_, &data, &length) != kPsOk || data == nullptr) {
    decision->verdict = DaemonServeVerdict::kFallThrough;
    decision->reason = daemon_serve_reason::kReadFailed;
    decision->serve_class = kPsServeClassOriginalPending;
    decision->selected = false;
    return;
  }
  decision->body = StringPiece(reinterpret_cast<const char*>(data), length);

  // ---- the emitted media type, which has to describe the BYTES ---------
  //
  // Taken from the served bytes and NOT from the mask's format field: that
  // field names the slot a variant was derived for, and the peer fills a
  // converted-format slot with the ORIGINAL bytes when the conversion does
  // not pay.  See ServeMediaTypeForServedBytes.  Where the bytes are not a
  // format this module can name, the helper says nothing and the origin's
  // field -- assigned above -- stands.
  const StringPiece served_media_type =
      ServeMediaTypeForServedBytes(content_type, decision->body);
  if (!served_media_type.empty()) {
    served_media_type.CopyToString(&decision->content_type);
  }

  // ---- the origin's freshness, answered by the peer -------------------
  PsFreshnessInput freshness_input;
  memset(&freshness_input, 0, sizeof(freshness_input));
  freshness_input.struct_size = sizeof(freshness_input);
  freshness_input.now_seconds = ClampToUint32(now_seconds);
  freshness_input.cache_inserted_at = abi_->ReadCacheInsertedAt(result_);
  freshness_input.origin_max_age = abi_->ReadOriginMaxAge(result_);
  freshness_input.origin_s_maxage = abi_->ReadOriginSMaxAge(result_);
  freshness_input.content_type = content_type;
  // SHARED, and never the other value.  The volume is written by the
  // optimizer and read by every request that reaches this server, so it is a
  // shared cache by definition -- and shared is also the peer's own zero and
  // its fail-safe direction: a shared cache that believed it was private
  // would ignore `s-maxage` and would reuse an origin's `private` response
  // for the next user.
  freshness_input.cache_scope = kPsCacheScopeShared;
  freshness_input.force_revalidate = request.force_revalidate ? 1 : 0;
  freshness_input.origin_cc_flags = abi_->ReadOriginCcFlags(result_);

  PsFreshnessResult freshness;
  memset(&freshness, 0, sizeof(freshness));
  freshness.struct_size = sizeof(freshness);
  if (abi_->EvaluateFreshness(&freshness_input, nullptr, &freshness) != kPsOk ||
      freshness.verdict == kPsFreshnessRevalidate ||
      freshness.verdict == kPsFreshnessStaleServeRetired) {
    // Do not serve.  Falling through goes to the origin, which IS the
    // revalidation -- and a failure to evaluate is treated the same way,
    // because an unevaluated entry is one whose lifetime is unknown.
    decision->verdict = DaemonServeVerdict::kFallThrough;
    decision->reason = daemon_serve_reason::kNeedsRevalidation;
    decision->serve_class = kPsServeClassOriginalPending;
    decision->selected = false;
    decision->body = StringPiece();
    // GENUINE AGE-BASED EXPIRY OF A SELECTED VARIANT, and only that, is
    // marked for the seam: the verdict alone is not enough, because the same
    // two verdicts are also reached by a client force-revalidate and by
    // origin `no-cache` content, and neither of those means the stored
    // variants are outdated.  The peer's evaluator draws exactly that
    // distinction in `expired_by_age` (guaranteed false on both -- pinned in
    // daemon_serve_arm_test.cc), so the flag is the verdict AND the expiry
    // AND the negotiated path.  A durable-original decline (negotiated ==
    // false) never carries it: an original-only entry has no variant set to
    // purge, and its fall-through converges by the ordinary
    // record -> notify path.  On an evaluation FAILURE the zeroed result
    // keeps the flag clear, which is the safe direction: an entry whose
    // lifetime is unknown is not evidence the variant set is stale.  What
    // the seam does with the flag, and the split-brain it heals, is
    // documented on the field and on DaemonServeOriginRefreshedNotify.
    decision->stale_variant_expired_by_age =
        negotiated && freshness.expired_by_age != 0 &&
        (freshness.verdict == kPsFreshnessRevalidate ||
         freshness.verdict == kPsFreshnessStaleServeRetired);
    return;
  }

  // ---- the emitted Cache-Control, built by the peer -------------------
  PsCacheControlInput cc_input;
  memset(&cc_input, 0, sizeof(cc_input));
  cc_input.struct_size = sizeof(cc_input);
  // SAFE, which is what makes SWR synthesis structurally impossible here
  // rather than merely not asked for: the aggressive mode is the only one
  // that may synthesize `stale-while-revalidate`, and a synthesized SWR
  // emitted from an origin authorises downstream staleness the origin never
  // permitted.
  cc_input.mode = kPsCacheModeSafe;
  cc_input.cache_scope = kPsCacheScopeShared;
  cc_input.effective_max_age = freshness.effective_max_age;
  cc_input.origin_max_age = freshness_input.origin_max_age;
  cc_input.content_type = content_type;
  cc_input.origin_cc_flags = freshness_input.origin_cc_flags;
  cc_input.synthesize_swr = 0;
  // FIDELITY, not storage permission.  An origin that said `no-cache` wants
  // every use revalidated; dropping the directive turns that resource into
  // one freely reusable for the whole of max-age, downstream, with nothing
  // to notice.  The fix-headers filter that would otherwise have shortened
  // the downstream lifetime is deliberately not attached on this substrate,
  // so nothing else is going to put this back.
  cc_input.relay_origin_no_cache = 1;
  // The response IS stored here, so the origin's storage restrictions are
  // not being passed through unstored; forwarding them would tell the next
  // cache in the chain not to keep what this one just kept.
  cc_input.forward_origin_restrictions = 0;

  char cc_buffer[kPsCacheControlBufferBytes];
  size_t cc_length = 0;
  if (abi_->BuildCacheControl(&cc_input, cc_buffer, sizeof(cc_buffer),
                              &cc_length, nullptr) == kPsOk &&
      cc_length > 0 && cc_length <= sizeof(cc_buffer)) {
    decision->cache_control.assign(cc_buffer, cc_length);
  }

  // ---- the derived validator ------------------------------------------
  const char* origin_etag = abi_->ReadOriginEtag(result_);
  decision->etag = ServeDerivedETag(
      stored_mask, stored_flags, abi_->ReadOriginHtmlHash(result_),
      origin_etag == nullptr ? StringPiece() : StringPiece(origin_etag),
      decision->origin_last_modified, length);
  decision->age_seconds = freshness.age_seconds;
  decision->not_modified =
      ServeDerivedValidatorMatches(request.if_none_match, decision->etag);
  // The ORIGIN-plane conditional, asked only where this arm is emitting the
  // origin's own `Last-Modified` -- which is the durable-original serve, the
  // one case `negotiated` is false for.  Asking it on an optimized variant
  // would answer 304 over a validator that response does not carry.
  if (!negotiated && !decision->not_modified &&
      ServeOriginDateIsUnmodified(request.if_modified_since,
                                  decision->origin_last_modified)) {
    decision->not_modified = true;
  }

  // ---- the Vary biconditional -----------------------------------------
  //
  // Clause (i): THIS ARM chose the bytes from the client's `Accept`.  The
  // only field of the capability mask `Accept` moves is the image-format
  // one, and the peer's variants for a non-image entry carry the original
  // format by construction -- so for anything but an image, a different
  // `Accept` could not have produced different bytes.  A durable-original
  // serve is never clause (i): no variant selection happened.
  //
  // Clause (ii): the ORIGIN negotiates on `Accept` itself, recorded on the
  // entry at store time.  Omitting it is the composed-negotiation defect:
  // the stored bytes are whichever representation the FIRST requester's
  // `Accept` elicited, and without the header every cache downstream serves
  // that one copy to every client, under one key.
  //
  // THIS CLAUSE IS WHY `Accept` IS A HANDLED `Vary` AXIS in the record arm's
  // reproducible set, and the two must move together.  The origin's
  // `Vary: Accept` is not dropped by serving from the entry -- it is put back
  // here, off the marker the record arm stored on the DURABLE ORIGINAL, on
  // every serve of the URL and not only on the ones that serve that original.
  // That is what makes the response reproducible on this axis.  Narrow
  // kHandledVaryTokens again and every `Accept`-negotiating origin is marked
  // at record time and declined before this line is ever reached, which would
  // make this clause dead code and the 1.7 stamping obligation pointless.
  decision->vary_accept_negotiated =
      negotiated && content_type == kPsContentImage;
  // OFF THE DURABLE ORIGINAL'S BYTE, NOT THE SERVED ENTRY'S.  This is the
  // origin's statement about the URL, and it lives only on the entry the
  // writer stamped: an optimizer-produced variant carries a clear flags byte
  // by construction.  Reading it from `stored_flags` -- the entry being
  // served -- is right on a durable-original serve and silently wrong on an
  // optimized one, which is the composed-negotiation defect this clause
  // exists to prevent, reintroduced one level down.  It bit exactly that way:
  // a non-image entry whose origin sent `Vary: Accept` served its optimized
  // variant with the header gone, on the class the record arm admits BECAUSE
  // this clause puts the header back.
  decision->vary_accept_origin_declared =
      (original_flags & kPsFlagOriginVariesAccept) != 0;
  decision->emit_vary_accept =
      decision->vary_accept_negotiated || decision->vary_accept_origin_declared;
}

DaemonServeDecision DaemonServeReader::Serve(const DaemonServeRequest& request,
                                             int64 now_seconds) {
  DaemonServeDecision decision;

  if (abi_ == nullptr || cache_ == nullptr) {
    decision.reason = daemon_serve_reason::kSubstrateUnavailable;
    decision.serve_class = kPsServeClassOriginalSkew;
    return decision;
  }

  // The same two URL restrictions the record arm applies, in the same order
  // and for the same reasons.  Asked here rather than assumed from the
  // record side: a request can reach this arm for a URL nothing recorded.
  if (IsRewrittenUrlShape(request.url)) {
    decision.reason = daemon_serve_reason::kRewrittenUrlShape;
    return decision;
  }
  if (!UrlIsKeySafe(request.url)) {
    decision.reason = daemon_serve_reason::kUrlNotKeySafe;
    return decision;
  }

  // ---- rule 6: the origin sent something this substrate cannot put back ---
  //
  // ASKED BEFORE ANYTHING IS SELECTED, and of the durable original alone.  A
  // marked URL is declined whichever class would have answered it: the
  // optimized variant is precisely the thing that would have been served with
  // the origin's other headers missing, and the durable original would be
  // served with them missing too.  So the substrate declines and the request
  // goes out by the plain, non-in-place path -- rule 6's ruled destination,
  // and exactly what a server with no daemon would do.
  //
  // The reason is its own, not folded into `no-entry`: see the constant.
  //
  // The probe's STATUS is kept and reused below, so a key with no durable
  // original pays for this read instead of paying for a second one later --
  // the extra read is confined to the path that ends up serving the original,
  // which is the one place the entry is genuinely read twice (the selection
  // in between replaces whatever result is held).
  //
  // The byte it hands back is ALSO what clause (ii) of the `Vary`
  // biconditional is answered from, further down -- see Compose.  Both
  // statements belong to the durable original and neither can be read off an
  // optimized variant.
  uint8_t original_flags = 0;
  int original_status = ProbeOriginalFlags(request, &original_flags);
  if ((original_flags & kPsFlagOriginHeadersNotReproducible) != 0) {
    decision.reason = daemon_serve_reason::kOriginHeadersNotReproducible;
    // The entry EXISTS and is being declined, which is the peer's
    // original-declined class.  Reporting it as cold or pending would put a
    // deliberate refusal in a bucket that means "nothing to serve yet".
    decision.serve_class = kPsServeClassOriginalDeclined;
    return decision;
  }

  // THE CAPABILITY MASK IS THE PEER'S ANSWER OVER THIS REQUEST'S HEADERS.
  // Four request fields in, one mask out, and nothing else reaches it -- no
  // configuration value, no options signature, no server state.  That is
  // what makes the entry this arm selects a function of the request alone,
  // which is the only thing the shared cache's key can express.
  GoogleString accept, user_agent, save_data, accept_encoding;
  decision.client_mask =
      abi_->Classify(CStrOrNull(request.accept, &accept),
                     CStrOrNull(request.user_agent, &user_agent),
                     CStrOrNull(request.save_data, &save_data),
                     CStrOrNull(request.accept_encoding, &accept_encoding));

  // THE MASK THE READ IS TAKEN AT is the classifier's answer with the
  // transfer-encoding field normalized to identity.  See
  // ServeMaskWithIdentityEncoding for why that is a correctness fix and not a
  // preference: without it every request from a real browser -- which
  // advertises brotli -- selects a stored brotli variant this arm must
  // refuse, and the URL degrades to the unoptimized durable original for the
  // whole population.
  decision.selection_mask = ServeMaskWithIdentityEncoding(decision.client_mask);

  // ---- variant selection ------------------------------------------------
  int status = ReadBest(request, decision.selection_mask);
  if (status == kPsOk) {
    const uint32_t stored_mask = abi_->ReadMask(result_);
    if (!ServeEncodingIsServable(stored_mask)) {
      // A FLOOR, not the working path.  With the selection mask normalized
      // to identity the peer hard-disqualifies every non-identity variant
      // itself, so reaching here means the peer returned something its own
      // scoring rule says it should not have.  Refuse rather than trust it:
      // emitting pre-compressed bytes the seam will not label produces a
      // response no client can decode.  The by-id retries below use the
      // normalized mask too, so they cannot land back here.
      Release();
      decision.reason = daemon_serve_reason::kEncodingNotServable;
      decision.serve_class = kPsServeClassOriginalPending;
      status = RetryByExactId(request, &decision);
    } else if (!ServeFormatIsAdvertised(decision.client_mask, stored_mask)) {
      // THE REFUSAL, AND THE RECOVERY IT OWES.
      //
      // Among ordinary variants the refusal costs nothing at the pinned peer
      // and cannot even be reached by them: a format-EXACT variant scores at
      // least 1000 while a mismatched raster format is hard-disqualified to
      // 0, so the selection returns the compatible one when it exists and
      // returns nothing at all when it does not.  Against those families this
      // branch is defense in depth against a scorer that is not published
      // contract -- see ServeFormatIsAdvertised.
      //
      // kSvg IS THE REGIME THAT MAKES THE RETRY NECESSARY.  The peer awards a
      // stored SVG variant +1200 regardless of the client's format, plus the
      // viewport, density and Save-Data bonuses unconditionally -- roughly
      // 1400 -- so on a vectorized URL the SVG variant outranks the WebP and
      // AVIF siblings that are sitting right beside it and perfectly
      // servable.  Refusing with no recovery would make EVERY optimized
      // variant of that URL unreachable.  So the arm asks for what it wants
      // by name: the request's own id first, then its format-normalized one.
      Release();
      decision.reason = daemon_serve_reason::kNoCompatibleVariant;
      decision.serve_class = kPsServeClassOriginalPending;
      status = RetryByExactId(request, &decision);
    }
  }

  if (status == kPsOk) {
    Compose(request, now_seconds, true /* negotiated */, original_flags,
            &decision);
    if (decision.selected) {
      decision.verdict = DaemonServeVerdict::kServeOptimized;
      decision.reason = daemon_serve_reason::kOptimizedVariant;
      decision.serve_class = kPsServeClassOptimized;
      // A FALLBACK HIT IS A SERVE, and it is answered here rather than
      // declined: the stored variant is one this client can decode, it is
      // merely not the one this client's mask names.  The comparison is
      // against the mask the read was taken at -- see the field's comment
      // for why `client_mask` would name the wrong axis.
      decision.fallback_hit = decision.stored_mask != decision.selection_mask;
    }
    return decision;
  }

  // ---- origin mode: the durable original, BY ID ------------------------
  //
  // THE TRAP THIS AVOIDS.  `ps_cache_read_best` can never return the
  // durable-originals class: the peer's selector skips it unconditionally
  // and by design, so that the original never competes with the optimized
  // variants.  A serving path that reached for the original through the
  // selector would find nothing, for every URL this module recorded, and
  // would read as a substrate with no entries rather than as a selector
  // doing what its own header says it does.  The class is read by exact id,
  // which is the read the peer blesses for it.
  //
  // The rule-6 probe above already asked this question and its answer is
  // reused: a key it found nothing under is not asked again, and one it DID
  // find is re-read because the selection in between released the result.
  if (original_status == kPsOk) {
    original_status = ReadById(request, kPsSentinelOriginal);
  }
  if (original_status != kPsOk) {
    if (!decision.selected &&
        decision.reason == daemon_serve_reason::kNoEntry) {
      // Nothing of any class under this key.  Distinguished from "entries
      // exist but none is usable", which the branches above already set.
      decision.serve_class = original_status == kPsErrNotFound
                                 ? kPsServeClassOriginalCold
                                 : kPsServeClassOriginalPending;
      decision.reason = original_status == kPsErrNotFound
                            ? daemon_serve_reason::kNoEntry
                            : daemon_serve_reason::kReadFailed;
    }
    return decision;
  }

  Compose(request, now_seconds, false /* negotiated */, original_flags,
          &decision);
  if (decision.selected) {
    decision.verdict = DaemonServeVerdict::kServeOriginal;
    decision.reason = daemon_serve_reason::kDurableOriginal;
    // ORIGIN BYTES, not an optimized serve.  The entry exists and no
    // acceptable alternate does, which is exactly the class the peer
    // publishes for it -- reporting it as OPTIMIZED would make the
    // partition say this substrate optimized something it did not.
    decision.serve_class = kPsServeClassOriginalPending;
  }
  return decision;
}

DaemonServeStats::DaemonServeStats(const DaemonAbi* abi, StringPiece cache_path)
    : abi_(abi), cache_path_(cache_path.data(), cache_path.size()) {}

DaemonServeStats::~DaemonServeStats() {
  if (handle_ != nullptr) {
    abi_->ServeStatsClose(handle_);
    handle_ = nullptr;
  }
}

void DaemonServeStats::Record(int serve_class) {
  if (abi_ == nullptr || cache_path_.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (!EnsureOpenLocked()) {
    return;
  }
  // kPsServeFlagNone always: the peer's flag on this call reports a FRONT
  // END's own cooldown or dedup having suppressed a request it would
  // otherwise have made, and this arm has no such cooldown to report.  Back
  // pressure is observe-only at this revision and nothing here branches on
  // it.
  abi_->ServeStatsRecordServeClass(handle_, serve_class, kPsServeFlagNone);
}

void DaemonServeStats::RecordHit(int content_type, uint64_t original_bytes,
                                 uint64_t optimized_bytes, uint32_t mask) {
  if (abi_ == nullptr || cache_path_.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (!EnsureOpenLocked()) {
    return;
  }
  abi_->ServeStatsRecordHit(handle_, content_type, original_bytes,
                            optimized_bytes, mask);
}

bool DaemonServeStats::EnsureOpenLocked() {
  if (handle_ == nullptr) {
    if (attempted_ && ++since_attempt_ < kReopenInterval) {
      return false;
    }
    attempted_ = true;
    since_attempt_ = 0;
    if (abi_->ServeStatsOpen(cache_path_.c_str(), &handle_) != kPsOk) {
      handle_ = nullptr;
      return false;
    }
  }
  return true;
}

DaemonServeReader* MakeDaemonServeReaderIfReady(DaemonAdapter* adapter) {
  if (adapter == nullptr || adapter->health() != DaemonHealth::kReady) {
    return nullptr;
  }
  // Asked for BEFORE the reader exists, not after: the volume handle is what
  // makes a reader able to answer at all, and one built without it would
  // classify every request as a cold cache.
  void* cache = adapter->RecordCache();
  if (cache == nullptr || adapter->abi() == nullptr) {
    return nullptr;
  }
  return new DaemonServeReader(adapter->abi(), cache);
}

bool DaemonServeFallbackRenotify(const DaemonAbi& abi, StringPiece socket_path,
                                 const DaemonServeRequest& request,
                                 const DaemonServeDecision& decision,
                                 StringPiece option_context,
                                 StringPiece option_signature,
                                 Variable* notified, Variable* notify_failed) {
  // Fallback serves only: an exact serve means the family already holds this
  // client's variant, and anything that was not an optimized serve has the
  // miss -> record -> notify path to converge on.
  if (decision.verdict != DaemonServeVerdict::kServeOptimized ||
      !decision.fallback_hit) {
    return false;
  }
  // THE 0x04 GATE.  See the declaration for the full reasoning: a URL whose
  // origin negotiates on `Accept` never acquires a variant family at the
  // current pin, so re-notifying would be permanent per-request traffic
  // asking for work the worker will never do.
  if (decision.vary_accept_origin_declared) {
    return false;
  }
  // Both halves or neither: a request whose configuration could not be named
  // asks for nothing, mirroring the record gate's kNoOptionContext outcome.
  if (option_context.empty() || option_signature.empty()) {
    return false;
  }

  GoogleString socket, url, hostname, scheme, context, signature;
  socket_path.CopyToString(&socket);
  request.url.CopyToString(&url);
  request.hostname.CopyToString(&hostname);
  request.scheme.CopyToString(&scheme);

  PsNotifyParams params;
  abi.NotifyParamsInit(&params);
  params.url = url.c_str();
  params.hostname = hostname.c_str();
  params.scheme = scheme.c_str();
  params.content_type = decision.ps_content_type;
  // THE RAW CLIENT MASK, as the classifier answered -- the same value the
  // record arm notifies at and the peer's own front end sends.  The worker
  // normalizes for dedup, so the normalization this arm applies at selection
  // time would only hide what the client actually advertised.
  params.mask = decision.client_mask;
  params.agent_request = 0;
  params.option_context = CStrOrNull(option_context, &context);
  params.option_context_length = option_context.size();
  params.option_signature = CStrOrNull(option_signature, &signature);
  // Fire and forget: the peer answers nothing, and a delivery failure costs
  // this request its convergence and nothing else.  No retry -- the next
  // fallback hit asks again.  A failure is not silent, though: it moves the
  // failure counter, so a dead socket is distinguishable from a quiet family
  // at rollout (see the declaration for why a suppression moves neither).
  if (abi.NotifyWorker(socket.c_str(), &params) == kPsOk) {
    if (notified != nullptr) {
      notified->Add(1);
    }
    return true;
  }
  if (notify_failed != nullptr) {
    notify_failed->Add(1);
  }
  return false;
}

bool DaemonServeOriginRefreshedNotify(
    const DaemonAbi& abi, StringPiece socket_path,
    const DaemonServeRequest& request, const DaemonServeDecision& decision,
    StringPiece option_context, StringPiece option_signature,
    Variable* notified, Variable* notify_failed) {
  // Flagged fall-throughs only: every other outcome either served (no purge
  // needed) or declined for a reason that is not "the variant set is
  // outdated" -- a cold key, a client force-revalidate, origin `no-cache` --
  // and those converge by the ordinary record -> notify path or have nothing
  // to purge.  See the field comment for why the verdict alone cannot draw
  // that line.
  if (decision.verdict != DaemonServeVerdict::kFallThrough ||
      !decision.stale_variant_expired_by_age) {
    return false;
  }
  // Both halves or neither, exactly as the record gate and the fallback
  // re-notify: a request whose configuration could not be named asks for
  // nothing, because a context-less notification is processed under the
  // default one.
  if (option_context.empty() || option_signature.empty()) {
    return false;
  }

  GoogleString socket, url, hostname, scheme, context, signature;
  socket_path.CopyToString(&socket);
  request.url.CopyToString(&url);
  request.hostname.CopyToString(&hostname);
  request.scheme.CopyToString(&scheme);

  PsNotifyParams params;
  abi.NotifyParamsInit(&params);
  params.url = url.c_str();
  params.hostname = hostname.c_str();
  params.scheme = scheme.c_str();
  params.content_type = decision.ps_content_type;
  // THE SENTINEL MASK, not a capability vector: this notification carries no
  // variant request at all -- it is the "the origin was re-fetched fresh,
  // the stored variants may be stale" event, and the worker reads the value
  // by name.  A mask a classifier could produce would be dedup-processed as
  // an ordinary notification and would not purge anything.
  params.mask = kOriginRefreshedSentinel;
  params.agent_request = 0;
  params.option_context = CStrOrNull(option_context, &context);
  params.option_context_length = option_context.size();
  params.option_signature = CStrOrNull(option_signature, &signature);
  // Fire and forget, once, no retry -- the same discipline as the fallback
  // re-notify, and the same failure accounting: a suppression moved neither
  // counter to get here, a send moves `notified`, and a failure moves
  // `notify_failed`, so a dead socket reads as the failure counter climbing
  // rather than as a quiet substrate.  The worker rate-limits the sentinel
  // per URL, so re-asking on the next age-expired fall-through is absorbed
  // rather than amplified.
  if (abi.NotifyWorker(socket.c_str(), &params) == kPsOk) {
    if (notified != nullptr) {
      notified->Add(1);
    }
    return true;
  }
  if (notify_failed != nullptr) {
    notify_failed->Add(1);
  }
  return false;
}

int64 DaemonServeReadersConstructed() {
  return g_serve_readers_constructed.value();
}

}  // namespace net_instaweb
