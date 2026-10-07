// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Geometry helpers for the layout specs: element boxes in CSS pixels,
// measured in the page after it has settled.

import { expect, type Locator, type Page } from "@playwright/test";
import * as F from "./fixtures";
import type { Leaf, Responder } from "./mock-admin";

export interface Box {
  left: number;
  top: number;
  right: number;
  bottom: number;
  width: number;
  height: number;
}

/** The element's border box in viewport coordinates. */
export async function box(locator: Locator): Promise<Box> {
  return locator.evaluate((el) => {
    const r = el.getBoundingClientRect();
    return { left: r.left, top: r.top, right: r.right, bottom: r.bottom, width: r.width, height: r.height };
  });
}

/** The page heading is shown and nothing is still loading. */
export async function pageReady(page: Page): Promise<void> {
  await expect(page.getByRole("heading", { level: 1 })).toBeVisible();
  await expect(page.locator("main .loading")).toHaveCount(0);
}

/** The viewport's size as the page sees it (CSS px). */
export async function viewport(page: Page): Promise<{ width: number; height: number }> {
  return page.evaluate(() => ({ width: window.innerWidth, height: window.innerHeight }));
}

/** Scrolls the document to a fraction (0..1) of its scrollable height. */
export async function scrollTo(page: Page, fraction: number): Promise<void> {
  await page.evaluate((f) => {
    const el = document.scrollingElement!;
    el.scrollTop = (el.scrollHeight - el.clientHeight) * f;
  }, fraction);
}

/** The document does not scroll sideways. */
export async function noHorizontalScroll(page: Page): Promise<boolean> {
  return page.evaluate(() => document.scrollingElement!.scrollWidth <= document.scrollingElement!.clientWidth + 1);
}

/** Every leaf answered: the whole-server console with an optimizer, a URL index and a log. */
export function wholeServerReplies(): Partial<Record<Leaf, Responder>> {
  return {
    ...F.allReplies(),
    // The whole-server configuration, with text (the Configuration page has something to show).
    config: F.ok({ ...(F.CONFIG_VHOST_WITH_TEXT.body as Record<string, unknown>), scope: "global", host: "" }),
    stats_json: F.STATS_GLOBAL,
    "v1/daemon/stats": F.OPT_STATS_PAGE,
    "v1/daemon/cache/urls": F.ok({
      urls: [
        { url: "/", hostname: "www.example.test", scheme: "https", alternate_count: 3 },
        { url: "/hero.png", hostname: "www.example.test", scheme: "https", alternate_count: 3 },
        { url: "/app.js", hostname: "cdn.example.test", scheme: "https", alternate_count: 1 },
      ],
      offset: 0,
      limit: 50,
      next_offset: 3,
      has_more: false,
      total: 3,
    }),
    "v1/daemon/cache/alternates": F.ALTERNATES_HERO,
    "v1/daemon/cache/content": F.CONTENT_HERO,
    "v1/daemon/logs": F.LOGS_PAGE,
  };
}

/** A per-host console with an optimizer whose savings name hosts. */
export function perHostReplies(): Partial<Record<Leaf, Responder>> {
  return { ...F.allReplies(), "v1/daemon/stats": F.OPT_STATS_PAGE };
}

/**
 * Widens every glyph a little, like the wider fonts of some systems, so a
 * layout that only just fits is caught on any machine.
 */
export async function widenText(page: Page): Promise<void> {
  await page.addStyleTag({ content: "body { letter-spacing: 0.05em; }" });
}

/** The horizontally scrollable wrappers in the page that are wider inside than out. */
export async function sidewaysScrollers(page: Page): Promise<string[]> {
  return page.locator("main").evaluate((m) =>
    [...m.querySelectorAll<HTMLElement>("*")]
      .filter((el) => ["auto", "scroll"].includes(getComputedStyle(el).overflowX) && el.tagName !== "PRE")
      .filter((el) => el.getClientRects().length > 0 && el.scrollWidth > el.clientWidth + 1)
      .map((el) => `${el.tagName.toLowerCase()}.${el.className} (${el.scrollWidth} > ${el.clientWidth})`),
  );
}
