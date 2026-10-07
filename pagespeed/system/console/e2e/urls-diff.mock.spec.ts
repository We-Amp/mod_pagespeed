// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The before/after image diff on the URL detail view: a keyboard slider
// outside any image role, a manual blink toggle that never refetches, both
// images through the module's two content slots, axe-clean.
// Self-contained; run after build.sh.

import AxeBuilder from "@axe-core/playwright";
import { expect, test, type Page } from "@playwright/test";
import * as F from "./support/fixtures";
import { mockAdmin, openConsole, type MockOptions } from "./support/mock-admin";

test.use({ locale: "en-US", timezoneId: "UTC" });

const DETAIL_HASH = "#/urls/detail?url=%2Fhero.png&host=www.example.test&scheme=https";

async function openDetailWithDiff(page: Page, options: MockOptions = {}) {
  const admin = await mockAdmin(
    page,
    "pagespeed_global_admin",
    {
      ...F.allReplies(),
      config: F.CONFIG_GLOBAL,
      "v1/daemon/cache/alternates": F.ALTERNATES_HERO,
      "v1/daemon/cache/content": F.CONTENT_HERO,
    },
    options,
  );
  await openConsole(page, "pagespeed_global_admin", DETAIL_HASH);
  await expect(page.getByTestId("variant-previews")).toBeVisible();
  // The webp variant (id 1) has a matching original (id 0: desktop, 1x,
  // no save-data); the avif one (mobile, 2x+, save-data) does not.
  await expect(page.getByTestId("compare-2")).toHaveCount(0);
  await page.getByTestId("compare-1").click();
  await expect(page.getByTestId("compare-panel")).toBeVisible();
  return admin;
}

test("the slider handle is a keyboard slider, not inside an image role", async ({ page }) => {
  await openDetailWithDiff(page);
  const slider = page.getByRole("slider", { name: "Comparison position" });
  await expect(slider).toHaveAttribute("aria-valuenow", "50");
  await expect(slider).toHaveAttribute("aria-valuetext", "50% original");
  expect(await slider.getAttribute("aria-orientation")).toBeNull(); // horizontal: the ARIA default
  await expect(page.locator('[role="img"] [role="slider"]')).toHaveCount(0);
  await expect(page.getByRole("group", { name: "Original and optimized comparison" })).toBeVisible();
  await slider.focus();
  await page.keyboard.press("ArrowRight");
  await expect(slider).toHaveAttribute("aria-valuenow", "55");
  await page.keyboard.press("ArrowLeft");
  await page.keyboard.press("ArrowLeft");
  await expect(slider).toHaveAttribute("aria-valuenow", "45");
  await expect(slider).toHaveAttribute("aria-valuetext", "45% original");
  await page.keyboard.press("End");
  await expect(slider).toHaveAttribute("aria-valuenow", "100");
  await page.keyboard.press("Home");
  await expect(slider).toHaveAttribute("aria-valuenow", "0");
});

test("the slider drags with a mouse, starting on the handle", async ({ page }) => {
  await openDetailWithDiff(page);
  const slider = page.getByRole("slider", { name: "Comparison position" });
  await expect(slider).toHaveAttribute("aria-valuenow", "50");
  const frame = page.locator(".slider-frame");
  await frame.scrollIntoViewIfNeeded(); // the raw mouse API does not scroll
  const box = await frame.boundingBox();
  expect(box).not.toBeNull();
  if (box === null) return;
  // Press on the handle — the obvious grab point — and drag to the left.
  await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
  await page.mouse.down();
  await page.mouse.move(box.x + box.width * 0.25, box.y + box.height / 2, { steps: 5 });
  await expect(slider).toHaveAttribute("aria-valuenow", "25");
  // The drag survives leaving the frame (pointer capture) and clamps at 0;
  // releasing outside the frame ends it, later moves change nothing.
  await page.mouse.move(box.x - 40, box.y + box.height / 2, { steps: 5 });
  await expect(slider).toHaveAttribute("aria-valuenow", "0");
  await page.mouse.up();
  await page.mouse.move(box.x + box.width * 0.8, box.y + box.height / 2, { steps: 5 });
  await expect(slider).toHaveAttribute("aria-valuenow", "0");
});

test("blink toggles with Enter and Space, never on a timer, never refetching", async ({ page }) => {
  const admin = await openDetailWithDiff(page);
  await page.getByRole("button", { name: "Blink" }).click();
  const panel = page.getByTestId("compare-panel");
  const label = panel.getByTestId("blink-label");
  await expect(label).toHaveText("Original");
  const imgs = panel.locator("img");
  await expect(imgs).toHaveCount(2); // both mounted
  for (const img of await imgs.all()) {
    await expect.poll(async () => img.evaluate((el: HTMLImageElement) => el.naturalWidth)).toBe(1);
  }
  const callsBefore = admin.calls("v1/daemon/cache/content");
  const toggle = page.getByRole("button", { name: "Toggle between original and optimized" });
  await toggle.focus();
  await page.keyboard.press("Enter");
  await expect(label).toHaveText("Optimized");
  await expect(toggle).toHaveAttribute("aria-pressed", "true");
  await page.keyboard.press(" ");
  await expect(label).toHaveText("Original");
  await page.waitForTimeout(1500); // no timer flips it back
  await expect(label).toHaveText("Original");
  expect(admin.calls("v1/daemon/cache/content")).toBe(callsBefore);
});

test("both images come from the content leaf and load", async ({ page }) => {
  await openDetailWithDiff(page);
  const imgs = page.getByTestId("compare-panel").locator("img");
  await expect(imgs).toHaveCount(2);
  for (const img of await imgs.all()) {
    await expect(img).toHaveAttribute("src", /v1\/daemon\/cache\/content\?url=%2Fhero\.png&hostname=www\.example\.test&scheme=https&alternate_id=[01]$/);
    await expect(img).toHaveAttribute("alt", /hero\.png/);
    await expect.poll(async () => img.evaluate((el: HTMLImageElement) => el.naturalWidth)).toBe(1);
  }
});

test("previews and the diff together stay within the two content slots", async ({ page }) => {
  const admin = await openDetailWithDiff(page, {
    oneInFlight: true,
    delayMs: { "v1/daemon/cache/content": 300 },
  });
  const all = page.locator("main img");
  await expect(all).toHaveCount(5, { timeout: 10_000 }); // 3 previews + 2 diff images
  for (const img of await all.all()) {
    await expect
      .poll(async () => img.evaluate((el: HTMLImageElement) => el.naturalWidth), { timeout: 10_000 })
      .toBe(1);
  }
  expect(admin.busyReplies("v1/daemon/cache/content")).toBe(0);
});

for (const scheme of ["light", "dark"] as const) {
  test(`the open diff view is axe-clean (${scheme})`, async ({ page }) => {
    await page.emulateMedia({ colorScheme: scheme });
    await openDetailWithDiff(page);
    for (const mode of ["Slider", "Blink"]) {
      await page.getByRole("button", { name: mode }).click();
      const report = await new AxeBuilder({ page })
        .withTags(["wcag2a", "wcag2aa", "wcag21a", "wcag21aa"])
        .analyze();
      expect(report.violations, `${scheme}/${mode}`).toEqual([]);
    }
  });
}

test("a variant compares against the cached original-content record the optimizer really sends", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", {
    ...F.allReplies(),
    config: F.CONFIG_GLOBAL,
    "v1/daemon/cache/alternates": F.ALTERNATES_SENTINEL_ORIGINAL,
    "v1/daemon/cache/content": F.CONTENT_HERO,
  });
  await openConsole(page, "pagespeed_global_admin", DETAIL_HASH);
  await expect(page.getByTestId("variant-previews")).toBeVisible();
  await page.getByTestId("compare-9").click();
  const panel = page.getByTestId("compare-panel");
  await expect(panel).toBeVisible();
  await expect(panel.getByRole("img", { name: /^Original variant 12 of / })).toHaveCount(1);
});
