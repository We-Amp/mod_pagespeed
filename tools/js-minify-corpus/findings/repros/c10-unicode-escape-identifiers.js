// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Unicode \uXXXX escapes inside identifiers, property names and method
// names -- including escapes that spell out keywords, which then name
// ordinary properties instead of accessors. The last two lines must stay
// apart: joining them would read as one identifier.
var caf\u00e9 = 1;
var \u0061 = 2;
var a\u0031 = 3;
var total = caf\u00e9 + \u0061 + a\u0031;
var o = { p\u0072op: 5, 'k\u00e9y': 6 };
var m = { \u0067et() { return 1; }, \u0073et(v) { return v; } };
function \u0066n(x) { return x; }
var c = \u0061
\u0062;
