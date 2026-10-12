// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_KERNEL_PACKS_ENTITY_DECODE_H_
#define PAGESPEED_KERNEL_PACKS_ENTITY_DECODE_H_

#include <string>
#include <string_view>

// UTF-8-safe HTML entity decoding for pack values.
//
// HtmlKeywords::Unescape (the engine entity decoder) sets decoding_error=true
// and returns an EMPTY string for ANY input byte > 127 or any entity that
// decodes to more than one Latin-1 byte. Routing page text through it naively
// would erase every non-ASCII text node (em-dashes, curly quotes and accents
// are literal multibyte UTF-8 in a document). This is the single place that
// decodes "&amp;" / "&lt;" to human glyphs without ever erasing a non-ASCII
// byte or producing invalid UTF-8.

namespace pagespeed::packs {

// True iff `s` is well-formed UTF-8. Rejects lone continuation bytes, invalid
// lead bytes (e.g. a lone Latin-1 0xB7 from &middot;), truncated/overlong
// sequences, surrogates, and out-of-range code points.
bool IsWellFormedUtf8(std::string_view s);

// Fold every Latin-1 NBSP (0xA0) — the one non-ASCII byte HtmlKeywords::Unescape
// emits for &nbsp; — to an ASCII space, in place. A lone 0xA0 is invalid UTF-8,
// so it must never reach a value raw.
void FoldLatin1Nbsp(std::string* s);

// Entity-decode `s`:
//   1. Fast path: no '&' -> return the input verbatim (preserves pure UTF-8,
//      and avoids the Unescape scan).
//   2. On decoding_error -> return the ORIGINAL input verbatim (never the empty
//      Unescape result), preserving the original UTF-8 bytes.
//   3. On success -> fold 0xA0 (NBSP) to a space, then require the result to be
//      well-formed UTF-8. A named/numeric entity that decodes to a single
//      Latin-1 byte 0x80-0xFF (&middot;=0xB7, &copy;=0xA9, &eacute;=0xE9) is a
//      lone invalid byte; in that case fall back to the original entity text
//      rather than ship an invalid byte. Otherwise return the decoded string.
//
// The result is always well-formed UTF-8 (or the verbatim original input, which
// the callers treat as already-UTF-8 rendered DOM). Decoding is intentionally
// done BEFORE any markdown-structural escaping so a decoded '<script>' stays
// inert text downstream.
std::string SafeDecodeHtmlEntities(std::string_view s);

}  // namespace pagespeed::packs

#endif  // PAGESPEED_KERNEL_PACKS_ENTITY_DECODE_H_
