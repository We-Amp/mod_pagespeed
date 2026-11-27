// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// `return` is a restricted production: the line break after it ends the
// statement, so every one of these returns undefined.
function none() {
  return
  { ok: true };
}
function later(x) {
  return
  (x + 1);
}
function re(x) {
  return
  /x/.test(x);
}
function chain(x) {
  return (
    x +
    1
  );
}
