<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2024-2026 We-Amp B.V.
-->

<script lang="ts">
  // The list of keyboard shortcuts, as a native modal dialog: Escape closes
  // it and the browser returns focus to where it was. Tab is trapped here
  // rather than left to the browser: showModal() does not reliably keep
  // focus inside a dialog with a single focusable control. The Versions
  // section repeats About's block; the optimizer is asked once, on the first
  // open.
  import { useConsole } from "$lib/api/context";
  import { SHORTCUT_HELP } from "$lib/shortcuts";
  import { optimizerVersionsStatus, optimizerVersionText, type OptimizerVersionsStatus } from "$lib/utils/versions";

  let { open, onclose }: { open: boolean; onclose: () => void } = $props();
  let dialog: HTMLDialogElement | undefined = $state();

  const { api } = useConsole();
  const appVersion = import.meta.env.VITE_APP_VERSION ?? "dev";
  let optimizerVersions = $state<OptimizerVersionsStatus | null>(null);
  let asked = false;

  $effect(() => {
    if (open && !asked) {
      asked = true;
      void optimizerVersionsStatus(() => api.daemonHealth()).then((status) => {
        optimizerVersions = status;
      });
    }
  });

  $effect(() => {
    if (dialog === undefined) return;
    if (open && !dialog.open) dialog.showModal();
    if (!open && dialog.open) dialog.close();
  });

  function trapTab(e: KeyboardEvent): void {
    if (e.key !== "Tab" || dialog === undefined) return;
    const focusable = [...dialog.querySelectorAll<HTMLElement>("button, a[href], input, select, textarea")];
    if (focusable.length === 0) return;
    const first = focusable[0];
    const last = focusable[focusable.length - 1];
    const edge = e.shiftKey ? first : last;
    const wrapTo = e.shiftKey ? last : first;
    // At the edge control, or already outside the dialog's own controls
    // (a `<dialog>` with a single focusable descendant does not reliably
    // keep Tab from leaving it): wrap back inside instead.
    if (document.activeElement === edge || !focusable.includes(document.activeElement as HTMLElement)) {
      e.preventDefault();
      wrapTo.focus();
    }
  }
</script>

<dialog bind:this={dialog} class="shortcut-help" aria-labelledby="shortcut-help-title" onclose={onclose} onkeydown={trapTab}>
  <h2 id="shortcut-help-title">Keyboard shortcuts</h2>
  <dl>
    {#each SHORTCUT_HELP as item (item.keys)}
      <div class="row">
        <dt><kbd>{item.keys}</kbd></dt>
        <dd>{item.description}</dd>
      </div>
    {/each}
  </dl>
  <h3 id="shortcut-versions-title">Versions</h3>
  <dl class="versions" data-testid="shortcut-versions" aria-labelledby="shortcut-versions-title">
    <div class="row">
      <dt>Module build</dt>
      <dd class="mono-value">{appVersion}</dd>
    </div>
    <div class="row">
      <dt>Optimizer</dt>
      <dd class="mono-value">{optimizerVersionText(optimizerVersions)}</dd>
    </div>
    <div class="row">
      <dt>Console build</dt>
      <dd class="mono-value">{appVersion}</dd>
    </div>
  </dl>
  <button type="button" class="close btn btn-secondary" onclick={onclose}>Close</button>
</dialog>

<style>
  .shortcut-help {
    margin: auto; /* the global reset zeroes the UA centring */
    max-width: 28rem;
    padding: var(--ps-space-lg);
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius-lg);
    background: var(--ps-surface);
    color: var(--ps-text);
  }

  .shortcut-help::backdrop {
    background: rgba(0, 0, 0, 0.4);
  }

  h2 {
    margin: 0 0 var(--ps-space-md);
    font-size: var(--ps-font-size-lg);
  }

  h3 {
    margin: 0 0 var(--ps-space-sm);
    font-size: var(--ps-font-size-base);
  }

  dl {
    display: grid;
    gap: var(--ps-space-xs);
    margin: 0 0 var(--ps-space-md);
  }

  .row {
    display: grid;
    grid-template-columns: 5rem 1fr;
    gap: var(--ps-space-sm);
  }

  dd {
    margin: 0;
  }

  .versions .row {
    grid-template-columns: 8rem 1fr;
  }

  .versions dt {
    color: var(--ps-text-secondary);
  }

  .versions dd {
    overflow-wrap: anywhere;
  }

  kbd {
    font-family: var(--ps-font-mono);
    padding: 0 var(--ps-space-xs);
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius);
    background: var(--ps-bg-secondary);
  }

</style>
