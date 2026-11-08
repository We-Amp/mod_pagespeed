// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Labelled loops and blocks: continue and break to an outer label, a
// labelled block, and a labelled empty statement.
outer: for (var i = 0; i < 3; i++) {
  inner: for (var j = 0; j < 3; j++) {
    if (j === 1) continue outer;
    if (i === 2) break outer;
  }
}
scan: for (const item of [1, 2, 3]) {
  for (const key of ['a', 'b']) {
    if (key === 'a') continue scan;
    break scan;
  }
}
row: for (const x of [1]) row2: for (const y of [2]) break row;
guard: {
  if (i) break guard;
  i = 0;
}
empty: ;
var done = i + item + x + y;
