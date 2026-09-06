// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import * as canonical from "./product-facts.mjs";
import {
  CONSOLE_TITLE,
  PRIVACY_URL,
  PRODUCT_DISPLAY_NAME,
  PRODUCT_NAME,
  SUPPORT_URL,
  TERMS_URL,
  VENDOR,
  VENDOR_HOST,
  VENDOR_URL,
  WEBSITE_HOST,
} from "./product-facts-console";

describe("product-facts-console", () => {
  it("re-exports the canonical identity facts unmodified", () => {
    expect(PRODUCT_NAME).toBe(canonical.PRODUCT_NAME);
    expect(PRODUCT_DISPLAY_NAME).toBe(canonical.PRODUCT_DISPLAY_NAME);
    expect(VENDOR).toBe(canonical.VENDOR);
    expect(VENDOR_URL).toBe(canonical.VENDOR_URL);
    expect(PRIVACY_URL).toBe(canonical.PRIVACY_URL);
    expect(TERMS_URL).toBe(canonical.TERMS_URL);
    expect(SUPPORT_URL).toBe(canonical.SUPPORT_URL);
  });

  it("composes the document title from the canonical display name", () => {
    expect(CONSOLE_TITLE).toBe(`${canonical.PRODUCT_DISPLAY_NAME} Admin Console`);
  });

  it("derives bare hosts for display from the canonical URLs", () => {
    expect(VENDOR_HOST).toBe(new URL(canonical.VENDOR_URL).host);
    expect(WEBSITE_HOST).toBe(new URL(canonical.WEBSITE).host);
  });
});
