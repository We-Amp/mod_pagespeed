// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import type { Page } from "@playwright/test";

// The committed console bundle: these specs exercise exactly what ships.
// Rebuild with build.sh before running them.
const SHELL = readFileSync(fileURLToPath(new URL("../../admin_console.html", import.meta.url)), "utf8");

/** A made-up origin: every request to it is answered by page.route(). */
export const ORIGIN = "http://console.test";

// The headers the module serves the console shell with (admin_site.cc:190-197),
// so a change that breaks under the production CSP fails here as well.
const SHELL_HEADERS: Record<string, string> = {
  "Content-Type": "text/html; charset=utf-8",
  "X-Content-Type-Options": "nosniff",
  "X-Frame-Options": "DENY",
  "Referrer-Policy": "no-referrer",
  "Content-Security-Policy":
    "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; " +
    "img-src 'self' data:; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'",
};

/** The two built-in admin paths, and a renamed one (the admin path is configurable). */
export type Admin = "pagespeed_admin" | "pagespeed_global_admin" | "renamed_admin";
export type Leaf =
  | "stats_json"
  | "config"
  | "message_history"
  | "histograms"
  | "cache"
  | "graphs"
  | "v1/daemon/health"
  | "v1/daemon/stats"
  | "v1/daemon/cooldowns"
  | "v1/daemon/cache/urls"
  | "v1/daemon/cache/alternates"
  | "v1/daemon/cache/content"
  | "v1/daemon/logs";

/** A reply. Status 0 means the connection fails: no HTTP answer arrives. */
export interface Reply {
  status: number;
  body: unknown;
  /** Overrides the JSON content type (the byte-serving content leaf). */
  contentType?: string;
  /** A binary body, served as-is instead of JSON.stringify(body). */
  bytes?: Buffer;
}

/** What a function responder sees of the request. */
export interface MockRequest {
  url: URL;
  method: string;
}

/**
 * A fixed reply, or one chosen by the 0-based call count and the request
 * (a paging responder branches on `offset`, a content responder on
 * `alternate_id` and on HEAD vs GET).
 */
export type Responder = Reply | ((call: number, request: MockRequest) => Reply);

export const NETWORK_ERROR: Reply = { status: 0, body: null };

export interface MockOptions {
  /** Hold the replies to these leaves for this many (real) milliseconds. */
  delayMs?: Partial<Record<Leaf, number>>;
  /** Like the module's daemon proxy: one concurrent read per v1/daemon leaf (two for v1/daemon/cache/content); one more gets 429. */
  oneInFlight?: boolean;
  /**
   * Checked live on every request. An unreachable server fails every leaf
   * alike, not only the ones a test happened to mock: while this returns
   * true, every leaf answers exactly like `NETWORK_ERROR`, overriding its
   * own responder (or the default 404 for a leaf nothing mocked).
   */
  down?: () => boolean;
}

export interface MockAdmin {
  calls(leaf: Leaf): number;
  /** 429s the one-in-flight rule produced for a leaf. */
  busyReplies(leaf: Leaf): number;
}

const BUSY_BODY = { error: "a request for this daemon endpoint is already in flight" };

export async function mockAdmin(
  page: Page,
  admin: Admin,
  replies: Partial<Record<Leaf, Responder>>,
  options: MockOptions = {},
): Promise<MockAdmin> {
  const counts = new Map<string, number>();
  const busy = new Map<string, number>();
  const pending = new Map<string, number>();
  const prefix = `/${admin}/`;
  await page.route(`${ORIGIN}/**`, async (route) => {
    const requestUrl = new URL(route.request().url());
    const path = requestUrl.pathname;
    if (path === prefix) {
      await route.fulfill({ status: 200, headers: SHELL_HEADERS, body: SHELL });
      return;
    }
    const leaf = path.startsWith(prefix) ? path.slice(prefix.length) : path;
    const slots = leaf === "v1/daemon/cache/content" ? 2 : 1;
    if (options.oneInFlight && leaf.startsWith("v1/daemon/") && (pending.get(leaf) ?? 0) >= slots) {
      busy.set(leaf, (busy.get(leaf) ?? 0) + 1);
      await route.fulfill({ status: 429, contentType: "application/json", body: JSON.stringify(BUSY_BODY) });
      return;
    }
    // Counted only for reads that are actually answered below: a busy 429
    // short-circuited above never reaches here, so it never advances the
    // call index an index-driven responder sees, nor `calls()`.
    const n = counts.get(leaf) ?? 0;
    counts.set(leaf, n + 1);
    pending.set(leaf, (pending.get(leaf) ?? 0) + 1);
    try {
      const responder = replies[leaf as Leaf];
      const reply: Reply = options.down?.()
        ? NETWORK_ERROR
        : responder === undefined
          ? { status: 404, body: { error: `Unknown admin page: ${leaf}` } }
          : typeof responder === "function"
            ? responder(n, { url: requestUrl, method: route.request().method() })
            : responder;
      const delay = options.delayMs?.[leaf as Leaf] ?? 0;
      if (delay > 0) await new Promise((resolve) => setTimeout(resolve, delay));
      // A test may end while a delayed reply is still held; ignore the closed page.
      if (reply.status === 0) {
        await route.abort("connectionrefused").catch(() => {});
      } else if (reply.bytes !== undefined) {
        await route
          .fulfill({
            status: reply.status,
            contentType: reply.contentType ?? "application/octet-stream",
            body: reply.bytes,
          })
          .catch(() => {});
      } else {
        await route
          .fulfill({
            status: reply.status,
            contentType: reply.contentType ?? "application/json",
            body: JSON.stringify(reply.body),
          })
          .catch(() => {});
      }
    } finally {
      pending.set(leaf, (pending.get(leaf) ?? 1) - 1);
    }
  });
  return { calls: (leaf) => counts.get(leaf) ?? 0, busyReplies: (leaf) => busy.get(leaf) ?? 0 };
}

export async function openConsole(page: Page, admin: Admin, hash = ""): Promise<void> {
  await page.goto(`${ORIGIN}/${admin}/${hash}`);
}
