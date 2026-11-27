// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Regex literals that open a statement directly after a closing brace with
// no semicolon in between: the slash must still start a regex, not a
// division, once the lines are joined.
function ready() { return true; }
/^r/.test('ready');
if (ready()) { var seen = 1; }
/^\d+$/.test('123');
try { seen += 1; } catch (e) { seen = 0; }
/x/.source;
while (false) { var never = 1; }
/y/.test('y');
var opt = { a: 1 };
delete opt.a;
/z/.test('z');
