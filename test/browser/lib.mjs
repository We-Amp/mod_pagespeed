// Shared helpers for the browser-based client-JS integration tests.

import { readFileSync } from 'node:fs';

// Extract the compiled JS string from a generated data2c .cc file: the
// checked-in net/instaweb/rewriter/generated/*.cc files embed the shipped
// asset as concatenated C string literals.
export function loadCompiledAsset(ccPath) {
  const cc = readFileSync(ccPath, 'utf8');
  const start = cc.indexOf('=');
  const end = cc.lastIndexOf('";');
  if (start === -1 || end === -1 || end < start) {
    throw new Error(`unrecognized data2c format in ${ccPath}`);
  }
  const literals = cc.slice(start, end + 1).match(/"(?:[^"\\]|\\.)*"/g);
  if (!literals || literals.length === 0) {
    throw new Error(`no string literals found in ${ccPath}`);
  }
  const escaped = literals.map((l) => l.slice(1, -1)).join('');
  return escaped.replace(/\\(x[0-9a-fA-F]{2}|.)/g, (m, e) => {
    if (e[0] === 'x') return String.fromCharCode(parseInt(e.slice(1), 16));
    return { n: '\n', t: '\t', r: '\r' }[e] ?? e;
  });
}
