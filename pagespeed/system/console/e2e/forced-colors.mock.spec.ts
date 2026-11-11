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
import * as F from "./support/fixtures";
import { box, pageReady, perHostReplies, wholeServerReplies } from "./support/layout";
import { mockAdmin, openConsole, type Leaf, type Responder } from "./support/mock-admin";

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

type Replies = Partial<Record<Leaf, Responder>>;

async function open(page: Page, scheme: Scheme, hash: string, replies: Replies = wholeServerReplies()): Promise<void> {
  await page.emulateMedia({ forcedColors: "active", colorScheme: scheme });
  await mockAdmin(page, ADMIN, replies);
  await openConsole(page, ADMIN, hash);
  await pageReady(page);
}

/** The whole-server fixtures, with CSS served two ways: a savings bar of two shares. */
function twoShareReplies(): Replies {
  const stats = JSON.parse(F.OPT_STATS_PAGE_BYTES) as Record<string, Record<string, unknown>>;
  return {
    ...wholeServerReplies(),
    "v1/daemon/stats": F.ok({
      ...stats,
      serve_savings: {
        ...stats.serve_savings,
        css: {
          by_encoding: { br: { bytes: 50_741, hits: 3 }, gzip: { bytes: 0, hits: 0 }, identity: { bytes: 180_000, hits: 5 } },
          hits: 8,
          optimized_bytes: 230_741,
          original_bytes: 884_736,
        },
      },
    }),
  };
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
      const plain = await fillOf(page, nav.locator("a:not([aria-current])").first());
      expect(await fillOf(page, current)).not.toBe(plain);
      // And it carries a bar at its leading edge: neither the row's fill
      // nor the sidebar's surface, so it shows as a bar and not as a notch.
      const b = await box(current);
      const { at } = await painted(page, await current.screenshot({ animations: "disabled" }), [
        [1, b.height / 2],
        [6, b.height / 2],
      ]);
      expect(at[0], "the bar differs from the row's fill").not.toBe(at[1]);
      expect(at[0], "the bar differs from the sidebar's surface").not.toBe(plain);
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

    test("savings bar: every share is painted, and a gap keeps two shares apart", async ({ page }) => {
      await open(page, scheme, "#/savings", twoShareReplies());
      const split = page.getByTestId("savings-split-css");
      const bar = split.locator(".split-bar");
      const shares = bar.locator(".bar-fill");
      await expect(shares).toHaveCount(2);
      const frame = await box(bar);
      const [first, second] = [await box(shares.nth(0)), await box(shares.nth(1))];
      const middle = frame.height / 2;
      const { at } = await painted(page, await bar.screenshot({ animations: "disabled" }), [
        [(first.left + first.right) / 2 - frame.left, middle],
        [(first.right + second.left) / 2 - frame.left, middle],
        [(second.left + second.right) / 2 - frame.left, middle],
      ]);
      const [firstFill, gap, secondFill] = at;
      // The palette paints both shares in one colour: only the gap tells them apart.
      expect(secondFill).toBe(firstFill);
      expect(gap).not.toBe(firstFill);
      // And a share is not the colour of the card it lies on.
      expect(firstFill).not.toBe(await fillOf(page, split));
    });

    test("focus on a filled item: the ring is the fill's text colour", async ({ page }) => {
      // The ring of the current page's link and of the selected tab is
      // drawn inside the item, on its fill: it is painted in the palette's
      // text colour for that fill, the one pair sure to contrast.
      const highlightText = async (): Promise<string> => {
        const id = "forced-colours-probe";
        await page.evaluate((id) => {
          const probe = document.createElement("div");
          probe.id = id;
          probe.style.cssText =
            "position:fixed;left:0;top:0;width:8px;height:8px;z-index:9999;forced-color-adjust:none;background:HighlightText";
          document.body.append(probe);
        }, id);
        const colour = await fillOf(page, page.locator(`#${id}`));
        await page.evaluate((id) => document.getElementById(id)!.remove(), id);
        return colour;
      };
      // [page, the item, how far inside the item's edge the middle of its ring lies]
      for (const [hash, item, inset] of [
        ["#/overview", () => page.getByRole("navigation", { name: "Console pages" }).locator('a[aria-current="page"]'), 1],
        ["#/caches", () => page.getByRole("tab", { selected: true }), 3],
      ] as const) {
        await open(page, scheme, hash);
        const expected = await highlightText();
        const el = item();
        await expect(el).toHaveCount(1);
        const fill = await fillOf(page, el);
        expect(expected, "the palette's pair differs").not.toBe(fill);
        // Reached with the keyboard.
        await el.focus();
        await page.keyboard.press("Shift+Tab");
        await page.keyboard.press("Tab");
        await expect(el).toBeFocused();
        const b = await box(el);
        // The 2 px ring inside the item's top, bottom and trailing edges.
        const { at } = await painted(page, await el.screenshot({ animations: "disabled" }), [
          [b.width / 2, inset],
          [b.width / 2, b.height - 1 - inset],
          [b.width - 1 - inset, b.height / 2],
        ]);
        for (const [side, colour] of at.entries()) expect(colour, `${hash}, side ${side}`).toBe(expected);
      }
    });

    test("histogram: a bucket's bar is painted", async ({ page }) => {
      await open(page, scheme, "#/histograms");
      const bar = page.locator("main .bar-fill").first();
      await expect(bar).toBeVisible();
      const b = await box(bar);
      expect(b.width).toBeGreaterThanOrEqual(2);
      // The bar's centre against the cell just above it.
      const clip = { x: b.left, y: b.top - 3, width: b.width, height: b.height + 3 };
      const { at } = await painted(page, await page.screenshot({ clip, animations: "disabled" }), [
        [b.width / 2, 3 + b.height / 2],
        [b.width / 2, 1],
      ]);
      expect(at[0], "the bar differs from its cell").not.toBe(at[1]);
    });

    test("status dot: the dot before a state word is painted", async ({ page }) => {
      await open(page, scheme, "#/overview");
      for (const dot of [page.getByTestId("health-dot"), page.getByTestId("optimizer-state").locator(".status-dot")]) {
        await expect(dot).toBeVisible();
        const b = await box(dot);
        // The dot's centre against a point just left of it, on the surface it sits on.
        const clip = { x: b.left - 4, y: b.top, width: b.width + 4, height: b.height };
        const { at } = await painted(page, await page.screenshot({ clip, animations: "disabled" }), [
          [4 + b.width / 2, b.height / 2],
          [1, b.height / 2],
        ]);
        expect(at[0], "the dot differs from its surface").not.toBe(at[1]);
      }
    });

    test("health strip: the leading rule is painted", async ({ page }) => {
      await open(page, scheme, "#/overview");
      const strip = page.getByTestId("health-strip");
      const b = await box(strip);
      // The middle of the 3 px rule against the band right of it.
      const clip = { x: b.left, y: b.top + b.height / 2 - 2, width: 12, height: 4 };
      const { at } = await painted(page, await page.screenshot({ clip, animations: "disabled" }), [
        [1, 2],
        [9, 2],
      ]);
      expect(at[0], "the rule differs from the band").not.toBe(at[1]);
    });

    test("muted card: the whole-server card on a per-host console keeps its dashed outline, undimmed", async ({ page }) => {
      await page.emulateMedia({ forcedColors: "active", colorScheme: scheme });
      await mockAdmin(page, "pagespeed_admin", perHostReplies());
      await openConsole(page, "pagespeed_admin", "#/overview");
      await pageReady(page);
      const card = page.getByTestId("optimizer-card");
      await expect(card).toHaveClass(/card-muted/);
      const b = await box(card);
      // Along the top edge a dashed line alternates between the line and the surface.
      const clip = { x: b.left + 20, y: b.top, width: 60, height: 1 };
      const points = Array.from({ length: 60 }, (_, x) => [x, 0] as const);
      const { at } = await painted(page, await page.screenshot({ clip, animations: "disabled" }), points);
      expect(new Set(at).size, "a dashed edge shows the line and the gaps").toBeGreaterThanOrEqual(2);
      // The solid card beside it shows one colour along the same edge.
      const solid = await box(page.getByTestId("module-card"));
      const solidClip = { x: solid.left + 20, y: solid.top, width: 60, height: 1 };
      const edge = await painted(page, await page.screenshot({ clip: solidClip, animations: "disabled" }), points);
      expect(new Set(edge.at).size, "a solid edge is one colour").toBe(1);
      // Nothing fades in a forced palette: the card is told apart by the outline alone.
      expect(Number(await card.evaluate((el) => getComputedStyle(el).opacity))).toBe(1);
    });

    test("field: keyboard focus draws a ring around a text field", async ({ page }) => {
      // An outline is painted in a forced palette; a border colour or a
      // shadow would not be. The four pages with a field of their own.
      for (const [hash, selector, tab] of [
        ["#/statistics", "main input.field", null],
        ["#/histograms", "main input.field", null],
        ["#/graphs", "main input.field", null],
        ["#/caches", "#lookup-url", "Cache Lookup"],
      ] as const) {
        await open(page, scheme, hash);
        if (tab !== null) await page.getByRole("tab", { name: tab }).click();
        const field = page.locator(selector);
        await expect(field).toBeVisible();
        const before = await ringOf(page, field);
        await field.focus();
        const after = await ringOf(page, field);
        for (const [side, colour] of after.entries()) {
          expect(colour, `${hash}, side ${side}`).not.toBe(before[side]);
        }
      }
    });
  });
}

test("the light and the dark forced palettes are two palettes", async ({ page }) => {
  await open(page, "light", "#/overview");
  const band = page.getByRole("banner");
  const light = await fillOf(page, band);
  await page.emulateMedia({ forcedColors: "active", colorScheme: "dark" });
  expect(await fillOf(page, band)).not.toBe(light);
});
