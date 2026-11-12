// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The configuration text the server sends (RewriteOptions::OptionsToString):
//
//   Version: 15: on
//
//   Filters
//   ah<TAB>Add Head
//   ...
//
//   Options
//     EnableRewriting (e)   1
//     ...
//
//   Domain Lawyer
//   ...
//
//   Invalidation Timestamp: (none)
//
// parseConfigText() reads it into the enabled filters (a list), the options
// that were set (name -> value) and blocks a page can lay out, and always
// keeps the raw lines. Text of another shape is not guessed at: `structured`
// is false and a page shows the lines as they are.

/** One enabled filter: its short id and its name. */
export interface ConfigFilter {
  id: string;
  name: string;
  /** Index into ParsedConfig.lines. */
  line: number;
}

/** One option that was set: name, short id and value as printed. */
export interface ConfigOption {
  name: string;
  id: string;
  value: string;
  /** Index into ParsedConfig.lines. */
  line: number;
}

/** One line of a block shown as text. */
export interface ConfigTextLine {
  text: string;
  /** Index into ParsedConfig.lines. */
  line: number;
}

/** A run of lines between blank lines, as a page lays it out. */
export type ConfigBlock =
  | { kind: "filters"; title: string; line: number; filters: ConfigFilter[] }
  | { kind: "options"; title: string; line: number; options: ConfigOption[] }
  /** `title` is set for a known section heading ("Domain Lawyer"), null otherwise. */
  | { kind: "text"; title: string | null; line: number | null; lines: ConfigTextLine[] };

export interface ParsedConfig {
  /** The text split into lines, unchanged (no trailing "\r"). */
  lines: string[];
  /** True when the text has both a Filters and an Options section and every line in them parsed. */
  structured: boolean;
  /** "15" from "Version: 15: on", or null. */
  version: string | null;
  /** "on", "off", "unplugged" or "standby" from the version line, or null. */
  state: string | null;
  /** The enabled filters, in the order printed. */
  filters: ConfigFilter[];
  /** The options that were set, by name ("EnableRewriting" -> "1"). */
  options: Record<string, string>;
  /** The same options in the order printed, with their ids. */
  optionList: ConfigOption[];
  /** The text in blocks; empty when nothing but blank lines. */
  blocks: ConfigBlock[];
}

/** Section headings the server prints over a block of text. */
const TEXT_HEADINGS = new Set([
  "Domain Lawyer",
  "URL cache invalidation entries",
  "Rejected request map",
  "Override caching wildcards",
]);

const VERSION_LINE = /^Version:\s*(\d+):\s*(\S+)\s*$/;
// "ah<TAB>Add Head": an id, a tab, a name (a name may end in a space).
const FILTER_LINE = /^([^\s]+)\t(.*\S)\s*$/;
// "  EnableRewriting (e)   1": indented, a name, an id in parentheses, the value (may be empty).
const OPTION_LINE = /^\s+([A-Za-z][\w.-]*) \(([^()\s]+)\)(?:\s+(.*?))?\s*$/;

export function parseConfigText(text: string): ParsedConfig {
  const lines = text.split("\n").map((l) => l.replace(/\r$/, ""));
  const out: ParsedConfig = {
    lines,
    structured: false,
    version: null,
    state: null,
    filters: [],
    options: {},
    optionList: [],
    blocks: [],
  };
  let sawFilters = false;
  let sawOptions = false;
  let wellFormed = true;

  // Runs of non-blank lines.
  const runs: number[][] = [];
  let run: number[] = [];
  lines.forEach((line, i) => {
    if (line.trim() === "") {
      if (run.length > 0) runs.push(run);
      run = [];
    } else {
      run.push(i);
    }
  });
  if (run.length > 0) runs.push(run);

  for (const indexes of runs) {
    const head = lines[indexes[0]].trim();
    const rest = indexes.slice(1);
    if (head === "Filters") {
      sawFilters = true;
      const filters: ConfigFilter[] = [];
      for (const i of rest) {
        const m = FILTER_LINE.exec(lines[i]);
        if (m) filters.push({ id: m[1], name: m[2].trim(), line: i });
        else wellFormed = false;
      }
      out.filters.push(...filters);
      out.blocks.push({ kind: "filters", title: head, line: indexes[0], filters });
    } else if (head === "Options") {
      sawOptions = true;
      const options: ConfigOption[] = [];
      for (const i of rest) {
        const m = OPTION_LINE.exec(lines[i]);
        if (m) options.push({ name: m[1], id: m[2], value: m[3] ?? "", line: i });
        else wellFormed = false;
      }
      for (const o of options) out.options[o.name] = o.value;
      out.optionList.push(...options);
      out.blocks.push({ kind: "options", title: head, line: indexes[0], options });
    } else {
      const titled = TEXT_HEADINGS.has(head);
      const body = titled ? rest : indexes;
      out.blocks.push({
        kind: "text",
        title: titled ? head : null,
        line: titled ? indexes[0] : null,
        lines: body.map((i) => ({ text: lines[i], line: i })),
      });
      if (out.version === null) {
        for (const i of body) {
          const m = VERSION_LINE.exec(lines[i]);
          if (m) {
            out.version = m[1];
            out.state = m[2];
            break;
          }
        }
      }
    }
  }
  out.structured = sawFilters && sawOptions && wellFormed;
  return out;
}

/** The value of an option by name or by id ("EnableRewriting" or "e"), or undefined when it was not set. */
export function optionValue(parsed: ParsedConfig, nameOrId: string): string | undefined {
  if (Object.prototype.hasOwnProperty.call(parsed.options, nameOrId)) return parsed.options[nameOrId];
  return parsed.optionList.find((o) => o.id === nameOrId)?.value;
}

/** Whether a filter is enabled, by id or name ("rj" or "Recompress Jpeg"; names ignore case). */
export function filterEnabled(parsed: ParsedConfig, idOrName: string): boolean {
  const lower = idOrName.toLowerCase();
  return parsed.filters.some((f) => f.id === idOrName || f.name.toLowerCase() === lower);
}

/**
 * A key per line that ignores layout: the server pads option names to the
 * longest one, so the same option lines up differently in two texts. Blank
 * lines key to "".
 */
export function lineKeys(parsed: ParsedConfig): string[] {
  const keys = parsed.lines.map((l) => l.trim().replace(/\s+/g, " "));
  for (const f of parsed.filters) keys[f.line] = `filter ${f.id}`;
  for (const o of parsed.optionList) keys[o.line] = `option ${o.name}=${o.value}`;
  return keys;
}

/** The lines of `a` (indexes) a search can match that have no equal line in `b`. */
export function differingLines(a: ParsedConfig, b: ParsedConfig): Set<number> {
  const other = new Set(lineKeys(b));
  const keys = lineKeys(a);
  return new Set(searchableLines(a).filter((i) => !other.has(keys[i])));
}

/**
 * The lines a search can match: every non-blank line, except a section
 * heading of text read as sections. Text of another shape has no headings.
 */
export function searchableLines(parsed: ParsedConfig): number[] {
  const headings = new Set<number>();
  if (parsed.structured) for (const b of parsed.blocks) if (b.line !== null) headings.add(b.line);
  const out: number[] = [];
  parsed.lines.forEach((l, i) => {
    if (l.trim() !== "" && !headings.has(i)) out.push(i);
  });
  return out;
}

/**
 * A line filter: every word of the query appears in the line, ignoring case
 * and spacing. An empty query matches everything.
 */
export function lineMatcher(query: string): (line: string) => boolean {
  const words = query.toLowerCase().split(/\s+/).filter((w) => w !== "");
  if (words.length === 0) return () => true;
  return (line) => {
    const text = line.toLowerCase().replace(/\s+/g, " ");
    return words.every((w) => text.includes(w));
  };
}

/** What the Configuration page shows of one text: the blocks with only the lines that pass, and the counts. */
export interface ConfigView {
  blocks: ConfigBlock[];
  /** Content lines shown. */
  shown: number;
  /** Content lines in the text. */
  total: number;
}

/**
 * The blocks of `parsed` keeping only the lines that match `query`. With no
 * query every block is kept as it is, an empty section included; otherwise a
 * block with no line left is dropped. Text of another shape is filtered line
 * by line as one block, so a line that did not parse can still match.
 */
export function configView(parsed: ParsedConfig, query: string): ConfigView {
  const searchable = searchableLines(parsed);
  const total = searchable.length;
  if (query.trim() === "") return { blocks: parsed.blocks, shown: total, total };
  const match = lineMatcher(query);
  const keep = (line: number) => match(parsed.lines[line]);
  if (!parsed.structured) {
    const lines = searchable.filter(keep).map((line) => ({ text: parsed.lines[line], line }));
    return { blocks: lines.length > 0 ? [{ kind: "text", title: null, line: null, lines }] : [], shown: lines.length, total };
  }
  const blocks: ConfigBlock[] = [];
  let shown = 0;
  for (const b of parsed.blocks) {
    if (b.kind === "filters") {
      const filters = b.filters.filter((f) => keep(f.line));
      shown += filters.length;
      if (filters.length > 0) blocks.push({ ...b, filters });
    } else if (b.kind === "options") {
      const options = b.options.filter((o) => keep(o.line));
      shown += options.length;
      if (options.length > 0) blocks.push({ ...b, options });
    } else {
      const lines = b.lines.filter((l) => keep(l.line));
      shown += lines.length;
      if (lines.length > 0) blocks.push({ ...b, lines });
    }
  }
  return { blocks, shown, total };
}
