// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Subresource extraction for the /examples/ gallery generator (generate.mjs).
//
// Finds the URLs a page asks the browser to fetch from <link href>,
// <script src> and <img src>, and returns them as absolute URLs.
//
// Attribute values are read the way an HTML tokenizer reads them: double-quoted,
// single-quoted or UNQUOTED. The remove_quotes filter emits unquoted values
// (`<img src=images/a.png>`), so a quote-only pattern silently
// drops those resources and the "after" side of an example looks like it saved
// a request and the resource's bytes.
//
// This is a small tokenizer, not a full HTML parser. It skips the contents of
// <script>/<style> blocks, matches attribute names exactly (so `data-src` is
// not `src`), and uses the first occurrence of a repeated attribute, as
// browsers do. Tags inside comments are counted, so an IE
// conditional-comment stylesheet is measured on both sides of an example.

const WANTED = { link: "href", script: "src", img: "src" };
const RAW_TEXT = new Set(["script", "style"]);
const isSpace = (c) =>
  c === " " || c === "\t" || c === "\n" || c === "\r" || c === "\f";

// <link rel=preconnect|dns-prefetch> opens a connection to an origin; its href
// is not a resource the browser downloads, so it is not measured.
const CONNECTION_HINTS = new Set(["preconnect", "dns-prefetch"]);
const isConnectionHint = (attrs) =>
  (attrs.get("rel") || "")
    .toLowerCase()
    .split(/\s+/)
    .some((r) => CONNECTION_HINTS.has(r));

// Parse the attributes of one start tag, starting just after its name.
// Returns { attrs: Map(lowercased name -> value), end: index after '>' }.
export function parseAttributes(html, start) {
  const attrs = new Map();
  const n = html.length;
  let i = start;
  while (i < n) {
    while (i < n && (isSpace(html[i]) || html[i] === "/")) i++;
    if (i >= n) break;
    if (html[i] === ">") return { attrs, end: i + 1 };
    // Attribute name: up to whitespace, '/', '>' or '=' (a leading '=' is
    // part of the name, per the HTML tokenizer).
    let nameStart = i;
    if (html[i] === "=") i++;
    while (
      i < n &&
      !isSpace(html[i]) &&
      html[i] !== "/" &&
      html[i] !== ">" &&
      html[i] !== "="
    )
      i++;
    const name = html.slice(nameStart, i).toLowerCase();
    while (i < n && isSpace(html[i])) i++;
    let value = "";
    if (html[i] === "=") {
      i++;
      while (i < n && isSpace(html[i])) i++;
      const q = html[i];
      if (q === '"' || q === "'") {
        const close = html.indexOf(q, i + 1);
        const stop = close === -1 ? n : close;
        value = html.slice(i + 1, stop);
        i = close === -1 ? n : close + 1;
      } else {
        const vStart = i;
        while (i < n && !isSpace(html[i]) && html[i] !== ">") i++;
        value = html.slice(vStart, i);
      }
    }
    if (name && !attrs.has(name)) attrs.set(name, value);
  }
  return { attrs, end: n };
}

// Return the raw (unresolved) subresource references in document order.
export function extractResourceRefs(html) {
  const refs = [];
  const n = html.length;
  let i = 0;
  while (i < n) {
    const lt = html.indexOf("<", i);
    if (lt === -1) break;
    const m = /^<([A-Za-z][A-Za-z0-9-]*)/.exec(html.slice(lt, lt + 64));
    if (!m) {
      i = lt + 1;
      continue;
    }
    const tag = m[1].toLowerCase();
    const { attrs, end } = parseAttributes(html, lt + m[0].length);
    const want = WANTED[tag];
    if (want && attrs.has(want) && !(tag === "link" && isConnectionHint(attrs)))
      refs.push(attrs.get(want).trim());
    i = end;
    if (RAW_TEXT.has(tag)) {
      const close = html.toLowerCase().indexOf(`</${tag}`, i);
      i = close === -1 ? n : close;
    }
  }
  return refs;
}

// Subresource URLs (stylesheets, scripts, images) as unique absolute URLs.
export function extractResources(html, pageUrl) {
  const urls = new Set();
  for (const raw of extractResourceRefs(html)) {
    if (!raw) continue;
    let url;
    try {
      url = new URL(raw, pageUrl);
    } catch {
      continue; // unparseable
    }
    // Only fetchable resources: this drops data:, javascript:, vbscript: and
    // every other non-network scheme.
    if (url.protocol === "http:" || url.protocol === "https:")
      urls.add(url.href);
  }
  return [...urls];
}
