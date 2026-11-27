// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_LIB_CACHE_ORIGIN_CACHE_CONTROL_H_
#define PAGESPEED_LIB_CACHE_ORIGIN_CACHE_CONTROL_H_

#include <cstdint>
#include <string_view>

namespace pagespeed {

// The origin's Cache-Control state, in the shape the stored entry keeps it:
// a bitfield of directives plus the two freshness lifetimes.  The flag bits
// are `AlternateMetadata::kCCOrigin*` and feed `EvaluateFreshness` and
// `BuildCacheControlHeader` unchanged.
struct OriginCacheControl {
  uint16_t cc_flags = 0;
  uint32_t max_age = 0;
  uint32_t s_maxage = 0;
};

// Parse ONE `Cache-Control` response header line into `out`, accumulating.
//
// Accumulating, not replacing, because a response may carry several
// Cache-Control lines and RFC 9111 §5.2 treats them as one directive list:
// the flags are OR-ed, and a later `max-age` / `s-maxage` overwrites an
// earlier one exactly as a single combined line would.  Call it once per
// header line, in order.  Every call sets `kCCOriginHeaderPresent`, so after
// the last call that bit answers "did the origin send Cache-Control at all"
// — the question the freshness defaults turn on.
//
// Directive-name matching is ASCII case-insensitive.  For `private` and
// `no-cache`, the FORM of each occurrence is recorded as well: an occurrence
// carrying an argument (`private="Set-Cookie"`) is the qualified form of
// RFC 9111 §5.2.2.7, which restricts only the named fields, and a consumer
// that acts destructively on the directive must not treat it as blanket.
// The test is the presence of `=`, not a non-empty argument, because the
// grammar permits an empty field-name list.  A second `s-maxage` (in the same
// line or a later one) also sets kCCOriginSMaxageRepeated; the value kept is
// still the last one.
//
// Exported on the C ABI as `ps_parse_cache_control`, so an embedder stamps the
// same flags into a stored entry as the engine's own reader expects to find
// there.
//
// The only implementation of the derivation IN THE C++ TREE: the nginx front
// stage calls it per header line and keeps only the header walk, so a
// correction here reaches every C++ writer at once.  Not the only one that
// ships — the ASP.NET Core middleware still scans directives itself
// (`ContainsDirective`), and reads a qualified `private="Set-Cookie"` as
// blanket `private` where this does not; those flags come over the FFI in
// Tier B (#772), which is what retires that scan.
//
// Behaviour is pinned two ways: the unit tests below state the contract, and
// OriginCacheControlEquivalence compares this against a verbatim transcription
// of the pre-consolidation implementation, so an unintended change to the
// flags a stored entry carries goes red rather than silently re-stamping
// entries that outlive the code that wrote them.
void AccumulateOriginCacheControl(std::string_view header_value,
                                  OriginCacheControl* out);

// Parse a Cache-Control delta-seconds argument, saturating at UINT32_MAX.
// Surrounding optional whitespace and a surrounding pair of double quotes are
// tolerated (both occur in the wild); anything non-numeric after that yields 0, which is
// how a malformed lifetime becomes "no lifetime" rather than a huge one.
uint32_t ParseCacheControlSeconds(std::string_view value);

}  // namespace pagespeed

#endif  // PAGESPEED_LIB_CACHE_ORIGIN_CACHE_CONTROL_H_
