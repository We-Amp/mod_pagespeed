// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_LIB_HTML_SAFE_ENTITY_DECODE_H_
#define PAGESPEED_LIB_HTML_SAFE_ENTITY_DECODE_H_

#include <string>
#include <string_view>

// UTF-8 HTML character-reference decoding for the agent extraction surface
// (Markdown copy body text, inline code, and page metadata). This is the
// single source of truth, so the paths cannot drift apart.
//
// It decodes every named reference of the HTML Standard and every numeric
// reference to UTF-8, reference by reference, on text that is itself UTF-8
// (literal em dashes, curly quotes and accents next to references are kept).

namespace net_instaweb {

// True iff `s` is well-formed UTF-8. Rejects lone continuation bytes, invalid
// lead bytes (e.g. a lone Latin-1 0xB7 from &middot;), truncated/overlong
// sequences, surrogates, and out-of-range code points.
bool IsWellFormedUtf8(std::string_view s);

// `s` with every byte that does not start a well-formed UTF-8 sequence replaced
// by U+FFFD. With `drop_cut_tail` (for a value the caller cut at a byte cap),
// an incomplete sequence at the very end is dropped instead. Deterministic;
// well-formed input is returned as is.
std::string ToWellFormedUtf8(std::string_view s, bool drop_cut_tail = false);

// Fold every UTF-8 no-break space (U+00A0, the bytes C2 A0) to one ASCII
// space, in place. Both bytes are replaced together, so a multi-byte sequence
// is never split.
void FoldNbsp(std::string* s);

// True iff `name` (without '&' and ';') is a named character reference of the
// HTML Standard, such as "lt" or "mdash".
bool IsHtml5EntityName(std::string_view name);

// Decode the character references in text `s` for an agent-facing feed:
//   - every named reference of the HTML Standard ("&mdash;", "&rarr;",
//     "&NotEqualTilde;") and the historical names it accepts without a ';'
//     ("&copy 2026");
//   - numeric references, decimal and hex, mapped as the standard maps them
//     (0x80..0x9F as windows-1252; NUL, surrogates, out-of-range values and
//     other control characters as U+FFFD);
//   - a reference it does not know ("&bogus;", a bare '&') stays as written,
//     and the rest of the text is still decoded.
// The result is always well-formed UTF-8: input bytes that are not UTF-8 are
// replaced as by ToWellFormedUtf8 (an incomplete sequence at the end is
// dropped only when `value_was_capped`), and every no-break space (decoded or
// literal) is folded to an ASCII space. Markdown escaping of the decoded text
// (a decoded '<' must not become markup) is the caller's job.
std::string SafeDecodeHtmlEntities(std::string_view s,
                                   bool value_was_capped = false);

}  // namespace net_instaweb

#endif  // PAGESPEED_LIB_HTML_SAFE_ENTITY_DECODE_H_
