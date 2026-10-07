<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2024-2026 We-Amp B.V.
-->

<script lang="ts">
  // A page's first load failed and there is nothing to show yet: announced
  // at once (role="alert"). A failed refresh above data already on screen is
  // RefreshNotice's job (polite).
  let { message }: { message: string } = $props();
</script>

<p class="load-error" role="alert">{message}</p>

<style>
  .load-error {
    color: var(--ps-error);
    padding: var(--ps-space-md) 0;
    overflow-wrap: anywhere;
  }
</style>
