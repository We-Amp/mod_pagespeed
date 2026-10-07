// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { getContext, setContext } from "svelte";
import type { AdminApiClient } from "./client";
import type { ConsoleScope } from "$lib/console-scope.svelte";
import type { HostLens } from "$lib/host-lens.svelte";

/** What every page gets from the console shell. */
export interface ConsoleContext {
  api: AdminApiClient;
  scope: ConsoleScope;
  lens: HostLens;
}

const KEY = Symbol("admin-console");

/** Called once by the shell (App.svelte) during initialisation. */
export function provideConsole(ctx: ConsoleContext): void {
  setContext(KEY, ctx);
}

/** The shell's client, scope and host lens; call during a page's initialisation. */
export function useConsole(): ConsoleContext {
  const ctx = getContext<ConsoleContext | undefined>(KEY);
  if (ctx === undefined) throw new Error("useConsole() called outside the console shell");
  return ctx;
}
