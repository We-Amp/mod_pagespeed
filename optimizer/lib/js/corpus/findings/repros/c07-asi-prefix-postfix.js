// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Increment and decrement split across lines: `i` newline `++` newline `j`
// is `i; ++j;`, joining them produces `i++j`.
var i = 1;
var j = 2;
i
++
j;
var a = 3;
var b = 4;
a
--
b;
var k = i + j + a + b;
