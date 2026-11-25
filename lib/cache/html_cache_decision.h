// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_LIB_CACHE_HTML_CACHE_DECISION_H_
#define PAGESPEED_LIB_CACHE_HTML_CACHE_DECISION_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace pagespeed {

// The one-line record of an HTML cache decision (html_cache_policy.h): what
// was decided for which URL and why, as a single JSON object a log can carry.
//
// A FORMATTER ONLY.  It owns no file, opens no socket and reads no clock; the
// caller supplies every value, including the timestamp, and decides where the
// line goes and how often.  `HtmlCacheDecisionDedupKey` is the key a caller
// can use to avoid writing the same decision twice.
//
// PRIVACY IS A PROPERTY OF THE KEY SET AND OF THE VALUE GRAMMAR.  The closed
// list of keys below has no key for a request or response body, a header
// value, a cookie, a client address or a user agent, and the formatter
// refuses any key outside the list.  Every value is then checked against its
// key's grammar, which admits fixed codes and counts only:
//
//   ts_ms, status, size, lifetime_s, age_s   a canonical decimal integer
//   host                                     hostname characters (letters,
//                                            digits, `.` `-` `_` `:` `[` `]`),
//                                            at most 255 bytes
//   path                                     the path the cache key was built
//                                            from: at most 300 bytes, never a
//                                            `?`, `#`, `%` or `;`
//   mode                                     off, report-only, serve-variants,
//                                            serve-cached-html
//   action                                   would_store, stored, skipped,
//                                            would_serve, served_html,
//                                            served_markdown, validated,
//                                            purged, invalidated, or a policy
//                                            verdict name (refuse, store,
//                                            store_no_notify, fall_through,
//                                            serve_html, serve_markdown,
//                                            validate)
//   reason                                   a store or serve reason name,
//                                            validated, validation_failed,
//                                            sidecar_write_failed,
//                                            invalidated, purge_race, budget,
//                                            sidecar_unpaired, url_disagree,
//                                            transport_conflict, or
//                                            `headers:<name>`
//                                            / `vary:<token>` with an RFC 9110
//                                            token of at most 128 bytes
//   validators                               none, or an ordered subset of
//                                            etag, last_modified
//   request                                  none, or an ordered subset of
//                                            authorization, cookie, no_cache,
//                                            accept_markdown
//   cc                                       known response directive names,
//                                            comma-separated, each at most
//                                            once, `=<integer>` only on
//                                            max-age, s-maxage,
//                                            stale-while-revalidate,
//                                            stale-if-error
//   vary                                     a comma-separated list of tokens,
//                                            at most 1024 bytes in all
//                                            (in cc and vary, spaces around
//                                            a comma are allowed)
//   sidecar                                  eligible_block, eligible_empty,
//                                            fall_through,
//                                            fall_through:<header-name>,
//                                            never:nonce_csp,
//                                            never:nonce_link
//   personalisation                          a signal name
//                                            (HtmlPersonalisationSignalNames)
//   before                                   a hash, decimal or lowercase hex
//   variant                                  0x7C or markdown
//   binding                                  live or stale
//   vhost_added                              header names the server's
//                                            own configuration added after
//                                            the store snapshot, as a
//                                            comma-separated list of tokens,
//                                            at most 1024 bytes (names only)
//
// Every value is non-empty (a fact that is absent is an absent key, which is
// how the line says "null") and carries no control byte.  A caller therefore
// cannot log a header value or a cookie by accident even under a legitimate
// key, and anyone can check the whole surface by reading
// HtmlCacheDecisionKeys() and the grammar table in the source.
//
// One rule spans two keys: a line whose mode is report-only states only
// would_store, would_store_no_notify, skipped or refuse as its action.  That
// mode stores nothing, so a would_serve line is refused.

struct HtmlCacheDecisionField {
  std::string_view key;
  std::string_view value;
};

// The closed key set, in canonical output order:
//   ts_ms, host, path, mode, action, reason, status, size, lifetime_s,
//   validators, cc, vary, sidecar, request, personalisation, before, variant,
//   binding, age_s, vhost_added
std::span<const std::string_view> HtmlCacheDecisionKeys();

// Longest `path` value the formatter accepts, in bytes.  The caller truncates.
inline constexpr size_t kMaxHtmlCacheDecisionPathBytes = 300;
inline constexpr size_t kMaxHtmlCacheDecisionVaryBytes = 1024;

// One JSON object on one line, keys in canonical order, only the keys given,
// values escaped (quote and backslash with a backslash, bytes 0x80 and above
// as \u00XX).  The keys ts_ms, status, size, lifetime_s and age_s are emitted
// as numbers (their grammar is a canonical decimal integer: digits only, no
// leading zero, at most 20 digits); every other value is a string.
//
// Returns nullopt for: an unknown key; a duplicate key; a value outside its
// key's grammar (see the table above), which includes an empty value, any C0
// control byte (0x00 to 0x1F) or DEL (0x7F), a `path` longer than
// kMaxHtmlCacheDecisionPathBytes or carrying `?`, `#`, `%` or `;`, a `vary`
// longer than kMaxHtmlCacheDecisionVaryBytes, and an empty `host`.
// True when the fields state nothing beyond the store side and the report:
// mode off or report-only, a report-only action, a store reason, no request
// fact or key that only describes a serve.  The C API admits only such a line.
bool HtmlCacheDecisionIsStoreSide(
    std::span<const HtmlCacheDecisionField> fields);

// The gate the C API applies (PS_API 1.14).  A line whose mode serves
// (serve-variants or serve-cached-html) is admitted whole; the formatter's
// grammar is then the only check.  Any other line must state nothing beyond
// the store side and the report, as HtmlCacheDecisionIsStoreSide requires,
// with two widenings: `request` may carry accept_markdown (report-only records
// whether a request asked for the Markdown copy), and `reason` may carry the
// front-end store refusals purge_race, budget and url_disagree.
bool HtmlCacheDecisionCApiAdmits(
    std::span<const HtmlCacheDecisionField> fields);

std::optional<std::string> FormatHtmlCacheDecisionLine(
    std::span<const HtmlCacheDecisionField> fields);

// Whether `value` is in the grammar of `key` (false for an unknown key): the
// same check the formatter applies, for a caller that wants to know which
// field it must leave out before it formats.
bool HtmlCacheDecisionValueFits(std::string_view key, std::string_view value);

// FNV-1a, 64 bit, over `bytes`: the `before` hash of stored bytes.  Not a
// security hash; it exists so a log line can say "the stored bytes changed"
// without carrying them.
uint64_t HtmlCacheFnv1a64(std::string_view bytes);

// The dedup key: host, path, mode, action, reason, sidecar and before, joined
// with '\x1f'.  A key that was not given contributes an empty component.
std::string HtmlCacheDecisionDedupKey(
    std::span<const HtmlCacheDecisionField> fields);

}  // namespace pagespeed

#endif  // PAGESPEED_LIB_CACHE_HTML_CACHE_DECISION_H_
