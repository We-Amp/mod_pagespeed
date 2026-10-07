// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { expect, type Locator } from "@playwright/test";

/** The tick length plus the gap between a value label and the plot. */
export const LABEL_INSET = 15;

export interface ValueAxisFit {
  /** The value axis's labels as the chart's formatter last produced them. */
  labels: string[];
  /** The widest of those labels, measured in the axis font (CSS px). */
  widest: number;
  /** Where the plot area starts: everything left of it is the value axis. */
  plotLeft: number;
}

/**
 * Measures a drawn chart's value axis: the labels the formatter produced
 * (the chart container's data-value-labels), the widest of them in the
 * axis font, and the plot area's left edge. A label is drawn right-aligned,
 * ending a tick (10 px) and a gap (5 px) short of the plot, so it shows in
 * full when the plot starts at least `widest + LABEL_INSET` in.
 */
export async function valueAxisFit(chart: Locator): Promise<ValueAxisFit> {
  const container = chart.locator(".ts-chart");
  await expect(container.locator(".u-over")).toHaveCount(1, { timeout: 15_000 });
  await expect(container).toHaveAttribute("data-value-labels", /\S/, { timeout: 15_000 });
  return container.evaluate((el: HTMLElement) => {
    const labels = (JSON.parse(el.dataset.valueLabels ?? "[]") as string[]).filter((s) => s !== "");
    const family = getComputedStyle(el).getPropertyValue("--ps-font-mono").trim() || "monospace";
    const font = /\d+px/.test(family) ? family : `12px ${family}`;
    const ctx = document.createElement("canvas").getContext("2d")!;
    ctx.font = font;
    const widest = labels.reduce((m, s) => Math.max(m, ctx.measureText(s).width), 0);
    const plotLeft = (el.querySelector(".u-over") as HTMLElement).offsetLeft;
    return { labels, widest, plotLeft };
  });
}
