<!-- SPDX-License-Identifier: Apache-2.0
     Copyright (c) 2024-2026 We-Amp B.V. -->

<script lang="ts">
  import { getContext, onDestroy } from 'svelte';
  import type {
    ApiTransport,
    ConnectionManager,
    ConnectionState,
    ConfigResponse,
    ConfigPatchResponse,
  } from '@pagespeed/api-client';
  import type { Readable } from 'svelte/store';
  import ConfigPanel from '#lib/components/ConfigPanel.svelte';
  import ConfigSnapshots from '#lib/components/ConfigSnapshots.svelte';
  import {
    exportConfigFile,
    generateCliFlags,
    copyToClipboard,
    readConfigFile,
  } from '#lib/config-persistence.ts';
  import { DEFAULT_VALUES, changedFields } from '#lib/config-presets.ts';

  // -- Context ---------------------------------------------------------------

  const transport = getContext<ApiTransport>('api');
  const connectionManager = getContext<ConnectionManager>('connection');
  const connState: Readable<ConnectionState> = connectionManager.state;

  // -- State -----------------------------------------------------------------

  let config = $state<ConfigResponse | null>(null);
  let serverConfig = $state<ConfigResponse | null>(null);
  let connected = $state(false);

  // Export/import UI state
  let exportDesc = $state('');
  let exportMenuOpen = $state(false);
  let importError = $state<string | null>(null);
  let importSuccess = $state<string | null>(null);
  let clipboardSuccess = $state<string | null>(null);
  let importing = $state(false);

  let fileInputEl: HTMLInputElement | undefined = $state(undefined);

  // -- Subscriptions ---------------------------------------------------------

  const unsubConn = connState.subscribe((s) => {
    connected = s === 'connected';
  });

  // -- Lifecycle -------------------------------------------------------------

  onDestroy(() => {
    unsubConn();
  });

  // -- Export actions --------------------------------------------------------

  function handleExportFile() {
    if (!config) return;
    exportConfigFile(config, exportDesc.trim());
    exportMenuOpen = false;
    exportDesc = '';
  }

  async function handleCopyCliFlags() {
    if (!config) return;
    const flags = generateCliFlags(config, DEFAULT_VALUES);
    if (!flags) {
      clipboardSuccess = 'No settings differ from defaults.';
    } else {
      const ok = await copyToClipboard(flags);
      clipboardSuccess = ok
        ? 'CLI flags copied to clipboard.'
        : 'Failed to copy to clipboard.';
    }
    setTimeout(() => {
      clipboardSuccess = null;
    }, 3000);
  }

  // -- Import actions --------------------------------------------------------

  function handleImportClick() {
    fileInputEl?.click();
  }

  async function handleFileSelected(event: Event) {
    const input = event.target as HTMLInputElement;
    const file = input.files?.[0];
    if (!file) return;

    importing = true;
    importError = null;
    importSuccess = null;

    const result = await readConfigFile(file);

    if (!result.valid || !result.config) {
      importError = result.errors.join(' ');
      importing = false;
      // Reset file input.
      input.value = '';
      return;
    }

    // Apply imported config via PATCH.
    try {
      const patch = serverConfig
        ? changedFields(result.config, serverConfig)
        : (result.config as Partial<ConfigResponse>);

      if (Object.keys(patch).length === 0) {
        importSuccess = 'Imported config is identical to the current config.';
      } else {
        const resp = await transport.patch<ConfigPatchResponse>(
          '/v1/config',
          patch,
        );
        config = { ...resp.config };
        serverConfig = { ...resp.config };
        importSuccess = `Imported and applied ${Object.keys(patch).length} setting(s)${result.description ? ` from "${result.description}"` : ''}.`;
      }
    } catch (err) {
      importError =
        err instanceof Error ? err.message : 'Failed to apply imported config.';
    } finally {
      importing = false;
      input.value = '';
    }

    setTimeout(() => {
      importSuccess = null;
    }, 4000);
  }

  function handleSnapshotRestore(restoredConfig: ConfigResponse) {
    config = { ...restoredConfig };
    // The ConfigPanel will detect the changes and let the user Apply.
  }
</script>

<!-- Hidden file input for import -->
<input
  type="file"
  accept=".pagespeed.json,.json"
  style="display: none"
  bind:this={fileInputEl}
  onchange={handleFileSelected}
  aria-label="Import config file"
/>

<!-- Export/Import toolbar -->
<div class="config-toolbar">
  <div class="toolbar-left">
    <button
      class="ps-btn ps-btn-secondary"
      onclick={() => (exportMenuOpen = !exportMenuOpen)}
      disabled={!config}
      aria-expanded={exportMenuOpen}
    >
      Export
    </button>
    <button
      class="ps-btn ps-btn-secondary"
      onclick={handleImportClick}
      disabled={!connected || importing}
    >
      {#if importing}
        <span class="ps-spinner" style="width:14px;height:14px" aria-hidden="true"></span>
        Importing...
      {:else}
        Import
      {/if}
    </button>
    <button
      class="ps-btn ps-btn-ghost"
      onclick={handleCopyCliFlags}
      disabled={!config}
      title="Copy CLI flags for changed settings to clipboard"
    >
      Copy CLI Flags
    </button>
  </div>
  {#if clipboardSuccess}
    <span class="toolbar-feedback toolbar-feedback-success">{clipboardSuccess}</span>
  {/if}
  {#if importError}
    <span class="toolbar-feedback toolbar-feedback-error">{importError}</span>
  {/if}
  {#if importSuccess}
    <span class="toolbar-feedback toolbar-feedback-success">{importSuccess}</span>
  {/if}
</div>

<!-- Export description popover -->
{#if exportMenuOpen}
  <div class="export-popover">
    <label class="export-label" for="export-desc">Description (optional)</label>
    <input
      id="export-desc"
      type="text"
      class="ps-input"
      placeholder="e.g., Production tuning 2024-01-15"
      bind:value={exportDesc}
      onkeydown={(e) => {
        if (e.key === 'Enter') handleExportFile();
        if (e.key === 'Escape') exportMenuOpen = false;
      }}
    />
    <div class="export-popover-actions">
      <button class="ps-btn ps-btn-primary" onclick={handleExportFile}>
        Download .pagespeed.json
      </button>
      <button
        class="ps-btn ps-btn-ghost"
        onclick={() => (exportMenuOpen = false)}
      >
        Cancel
      </button>
    </div>
  </div>
{/if}

<ConfigPanel bind:config bind:serverConfig />

<!-- Snapshots panel -->
<div class="snapshots-section">
  <ConfigSnapshots
    currentConfig={config}
    onrestore={handleSnapshotRestore}
  />
</div>

<style>
  .config-toolbar {
    display: flex;
    align-items: center;
    flex-wrap: wrap;
    gap: var(--ps-space-sm);
    padding: var(--ps-space-md) var(--ps-space-xl);
    max-width: 900px;
    border-bottom: 1px solid var(--ps-border);
  }

  .toolbar-left {
    display: flex;
    align-items: center;
    gap: var(--ps-space-xs);
  }

  .toolbar-feedback {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    padding: var(--ps-space-xs) var(--ps-space-sm);
    border-radius: var(--ps-radius-sm);
  }

  .toolbar-feedback-success {
    background: var(--ps-success-bg);
    color: var(--ps-success);
  }

  .toolbar-feedback-error {
    background: var(--ps-error-bg);
    color: var(--ps-error);
  }

  .export-popover {
    display: flex;
    flex-direction: column;
    gap: var(--ps-space-sm);
    padding: var(--ps-space-md) var(--ps-space-xl);
    max-width: 900px;
    background: var(--ps-bg-primary);
    border-bottom: 1px solid var(--ps-border);
  }

  .export-label {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    font-weight: 500;
    color: var(--ps-fg-secondary);
  }

  .export-popover-actions {
    display: flex;
    gap: var(--ps-space-sm);
  }

  .snapshots-section {
    padding: var(--ps-space-lg) var(--ps-space-xl);
    max-width: 900px;
    border-top: 1px solid var(--ps-border);
  }

  @media (max-width: 767px) {
    .config-toolbar {
      padding: var(--ps-space-sm) var(--ps-space-md);
    }

    .toolbar-left {
      flex-wrap: wrap;
    }

    .export-popover {
      padding: var(--ps-space-sm) var(--ps-space-md);
    }

    .export-popover-actions {
      flex-direction: column;
    }

    .snapshots-section {
      padding: var(--ps-space-md);
    }
  }
</style>
