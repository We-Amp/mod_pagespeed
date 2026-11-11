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
    /** False on a page without per-host figures: a chosen host is then said not to apply. */
    applied?: boolean;
    onselect: (value: string) => void;
  }

  import { TERMS } from "$lib/data/terms";

  let { id, testid, host, options, applied = true, onselect }: Props = $props();
  // Nothing to choose from yet: say so, rather than offer a list of one.
  let empty = $derived(options.length === 0 && host === null);
  let notApplied = $derived(host !== null && !applied);
</script>

<span class="host-lens" data-testid={testid}>
  <label for={id}>{TERMS.hostFilter}</label>
  <select
    {id}
    value={host ?? ""}
    disabled={empty}
    aria-describedby={notApplied ? `${id}-note` : undefined}
    onchange={(e) => onselect(e.currentTarget.value)}
  >
    <option value="">{empty ? "No hosts seen yet" : "All hosts"}</option>
    {#each options as option (option)}
      <option value={option}>{option}</option>
    {/each}
  </select>
  {#if notApplied}
    <span class="lens-off" id="{id}-note" data-testid="{testid}-note">(not applied on this page)</span>
  {/if}
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

  label {
    white-space: nowrap;
  }

  /* One width on every page, whatever hosts it lists, so the top bar's
     right group never shifts when navigating; a long name is cut short.
     The compact control height: the band is a toolbar, not a form. */
  select {
    width: var(--ps-lens-width);
    max-width: 100%;
    min-width: 0;
    height: var(--ps-control-height-compact);
    padding: 0 var(--ps-space-sm);
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius);
    background: var(--ps-bg);
    color: var(--ps-text);
    font-size: var(--ps-font-size-xs);
    text-overflow: ellipsis;
  }

  /* Two short lines at most, so the band keeps its height. */
  .lens-off {
    max-width: 7rem;
    font-weight: 400;
    line-height: 1.2;
  }
</style>
