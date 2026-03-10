import { test, expect } from "@playwright/test";

const BASE = "http://localhost:8080/pagespeed_admin/";

test("debug console page", async ({ page }) => {
  const errors: string[] = [];
  const logs: string[] = [];
  page.on("pageerror", (err) => errors.push(err.message));
  page.on("console", (msg) => logs.push(`[${msg.type()}] ${msg.text()}`));

  await page.goto(`${BASE}#/console`);
  await page.waitForTimeout(5000);

  console.log("=== JS Errors ===");
  for (const e of errors) console.log(e);
  console.log("=== Console Logs ===");
  for (const l of logs) console.log(l);

  // Check what's actually visible
  const bodyText = await page.locator(".page, .content, main, [class*=console]").first().textContent();
  console.log("=== Page content (first 500 chars) ===");
  console.log(bodyText?.substring(0, 500));

  // Check if there's a pre tag with raw content
  const preCount = await page.locator("pre").count();
  console.log(`=== pre tags: ${preCount} ===`);
  if (preCount > 0) {
    const preText = await page.locator("pre").first().textContent();
    console.log("Pre content (first 200 chars):", preText?.substring(0, 200));
  }
});
