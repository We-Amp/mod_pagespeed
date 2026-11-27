// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_SRC_WORKER_ASYNC_CSS_LOADER_H_
#define PAGESPEED_SRC_WORKER_ASYNC_CSS_LOADER_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace pagespeed {

// The loader script. Each deferred sheet ships as
// `<link rel="preload" as="style" data-pagespeed-media="<original>">`, which the
// browser fetches at the normal stylesheet priority. This loader turns each one
// into the stylesheet it was: restore the recorded media, then flip
// rel preload -> stylesheet. The flipped element IS the preload's consumer, so
// the already-downloaded bytes are reused -- no second <link>, no second fetch.
//
// Three triggers, one idempotent swap (`s` returns unless the link is still a
// preload, so any two of them firing is harmless):
//   1. the link's `load` event -- the common case for a sheet still in flight
//      when this deferred script runs;
//   2. a Resource Timing entry for the href -- the sheet finished BEFORE this
//      script ran, so its `load` already fired and will not fire again;
//   3. window `load` -- the backstop for (2) missing its entry (the resource
//      timing buffer is finite and can drop entries on a very large page);
//      without it such a sheet would never apply at all.
// `error` swaps too. That is a RECOVERY HOOK, not a refetch guarantee: measured
// in the pinned Chromium, flipping rel after a failed preload issues ZERO
// additional requests -- the browser reuses the failed response rather than
// retrying. The flip is still worth doing, because it is what hands the sheet
// to the stylesheet machinery at all; it just must not be described as a retry.
// No inline handlers, no globals.
//
// `as="style"` is deliberately LEFT on the link after the flip. It is inert on
// a rel="stylesheet" link, and removing it is only ever a risk: strip it before
// (or instead of) the rel flip and the preload no longer matches its consumer,
// so the browser downloads the sheet a second time. Nothing here should ever
// delete it -- the server-side un-transform (RestoreDeferredLink) is the only
// place `as` comes off, and only together with rel going back to "stylesheet".
//
// THIS BODY IS THE CACHE-BUSTING INPUT: kAsyncCssLoaderPath below embeds an
// FNV-1a hash of these exact bytes. Edit the loader -> the hash changes -> a
// new path is emitted by every front-end -> stale year-long caches at the old
// path are bypassed correct-by-construction. Nothing hardcodes the hashed path:
// the C++/C#/nginx paths all read kAsyncCssLoaderPath and the Python harnesses
// (tools/common/async_css_loader_path.py) recompute the digest from THIS
// initializer -- keep it a run of adjacent string literals ending in `";` or
// that extractor stops matching.
inline constexpr std::string_view kAsyncCssLoaderJs =
    "(function(){"
    "function s(l){if(l.rel!=='preload')return;var "
    "m=l.getAttribute('data-pagespeed-media');l.media=m||'all';"
    "l.rel='stylesheet';}"
    "var ls=document.querySelectorAll('link[data-pagespeed-async]');"
    "for(var i=0;i<ls.length;i++){(function(l){"
    "l.addEventListener('load',function(){s(l);},{once:true});"
    "l.addEventListener('error',function(){s(l);},{once:true});"
    "if(window.performance&&performance.getEntriesByName(l.href).length)s(l);"
    "})(ls[i]);}"
    "window.addEventListener('load',function(){"
    "for(var j=0;j<ls.length;j++)s(ls[j]);});"
    "})();";

namespace async_css_detail {

// FNV-1a 32-bit over the loader body, computed at compile time. 32 bits / 8 hex
// chars is deliberate: the only content ever served at this path is one ~350B
// loader that changes at most once per release, so this is a cache-buster on
// edit, not a collision-resistant digest. `unsigned char` masking is required
// because `char` is signed on x86/arm; without it the hash would diverge from a
// reference computation and differ across platforms.
inline constexpr std::uint32_t Fnv1a32(std::string_view s) {
  std::uint32_t h = 0x811c9dc5u;  // FNV offset basis
  for (char c : s) {
    h ^= static_cast<std::uint32_t>(static_cast<unsigned char>(c));
    h *= 0x01000193u;  // FNV prime; wraps mod 2^32 by unsigned overflow
  }
  return h;
}

// Build "/pagespeed_static/async_css.<8 lowercase hex>.js\0" into a fixed
// std::array at compile time. The trailing '\0' makes kAsyncCssLoaderPath.data()
// a valid C string for ps_async_css_loader_path() (which returns .data() and is
// consumed by EXPECT_STREQ / P/Invoke as a NUL-terminated string).
inline constexpr std::string_view kPathPrefix = "/pagespeed_static/async_css.";
inline constexpr std::string_view kPathSuffix = ".js";
inline constexpr std::size_t kHexLen = 8;

// prefix + 8 hex + suffix + NUL
inline constexpr std::size_t kPathBufLen =
    kPathPrefix.size() + kHexLen + kPathSuffix.size() + 1;

inline constexpr std::array<char, kPathBufLen> MakePathBuf() {
  std::array<char, kPathBufLen> buf{};  // value-initialized -> all '\0'
  std::size_t i = 0;
  for (char c : kPathPrefix) buf[i++] = c;

  // Render the 32-bit hash MSB-nibble-first so the filename is stable and
  // grep-friendly (matches a host-side `printf "%08x"`).
  const std::uint32_t h = Fnv1a32(kAsyncCssLoaderJs);
  constexpr char kHexDigits[] = "0123456789abcdef";
  for (std::size_t n = 0; n < kHexLen; ++n) {
    const unsigned shift = static_cast<unsigned>((kHexLen - 1 - n) * 4);
    buf[i++] = kHexDigits[(h >> shift) & 0xFu];
  }

  for (char c : kPathSuffix) buf[i++] = c;
  buf[i] = '\0';  // explicit terminator (kPathBufLen-1)
  return buf;
}

// Static storage with static lifetime: kAsyncCssLoaderPath.data() outlives any
// caller and stays valid for the C ABI.
inline constexpr std::array<char, kPathBufLen> kPathBuf = MakePathBuf();

}  // namespace async_css_detail

// Same-origin, CONTENT-ADDRESSED path at which the async-CSS loader script is
// served: "/pagespeed_static/async_css.<fnv1a32(loader)>.js". The hash is over
// kAsyncCssLoaderJs, so any change to the loader body yields a new path and a
// future bugfix is never masked by a stale immutable cache -- which lets the
// front-ends serve it as `Cache-Control: public, max-age=31536000, immutable`
// (and lets prod's generic `location ~* \.(js)$ { expires 1y; }` be correct).
//
// The HtmlTransformFilter injects <script defer src="{path}"> once per document
// when it defers any stylesheet; the front-end (nginx module / in-process .NET
// middleware) serves kAsyncCssLoaderJs at this exact path. All three derive
// from this constant (the filter's AddAttribute, the nginx exact-match, and the
// C getter ps_async_css_loader_path()), so they track the hash automatically.
//
// CSP rationale: an external, same-origin <script src> runs under a host-source
// policy (`script-src 'self'`), which an inline `onload=` handler cannot (that
// needs `'unsafe-inline'`/`'unsafe-hashes'`). So this loader is no worse than,
// and usually better than, the inline-onload swap. Caveat: under a nonce +
// `strict-dynamic` (or hash/nonce-only) CSP, host allowlists including 'self'
// are ignored, so this external loader is blocked just as inline handlers are;
// deferred sheets stay at rel="preload" (they download, but nothing consumes
// them) and the <noscript> copy does not help JS-enabled clients. Same
// never-applies outcome as before, reached by a different route: the sheet is
// now fetched-but-unused rather than fetched-as-print. Sites on such policies
// should disable this (--no-async-css).
//
// The string_view spans the buffer EXCLUDING the trailing '\0', but the byte at
// .data()[size()] is that '\0', so .data() is a valid C string.
inline constexpr std::string_view kAsyncCssLoaderPath{
    async_css_detail::kPathBuf.data(), async_css_detail::kPathBufLen - 1};

// Lock the C ABI contract: kAsyncCssLoaderPath.data() must be NUL-terminated
// (ps_async_css_loader_path() returns it for EXPECT_STREQ / Marshal.PtrToStringUTF8).
static_assert(
    kAsyncCssLoaderPath.data()[kAsyncCssLoaderPath.size()] == '\0',
    "kAsyncCssLoaderPath must be NUL-terminated for the C ABI getter");
// Sanity: the path keeps its reserved shape after content-addressing.
static_assert(kAsyncCssLoaderPath.starts_with("/pagespeed_static/async_css."));
static_assert(kAsyncCssLoaderPath.ends_with(".js"));

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_WORKER_ASYNC_CSS_LOADER_H_
