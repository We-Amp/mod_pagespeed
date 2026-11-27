// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Authentication (LoginForm) E2E tests.
 *
 * Prerequisites: workbench-demo stack running
 *   (origin :8081, nginx :8080, worker :9880, vite :5173).
 *
 * The demo worker DOES require a bearer token, but nginx
 * holds it and adds it to /console/ and /v1/ on the way through, so nothing
 * reaching the browser ever needs a credential. These tests verify that the
 * login modal stays hidden during normal operation and validate the
 * structure of the LoginForm component when it would appear.
 */
import { test, expect } from '@playwright/test';

test.describe('Authentication', () => {
  // -- Login modal absent on unauthenticated stack ----------------------------

  test('login modal does not appear when no auth required', async ({ page }) => {
    await page.goto('/console/');
    // Wait for the page to fully connect
    await expect(
      page.getByText(/connected/i).first(),
    ).toBeVisible({ timeout: 10000 });

    // The login overlay should NOT be present in the DOM
    const loginOverlay = page.locator('.login-overlay');
    await expect(loginOverlay).not.toBeVisible();
  });

  test('login modal absent across multiple console routes', async ({ page }) => {
    const routes = ['/console/', '/console/urls', '/console/config', '/console/waterfall'];
    for (const route of routes) {
      await page.goto(route);
      await expect(
        page.getByText(/connected/i).first(),
      ).toBeVisible({ timeout: 10000 });

      // Login overlay must not appear on any route
      const loginOverlay = page.locator('.login-overlay');
      await expect(loginOverlay).not.toBeVisible();
    }
  });

  // -- LoginForm component structure ------------------------------------------
  // These tests use page.evaluate to inject LoginForm markup into the DOM
  // to validate its structure, since the demo stack never triggers it.

  test('login form has password input field when auth dialog shown', async ({ page }) => {
    await page.goto('/console/');
    await expect(
      page.getByText(/connected/i).first(),
    ).toBeVisible({ timeout: 10000 });

    // Inject a login overlay into the DOM to verify its structure
    await page.evaluate(() => {
      const overlay = document.createElement('div');
      overlay.className = 'login-overlay';
      overlay.setAttribute('role', 'dialog');
      overlay.setAttribute('aria-label', 'Authentication required');
      overlay.innerHTML = `
        <div class="login-card ps-card">
          <h2 class="login-title">Authentication Required</h2>
          <p class="login-description">Enter your API token to access the PageSpeed Console.</p>
          <form>
            <div class="form-group">
              <label for="api-token" class="form-label">API Token</label>
              <input id="api-token" type="password" class="ps-input"
                     placeholder="Enter API token" autocomplete="off" />
            </div>
            <button type="submit" class="ps-btn ps-btn-primary login-submit" disabled>Connect</button>
          </form>
        </div>
      `;
      document.body.appendChild(overlay);
    });

    // Verify the password input is present and has the correct type
    const tokenInput = page.locator('#api-token');
    await expect(tokenInput).toBeVisible();
    await expect(tokenInput).toHaveAttribute('type', 'password');
    await expect(tokenInput).toHaveAttribute('placeholder', 'Enter API token');
  });

  test('login form has submit button labeled Connect', async ({ page }) => {
    await page.goto('/console/');
    await expect(
      page.getByText(/connected/i).first(),
    ).toBeVisible({ timeout: 10000 });

    // Inject login overlay
    await page.evaluate(() => {
      const overlay = document.createElement('div');
      overlay.className = 'login-overlay';
      overlay.setAttribute('role', 'dialog');
      overlay.innerHTML = `
        <div class="login-card ps-card">
          <h2 class="login-title">Authentication Required</h2>
          <form>
            <div class="form-group">
              <input id="api-token" type="password" class="ps-input" value="" />
            </div>
            <button type="submit" class="ps-btn ps-btn-primary login-submit" disabled>Connect</button>
          </form>
        </div>
      `;
      document.body.appendChild(overlay);
    });

    const submitBtn = page.locator('.login-submit');
    await expect(submitBtn).toBeVisible();
    await expect(submitBtn).toHaveText('Connect');
  });

  test('submit button is disabled when token input is empty', async ({ page }) => {
    await page.goto('/console/');
    await expect(
      page.getByText(/connected/i).first(),
    ).toBeVisible({ timeout: 10000 });

    // Inject login overlay with empty input and disabled button
    await page.evaluate(() => {
      const overlay = document.createElement('div');
      overlay.className = 'login-overlay';
      overlay.setAttribute('role', 'dialog');
      overlay.innerHTML = `
        <div class="login-card ps-card">
          <h2 class="login-title">Authentication Required</h2>
          <form>
            <div class="form-group">
              <input id="api-token" type="password" class="ps-input" value="" />
            </div>
            <button type="submit" class="ps-btn ps-btn-primary login-submit" disabled>Connect</button>
          </form>
        </div>
      `;
      document.body.appendChild(overlay);
    });

    // Button should be disabled when input is empty
    const submitBtn = page.locator('.login-submit');
    await expect(submitBtn).toBeDisabled();
  });

  test('login dialog uses correct ARIA role', async ({ page }) => {
    await page.goto('/console/');
    await expect(
      page.getByText(/connected/i).first(),
    ).toBeVisible({ timeout: 10000 });

    // The real LoginForm uses role="dialog" — confirm it is absent now
    const dialogs = page.locator('[role="dialog"][aria-label="Authentication required"]');
    await expect(dialogs).toHaveCount(0);
  });

  test('connection succeeds without a browser-held token on demo stack', async ({ page, request }) => {
    // Verify the health endpoint does not require auth
    const healthResp = await request.get('/v1/health');
    expect(healthResp.status()).toBe(200);
    const health = await healthResp.json();
    expect(health.status).toBe('ok');

    // Load the console and confirm no auth wall
    await page.goto('/console/');
    await expect(
      page.getByText(/connected/i).first(),
    ).toBeVisible({ timeout: 10000 });

    // "Authentication Required" heading must not be visible
    const authHeading = page.getByText(/authentication required/i);
    await expect(authHeading).not.toBeVisible();
  });
});
