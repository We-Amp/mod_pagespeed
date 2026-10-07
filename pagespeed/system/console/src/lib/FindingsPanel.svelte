<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2024-2026 We-Amp B.V.
-->

<script lang="ts">
  // The overview's findings, worst first: what is wrong, the fix, and where
  // to read more. Acknowledged findings wait in a collapsed group of their
  // own until their condition clears (alerts.ts). Messages and details may
  // carry message-log text -- visitor-controlled URLs and host names -- so
  // everything here renders as text; the only links are the rule's own
  // route and documentation constants.
  import type { ActiveAlert, AlertSeverity } from "$lib/alerts";

  let {
    findings,
    acknowledged,
    onAcknowledge,
    onUnacknowledge,
  }: {
    findings: ActiveAlert[];
    acknowledged: ActiveAlert[];
    onAcknowledge: (id: string) => void;
    onUnacknowledge: (id: string) => void;
  } = $props();

  const SEVERITY_WORD: Record<AlertSeverity, string> = { error: "Error", warning: "Warning", info: "Info" };
</script>

{#if findings.length > 0}
  <section class="findings" aria-label="Findings" role="status" aria-live="polite" data-testid="alerts">
    <ul class="finding-list">
      {#each findings as finding (finding.id)}
        <li class="finding finding-{finding.severity}" data-testid="alert-{finding.id}">
          <span class="level-badge level-badge-{finding.severity}">{SEVERITY_WORD[finding.severity]}</span>
          <div class="finding-body">
            <p class="finding-why">
              <strong class="finding-label">{finding.label}</strong>
              <span class="alert-message">{finding.message}</span>
            </p>
            {#if finding.details.length > 0}
              <ul class="finding-details" data-testid="finding-details">
                {#each finding.details as line, i (i)}
                  <li>{line}</li>
                {/each}
              </ul>
            {/if}
            <p class="finding-fix" data-testid="finding-fix"><span class="fix-label">Fix:</span> {finding.fix}</p>
            {#if finding.link || finding.doc}
              <p class="finding-links">
                {#if finding.link}
                  <a href={finding.link}>Details</a>
                {/if}
                {#if finding.doc}
                  <a href={finding.doc} target="_blank" rel="noopener noreferrer">Documentation</a>
                {/if}
              </p>
            {/if}
          </div>
          <button
            type="button"
            class="finding-button btn btn-secondary btn-compact"
            onclick={() => onAcknowledge(finding.id)}
            aria-label="Acknowledge {finding.label}"
          >Acknowledge</button>
        </li>
      {/each}
    </ul>
  </section>
{/if}

{#if acknowledged.length > 0}
  <details class="acknowledged" data-testid="findings-acknowledged">
    <summary>Acknowledged ({acknowledged.length})</summary>
    <ul class="finding-list">
      {#each acknowledged as finding (finding.id)}
        <li class="finding finding-info" data-testid="acknowledged-{finding.id}">
          <span class="level-badge level-badge-info">Info</span>
          <div class="finding-body">
            <p class="finding-why">
              <strong class="finding-label">{finding.label}</strong>
              <span class="alert-message">{finding.message}</span>
            </p>
          </div>
          <button
            type="button"
            class="finding-button btn btn-secondary btn-compact"
            onclick={() => onUnacknowledge(finding.id)}
            aria-label="Un-acknowledge {finding.label}"
          >Un-acknowledge</button>
        </li>
      {/each}
    </ul>
  </details>
{/if}

<style>
  .findings {
    margin-bottom: var(--ps-space-md);
  }

  .finding-list {
    display: flex;
    flex-direction: column;
    gap: var(--ps-space-sm);
    margin: 0;
    padding: 0;
    list-style: none;
  }

  /* Text stays in --ps-text (AA contrast); the severity colour is on the border and the badge. */
  .finding {
    display: flex;
    flex-wrap: wrap;
    align-items: flex-start;
    gap: var(--ps-space-sm) var(--ps-space-md);
    padding: var(--ps-space-sm) var(--ps-space-md);
    border: 1px solid var(--ps-border);
    border-left-width: 4px;
    border-radius: var(--ps-border-radius);
    background: var(--ps-bg-secondary);
    color: var(--ps-text);
  }

  .finding-error {
    border-left-color: var(--ps-error);
  }

  .finding-warning {
    border-left-color: var(--ps-warning);
  }

  .finding-info {
    border-left-color: var(--ps-primary);
  }

  /* The text keeps a readable measure and the button follows it; the bar
     itself still spans the row. */
  .finding-body {
    flex: 0 1 75ch;
    min-width: 0;
    font-size: var(--ps-font-size-sm);
    overflow-wrap: anywhere;
  }

  .finding-why,
  .finding-fix,
  .finding-links {
    margin: 0 0 var(--ps-space-xs);
  }

  .finding-label {
    margin-right: var(--ps-space-xs);
  }

  /* A log excerpt keeps the text's measure: one line, cut with an ellipsis. */
  .finding-details li {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  /* Its width comes from the text block, never from the excerpt's length. */
  .finding-details {
    contain: inline-size;
    margin: 0 0 var(--ps-space-xs) var(--ps-space-md);
    padding: 0;
    font-family: var(--ps-font-mono);
    font-size: var(--ps-font-size-xs);
  }

  .fix-label {
    font-weight: 600;
  }

  .finding-links {
    display: flex;
    flex-wrap: wrap;
    gap: var(--ps-space-md);
  }

  .finding-links a {
    color: var(--ps-primary);
  }

  .finding-button {
    flex-shrink: 0;
  }

  .finding-button:focus-visible {
    background: var(--ps-surface-hover);
  }

  .acknowledged {
    margin-bottom: var(--ps-space-md);
    font-size: var(--ps-font-size-sm);
  }

  .acknowledged summary {
    cursor: pointer;
    color: var(--ps-text-secondary);
    margin-bottom: var(--ps-space-sm);
  }
</style>
