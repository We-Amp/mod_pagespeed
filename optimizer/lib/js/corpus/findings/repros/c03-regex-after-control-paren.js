// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Regex literals right after the closing paren of a control-flow header,
// where the paren closes a condition rather than an operand, next to real
// division in the same expressions.
var c = 1;
var s = 'x';
if (c) /re/.test(s);
while (0) /re/.test(s);
for (; /a/.test(s);) { break; }
label: /a/.test(s);
var paren = (1, /re/);
var choose = c ? /a/ : /b/;
var division = (4) / 2 / 1;
var mixed = /a/.test(s) ? 4 / 2 : 2 / 4;
