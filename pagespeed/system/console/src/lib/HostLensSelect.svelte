<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2024-2026 We-Amp B.V.
-->

<script lang="ts">
  // The host lens control: all hosts, or one host the console has seen.
  // Host names are visitor-controlled: here they are option text and a
  // bound value only -- never markup, never a style.
  interface Props {
    id: string;
    testid: string;
    host: string | null;
    options: readonly string[];
    onselect: (value: string) => void;
  }

  let { id, testid, host, options, onselect }: Props = $props();
</script>

<span class="host-lens" data-testid={testid}>
  <label for={id}>Host</label>
  <select {id} value={host ?? ""} onchange={(e) => onselect(e.currentTarget.value)}>
    <option value="">All hosts</option>
    {#each options as option (option)}
      <option value={option}>{option}</option>
    {/each}
  </select>
</span>

<style>
  .host-lens {
    display: inline-flex;
    align-items: center;
    gap: var(--ps-space-sm);
    font-size: var(--ps-font-size-xs);
    font-weight: 600;
    min-width: 0;
  }

  /* One width on every page, whatever hosts it lists, so the top bar's
     right group never shifts when navigating; a long name is cut short. */
  select {
    width: var(--ps-lens-width);
    max-width: 100%;
    min-width: 0;
    padding: 2px var(--ps-space-sm);
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius);
    background: var(--ps-bg);
    color: var(--ps-text);
    font-size: var(--ps-font-size-xs);
    text-overflow: ellipsis;
  }
</style>
