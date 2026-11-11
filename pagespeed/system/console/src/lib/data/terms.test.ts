// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// One name per thing. The console's copy lives in string tables and page
// templates; this reads all of it and fails on a second name for the
// optimizer, for either savings figure, for the served share or for the
// host filter, and on a documentation link that does not come from the one
// table of links.

import { readFileSync, readdirSync, statSync } from "node:fs";
import { dirname, join, relative } from "node:path";
import { fileURLToPath } from "node:url";
import { describe, expect, it } from "vitest";
import { ALERT_RULES } from "$lib/alerts";
import { routes } from "$lib/routes";
import { optimizerAbsenceText, type OptimizerAbsence } from "$lib/utils/daemon";
import { CURATED_CHARTS } from "$lib/utils/graphs-curated";
import { DOCS, DOCS_BASE, DOCS_URL, WEBSITE } from "./product-facts-console";
import { SPLIT_TEXT } from "./split-text";
import { TERMS } from "./terms";

const SRC = join(dirname(fileURLToPath(import.meta.url)), "..", "..");

function sourceFiles(dir: string): string[] {
  const out: string[] = [];
  for (const name of readdirSync(dir)) {
    const path = join(dir, name);
    if (statSync(path).isDirectory()) out.push(...sourceFiles(path));
    else if (/\.(svelte|ts|mjs)$/.test(name) && !/\.test\.ts$/.test(name) && !/\.d\.(ts|mts)$/.test(name)) out.push(path);
  }
  return out;
}

/**
 * The module's own log line, quoted so a finding can recognise it: the
 * module's words, not the console's.
 */
const QUOTED_LOG_LINE = /export const VOLUME_GENERATION_TEMPLATE =[\s\S]*?;\n/;

/** A file's copy: its text without comments, so only what can reach the page is read. */
function copyOf(path: string): string {
  return readFileSync(path, "utf8")
    .replace(QUOTED_LOG_LINE, "")
    .replace(/<!--[\s\S]*?-->/g, "")
    .replace(/\/\*[\s\S]*?\*\//g, "")
    .replace(/^\s*\/\/.*$/gm, "")
    .replace(/\s\/\/ .*$/gm, "");
}

const FILES = sourceFiles(SRC).map((path) => ({ name: relative(SRC, path), copy: copyOf(path) }));

/** The files whose copy matches, as "file: matched text". */
function uses(pattern: RegExp, skip: readonly string[] = []): string[] {
  const hits: string[] = [];
  for (const file of FILES) {
    if (skip.includes(file.name)) continue;
    const match = pattern.exec(file.copy);
    if (match !== null) hits.push(`${file.name}: ${match[0]}`);
  }
  return hits;
}

describe("the console's vocabulary", () => {
  it("reads every page and string table", () => {
    const names = FILES.map((f) => f.name);
    expect(names).toContain("pages/Overview.svelte");
    expect(names).toContain("lib/alerts.ts");
    expect(names).toContain("lib/data/stat-descriptions.ts");
    expect(names.length).toBeGreaterThan(60);
  });

  it("calls the optimizer by one name", () => {
    expect(uses(/optimizer worker/i)).toEqual([]);
    expect(uses(/\bthe worker\b|\bworker's\b/i)).toEqual([]);
    expect(uses(/\b(the|a|this) daemon\b/i)).toEqual([]);
    // The option names are the product's own and stay as they are.
    expect(uses(/DaemonVolumePath/)).not.toEqual([]);
  });

  it("names the module's savings figure one way", () => {
    expect(TERMS.savedByRewriting).toBe("Saved by rewriting");
    for (const retired of [/bytes saved by rewriting/i, /bytes saved by optimization/i, /module rewrite savings/i]) {
      expect(uses(retired), String(retired)).toEqual([]);
    }
    // Spelled out in the glossary only; a page imports it.
    expect(uses(/Saved by rewriting/)).toEqual([`lib/data/terms.ts: ${TERMS.savedByRewriting}`]);
  });

  it("names the optimizer's savings figure one way", () => {
    expect(TERMS.savedByOptimizerCache).toBe("Saved by the optimizer's cache");
    for (const retired of [
      /saved on served responses/i,
      /saved on optimizer cache serves/i,
      /saved on responses served/i,
      /optimizer cache savings/i,
      /optimizer serve savings/i,
      /\bServe savings\b/,
    ]) {
      expect(uses(retired), String(retired)).toEqual([]);
    }
    expect(uses(/Saved by the optimizer's cache/)).toEqual([`lib/data/terms.ts: ${TERMS.savedByOptimizerCache}`]);
  });

  it("keeps the two savings figures apart, and each chart under its figure's name", () => {
    expect(TERMS.savedByRewriting).not.toBe(TERMS.savedByOptimizerCache);
    const titles = CURATED_CHARTS.map((c) => c.title);
    expect(titles.filter((t) => t === TERMS.savedByRewriting)).toHaveLength(1);
    expect(titles.filter((t) => t === TERMS.savedByOptimizerCache)).toHaveLength(1);
    expect(titles.filter((t) => t === TERMS.servedOptimized)).toHaveLength(1);
    expect(new Set(titles).size).toBe(titles.length);
  });

  it("names the served share one way, outside the Statistics page's counter descriptions", () => {
    // Statistics describes the module's counters under the feature's own name, which is also what a reader searches for.
    const counterText = ["lib/data/stat-descriptions.ts", "lib/utils/stat-table.ts"];
    expect(uses(/hit rate/i, counterText)).toEqual([]);
    expect(uses(/in-place requests/i, counterText)).toEqual([]);
    expect(uses(/cacheable resource requests/i)).toEqual([]);
  });

  it("says host filter, never lens, in what a reader sees", () => {
    expect(TERMS.hostFilter).toBe("Host filter");
    expect(uses(/host lens/i)).toEqual([]);
    expect(uses(/\((the )?lens\)|the lens is|lens is on/i)).toEqual([]);
  });

  it("writes the savings split as whole sentences", () => {
    for (const text of [SPLIT_TEXT.unavailable, SPLIT_TEXT.notReported, SPLIT_TEXT.coverageNote(80)]) {
      expect(text, text).toMatch(/^[A-Z].*\.$/);
    }
    // "The split" is never left unexplained.
    expect(SPLIT_TEXT.notReported).toMatch(/compression/);
    expect(SPLIT_TEXT.unavailable).toMatch(/compression and optimization/);
  });

  it("keeps Logs with the pages every reader starts from", () => {
    expect(routes.find((r) => r.path === "#/logs")?.group).toBe("overview");
  });
});

describe("documentation links", () => {
  it("share one base on the product site", () => {
    expect(DOCS_BASE).toBe(`${WEBSITE}/docs`);
    for (const [name, href] of Object.entries(DOCS)) {
      expect(href.startsWith(`${DOCS_BASE}/`), name).toBe(true);
      expect(href, name).toMatch(/^https:\/\/[a-z.]+\/docs\/[a-z-]+\/(#[a-z-]+)?$/);
    }
    expect(DOCS_URL).toBe(DOCS.console);
  });

  it("come from the one table: findings and the optimizer status page link nowhere else", () => {
    const known: readonly string[] = Object.values(DOCS);
    for (const rule of ALERT_RULES) {
      if (rule.doc !== undefined) expect(known, rule.id).toContain(rule.doc);
    }
    const kinds: OptimizerAbsence[] = ["not-configured", "not-answering", "did-not-answer", "optimizer-outdated", "module-unsupported"];
    for (const kind of kinds) {
      const doc = optimizerAbsenceText(kind).doc;
      if (doc !== null) expect(known, kind).toContain(doc.href);
    }
  });

  it("are not spelled out anywhere else in the source", () => {
    // The product-facts files hold the site address; every other file builds on them.
    const facts = ["lib/data/product-facts.mjs", "lib/data/product-facts-console.ts"];
    expect(uses(/modpagespeed\.com/, facts)).toEqual([]);
    expect(uses(/\/1\.1\/docs/)).toEqual([]);
    expect(uses(/\/docs\//, facts)).toEqual([]);
  });
});
