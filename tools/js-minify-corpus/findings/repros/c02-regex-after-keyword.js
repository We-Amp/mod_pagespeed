// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Regex literals in operand position of keywords rather than after a
// completed operand, where a slash would read as division.
function kind(v) {
  if (typeof v === 'string') return /^_/.test(v);
  return null;
}
function walk(xs) {
  for (const x of xs) void /x/.exec(x);
}
function pick(c) {
  switch (c) {
    case 'a': /a/.test('a'); break;
    default: do /b/.test('b'); while (false);
  }
}
var table = { '/': 1 };
delete table[/\//];
var isRe = /x/ instanceof RegExp;
var built = new RegExp(/x/.source);
var negated = !/a/.test('a') && /b/.test('b');
