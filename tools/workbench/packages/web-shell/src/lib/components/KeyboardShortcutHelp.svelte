<!-- SPDX-License-Identifier: Apache-2.0
     Copyright (c) 2024-2026 We-Amp B.V. -->

<script lang="ts">
  import { SHORTCUTS, shortcutLabel } from '#lib/keyboard-shortcuts.ts';

  let {
    visible = false,
    onClose,
  }: {
    visible?: boolean;
    onClose?: () => void;
  } = $props();
</script>

{#if visible}
  <!-- svelte-ignore a11y_no_static_element_interactions -->
  <div
    class="shortcut-backdrop"
    onclick={onClose}
    onkeydown={(e) => {
      if (e.key === 'Escape') onClose?.();
    }}
    role="presentation"
  >
    <!-- svelte-ignore a11y_click_events_have_key_events -->
    <div
      class="shortcut-popover"
      onclick={(e) => e.stopPropagation()}
      role="dialog"
      aria-label="Keyboard shortcuts"
      tabindex="-1"
    >
      <div class="shortcut-header">
        <span class="shortcut-title">Keyboard Shortcuts</span>
        <button
          class="ps-btn ps-btn-ghost close-btn"
          onclick={onClose}
          aria-label="Close"
        >
          &times;
        </button>
      </div>
      <table class="shortcut-table">
        <tbody>
          {#each SHORTCUTS as entry}
            <tr>
              <td class="shortcut-key">
                <kbd>{shortcutLabel(entry)}</kbd>
              </td>
              <td class="shortcut-desc">{entry.description}</td>
            </tr>
          {/each}
        </tbody>
      </table>
    </div>
  </div>
{/if}

<style>
  .shortcut-backdrop {
    position: fixed;
    top: 0;
    left: 0;
    right: 0;
    bottom: 0;
    z-index: 200;
    background: rgba(0, 0, 0, 0.25);
    display: flex;
    align-items: center;
    justify-content: center;
  }

  .shortcut-popover {
    background: var(--ps-bg-primary);
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-radius-md);
    box-shadow: 0 8px 24px rgba(0, 0, 0, 0.2);
    min-width: 280px;
    max-width: 400px;
    padding: var(--ps-space-md) var(--ps-space-lg);
  }

  .shortcut-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    margin-bottom: var(--ps-space-md);
  }

  .shortcut-title {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size);
    font-weight: 600;
    color: var(--ps-fg-primary);
  }

  .close-btn {
    font-size: 1.2em;
    padding: 0 var(--ps-space-xs);
    line-height: 1;
  }

  .shortcut-table {
    width: 100%;
    border-collapse: collapse;
  }

  .shortcut-table tr {
    border-bottom: 1px solid var(--ps-border);
  }

  .shortcut-table tr:last-child {
    border-bottom: none;
  }

  .shortcut-key {
    padding: var(--ps-space-xs) var(--ps-space-sm) var(--ps-space-xs) 0;
    white-space: nowrap;
  }

  .shortcut-key kbd {
    display: inline-block;
    padding: 2px var(--ps-space-sm);
    font-family: var(--ps-font-mono);
    font-size: var(--ps-font-size-sm);
    background: var(--ps-bg-secondary);
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-radius-sm);
    color: var(--ps-fg-primary);
  }

  .shortcut-desc {
    padding: var(--ps-space-xs) 0;
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-secondary);
  }
</style>
