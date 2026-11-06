// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Slashes that are division and slashes that open regex literals, mixed
// within single expressions and adjacent lines.
var a = 10 / 2 / 5;
var b = (4) / 2;
var c = a / b / 1;
var d = /a/.test('a') ? 1 / 2 : 2 / 4;
var e = [1, 2].map(function (x) { return x / 2; }).reduce((p, q) => p / q, 1);
var f = 6 / 2 * /z/.test('z') ? 1 : 0;
var g = a /= 2;
var h = (1, /x/);
var i = !/a/.test('a') || /b/.test('b') && 1 / 2;
