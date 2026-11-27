// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// A line break in front of `-->` keeps it a close comment; joining the
// lines would turn the marker into a decrement followed by a comparison.
var i = 5;
var j = 1;
var k = i
--> j is discarded with the rest of the line;
var after = i + j;
