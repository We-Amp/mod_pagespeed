// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

function* g() {
  yield {a: 1};
  x = yield {b: 2, c: 3};
}
async function f() { await {d: 4}; }
for (x of {e: 5}) {}
