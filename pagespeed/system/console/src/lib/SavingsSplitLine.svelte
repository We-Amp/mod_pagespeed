<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2024-2026 We-Amp B.V. -->
<script lang="ts">
  import { SPLIT_TEXT } from "$lib/data/split-text";
  import { formatSignedBytes } from "$lib/utils/format";
  import type { EncodingSplit } from "$lib/utils/serve-savings";

  let { split }: { split: EncodingSplit | null } = $props();

  const coveragePercent = $derived(split === null ? 0 : Math.floor(split.coverage * 100));
</script>

{#if split !== null && split.available}
  <dl class="split" data-testid="savings-split">
    <div class="split-row">
      <dt>{SPLIT_TEXT.encodingLabel}</dt>
      <dd class="num" data-testid="savings-split-encoding">{formatSignedBytes(split.encodingSaved)}</dd>
    </div>
    <div class="split-row">
      <dt>{SPLIT_TEXT.rewriteLabel}</dt>
      <dd class="num" data-testid="savings-split-rewrite">
        {formatSignedBytes(split.rewriteSaved)}
        {#if split.rewriteSaved < 0}<span class="detail" data-testid="savings-split-negative-note"
          >{split.htmlGrew ? SPLIT_TEXT.negativeNoteMarkup : SPLIT_TEXT.negativeNote}</span
        >{/if}
      </dd>
    </div>
    {#if split.coverage < 1}
      <div class="split-row">
        <dt>{SPLIT_TEXT.unsplitLabel}</dt>
        <dd class="num" data-testid="savings-split-unsplit">{formatSignedBytes(split.unsplit)}</dd>
      </div>
      <p class="note" data-testid="savings-split-coverage">{SPLIT_TEXT.coverageNote(coveragePercent)}</p>
    {/if}
  </dl>
{:else if split === null}
  <p class="note" data-testid="savings-split-not-reported">{SPLIT_TEXT.notReported}</p>
{:else}
  <p class="note" data-testid="savings-split-unavailable">{SPLIT_TEXT.unavailable}</p>
{/if}

<style>
  .split {
    margin: 0.5rem 0 0;
  }
  .split-row {
    display: flex;
    justify-content: space-between;
    gap: 1rem;
  }
  .split-row dt {
    margin: 0;
  }
  .split-row dd {
    margin: 0;
  }
</style>
