// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/** The before/after comparison's modes and its slider's keyboard model. */

export type DiffMode = "slider" | "blink";

const STEP = 5;

export function sliderKeyTarget(key: string, current: number): number | null {
  const next =
    key === "ArrowLeft" || key === "ArrowDown"
      ? current - STEP
      : key === "ArrowRight" || key === "ArrowUp"
        ? current + STEP
        : key === "Home"
          ? 0
          : key === "End"
            ? 100
            : null;
  return next === null ? null : Math.max(0, Math.min(100, next));
}

export function sliderValueText(position: number): string {
  return `${Math.round(position)}% original`;
}
