// SPDX-License-Identifier: Apache-2.0
// Hand-written fixture in the style of esbuild bundler output (not vendored
// from any third party).  Exercises the ES2017-ES2020 constructs that the
// tokenizer supports natively: optional chaining, nullish coalescing, logical
// assignment, optional catch binding, destructuring declarations, dynamic
// import, import.meta, import/export statements, numeric separators, BigInt,
// binary/octal literals, exponentiation, and template literals.
"use strict";
import { helper } from "./helper.js";
var __defaults = { retries: 3, timeout: 1_000, mask: 0b1010, mode: 0o17 };
var state = { big: 123n, factor: 2 ** 8 };
var fetchImpl = globalThis.fetch ?? null;
var config = window.APP_CONFIG ?? __defaults;
config.timeout ??= 5_000;
state.ready &&= config != null;
state.lazy ||= false;
const { retries, timeout: waitMs = 250, ...restCfg } = config;
const [first = 0, second] = config.order ?? [1, 2];
let { mode } = __defaults;
var url = import.meta.url;
var version = config?.meta?.version ?? "0.0.0";
var handler = config.handlers?.[mode];
var result = handler?.(state) ?? null;
function loadChunk(name) {
  return import("./chunk-" + name + ".js").then((mod) => {
    return mod.default ?? mod;
  });
}
function parseAll(items) {
  var out = [];
  for (const { id, value = null } of items) {
    out.push(`${id}=${value ?? ""}`);
  }
  return out;
}
function safeParse(text) {
  try {
    return JSON.parse(text) ?? {};
  } catch {
    return {};
  }
}
var let_like = { let: 1, class: 2 };
var picked = let_like.let + let_like.class;
var pattern = /[0-9]+/;
var ratio = timeout / (retries ?? 1) / 2;
var m = ("" + version).match(pattern) ?? [];
export { loadChunk, parseAll, safeParse, picked, restCfg, first, second, m, ratio, result, url, waitMs, helper, fetchImpl };
