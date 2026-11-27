// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// A module that declares with `let` and a binding pattern directly in a
// `for` header. The minifier must treat `let` there as a declaration, as it
// does for `const`.
import { load as loadRows } from './rows.js';

export function sumPairs(xs) {
  let total = 0;
  for (let { a, b } of xs) {
    total += a * b;
  }
  return total;
}

export function swapAll(pairs) {
  const out = [];
  for (let [x, y] of pairs) {
    out.push([y, x]);
  }
  return out;
}

export function keysOf(obj) {
  const keys = [];
  for (let { k } in obj) {
    keys.push(k);
  }
  return keys;
}

export function nested(items) {
  let seen = 0;
  for (let { id, meta: { tags = [], weight = 1 } = {} } of items) {
    seen += tags.length * weight + id;
  }
  for (let [first = 0, { second = 1 } = {}] of loadRows()) {
    seen += first + second;
  }
  return seen;
}

export default nested;
