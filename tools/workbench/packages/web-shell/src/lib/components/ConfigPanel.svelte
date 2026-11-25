<!-- SPDX-License-Identifier: Apache-2.0
     Copyright (c) 2024-2026 We-Amp B.V. -->

<script lang="ts">
  import { getContext, onMount, onDestroy } from 'svelte';
  import type { Readable } from 'svelte/store';
  import type {
    ApiTransport,
    ConnectionManager,
    ConnectionState,
    ConfigResponse,
    ConfigPatchResponse,
  } from '@pagespeed/api-client';
  import QualitySlider from './QualitySlider.svelte';
  import FeatureToggle from './FeatureToggle.svelte';
  import HelpIcon from './HelpIcon.svelte';
  import {
    PRESETS,
    DEFAULT_VALUES,
    IMAGE_QUALITY_FIELDS,
    SAVEDATA_QUALITY_FIELDS,
    SSIMULACRA2_FIELDS,
    IMAGE_FEATURE_TOGGLES,
    LEARNED_QUALITY_TOGGLES,
    HTML_FEATURE_TOGGLES,
    HTML_MODE_SELECT,
    CSS_FEATURE_TOGGLES,
    JS_FEATURE_TOGGLES,
    COMPRESSION_FIELDS,
    VIEWPORT_FIELDS,
    LIMIT_FIELDS,
    GENERAL_FEATURE_TOGGLES,
    SVG_FIELDS,
    SVG_FEATURE_TOGGLES,
    SVG_PRESET_FIELD,
    SVG_MODE_FIELD,
    CACHE_MODE_SELECT,
    QUALITY_CAP_FIELDS,
    QUALITY_CAP_TOGGLES,
    BROWSER_FEATURE_TOGGLES,
    BROWSER_FIELDS,
    changedFields,
    changedFieldCount,
    applyPreset,
  } from '#lib/config-presets.ts';
  import type { ConfigPreset } from '#lib/config-presets.ts';

  // -- Context ---------------------------------------------------------------

  const transport = getContext<ApiTransport>('api');
  const connectionManager = getContext<ConnectionManager>('connection');
  const connState: Readable<ConnectionState> = connectionManager.state;

  // -- Props -----------------------------------------------------------------

  interface Props {
    /**
     * Working copy of the config. Owned by the parent Config page so that the
     * snapshots panel, import, and export all observe the same live edits this
     * panel makes; ConfigPanel reads and writes it through the binding.
     */
    config?: ConfigResponse | null;
    /** Last-known server values, parent-owned — used for change tracking. */
    serverConfig?: ConfigResponse | null;
  }

  let { config = $bindable(null), serverConfig = $bindable(null) }: Props = $props();

  // -- State -----------------------------------------------------------------

  /** Whether we're loading the initial config. */
  let loading = $state(true);

  /** Error message from the last operation. */
  let errorMessage = $state<string | null>(null);

  /** Success message (auto-dismiss). */
  let successMessage = $state<string | null>(null);

  /** Whether a save is in progress. */
  let saving = $state(false);

  /** Connection state. */
  let connected = $state(false);

  /** Collapsible section state. */
  let featuresOpen = $state(false);
  let advancedOpen = $state(false);

  /** Undo/redo history. */
  let history: ConfigResponse[] = $state([]);
  let historyIndex = $state(-1);

  /** Safety confirmation for aggressive quality changes. */
  let safetyConfirmOpen = $state(false);
  let pendingSafetyAction: (() => void) | null = $state(null);
  let safetyWarnings: string[] = $state([]);

  /** Flag to prevent pushing to history when undoing/redoing. */
  let isHistoryNav = false;

  // -- Derived ---------------------------------------------------------------

  let numChanges = $derived(
    config && serverConfig ? changedFieldCount(config, serverConfig) : 0,
  );

  let hasChanges = $derived(numChanges > 0);

  let canUndo = $derived(historyIndex > 0);
  let canRedo = $derived(historyIndex < history.length - 1);

  // -- Connection subscription -----------------------------------------------

  const unsubConn = connState.subscribe((s) => {
    connected = s === 'connected';
  });

  // -- Lifecycle -------------------------------------------------------------

  onMount(() => {
    loadConfig();
    if (typeof window !== 'undefined') {
      window.addEventListener('keydown', handleKeyboard);
    }
  });

  onDestroy(() => {
    unsubConn();
    if (typeof window !== 'undefined') {
      window.removeEventListener('keydown', handleKeyboard);
    }
  });

  // -- API -------------------------------------------------------------------

  async function loadConfig() {
    loading = true;
    errorMessage = null;
    try {
      const data = await transport.get<ConfigResponse>('/v1/config');
      config = { ...data };
      serverConfig = { ...data };
      // Initialize history with the loaded config.
      history = [{ ...data }];
      historyIndex = 0;
    } catch (err) {
      errorMessage =
        err instanceof Error ? err.message : 'Failed to load configuration. Check that the worker is running and accessible.';
    } finally {
      loading = false;
    }
  }

  /**
   * Check for aggressive quality changes and show confirmation if needed.
   * Returns true if the action should proceed immediately, false if a
   * confirmation dialog was shown.
   */
  function checkSafetyGate(action: () => void): boolean {
    if (!config || !serverConfig) return true;
    const warnings: string[] = [];

    // Check SSIMULACRA2 target below 50.
    const ssim = (config as Record<string, unknown>)['target_ssimulacra2'];
    if (typeof ssim === 'number' && ssim < 50) {
      warnings.push(
        `SSIMULACRA2 target is ${ssim} (below 50). This will produce visibly degraded images.`,
      );
    }

    // Check quality drops > 30 points.
    const qualityKeys = [
      'jpeg_quality',
      'webp_quality',
      'avif_quality',
      'savedata_jpeg_quality',
      'savedata_webp_quality',
      'savedata_avif_quality',
    ];
    for (const key of qualityKeys) {
      const cur = (config as Record<string, unknown>)[key];
      const prev = (serverConfig as Record<string, unknown>)[key];
      if (typeof cur === 'number' && typeof prev === 'number') {
        const drop = prev - cur;
        if (drop > 30) {
          warnings.push(
            `${key.replace(/_/g, ' ')} dropped by ${drop} points (${prev} \u2192 ${cur}).`,
          );
        }
      }
    }

    if (warnings.length > 0) {
      safetyWarnings = warnings;
      pendingSafetyAction = action;
      safetyConfirmOpen = true;
      return false;
    }
    return true;
  }

  function handleSafetyConfirm() {
    safetyConfirmOpen = false;
    if (pendingSafetyAction) {
      pendingSafetyAction();
      pendingSafetyAction = null;
    }
  }

  function handleSafetyCancel() {
    safetyConfirmOpen = false;
    pendingSafetyAction = null;
    safetyWarnings = [];
  }

  async function applyConfig() {
    if (!config || !serverConfig || !hasChanges) return;
    saving = true;
    errorMessage = null;
    successMessage = null;
    try {
      const patch = changedFields(config, serverConfig);
      const result = await transport.patch<ConfigPatchResponse>(
        '/v1/config',
        patch,
      );
      // Update both config and serverConfig from the response. The worker is
      // authoritative: result.config reflects what actually took effect, and
      // result.rejected lists fields it refused (e.g. out-of-range values).
      config = { ...result.config };
      serverConfig = { ...result.config };
      pushHistory(config);

      const rejectedKeys = Object.keys(result.rejected ?? {});
      const appliedCount = Object.keys(patch).length - rejectedKeys.length;
      if (rejectedKeys.length > 0) {
        // Never report success for values the worker rejected — the form and the
        // live config would silently diverge. Surface the rejected fields (with
        // the worker's reason when it sent one) as an error.
        const detail = rejectedKeys
          .map((k) => {
            const reason = (result.rejected as Record<string, unknown>)[k];
            return typeof reason === 'string' && reason ? `${k} (${reason})` : k;
          })
          .join(', ');
        errorMessage =
          `Worker rejected ${rejectedKeys.length} setting(s): ${detail}.` +
          (appliedCount > 0 ? ` ${appliedCount} other setting(s) applied.` : '');
      } else if (result.warnings.length > 0) {
        successMessage = `Applied with warnings: ${result.warnings.join(', ')}`;
        autoHideSuccess();
      } else {
        successMessage = `${appliedCount} setting(s) applied`;
        autoHideSuccess();
      }
    } catch (err) {
      errorMessage =
        err instanceof Error ? err.message : 'Failed to apply configuration. The worker may have rejected invalid values.';
    } finally {
      saving = false;
    }
  }

  async function applyAndReoptimize() {
    await applyConfig();
    // After applying, try to trigger a reprocess if there's no error.
    if (!errorMessage) {
      successMessage = 'Configuration applied. To re-optimize specific URLs, use Reprocess in the URL Inspector.';
      autoHideSuccess();
    }
  }

  function resetToDefaults() {
    if (!config) return;
    config = { ...config, ...DEFAULT_VALUES };
    pushHistory(config);
  }

  function selectPreset(preset: ConfigPreset) {
    if (!config) return;
    config = applyPreset(config, preset);
    pushHistory(config);
  }

  // -- Undo/Redo -------------------------------------------------------------

  function pushHistory(cfg: ConfigResponse) {
    if (isHistoryNav) return;
    // Truncate any redo history beyond current index.
    history = [...history.slice(0, historyIndex + 1), { ...cfg }];
    historyIndex = history.length - 1;
  }

  function undo() {
    if (!canUndo) return;
    isHistoryNav = true;
    historyIndex--;
    config = { ...history[historyIndex] };
    isHistoryNav = false;
  }

  function redo() {
    if (!canRedo) return;
    isHistoryNav = true;
    historyIndex++;
    config = { ...history[historyIndex] };
    isHistoryNav = false;
  }

  function handleKeyboard(e: KeyboardEvent) {
    const mod = e.metaKey || e.ctrlKey;
    if (mod && e.key === 'z' && !e.shiftKey) {
      e.preventDefault();
      undo();
    } else if (
      (mod && e.key === 'z' && e.shiftKey) ||
      (mod && e.key === 'y')
    ) {
      e.preventDefault();
      redo();
    }
  }

  // -- Helpers ---------------------------------------------------------------

  function autoHideSuccess() {
    setTimeout(() => {
      successMessage = null;
    }, 4000);
  }

  /**
   * For "disable_X" toggles, the UI should show "enabled" when the value
   * is false (not disabled). This helper inverts appropriately.
   */
  function getToggleValue(
    cfg: ConfigResponse,
    key: keyof ConfigResponse,
    invertDisplay?: boolean,
  ): boolean {
    const raw = cfg[key];
    if (typeof raw !== 'boolean') return false;
    return invertDisplay ? !raw : raw;
  }

  function setToggleValue(
    key: keyof ConfigResponse,
    value: boolean,
    invertDisplay?: boolean,
  ) {
    if (!config) return;
    const actual = invertDisplay ? !value : value;
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    (config as any)[key] = actual;
    // Trigger reactivity by reassigning.
    config = { ...config };
    pushHistory(config);
  }

  function isFieldChanged(key: keyof ConfigResponse): boolean {
    if (!config || !serverConfig) return false;
    return config[key] !== serverConfig[key];
  }

  function handleSliderInput() {
    // Debounce history pushes for slider drags.
    // We'll push on change (mouseup) instead via a separate handler.
  }

  function handleSliderChange() {
    if (config) {
      pushHistory(config);
    }
  }
</script>

{#if loading}
  <div class="config-loading">
    <div class="ps-card loading-card">
      <div class="ps-spinner" aria-label="Loading configuration"></div>
      <p class="loading-text">Loading configuration...</p>
    </div>
  </div>
{:else if errorMessage && !config}
  <div class="config-loading">
    <div class="ps-card loading-card">
      <p class="error-text">{errorMessage}</p>
      <button class="ps-btn ps-btn-primary" onclick={loadConfig}>
        Retry
      </button>
    </div>
  </div>
{:else if config}
  <div class="config-panel">
    <!-- Header -->
    <div class="config-header">
      <div class="config-header-left">
        <h1 class="config-title">Configuration</h1>
        {#if hasChanges}
          <span class="ps-badge ps-badge-info"
            >{numChanges} change{numChanges === 1 ? '' : 's'}</span
          >
        {/if}
      </div>
      <div class="config-header-right">
        <button
          class="ps-btn ps-btn-ghost"
          onclick={undo}
          disabled={!canUndo || !connected}
          title="Undo (Ctrl+Z)"
          aria-label="Undo"
        >
          <svg width="14" height="14" viewBox="0 0 16 16" fill="currentColor" aria-hidden="true">
            <path d="M2.5 8l4-4v2.5c4 0 6.5 1.5 7 5.5-1-3-3.5-4-7-4V10.5l-4-2.5z"/>
          </svg>
        </button>
        <button
          class="ps-btn ps-btn-ghost"
          onclick={redo}
          disabled={!canRedo || !connected}
          title="Redo (Ctrl+Shift+Z)"
          aria-label="Redo"
        >
          <svg width="14" height="14" viewBox="0 0 16 16" fill="currentColor" aria-hidden="true">
            <path d="M13.5 8l-4-4v2.5c-4 0-6.5 1.5-7 5.5 1-3 3.5-4 7-4V10.5l4-2.5z"/>
          </svg>
        </button>
      </div>
    </div>

    <!-- Status messages -->
    {#if errorMessage}
      <div class="config-message config-message-error" role="alert">
        {errorMessage}
      </div>
    {/if}
    {#if successMessage}
      <div class="config-message config-message-success" role="status">
        {successMessage}
      </div>
    {/if}
    {#if !connected}
      <div class="config-message config-message-warning" role="status">
        Disconnected from worker. Controls are disabled.
      </div>
    {/if}

    <!-- Presets -->
    <section class="config-section">
      <h2 class="section-heading">Presets</h2>
      <div class="preset-bar">
        {#each PRESETS as preset}
          <button
            class="ps-btn ps-btn-secondary preset-btn"
            disabled={!connected}
            onclick={() => selectPreset(preset)}
            title={preset.description}
          >
            {preset.name}
          </button>
        {/each}
      </div>
    </section>

    <!-- Image Quality (expanded by default) -->
    <section class="config-section">
      <h2 class="section-heading">Image Quality</h2>
      <div class="quality-grid">
        <div class="quality-column">
          <h3 class="quality-subheading">Standard</h3>
          {#each IMAGE_QUALITY_FIELDS as field}
            <QualitySlider
              label={field.label}
              bind:value={config[field.key]}
              min={field.min}
              max={field.max}
              step={field.step}
              warningBelow={field.warningBelow}
              warningAbove={field.warningAbove}
              changed={isFieldChanged(field.key)}
              disabled={!connected}
              tooltip={field.tooltip}
              oninput={handleSliderInput}
              onchange={handleSliderChange}
            />
          {/each}
        </div>
        <div class="quality-column">
          <h3 class="quality-subheading">Save-Data</h3>
          {#each SAVEDATA_QUALITY_FIELDS as field}
            <QualitySlider
              label={field.label}
              bind:value={config[field.key]}
              min={field.min}
              max={field.max}
              step={field.step}
              warningBelow={field.warningBelow}
              warningAbove={field.warningAbove}
              changed={isFieldChanged(field.key)}
              disabled={!connected}
              tooltip={field.tooltip}
              oninput={handleSliderInput}
              onchange={handleSliderChange}
            />
          {/each}
        </div>
      </div>

      <div class="quality-ssim">
        <h3 class="quality-subheading">SSIMULACRA2</h3>
        <div class="quality-ssim-fields">
          {#each SSIMULACRA2_FIELDS as field}
            <QualitySlider
              label={field.label}
              bind:value={config[field.key]}
              min={field.min}
              max={field.max}
              step={field.step}
              warningBelow={field.warningBelow}
              warningAbove={field.warningAbove}
              changed={isFieldChanged(field.key)}
              disabled={!connected}
              tooltip={field.tooltip}
              oninput={handleSliderInput}
              onchange={handleSliderChange}
            />
          {/each}
        </div>
      </div>

      <div class="quality-ssim">
        <h3 class="quality-subheading">Quality Capping</h3>
        {#each QUALITY_CAP_TOGGLES as field}
          <FeatureToggle
            label={field.label}
            description={field.description}
            checked={getToggleValue(config, field.key, field.invertDisplay)}
            changed={isFieldChanged(field.key)}
            disabled={!connected}
            onchange={() => {
              const current = getToggleValue(config, field.key, field.invertDisplay);
              setToggleValue(field.key, !current, field.invertDisplay);
            }}
          />
        {/each}
        <div class="quality-ssim-fields">
          {#each QUALITY_CAP_FIELDS as field}
            <QualitySlider
              label={field.label}
              bind:value={config[field.key]}
              min={field.min}
              max={field.max}
              step={field.step}
              changed={isFieldChanged(field.key)}
              disabled={!connected}
              tooltip={field.tooltip}
              oninput={handleSliderInput}
              onchange={handleSliderChange}
            />
          {/each}
        </div>
      </div>
    </section>

    <!-- Features (collapsible) -->
    <section class="config-section">
      <button
        class="section-toggle"
        onclick={() => (featuresOpen = !featuresOpen)}
        aria-expanded={featuresOpen}
      >
        <svg
          class="toggle-chevron"
          class:toggle-chevron-open={featuresOpen}
          width="12"
          height="12"
          viewBox="0 0 12 12"
          fill="currentColor"
          aria-hidden="true"
        >
          <path d="M4 2l4 4-4 4z" />
        </svg>
        <h2 class="section-heading section-heading-toggle">Features</h2>
        <span class="section-count">
          {IMAGE_FEATURE_TOGGLES.length +
            LEARNED_QUALITY_TOGGLES.length +
            HTML_FEATURE_TOGGLES.length +
            CSS_FEATURE_TOGGLES.length +
            JS_FEATURE_TOGGLES.length} toggles
        </span>
      </button>

      {#if featuresOpen}
        <div class="toggle-sections">
          <!-- Image Optimization -->
          <div class="toggle-group">
            <h3 class="toggle-group-heading">
              Image Optimization ({IMAGE_FEATURE_TOGGLES.length})
            </h3>
            {#each IMAGE_FEATURE_TOGGLES as field}
              <FeatureToggle
                label={field.label}
                description={field.description}
                checked={getToggleValue(config, field.key, field.invertDisplay)}
                changed={isFieldChanged(field.key)}
                disabled={!connected}
                onchange={() => {
                  const current = getToggleValue(config, field.key, field.invertDisplay);
                  setToggleValue(field.key, !current, field.invertDisplay);
                }}
              />
            {/each}
          </div>

          <!-- Learned Quality Prediction -->
          <div class="toggle-group">
            <h3 class="toggle-group-heading">
              Learned Quality Prediction ({LEARNED_QUALITY_TOGGLES.length})
            </h3>
            {#each LEARNED_QUALITY_TOGGLES as field}
              <FeatureToggle
                label={field.label}
                description={field.description}
                checked={getToggleValue(config, field.key, field.invertDisplay)}
                changed={isFieldChanged(field.key)}
                disabled={!connected}
                onchange={() => {
                  const current = getToggleValue(config, field.key, field.invertDisplay);
                  setToggleValue(field.key, !current, field.invertDisplay);
                }}
              />
            {/each}
          </div>

          <!-- HTML Transforms -->
          <div class="toggle-group">
            <h3 class="toggle-group-heading">
              HTML Transforms ({HTML_FEATURE_TOGGLES.length})
            </h3>
            <div class="number-field">
              <span class="number-label">
                {HTML_MODE_SELECT.label}
                {#if isFieldChanged(HTML_MODE_SELECT.key)}
                  <span class="changed-dot" title="Modified"></span>
                {/if}
                {#if HTML_MODE_SELECT.tooltip}
                  <HelpIcon tooltip={HTML_MODE_SELECT.tooltip} />
                {/if}
              </span>
              <select
                class="ps-select"
                bind:value={config[HTML_MODE_SELECT.key]}
                disabled={!connected}
                aria-label={HTML_MODE_SELECT.label}
                onchange={handleSliderChange}
              >
                {#each HTML_MODE_SELECT.options as opt}
                  <option value={opt.value}>{opt.label}</option>
                {/each}
              </select>
            </div>
            {#each HTML_FEATURE_TOGGLES as field}
              <FeatureToggle
                label={field.label}
                description={field.description}
                checked={getToggleValue(config, field.key, field.invertDisplay)}
                changed={isFieldChanged(field.key)}
                disabled={!connected}
                onchange={() => {
                  const current = getToggleValue(config, field.key, field.invertDisplay);
                  setToggleValue(field.key, !current, field.invertDisplay);
                }}
              />
            {/each}
          </div>

          <!-- CSS Transforms -->
          <div class="toggle-group">
            <h3 class="toggle-group-heading">
              CSS Transforms ({CSS_FEATURE_TOGGLES.length})
            </h3>
            {#each CSS_FEATURE_TOGGLES as field}
              <FeatureToggle
                label={field.label}
                description={field.description}
                checked={getToggleValue(config, field.key, field.invertDisplay)}
                changed={isFieldChanged(field.key)}
                disabled={!connected}
                onchange={() => {
                  const current = getToggleValue(config, field.key, field.invertDisplay);
                  setToggleValue(field.key, !current, field.invertDisplay);
                }}
              />
            {/each}
          </div>

          <!-- JS Transforms -->
          <div class="toggle-group">
            <h3 class="toggle-group-heading">
              JS Processing ({JS_FEATURE_TOGGLES.length})
            </h3>
            {#each JS_FEATURE_TOGGLES as field}
              <FeatureToggle
                label={field.label}
                description={field.description}
                checked={getToggleValue(config, field.key, field.invertDisplay)}
                changed={isFieldChanged(field.key)}
                disabled={!connected}
                onchange={() => {
                  const current = getToggleValue(config, field.key, field.invertDisplay);
                  setToggleValue(field.key, !current, field.invertDisplay);
                }}
              />
            {/each}
          </div>
        </div>
      {/if}
    </section>

    <!-- Advanced (collapsible) -->
    <section class="config-section">
      <button
        class="section-toggle"
        onclick={() => (advancedOpen = !advancedOpen)}
        aria-expanded={advancedOpen}
      >
        <svg
          class="toggle-chevron"
          class:toggle-chevron-open={advancedOpen}
          width="12"
          height="12"
          viewBox="0 0 12 12"
          fill="currentColor"
          aria-hidden="true"
        >
          <path d="M4 2l4 4-4 4z" />
        </svg>
        <h2 class="section-heading section-heading-toggle">Advanced</h2>
        <span class="section-count">
          compression, viewports
        </span>
      </button>

      {#if advancedOpen}
        <div class="advanced-sections">
          <!-- Cache Mode -->
          <div class="advanced-group">
            <h3 class="toggle-group-heading">Caching</h3>
            <div class="advanced-fields">
              <div class="number-field">
                <span class="number-label">
                  {CACHE_MODE_SELECT.label}
                  {#if isFieldChanged(CACHE_MODE_SELECT.key)}
                    <span class="changed-dot" title="Modified"></span>
                  {/if}
                  {#if CACHE_MODE_SELECT.tooltip}
                    <HelpIcon tooltip={CACHE_MODE_SELECT.tooltip} />
                  {/if}
                </span>
                <select
                  class="ps-select"
                  bind:value={config[CACHE_MODE_SELECT.key]}
                  disabled={!connected}
                  aria-label={CACHE_MODE_SELECT.label}
                  onchange={handleSliderChange}
                >
                  {#each CACHE_MODE_SELECT.options as opt}
                    <option value={opt.value}>{opt.label}</option>
                  {/each}
                </select>
              </div>
            </div>
          </div>

          <!-- Compression Levels -->
          <div class="advanced-group">
            <h3 class="toggle-group-heading">Compression Levels</h3>
            <div class="advanced-fields">
              {#each COMPRESSION_FIELDS as field}
                <QualitySlider
                  label={field.label}
                  bind:value={config[field.key]}
                  min={field.min}
                  max={field.max}
                  step={field.step}
                  changed={isFieldChanged(field.key)}
                  disabled={!connected}
                  tooltip={field.tooltip}
                  oninput={handleSliderInput}
                  onchange={handleSliderChange}
                />
              {/each}
            </div>
          </div>

          <!-- Viewport Widths -->
          <div class="advanced-group">
            <h3 class="toggle-group-heading">Viewport Breakpoints</h3>
            <div class="advanced-fields">
              {#each VIEWPORT_FIELDS as field}
                <div class="number-field">
                  <span class="number-label">
                    {field.label}
                    {#if isFieldChanged(field.key)}
                      <span class="changed-dot" title="Modified"></span>
                    {/if}
                    {#if field.tooltip}
                      <HelpIcon tooltip={field.tooltip} />
                    {/if}
                  </span>
                  <div class="number-input-wrapper">
                    <input
                      type="number"
                      class="ps-input number-input"
                      bind:value={config[field.key]}
                      min={field.min}
                      max={field.max}
                      step={field.step}
                      disabled={!connected}
                      aria-label={field.label}
                      onchange={handleSliderChange}
                    />
                    {#if field.unit}
                      <span class="number-unit">{field.unit}</span>
                    {/if}
                  </div>
                </div>
              {/each}
            </div>
          </div>

          <!-- Size Limits -->
          <div class="advanced-group">
            <h3 class="toggle-group-heading">Size Limits</h3>
            <div class="advanced-fields">
              {#each LIMIT_FIELDS as field}
                <QualitySlider
                  label={field.label}
                  bind:value={config[field.key]}
                  min={field.min}
                  max={field.max}
                  step={field.step}
                  changed={isFieldChanged(field.key)}
                  disabled={!connected}
                  tooltip={field.tooltip}
                  oninput={handleSliderInput}
                  onchange={handleSliderChange}
                />
              {/each}
            </div>
          </div>

          <!-- SVG Auto-Vectorization -->
          <div class="advanced-group">
            <h3 class="toggle-group-heading">SVG Auto-Vectorization</h3>
            {#each SVG_FEATURE_TOGGLES as field}
              <FeatureToggle
                label={field.label}
                description={field.description}
                checked={getToggleValue(config, field.key, field.invertDisplay)}
                changed={isFieldChanged(field.key)}
                disabled={!connected}
                onchange={() => {
                  const current = getToggleValue(config, field.key, field.invertDisplay);
                  setToggleValue(field.key, !current, field.invertDisplay);
                }}
              />
            {/each}
            <div class="advanced-fields">
              {#each [SVG_MODE_FIELD, SVG_PRESET_FIELD] as selectField}
                <div class="number-field">
                  <span class="number-label">
                    {selectField.label}
                    {#if isFieldChanged(selectField.key)}
                      <span class="changed-dot" title="Modified"></span>
                    {/if}
                    {#if selectField.tooltip}
                      <HelpIcon tooltip={selectField.tooltip} />
                    {/if}
                  </span>
                  <select
                    class="ps-select"
                    bind:value={config[selectField.key]}
                    disabled={!connected}
                    aria-label={selectField.label}
                    onchange={handleSliderChange}
                  >
                    {#each selectField.options as opt}
                      <option value={opt.value}>{opt.label}</option>
                    {/each}
                  </select>
                </div>
              {/each}
              {#each SVG_FIELDS as field}
                <div class="number-field">
                  <span class="number-label">
                    {field.label}
                    {#if isFieldChanged(field.key)}
                      <span class="changed-dot" title="Modified"></span>
                    {/if}
                    {#if field.tooltip}
                      <HelpIcon tooltip={field.tooltip} />
                    {/if}
                  </span>
                  <div class="number-input-wrapper">
                    <input
                      type="number"
                      class="ps-input number-input"
                      bind:value={config[field.key]}
                      min={field.min}
                      max={field.max}
                      step={field.step}
                      disabled={!connected}
                      aria-label={field.label}
                      onchange={handleSliderChange}
                    />
                    {#if field.unit}
                      <span class="number-unit">{field.unit}</span>
                    {/if}
                  </div>
                </div>
              {/each}
            </div>
          </div>

          <!-- General -->
          <div class="advanced-group">
            <h3 class="toggle-group-heading">General</h3>
            {#each GENERAL_FEATURE_TOGGLES as field}
              <FeatureToggle
                label={field.label}
                description={field.description}
                checked={getToggleValue(config, field.key, field.invertDisplay)}
                changed={isFieldChanged(field.key)}
                disabled={!connected}
                onchange={() => {
                  const current = getToggleValue(config, field.key, field.invertDisplay);
                  setToggleValue(field.key, !current, field.invertDisplay);
                }}
              />
            {/each}
          </div>

          <!-- Browser Analysis -->
          <div class="advanced-group">
            <h3 class="toggle-group-heading">Browser Analysis</h3>
            <p class="reserved-note">
              Browser analysis uses headless Chrome for critical CSS extraction, LCP detection, and lazy loading decisions. These settings are CLI-only (<code>--enable-browser-analysis</code>, etc.) and cannot be changed at runtime via the config API.
            </p>
            {#each BROWSER_FEATURE_TOGGLES as field}
              <FeatureToggle
                label={field.label}
                description={field.description}
                checked={getToggleValue(config, field.key, field.invertDisplay)}
                changed={isFieldChanged(field.key)}
                disabled={!connected}
                onchange={() => {
                  const current = getToggleValue(config, field.key, field.invertDisplay);
                  setToggleValue(field.key, !current, field.invertDisplay);
                }}
              />
            {/each}
            <div class="advanced-fields">
              {#each BROWSER_FIELDS as field}
                <div class="number-field">
                  <span class="number-label">
                    {field.label}
                    {#if isFieldChanged(field.key)}
                      <span class="changed-dot" title="Modified"></span>
                    {/if}
                    {#if field.tooltip}
                      <HelpIcon tooltip={field.tooltip} />
                    {/if}
                  </span>
                  <div class="number-input-wrapper">
                    <input
                      type="number"
                      class="ps-input number-input"
                      bind:value={config[field.key]}
                      min={field.min}
                      max={field.max}
                      step={field.step}
                      disabled={!connected}
                      aria-label={field.label}
                      onchange={handleSliderChange}
                    />
                    {#if field.unit}
                      <span class="number-unit">{field.unit}</span>
                    {/if}
                  </div>
                </div>
              {/each}
            </div>
          </div>
        </div>
      {/if}
    </section>

    <!-- Action bar -->
    <div class="action-bar">
      <button
        class="ps-btn ps-btn-primary"
        onclick={() => checkSafetyGate(applyConfig) && applyConfig()}
        disabled={!hasChanges || saving || !connected}
      >
        {#if saving}
          <span class="ps-spinner" style="width:14px;height:14px" aria-hidden="true"></span>
        {/if}
        Apply
      </button>
      <button
        class="ps-btn ps-btn-secondary"
        onclick={() => checkSafetyGate(applyAndReoptimize) && applyAndReoptimize()}
        disabled={!hasChanges || saving || !connected}
      >
        Apply & Re-optimize
      </button>
      <button
        class="ps-btn ps-btn-ghost"
        onclick={resetToDefaults}
        disabled={saving || !connected}
      >
        Reset to Defaults
      </button>
    </div>
  </div>
{/if}

{#if safetyConfirmOpen}
  <!-- svelte-ignore a11y_no_static_element_interactions -->
  <div class="safety-overlay" onclick={handleSafetyCancel} onkeydown={(e) => { if (e.key === 'Escape') handleSafetyCancel(); }} role="presentation">
    <!-- svelte-ignore a11y_no_static_element_interactions -->
    <div class="safety-dialog" onclick={(e) => e.stopPropagation()} onkeydown={() => {}} role="dialog" aria-labelledby="safety-title">
      <h3 id="safety-title" class="safety-title">Confirm aggressive quality change</h3>
      <ul class="safety-warnings">
        {#each safetyWarnings as warning}
          <li>{warning}</li>
        {/each}
      </ul>
      <div class="safety-actions">
        <button class="ps-btn ps-btn-primary" onclick={handleSafetyConfirm}>
          Apply Anyway
        </button>
        <button class="ps-btn ps-btn-ghost" onclick={handleSafetyCancel}>
          Cancel
        </button>
      </div>
    </div>
  </div>
{/if}

<style>
  /* Layout */
  .config-panel {
    padding: var(--ps-space-lg) var(--ps-space-xl);
    max-width: 900px;
  }

  .config-loading {
    display: flex;
    align-items: center;
    justify-content: center;
    min-height: 60vh;
    padding: var(--ps-space-xl);
  }

  .loading-card {
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: var(--ps-space-md);
    max-width: 320px;
    text-align: center;
  }

  .loading-text {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size);
    color: var(--ps-fg-secondary);
    margin: 0;
  }

  .error-text {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size);
    color: var(--ps-error);
    margin: 0;
  }

  /* Header */
  .config-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    margin-bottom: var(--ps-space-lg);
    flex-wrap: wrap;
    gap: var(--ps-space-sm);
  }

  .config-header-left {
    display: flex;
    align-items: center;
    gap: var(--ps-space-sm);
  }

  .config-header-right {
    display: flex;
    align-items: center;
    gap: var(--ps-space-xs);
  }

  .config-title {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-lg);
    font-weight: 600;
    color: var(--ps-fg-primary);
    margin: 0;
  }

  /* Status messages */
  .config-message {
    padding: var(--ps-space-sm) var(--ps-space-md);
    border-radius: var(--ps-radius-sm);
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size);
    margin-bottom: var(--ps-space-md);
  }

  .config-message-error {
    background: var(--ps-error-bg);
    color: var(--ps-error);
    border: 1px solid var(--ps-error);
  }

  .config-message-success {
    background: var(--ps-success-bg);
    color: var(--ps-success);
    border: 1px solid var(--ps-success);
  }

  .config-message-warning {
    background: var(--ps-warning-bg);
    color: var(--ps-warning);
    border: 1px solid var(--ps-warning);
  }

  /* Sections */
  .config-section {
    background: var(--ps-bg-primary);
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-radius-md);
    padding: var(--ps-space-lg);
    margin-bottom: var(--ps-space-md);
  }

  .section-heading {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size);
    font-weight: 600;
    color: var(--ps-fg-primary);
    margin: 0 0 var(--ps-space-md);
  }

  /* Presets */
  .preset-bar {
    display: flex;
    flex-wrap: wrap;
    gap: var(--ps-space-xs);
  }

  .preset-btn {
    flex: 1;
    min-width: 120px;
  }

  /* Quality grid */
  .quality-grid {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: var(--ps-space-xl);
  }

  .quality-column {
    display: flex;
    flex-direction: column;
    gap: var(--ps-space-md);
  }

  .quality-subheading {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    font-weight: 600;
    color: var(--ps-fg-secondary);
    text-transform: uppercase;
    letter-spacing: 0.03em;
    margin: 0;
  }

  .quality-ssim {
    margin-top: var(--ps-space-lg);
    padding-top: var(--ps-space-md);
    border-top: 1px solid var(--ps-border);
  }

  .quality-ssim-fields {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: var(--ps-space-md) var(--ps-space-xl);
    margin-top: var(--ps-space-md);
  }

  /* Collapsible sections */
  .section-toggle {
    display: flex;
    align-items: center;
    gap: var(--ps-space-sm);
    width: 100%;
    padding: 0;
    margin: 0;
    background: none;
    border: none;
    cursor: pointer;
    text-align: left;
    font-family: var(--ps-font-family);
    color: var(--ps-fg-primary);
  }

  .section-toggle:hover {
    opacity: 0.8;
  }

  .section-toggle:focus-visible {
    outline: 2px solid var(--ps-border-focus);
    outline-offset: 2px;
    border-radius: var(--ps-radius-sm);
  }

  .section-heading-toggle {
    margin: 0;
  }

  .toggle-chevron {
    flex-shrink: 0;
    transition: transform 0.15s ease;
  }

  .toggle-chevron-open {
    transform: rotate(90deg);
  }

  .section-count {
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-muted);
    font-weight: 400;
  }

  /* Toggle groups */
  .toggle-sections {
    display: flex;
    flex-direction: column;
    gap: var(--ps-space-lg);
    margin-top: var(--ps-space-lg);
    padding-top: var(--ps-space-md);
    border-top: 1px solid var(--ps-border);
  }

  .toggle-group {
    display: flex;
    flex-direction: column;
    gap: var(--ps-space-xs);
  }

  .toggle-group-heading {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    font-weight: 600;
    color: var(--ps-fg-secondary);
    text-transform: uppercase;
    letter-spacing: 0.03em;
    margin: 0 0 var(--ps-space-xs);
  }

  /* Advanced sections */
  .advanced-sections {
    display: flex;
    flex-direction: column;
    gap: var(--ps-space-lg);
    margin-top: var(--ps-space-lg);
    padding-top: var(--ps-space-md);
    border-top: 1px solid var(--ps-border);
  }

  .advanced-group {
    display: flex;
    flex-direction: column;
    gap: var(--ps-space-sm);
  }

  .advanced-fields {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: var(--ps-space-md) var(--ps-space-xl);
  }

  /* Number field (for viewport widths etc.) */
  .number-field {
    display: flex;
    flex-direction: column;
    gap: var(--ps-space-xs);
  }

  .number-label {
    display: flex;
    align-items: center;
    gap: var(--ps-space-xs);
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    font-weight: 500;
    color: var(--ps-fg-secondary);
  }

  .changed-dot {
    display: inline-block;
    width: 6px;
    height: 6px;
    border-radius: 50%;
    background: var(--ps-accent);
    flex-shrink: 0;
  }

  .number-input-wrapper {
    display: flex;
    align-items: center;
    gap: var(--ps-space-xs);
  }

  .number-input {
    width: 120px;
    text-align: right;
    font-family: var(--ps-font-mono);
  }

  .number-unit {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-muted);
  }

  /* SVG reserved note */
  .reserved-note {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-muted);
    font-style: italic;
    margin: 0 0 var(--ps-space-sm);
    line-height: 1.5;
  }

  /* Action bar */
  .action-bar {
    display: flex;
    align-items: center;
    gap: var(--ps-space-sm);
    padding: var(--ps-space-md) 0;
    position: sticky;
    bottom: 0;
    background: var(--ps-bg-primary);
    border-top: 1px solid var(--ps-border);
    margin-top: var(--ps-space-md);
    padding-top: var(--ps-space-md);
  }

  /* Responsive */
  @media (max-width: 767px) {
    .config-panel {
      padding: var(--ps-space-md);
    }

    .quality-grid {
      grid-template-columns: 1fr;
      gap: var(--ps-space-lg);
    }

    .quality-ssim-fields {
      grid-template-columns: 1fr;
    }

    .advanced-fields {
      grid-template-columns: 1fr;
    }

    .preset-bar {
      flex-direction: column;
    }

    .preset-btn {
      min-width: unset;
    }

    .action-bar {
      flex-direction: column;
    }

    .action-bar .ps-btn {
      width: 100%;
    }
  }

  /* Safety confirmation dialog */
  .safety-overlay {
    position: fixed;
    inset: 0;
    z-index: 200;
    background: rgba(0, 0, 0, 0.4);
    display: flex;
    align-items: center;
    justify-content: center;
  }

  .safety-dialog {
    background: var(--ps-bg-primary);
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-radius-lg);
    padding: var(--ps-space-xl);
    max-width: 480px;
    width: 90%;
    box-shadow: 0 4px 24px rgba(0, 0, 0, 0.15);
  }

  .safety-title {
    margin: 0 0 var(--ps-space-md);
    font-size: var(--ps-font-size-lg);
    color: var(--ps-warning);
  }

  .safety-warnings {
    margin: 0 0 var(--ps-space-lg);
    padding-left: var(--ps-space-lg);
    color: var(--ps-fg-secondary);
    line-height: 1.6;
  }

  .safety-actions {
    display: flex;
    gap: var(--ps-space-sm);
    justify-content: flex-end;
  }
</style>
