<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2024-2026 We-Amp B.V.
-->

<script lang="ts">
  // Shown while the console cannot reach the server. Every page keeps its
  // last data and retries on its own, less often the longer the outage lasts;
  // "Retry now" asks every open panel again at once.
  import { onMount } from "svelte";
  import { connection, type ConnectionView } from "$lib/api/connection";
  import { pollers } from "$lib/api/poller";
  import { useConsole } from "$lib/api/context";

  const { api } = useConsole();

  let view = $state<ConnectionView>(connection.view);
  let retrying = $state(false);

  onMount(() =>
    connection.subscribe((v) => {
      view = v;
    }),
  );

  // Every open page's pollers may hit only optimizer endpoints (Optimizer
  // status): a plain 5xx there is neutral for the
  // connection state, so refreshing them alone can never prove the server
  // is back. Reading the configuration -- the same read the connection
  // monitor already treats as proof of reachability -- always accompanies
  // the refresh, regardless of which pages are open or what they poll.
  function probe(): Promise<void> {
    return api.getConfig().then(
      () => {},
      () => {},
    );
  }

  function retryNow(): void {
    retrying = true;
    Promise.all([pollers.refreshAll(), probe()]).finally(() => {
      retrying = false;
    });
  }
</script>

{#if view.status === "reconnecting"}
  <div class="connection-banner" role="alert" data-testid="connection-banner">
    <p class="connection-text">
      <strong>Cannot reach the server.</strong>
      The console keeps trying, less often the longer this lasts{#if view.lastAnsweredAt !== null}; the figures below are from {new Date(view.lastAnsweredAt).toLocaleTimeString()}{/if}.
    </p>
    <button type="button" class="btn btn-primary" onclick={retryNow} disabled={retrying}>
      {retrying ? "Retrying…" : "Retry now"}
    </button>
  </div>
{/if}

<style>
  .connection-banner {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    justify-content: space-between;
    gap: var(--ps-space-sm);
    margin-bottom: var(--ps-space-md);
    padding: var(--ps-space-sm) var(--ps-space-md);
    border: 1px solid var(--ps-border);
    border-left: 4px solid var(--ps-error);
    border-radius: var(--ps-border-radius);
    background: var(--ps-bg-secondary);
    color: var(--ps-text);
  }

  .connection-text {
    margin: 0;
    font-size: var(--ps-font-size-sm);
  }
</style>
