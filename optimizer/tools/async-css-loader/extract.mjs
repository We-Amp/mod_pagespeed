// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Extract the async-CSS loader body out of the C++ header that defines it.
//
// TWIN of tools/common/async_css_loader_path.py — same header, same two-stage
// regex, same loud failure when the parse comes up empty. The Python twin
// exists so the pytest harnesses can recompute the SERVED PATH; this one exists
// so the loader's BEHAVIOUR can be executed. They must extract the same bytes,
// and `loader.test.mjs` asserts exactly that by running the Python twin and
// comparing byte-for-byte — so "same way" is verified, not merely intended.
//
// Why extract at all instead of keeping a JS copy: a copy would drift silently
// on the very edit that most needs testing. Reading src/worker/async_css_loader.h
// means the test always runs the bytes the compiler hashes and the front-ends
// serve.

import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const REPO_ROOT = join(dirname(fileURLToPath(import.meta.url)), '..', '..');

export const ASYNC_CSS_LOADER_HEADER = join(
  REPO_ROOT,
  'src',
  'worker',
  'async_css_loader.h',
);

/** The exact loader bytes the C++ compiler hashes. */
export function asyncCssLoaderJs(headerPath = ASYNC_CSS_LOADER_HEADER) {
  const text = readFileSync(headerPath, 'utf8');
  // Capture the whole `kAsyncCssLoaderJs = "..." "..." ...;` initializer up to
  // the final closing quote before the statement `;` — the body itself contains
  // `;`, so we must not stop at the first one. `[\s\S]` is the JS spelling of
  // Python's re.S.
  const init = /kAsyncCssLoaderJs\s*=\s*([\s\S]*?")\s*;/.exec(text);
  if (!init) {
    throw new Error(`kAsyncCssLoaderJs not found in ${headerPath}`);
  }
  const chunks = [...init[1].matchAll(/"((?:[^"\\]|\\.)*)"/g)].map((m) => m[1]);
  const body = chunks.join('');
  // Fail LOUD if the parse extracted nothing (e.g. a future loader edit defeats
  // the extraction). Silently testing the empty string would be a green suite
  // that proves nothing at all — the failure mode this whole file exists for.
  if (!body) {
    throw new Error(
      `Failed to extract kAsyncCssLoaderJs from ${headerPath} (empty parse) ` +
        '— the loader source/format may have changed.',
    );
  }
  // The loader body is pure ASCII with no C escape sequences today; decode any
  // that ever appear so the bytes match what the C++ compiler hashes. Mirrors
  // the Python twin's latin-1 / unicode_escape round-trip.
  return body.replace(/\\(["'\\nrt0])/g, (_, c) => {
    switch (c) {
      case 'n':
        return '\n';
      case 'r':
        return '\r';
      case 't':
        return '\t';
      case '0':
        return '\0';
      default:
        return c;
    }
  });
}

/** FNV-1a 32, byte-wise — the digest the header computes at compile time. */
export function fnv1a32(str) {
  let h = 0x811c9dc5; // FNV offset basis
  for (let i = 0; i < str.length; i++) {
    h ^= str.charCodeAt(i) & 0xff;
    // Math.imul keeps the multiply in 32-bit two's complement; a plain `*`
    // would lose precision past 2^53 and diverge from the C++ result.
    h = Math.imul(h, 0x01000193) >>> 0; // FNV prime, mod 2^32
  }
  return h >>> 0;
}

/** Content-addressed loader path, recomputed from the worker header. */
export function asyncCssLoaderPathFromSource(
  headerPath = ASYNC_CSS_LOADER_HEADER,
) {
  const hex = fnv1a32(asyncCssLoaderJs(headerPath)).toString(16).padStart(8, '0');
  return `/pagespeed_static/async_css.${hex}.js`;
}
