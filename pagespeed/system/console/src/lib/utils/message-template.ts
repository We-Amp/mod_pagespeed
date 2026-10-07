// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Message templates: the module's rules (pagespeed/system/admin_site.cc,
 * MessageLineBody / MessageLineTimeMs / MessageTemplate) implemented again,
 * character class for character class. Both implementations are tested
 * against the module's test/pagespeed/system/testdata/message_templates.tsv;
 * change the rules in both places and in that file together.
 *
 * Only ASCII characters are classified (digits, letters, the URL
 * terminators); every other UTF-16 unit is copied as it is, so the output is
 * the module's byte-level output once encoded as UTF-8. The one deliberate
 * difference: a header time is read with Date.parse here and with the
 * module's own parser there, so a time format other than the module's
 * "Fri, 02 Oct 2026 10:25:51 GMT" may be read differently (the template is
 * unaffected).
 */

const LEVELS: ReadonlySet<string> = new Set(["Info", "Warning", "Error", "Fatal"]);

const isDigit = (c: string): boolean => c >= "0" && c <= "9";
const isLetter = (c: string): boolean => (c >= "a" && c <= "z") || (c >= "A" && c <= "Z");
const isAlnum = (c: string): boolean => isDigit(c) || isLetter(c);
const isHex = (c: string): boolean => isDigit(c) || (c >= "a" && c <= "f") || (c >= "A" && c <= "F");
const endsUrl = (c: string): boolean =>
  c === " " || c === "\t" || c === "\r" || c === "\n" || c === '"' || c === "'" || c === "<" || c === ">" || c === "`";

function allDigits(s: string): boolean {
  if (s.length === 0) return false;
  for (let i = 0; i < s.length; i++) if (!isDigit(s[i])) return false;
  return true;
}

/** "0x1f" style, or at least 8 hex digits mixing digits and letters. */
function isHexId(word: string): boolean {
  if (word.length >= 3 && word[0] === "0" && (word[1] === "x" || word[1] === "X")) {
    let allHex = true;
    for (let i = 2; i < word.length; i++) {
      if (!isHex(word[i])) {
        allHex = false;
        break;
      }
    }
    if (allHex) return true;
  }
  if (word.length < 8) return false;
  let digit = false;
  let letter = false;
  for (let i = 0; i < word.length; i++) {
    const c = word[i];
    if (!isHex(c)) return false;
    if (isDigit(c)) digit = true;
    else letter = true;
  }
  return digit && letter;
}

/** A leading "[<text without ']'>] ": its text and what follows. */
function takeBracketGroup(rest: string): { inside: string; rest: string } | null {
  if (rest[0] !== "[") return null;
  const close = rest.indexOf("]");
  if (close === -1 || close + 1 >= rest.length || rest[close + 1] !== " ") return null;
  return { inside: rest.slice(1, close), rest: rest.slice(close + 2) };
}

/** "<file>:<line>": split at the last colon, a non-empty file without spaces, a line of digits. */
function isFileAndLine(s: string): boolean {
  const colon = s.lastIndexOf(":");
  if (colon <= 0) return false;
  if (s.slice(0, colon).includes(" ")) return false;
  return allDigits(s.slice(colon + 1));
}

/** "[<time>] [<Level>] " at the start of a line: the time text and the rest. */
function takeHeader(line: string): { time: string; rest: string } | null {
  const time = takeBracketGroup(line);
  if (time === null) return null;
  const level = takeBracketGroup(time.rest);
  if (level === null || !LEVELS.has(level.inside)) return null;
  return { time: time.inside, rest: level.rest };
}

/**
 * The message text of a buffered line without its "[time] [Level] [pid] "
 * (and "[file:line] ") header. A line without a header — a continuation
 * line of a multi-line message — comes back unchanged, so a line has a
 * header exactly when the result is shorter.
 */
export function messageLineBody(line: string): string {
  const header = takeHeader(line);
  if (header === null) return line;
  let rest = header.rest;
  const pid = takeBracketGroup(rest);
  if (pid !== null && allDigits(pid.inside)) {
    rest = pid.rest;
    const where = takeBracketGroup(rest);
    if (where !== null && isFileAndLine(where.inside)) rest = where.rest;
  }
  return rest;
}

/** The header's time in epoch ms; null without a header or with an unreadable time. */
export function messageLineTimeMs(line: string): number | null {
  const header = takeHeader(line);
  if (header === null) return null;
  const ms = Date.parse(header.time);
  return Number.isFinite(ms) ? ms : null;
}

const SCHEME = /^https?:\/\//i;

/** A message's template: URLs become "URL", hex ids "ID", digit runs "N". */
export function messageTemplate(body: string): string {
  let out = "";
  let i = 0;
  const n = body.length;
  while (i < n) {
    const c = body[i];
    if ((c === "h" || c === "H") && SCHEME.test(body.slice(i, i + 8))) {
      let j = i;
      while (j < n && !endsUrl(body[j])) j++;
      out += "URL";
      i = j;
    } else if (isAlnum(c)) {
      let j = i;
      while (j < n && isAlnum(body[j])) j++;
      const word = body.slice(i, j);
      out += isHexId(word) ? "ID" : word.replace(/[0-9]+/g, "N");
      i = j;
    } else {
      out += c;
      i++;
    }
  }
  // Trailing spaces, tabs and CRs go -- by a loop, not an end-anchored
  // regular expression, which backtracks quadratically on a long run of
  // spaces that is not at the end (message text is visitor-controlled).
  let end = out.length;
  while (end > 0 && (out[end - 1] === " " || out[end - 1] === "\t" || out[end - 1] === "\r")) end--;
  return out.slice(0, end);
}
