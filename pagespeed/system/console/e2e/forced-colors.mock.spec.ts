// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Forced colours (Windows contrast themes) replace every author colour with
// the user's palette, so a state shown only by a colour or a fill vanishes
// unless the console marks it another way. One row per marker, in a light
// and a dark forced palette, on a whole-server console with data.
//
// Every assertion is about what is painted: the colours in a screenshot of
// the element. Computed styles are not used, because forced colours are
// applied at paint time and a computed colour can report the author value
// whether or not it is what the user sees.
//
// A change that adds a mark carried by colour alone adds its row here.
// Self-contained (support/mock-admin.ts); run after build.sh.

import { expect, test, type Locator, type Page } from "@playwright/test";
import { box, pageReady, wholeServerReplies } from "./support/layout";
import { mockAdmin, openConsole } from "./support/mock-admin";

test.use({ locale: "en-US", timezoneId: "UTC", viewport: { width: 1280, height: 800 } });

const ADMIN = "pagespeed_global_admin";

/** What a screenshot holds: its most frequent colour, and the colour at each asked point ("#rrggbb"). */
interface Painted {
  fill: string;
  at: string[];
}

/**
 * Reads a PNG's pixels. The image is decoded on a canvas in a blank page of
 * the same browser, so the spec needs no image library.
 */
async function painted(page: Page, png: Buffer, points: ReadonlyArray<readonly [x: number, y: number]> = []): Promise<Painted> {
  const scratch = await page.context().newPage();
  try {
    return await scratch.evaluate(
      async ({ data, points }) => {
        const image = new Image();
        image.src = `data:image/png;base64,${data}`;
        await image.decode();
        const canvas = document.createElement("canvas");
        canvas.width = image.width;
        canvas.height = image.height;
        const context = canvas.getContext("2d")!;
        context.drawImage(image, 0, 0);
        const pixels = context.getImageData(0, 0, image.width, image.height).data;
        const hex = (i: number) =>
          `#${[pixels[i], pixels[i + 1], pixels[i + 2]].map((v) => v.toString(16).padStart(2, "0")).join("")}`;
        const counts = new Map<string, number>();
        for (let i = 0; i < pixels.length; i += 4) counts.set(hex(i), (counts.get(hex(i)) ?? 0) + 1);
        const fill = [...counts.entries()].sort((a, b) => b[1] - a[1])[0][0];
        return { fill, at: points.map(([x, y]) => hex((Math.round(y) * image.width + Math.round(x)) * 4)) };
      },
      { data: png.toString("base64"), points },
    );
  } finally {
    await scratch.close();
  }
}

/** The colour that fills most of an element's box. */
async function fillOf(page: Page, element: Locator): Promise<string> {
  return (await painted(page, await element.screenshot({ animations: "disabled" }))).fill;
}

/**
 * The colours just outside an element's border box, where a focus ring is
 * drawn (the console's ring is 2 px wide, 2 px away): the middle of each side.
 */
async function ringOf(page: Page, element: Locator): Promise<string[]> {
  const b = await box(element);
  const margin = 6;
  const out = 3;
  const clip = { x: b.left - margin, y: b.top - margin, width: b.width + 2 * margin, height: b.height + 2 * margin };
  const png = await page.screenshot({ clip, animations: "disabled" });
  return (
    await painted(page, png, [
      [clip.width / 2, margin - out],
      [clip.width / 2, clip.height - 1 - (margin - out)],
      [margin - out, clip.height / 2],
      [clip.width - 1 - (margin - out), clip.height / 2],
    ])
  ).at;
}

type Scheme = "light" | "dark";

async function open(page: Page, scheme: Scheme, hash: string): Promise<void> {
  await page.emulateMedia({ forcedColors: "active", colorScheme: scheme });
  await mockAdmin(page, ADMIN, wholeServerReplies());
  await openConsole(page, ADMIN, hash);
  await pageReady(page);
}

for (const scheme of ["light", "dark"] as const) {
  test.describe(`Forced colours (${scheme} palette)`, () => {
    test("the palette is forced, and differs from the console's own", async ({ page }) => {
      await open(page, scheme, "#/overview");
      expect(await page.evaluate(() => matchMedia("(forced-colors: active)").matches)).toBe(true);
      // The top bar is a blue band in the console's own colours.
      const band = page.getByRole("banner");
      const forced = await fillOf(page, band);
      await page.emulateMedia({ forcedColors: "none", colorScheme: scheme });
      expect(await fillOf(page, band)).not.toBe(forced);
    });

    test("sidebar: the current page is filled, the other pages are not", async ({ page }) => {
      await open(page, scheme, "#/overview");
      const nav = page.getByRole("navigation", { name: "Console pages" });
      const current = nav.locator('a[aria-current="page"]');
      await expect(current).toHaveCount(1);
      expect(await fillOf(page, current)).not.toBe(await fillOf(page, nav.locator("a:not([aria-current])").first()));
    });

    test("tabs: the selected tab is filled, the other tabs are not", async ({ page }) => {
      await open(page, scheme, "#/caches");
      const selected = page.getByRole("tab", { selected: true });
      await expect(selected).toHaveCount(1);
      expect(await fillOf(page, selected)).not.toBe(await fillOf(page, page.getByRole("tab", { selected: false }).first()));
    });

    test("view switch: the current view is filled, the other view is not", async ({ page }) => {
      await open(page, scheme, "#/statistics");
      const views = page.locator("main nav");
      const current = views.locator('a[aria-current="page"]');
      await expect(current).toHaveCount(1);
      expect(await fillOf(page, current)).not.toBe(await fillOf(page, views.locator("a:not([aria-current])").first()));
    });

    test("toggle: a button that is on is filled, and not once it is off", async ({ page }) => {
      await open(page, scheme, "#/statistics");
      const toggle = page.getByRole("button", { name: /^Auto-refresh/ });
      await expect(toggle).toHaveAttribute("aria-pressed", "true");
      const on = await fillOf(page, toggle);
      await toggle.click();
      await expect(toggle).toHaveAttribute("aria-pressed", "false");
      // Off, and neither hovered nor focused: the plain button.
      await page.mouse.move(0, 0);
      await toggle.blur();
      expect(await fillOf(page, toggle)).not.toBe(on);
    });

    test("field: keyboard focus draws a ring around a text field", async ({ page }) => {
      // Not met yet: the search fields replace the ring with a border colour
      // and a shadow, and forced colours drop both. Remove this line with
      // the change that gives fields the console's outline.
      test.fail();
      await open(page, scheme, "#/statistics");
      const field = page.getByRole("searchbox").first();
      await expect(field).toBeVisible();
      const before = await ringOf(page, field);
      await field.focus();
      const after = await ringOf(page, field);
      for (const [side, colour] of after.entries()) {
        expect(colour, `side ${side}`).not.toBe(before[side]);
      }
    });
  });
}
