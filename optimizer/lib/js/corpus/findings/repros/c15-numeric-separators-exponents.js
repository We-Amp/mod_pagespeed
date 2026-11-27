// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Numeric separators, exponents, BigInt literals and member access on
// number literals, next to real division.
var million = 1_000_000;
var tiny = 0.000_001;
var exp = 1_0e1_0;
var neg = 1.5e-3_5;
var upper = 2E-2;
var hex = 0xFF_FF;
var bin = 0b1010_1010;
var oct = 0o7_7_7;
var big = 9007199254740993n;
var bigSep = 1_000_000n;
var dotEnd = 5.;
var twoDots = 5..toFixed(1);
var spaced = 5 .toString(2);
var leading = .5e+2;
var ratio = million / 1_0 / 5 + exp / 2;
