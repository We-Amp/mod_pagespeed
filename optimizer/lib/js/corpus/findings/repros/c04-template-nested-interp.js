// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Nested template substitutions three levels deep, with braces, backticks
// and a regex inside the inner ones.
const who = 'ada';
const tags = ['x', 'y'];
const line = `${who}: ${tags.map((t) => `[\`${t}\`]`).join('')}`;
const deep = `a${`b${`c${who}d`}e`}f`;
const braces = `ok ${{ p: '{' }.p}}}}`;
const withRegex = `${/\//.source}`;
const adjacent = `${'a'}${`b${'c'}`}`;
