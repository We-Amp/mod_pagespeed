// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Optional chaining and nullish coalescing, with the `?` `.5` boundary that
// is a conditional over a decimal, not an optional chain.
var cfg = { a: { b: [{ c: 1 }] } };
var v1 = cfg?.a?.b?.[0]?.c;
var v2 = cfg?.a?.missing?.();
var v3 = cfg?.['a']?.b;
var v4 = cfg?.x?.y ?? 0;
var call = cfg?.a?.b?.slice?.(0, 1);
var ternary = v1 ? .5 : .25;
var other = v3 ? .75e1 : 1e-1;
var flags = { on: 0 };
flags.on ??= 5;
flags.on ||= 2;
flags.on &&= 3;
var chained = cfg?.a?.b?.[0]?.c?.toString?.();
