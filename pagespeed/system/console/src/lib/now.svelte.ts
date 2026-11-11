// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * The current time as a reactive value, for text that ages on screen. One
 * shared once-a-second tick (lib/utils/ticker.ts) runs only while something
 * on the page reads `clock.now` reactively, and never in a hidden tab. It
 * makes no request, so it is not polling; data still refreshes only through
 * usePolling.
 */

import { createSubscriber } from "svelte/reactivity";
import { documentVisibility, realClock } from "$lib/api/poller";
import { Ticker } from "$lib/utils/ticker";

const ticker = new Ticker(1000, realClock, documentVisibility());

const subscribe = createSubscriber((update) => ticker.start(update));

export const clock = {
  /** Epoch ms. Read in a template or a $derived, it is read again every second. */
  get now(): number {
    subscribe();
    return realClock.now();
  },
};
