// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Defensive leading semicolons in concatenated bundles, ahead of a bracket,
// a paren, an arrow and a template literal.
var arr = [1, 2];
var mod = {};
(function (m) { m.a = 1; })(mod);
;[1, 2].forEach(function (v) { arr.push(v); });
;(() => { arr.push(3); })();
;`t`.length;
;({ b: 2 }).b;
