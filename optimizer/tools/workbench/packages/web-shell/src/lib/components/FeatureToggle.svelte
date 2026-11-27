<!-- SPDX-License-Identifier: Apache-2.0
     Copyright (c) 2024-2026 We-Amp B.V. -->

<script lang="ts">
  let {
    label,
    description = '',
    checked = $bindable(),
    changed = false,
    disabled = false,
  }: {
    label: string;
    description?: string;
    checked: boolean;
    changed?: boolean;
    disabled?: boolean;
  } = $props();
</script>

<label class="feature-toggle" class:feature-toggle-disabled={disabled}>
  <div class="toggle-track" class:toggle-track-on={checked}>
    <input
      type="checkbox"
      class="toggle-input"
      bind:checked
      {disabled}
      aria-label={label}
    />
    <span class="toggle-thumb" class:toggle-thumb-on={checked}></span>
  </div>

  <div class="toggle-content">
    <span class="toggle-label">
      {label}
      {#if changed}
        <span class="changed-dot" title="Modified from server value"></span>
      {/if}
    </span>
    {#if description}
      <span class="toggle-description">{description}</span>
    {/if}
  </div>
</label>

<style>
  .feature-toggle {
    display: flex;
    align-items: flex-start;
    gap: var(--ps-space-sm);
    cursor: pointer;
    padding: var(--ps-space-xs) 0;
  }

  .feature-toggle-disabled {
    opacity: 0.5;
    cursor: not-allowed;
  }

  /* Track */
  .toggle-track {
    position: relative;
    flex-shrink: 0;
    width: 32px;
    height: 18px;
    margin-top: 1px;
    background: var(--ps-border-input);
    border-radius: 9px;
    transition: background-color 0.15s ease;
  }

  .toggle-track-on {
    background: var(--ps-accent);
  }

  /* Hidden native checkbox */
  .toggle-input {
    position: absolute;
    opacity: 0;
    width: 100%;
    height: 100%;
    top: 0;
    left: 0;
    cursor: pointer;
    margin: 0;
  }

  .toggle-input:focus-visible + .toggle-thumb {
    outline: 2px solid var(--ps-border-focus);
    outline-offset: 2px;
  }

  .toggle-input:disabled {
    cursor: not-allowed;
  }

  /* Thumb */
  .toggle-thumb {
    position: absolute;
    top: 2px;
    left: 2px;
    width: 14px;
    height: 14px;
    background: var(--ps-bg-primary);
    border-radius: 50%;
    transition: transform 0.15s ease;
    pointer-events: none;
  }

  .toggle-thumb-on {
    transform: translateX(14px);
  }

  /* Text content */
  .toggle-content {
    display: flex;
    flex-direction: column;
    gap: 1px;
    min-width: 0;
  }

  .toggle-label {
    display: flex;
    align-items: center;
    gap: var(--ps-space-xs);
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size);
    font-weight: 500;
    color: var(--ps-fg-primary);
    line-height: 1.3;
  }

  .changed-dot {
    display: inline-block;
    width: 6px;
    height: 6px;
    border-radius: 50%;
    background: var(--ps-accent);
    flex-shrink: 0;
  }

  .toggle-description {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-muted);
    line-height: 1.3;
  }
</style>
