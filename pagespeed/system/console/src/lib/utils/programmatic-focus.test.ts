// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import { PROGRAMMATIC_FOCUS_CLASS, markProgrammaticFocus } from "./programmatic-focus";

function fakeElement(): { el: HTMLElement; classes: Set<string> } {
  const classes = new Set<string>();
  const classList = {
    add: (c: string) => void classes.add(c),
    remove: (c: string) => void classes.delete(c),
    contains: (c: string) => classes.has(c),
  };
  return { el: { classList } as unknown as HTMLElement, classes };
}

describe("markProgrammaticFocus", () => {
  it("marks the element until the next key press", () => {
    const win = new EventTarget();
    const { el, classes } = fakeElement();
    markProgrammaticFocus(el, win);
    expect(classes.has(PROGRAMMATIC_FOCUS_CLASS)).toBe(true);
    win.dispatchEvent(new Event("keydown"));
    expect(classes.has(PROGRAMMATIC_FOCUS_CLASS)).toBe(false);
  });

  it("a pointer press clears it too, and the listeners go with it", () => {
    const win = new EventTarget();
    const { el, classes } = fakeElement();
    markProgrammaticFocus(el, win);
    win.dispatchEvent(new Event("pointerdown"));
    expect(classes.has(PROGRAMMATIC_FOCUS_CLASS)).toBe(false);
    classes.add(PROGRAMMATIC_FOCUS_CLASS); // a later mark by someone else
    win.dispatchEvent(new Event("keydown"));
    expect(classes.has(PROGRAMMATIC_FOCUS_CLASS)).toBe(true);
  });

  it("many marks without input: one pending clear, only the newest heading marked", () => {
    let listeners = 0;
    class CountingTarget extends EventTarget {
      addEventListener(...args: Parameters<EventTarget["addEventListener"]>): void {
        listeners++;
        super.addEventListener(...args);
      }
      removeEventListener(...args: Parameters<EventTarget["removeEventListener"]>): void {
        listeners--;
        super.removeEventListener(...args);
      }
    }
    const win = new CountingTarget();
    const marked = Array.from({ length: 5 }, () => fakeElement());
    for (const { el } of marked) markProgrammaticFocus(el, win);
    expect(listeners).toBe(2);
    expect(marked.map(({ classes }) => classes.has(PROGRAMMATIC_FOCUS_CLASS))).toEqual([false, false, false, false, true]);
    win.dispatchEvent(new Event("keydown"));
    expect(listeners).toBe(0);
    expect(marked[4].classes.has(PROGRAMMATIC_FOCUS_CLASS)).toBe(false);
  });

  it("the class name is the one theme.css styles", () => {
    expect(PROGRAMMATIC_FOCUS_CLASS).toBe("programmatic-focus");
  });
});
