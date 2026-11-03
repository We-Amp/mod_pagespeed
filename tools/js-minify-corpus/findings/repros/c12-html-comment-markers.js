// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Annex-B HTML comment markers: `<!--` opens a line comment anywhere,
// `-->` opens one only at the start of a line.
<!-- open with an HTML comment marker
var a = 1;
<!-- a second marker, again at line start
var b = 2;
var c = a + b; <!-- a marker after code on the same line
var d = c * 2;
--> close marker at the start of a line
var e = d + 1;
