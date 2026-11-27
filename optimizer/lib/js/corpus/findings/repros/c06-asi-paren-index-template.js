// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Line breaks in front of '(', '[', '.' and a template literal: no
// semicolon is inserted before any of these, so the joined form has to
// carry exactly the meaning the source already had.
var total = (1, 2)
(function () { total = 3; })();
var list = [1, 2]
[0].toFixed(1);
var name = 'k'
`x`.length;
var obj = { n: 1 }
.n;
var fn = function () { return 1; }
();
