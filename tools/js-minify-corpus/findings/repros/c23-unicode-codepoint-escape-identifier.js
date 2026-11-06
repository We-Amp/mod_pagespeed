// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// ES2015 code-point escapes in identifiers (an astral-plane name, a BMP
// name, and one in continuation position) next to the same escape inside
// a string literal.
var x = 1;
var smile = '\u{1F600}';
var wide = '\u{1D49C}';
var \u{1D49C}x = x + 1;
var \u{61}b = 2;
var a\u{1D49C} = 3;
var sum = \u{1D49C}x + \u{61}b + a\u{1D49C};
