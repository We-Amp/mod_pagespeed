// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The console has one polling mechanism (lib/api/poller.ts via usePolling).
// These checks keep it that way: no interval timers, no page effects (an
// $effect that reads a fetch result re-arms itself -- a request storm), and
// every page that refreshes data goes through usePolling.

import { readFileSync, readdirSync, statSync } from "node:fs";
import { join, relative } from "node:path";
import { fileURLToPath } from "node:url";
import { describe, expect, it } from "vitest";

const SRC = fileURLToPath(new URL("..", import.meta.url));

function sourceFiles(dir: string): string[] {
  const out: string[] = [];
  for (const name of readdirSync(dir)) {
    const path = join(dir, name);
    if (statSync(path).isDirectory()) out.push(...sourceFiles(path));
    else if (/\.(svelte|ts)$/.test(name) && !name.endsWith(".test.ts")) out.push(path);
  }
  return out;
}

const offenders = (files: string[], pattern: RegExp) =>
  files.filter((f) => pattern.test(readFileSync(f, "utf8"))).map((f) => relative(SRC, f));

const POLLING_PAGES = [
  "Overview", "Savings", "Statistics", "Histograms", "Cache", "Graphs",
  "OptimizerStatus", "Urls", "UrlDetail", "Logs",
];

describe("the console's polling discipline", () => {
  // Calls, not prose: a line that is a comment ("//" or a "*" doc line) is skipped.
  it("has no setInterval anywhere", () => {
    expect(offenders(sourceFiles(SRC), /^(?!\s*(\/\/|\*)).*\bsetInterval\s*\(/m)).toEqual([]);
  });

  it("has no $effect in any page", () => {
    expect(offenders(sourceFiles(join(SRC, "pages")), /^(?!\s*(\/\/|\*)).*\$effect(\.\w+)?\s*\(/m)).toEqual([]);
  });

  it("refreshes every data page through usePolling", () => {
    for (const page of POLLING_PAGES) {
      const text = readFileSync(join(SRC, "pages", `${page}.svelte`), "utf8");
      expect(text, page).toMatch(/\busePolling\s*[<(]/);
    }
  });
});
