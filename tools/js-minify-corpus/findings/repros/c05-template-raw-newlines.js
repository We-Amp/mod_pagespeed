// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Template literals carrying real line breaks and tabs, a tagged template,
// String.raw and an escaped (non-substituting) ${.
const poem = `roses are red
violets are blue`;
const raw = String.raw`c:\temp\${dir}\x`;
function count(strings, ...values) {
  return strings.length + values.length;
}
const n = count`a${1}b${2}c`;
const block = `first
	indented	${poem.length}
last`;
