// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Message text split into plain text and links, for the Logs page.
 * Message text carries visitor-controlled bytes, so this is the one place it
 * becomes an element: a run that starts with http:// or https:// at a word
 * boundary, ends at whitespace, a quote, an angle bracket or a backtick,
 * loses trailing punctuation, AND parses with the URL parser to the http:
 * or https: protocol becomes a link whose href is the PARSED URL and whose
 * text is the original run. Nothing else is ever a link, and the caller
 * renders every segment as a text node (never as markup).
 */

export type LinkSegment = { kind: "text"; text: string } | { kind: "link"; text: string; href: string };

const URL_RUN = /https?:\/\/[^\s"'<>`]+/gi;
// Trailing punctuation is trimmed by a loop, not an end-anchored regular
// expression: `[.,)]+$` backtracks quadratically on a long run of them that
// is not at the end of the URL, and the text is visitor-controlled.
const TRAILING: ReadonlySet<string> = new Set([".", ",", ";", ":", "!", "?", ")", "]"]);
const WORD_CHAR = /[A-Za-z0-9]/;

export function linkSegments(text: string): LinkSegment[] {
  const out: LinkSegment[] = [];
  let last = 0;
  for (const match of text.matchAll(URL_RUN)) {
    const start = match.index ?? 0;
    if (start > 0 && WORD_CHAR.test(text[start - 1])) continue;
    let end = match[0].length;
    while (end > 0 && TRAILING.has(match[0][end - 1])) end--;
    const candidate = match[0].slice(0, end);
    let href: string | null = null;
    try {
      const url = new URL(candidate);
      if (url.protocol === "http:" || url.protocol === "https:") href = url.href;
    } catch {
      href = null;
    }
    if (href === null) continue;
    if (start > last) out.push({ kind: "text", text: text.slice(last, start) });
    out.push({ kind: "link", text: candidate, href });
    last = start + candidate.length;
  }
  if (last < text.length) out.push({ kind: "text", text: text.slice(last) });
  return out;
}
