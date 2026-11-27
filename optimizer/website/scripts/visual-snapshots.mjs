// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// visual-snapshots.mjs — visual, accessibility and layout-shift harness.
// Runs against an already-serving build (`npx astro preview --port <free>`)
// or a dev server; it never starts or stops a server itself (stop yours with
// `npx astro preview stop` / `npx astro dev stop` when done).
//
// For each path given:
//   - PNG screenshots at 1440x900 and 390x844, dark (default) and light
//     (forced by removing the .dark class after DOMContentLoaded), full page
//     plus first viewport;
//   - axe results (WCAG 2.1 A/AA tags) in both themes, whole page, plus a
//     light-theme run scoped to [data-ui] regions, written to JSON;
//   - CLS: PerformanceObserver('layout-shift') sum (entries without recent
//     input) over load + 3s, at both widths, plus horizontal-scroll metrics
//     at 360/390/1440, written to JSON.
//
// Gates (the process exits non-zero when any holds): a serious/critical axe
// violation in dark (whole page) or in light inside [data-ui], CLS > 0.02 at
// either width, or horizontal scroll at any checked width. The per-path
// summary line prints every count; the light whole-page axe run is
// report-only (legacy chrome outside [data-ui] is allowed to trip it).
//
// Usage:
//   node scripts/visual-snapshots.mjs --base http://localhost:4321 --out <dir> <path...>
// Example:
//   node scripts/visual-snapshots.mjs --base http://localhost:4321 \
//     --out ../../shots/run-1 / /docs/

import { mkdirSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { chromium } from 'playwright';
import { AxeBuilder } from '@axe-core/playwright';

const args = process.argv.slice(2);
let base = null;
let out = null;
const paths = [];
for (let i = 0; i < args.length; i++) {
  if (args[i] === '--base') base = args[++i];
  else if (args[i] === '--out') out = args[++i];
  else paths.push(args[i]);
}
if (!base || !out || paths.length === 0) {
  console.error('usage: node scripts/visual-snapshots.mjs --base <url> --out <dir> <path...>');
  process.exit(2);
}

mkdirSync(resolve(out), { recursive: true });

const AXE_TAGS = ['wcag2a', 'wcag2aa', 'wcag21a', 'wcag21aa'];
const VIEWPORTS = [
  { name: '1440', width: 1440, height: 900 },
  { name: '390', width: 390, height: 844 },
];
const SCROLL_WIDTHS = [360, 390, 1440];

const slug = (p) => (p === '/' ? 'index' : p.replace(/^\/|\/$/g, '').replaceAll('/', '-'));

const CLS_GATE = 0.02;
const seriousCritical = (axe) =>
  (axe.violations ?? []).filter((v) => v.impact === 'serious' || v.impact === 'critical').length;
const failures = [];

const CLS_INIT = `
window.__cls = 0;
new PerformanceObserver((list) => {
  for (const e of list.getEntries()) if (!e.hadRecentInput) window.__cls += e.value;
}).observe({ type: 'layout-shift', buffered: true });
`;

const browser = await chromium.launch();
try {
  for (const path of paths) {
    const name = slug(path);
    const url = new URL(path, base).href;
    const clsResult = { path, widths: {}, scroll: {} };

    for (const vp of VIEWPORTS) {
      const context = await browser.newContext({
        viewport: { width: vp.width, height: vp.height },
        deviceScaleFactor: 1,
      });
      const page = await context.newPage();
      await page.addInitScript(CLS_INIT);
      await page.goto(url, { waitUntil: 'load' });
      // Reduced motion freezes the one-shot fades (telemetry strip) so the axe passes measure resting colours, as in tests/accessibility.spec.ts.
      await page.emulateMedia({ reducedMotion: 'reduce' });
      // CLS window: load + 3s, before any screenshot (a full-page capture
      // resizes the viewport internally and must not feed the sum).
      await page.waitForTimeout(3000);
      clsResult.widths[vp.width] = {
        cls: await page.evaluate(() => window.__cls),
        scrollWidth: await page.evaluate(() => document.documentElement.scrollWidth),
        innerWidth: vp.width,
      };

      // axe, dark (as served).
      const axeDark = await new AxeBuilder({ page }).withTags(AXE_TAGS).analyze();
      writeFileSync(
        resolve(out, `${name}-${vp.name}.axe-dark.json`),
        JSON.stringify(axeDark, null, 2),
      );
      const darkSC = seriousCritical(axeDark);

      // Screenshots, dark: first viewport, then full page.
      await page.screenshot({ path: resolve(out, `${name}-${vp.name}-dark-viewport.png`) });
      await page.screenshot({
        path: resolve(out, `${name}-${vp.name}-dark-full.png`),
        fullPage: true,
      });

      // Force the light theme: remove .dark (after DOMContentLoaded, per spec),
      // then let the style recalc settle.
      await page.evaluate(() => document.documentElement.classList.remove('dark'));
      await page.evaluate(
        () => new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r))),
      );

      const axeLight = await new AxeBuilder({ page }).withTags(AXE_TAGS).analyze();
      writeFileSync(
        resolve(out, `${name}-${vp.name}.axe-light.json`),
        JSON.stringify(axeLight, null, 2),
      );
      const lightSC = seriousCritical(axeLight);
      // The [data-ui]-scoped run only makes sense on pages that carry new
      // components; axe throws when an include selector matches nothing.
      const hasUi = await page.evaluate(() => Boolean(document.querySelector('[data-ui]')));
      let uiSC = null;
      if (hasUi) {
        const axeLightUi = await new AxeBuilder({ page })
          .withTags(AXE_TAGS)
          .include('[data-ui]')
          .analyze();
        writeFileSync(
          resolve(out, `${name}-${vp.name}.axe-light-dataui.json`),
          JSON.stringify(axeLightUi, null, 2),
        );
        uiSC = seriousCritical(axeLightUi);
      } else {
        writeFileSync(
          resolve(out, `${name}-${vp.name}.axe-light-dataui.json`),
          JSON.stringify({ skipped: 'no [data-ui] regions on this page' }, null, 2),
        );
      }

      clsResult.widths[vp.width].axe = {
        darkSeriousCritical: darkSC,
        lightDataUiSeriousCritical: uiSC,
        lightWholePageSeriousCritical: lightSC, // report-only, never gated
      };
      const cls = clsResult.widths[vp.width].cls;
      if (darkSC > 0)
        failures.push(`${path} @${vp.name}: ${darkSC} serious/critical axe violation(s), dark`);
      if (uiSC !== null && uiSC > 0)
        failures.push(
          `${path} @${vp.name}: ${uiSC} serious/critical axe violation(s), light [data-ui]`,
        );
      if (cls > CLS_GATE) failures.push(`${path} @${vp.name}: CLS ${cls.toFixed(4)} > ${CLS_GATE}`);

      await page.screenshot({ path: resolve(out, `${name}-${vp.name}-light-viewport.png`) });
      await page.screenshot({
        path: resolve(out, `${name}-${vp.name}-light-full.png`),
        fullPage: true,
      });

      await context.close();
    }

    // Horizontal-scroll metrics at the three gate widths (theme-independent).
    for (const w of SCROLL_WIDTHS) {
      const context = await browser.newContext({
        viewport: { width: w, height: 844 },
        deviceScaleFactor: 1,
      });
      const page = await context.newPage();
      await page.goto(url, { waitUntil: 'load' });
      await page.waitForTimeout(500);
      const sw = await page.evaluate(() => document.documentElement.scrollWidth);
      clsResult.scroll[w] = { scrollWidth: sw, innerWidth: w, ok: sw <= w };
      if (sw > w) failures.push(`${path} @${w}: horizontal scroll (scrollWidth ${sw} > ${w})`);
      await context.close();
    }

    writeFileSync(resolve(out, `${name}.cls.json`), JSON.stringify(clsResult, null, 2));
    console.log(
      `${path}: cls1440=${clsResult.widths[1440].cls.toFixed(4)} ` +
        `cls390=${clsResult.widths[390].cls.toFixed(4)} ` +
        `hscroll360=${clsResult.scroll[360].ok} hscroll390=${clsResult.scroll[390].ok} ` +
        `hscroll1440=${clsResult.scroll[1440].ok}`,
    );
    for (const vp of VIEWPORTS) {
      const a = clsResult.widths[vp.width].axe;
      console.log(
        `  @${vp.name}: axe serious/critical dark=${a.darkSeriousCritical} ` +
          `light[data-ui]=${a.lightDataUiSeriousCritical ?? 'n/a'} ` +
          `light-whole-page=${a.lightWholePageSeriousCritical} (report-only)`,
      );
    }
  }
} finally {
  await browser.close();
}
if (failures.length > 0) {
  process.exitCode = 1;
  console.log('gate: FAIL');
  for (const f of failures) console.log(`  - ${f}`);
} else {
  console.log('gate: PASS');
}
console.log(`snapshots written to ${resolve(out)}`);
